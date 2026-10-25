// kernel/usb/usb_proxy.c
//
// [FASE 5.4] HCD proxy: implementa usb_hcd_t reenviando cada operación a un
// controlador xHCI que corre en Ring 3 (xhci.aur) a través de /dev/usbproxy.
// Contrato del ABI en aur.h.
//
//   usb_core / hub / hid / storage
//        ↓ hcd->control_xfer / interrupt_xfer / new_device / ...
//   uproxy_call()  ──pending──►  UPROXY_FETCH ──► xhci.aur
//        ▲                                            │
//        └── complete() ◄── UPROXY_COMPLETE ◄─────────┘
//
// El que llama bloquea (completion) hasta que el .aur completa la petición.
// Si el .aur muere/cierra el fd, todo lo pendiente falla con -EIO.
//
// [FASE 6.1] Cada petición tiene plazo: su timeout_ms + UPROXY_GRACE_MS (para
// que el .aur informe antes de su propio timeout), o UPROXY_DEFAULT_MS si la
// operación no lleva timeout. Las transferencias con timeout_ms == 0 (p.ej.
// el interrupt-IN de un HID, que espera indefinidamente a propósito) no
// caducan. Un watchdog, activo mientras el .aur está conectado, falla con
// -ETIMEDOUT las vencidas; un COMPLETE tardío recibe -ENOENT.
//
// La política de hotplug (qué puertos escanear, reintentos, enumerar) vive
// aquí, en un kthread; el .aur solo expone la mecánica de PORT_STATUS /
// PORT_RESET y avisa con UPROXY_EVENT cuando cambia un puerto.

#include "usb_proxy.h"
#include "../aur.h"
#include "../completion.h"
#include "../heap.h"
#include "../klog.h"
#include "../kthread.h"
#include "../process.h"
#include "../sched.h"
#include "../signal.h"
#include "../spinlock.h"
#include "../string.h"
#include "../test.h"
#include "../uaccess.h"
#include "../wait.h"
#include "usb_core.h"

#define UPROXY_GRACE_MS 5000u
#define UPROXY_DEFAULT_MS 15000u // NEW_DEVICE, CONFIGURE_EP, PORT_*, ...
#define UPROXY_WATCHDOG_PERIOD_MS 1000u
#define UPROXY_NO_DEADLINE UINT64_MAX

typedef struct uproxy_req {
  struct uproxy_req *next;
  uint64_t deadline; // tick límite, UPROXY_NO_DEADLINE = sin plazo
  struct aur_usb_req req;
  const void *data_out; // payload OUT (buffer del llamante)
  void *data_in;        // destino de los datos IN (buffer del llamante)
  completion_t done;
  int status;
  uint32_t actual, out0, out1;
} uproxy_req_t;

typedef struct {
  uint8_t slot;
} uproxy_dev_t;

static uproxy_req_t *g_pend_head, *g_pend_tail; // aún no entregadas
static uproxy_req_t *g_inflight;                // entregadas, sin completar
static spinlock_t g_lock = {.locked = 0};
static wait_queue_t g_wq;     // waiters de FETCH
static wait_queue_t g_hub_wq; // hotplug
static uint64_t g_next_tag;
static int g_open;
static vfs_node_t *g_owner;
static int g_ready;
static uint32_t g_max_ports;
static int g_thread_started;
static volatile int g_hotplug_pending;
static volatile int g_attach_busy;
static uint32_t g_attached[8]; // bitmap de puertos raíz ya enumerados
static int g_init_done;
static int g_wd_running; // hay un watchdog vivo (protegido por g_lock)
static uint32_t g_owner_pid; // pid del .aur conectado
static int g_wd_killed;      // ya se mató al driver de esta conexión
static wait_queue_t g_wd_wq;

extern usb_hcd_t g_uproxy_hcd;

// ---------------------------------------------------------------------------
// Modo usuario
// ---------------------------------------------------------------------------
int xhci_user_mode(void) {
  static int cached = -1;
  if (cached < 0) {
    vfs_node_t *n = vfs_lookup("/usr/lib/aur/xhci.user");
    cached = n ? 1 : 0;
    if (n)
      vfs_node_free(n);
  }
  return cached;
}

// ---------------------------------------------------------------------------
// Colas (llamar con g_lock tomado)
// ---------------------------------------------------------------------------
static void pend_push(uproxy_req_t *r) {
  r->next = NULL;
  if (g_pend_tail)
    g_pend_tail->next = r;
  else
    g_pend_head = r;
  g_pend_tail = r;
}

static uproxy_req_t *pend_pop(void) {
  uproxy_req_t *r = g_pend_head;
  if (r) {
    g_pend_head = r->next;
    if (!g_pend_head)
      g_pend_tail = NULL;
    r->next = NULL;
  }
  return r;
}

static void inflight_add(uproxy_req_t *r) {
  r->next = g_inflight;
  g_inflight = r;
}

static uproxy_req_t *inflight_remove(uint64_t tag) {
  for (uproxy_req_t **pp = &g_inflight; *pp; pp = &(*pp)->next) {
    if ((*pp)->req.tag == tag) {
      uproxy_req_t *r = *pp;
      *pp = r->next;
      r->next = NULL;
      return r;
    }
  }
  return NULL;
}

// Completa una petición. Tras complete() el llamante puede liberar `r`.
static void uproxy_finish(uproxy_req_t *r, int status) {
  r->status = status;
  complete(&r->done);
}

// Plazo de una petición a partir de `now`. Se renueva al entregarla al .aur.
static uint64_t req_deadline(const struct aur_usb_req *q, uint64_t now) {
  if (q->op == AUR_USB_OP_CONTROL || q->op == AUR_USB_OP_XFER) {
    if (q->timeout_ms == 0)
      return UPROXY_NO_DEADLINE;
    return now + q->timeout_ms + UPROXY_GRACE_MS;
  }
  return now + UPROXY_DEFAULT_MS;
}

// Falla con -ETIMEDOUT las peticiones vencidas (deadline < now), pendientes o
// en vuelo. Devuelve cuántas.
static int uproxy_reap_expired(uint64_t now) {
  uproxy_req_t *failed = NULL;

  unsigned long fl = spin_lock_irqsave(&g_lock);
  uproxy_req_t *prev = NULL;
  for (uproxy_req_t *r = g_pend_head, *nx; r; r = nx) {
    nx = r->next;
    if (r->deadline != UPROXY_NO_DEADLINE && now > r->deadline) {
      if (prev)
        prev->next = nx;
      else
        g_pend_head = nx;
      if (g_pend_tail == r)
        g_pend_tail = prev;
      r->next = failed;
      failed = r;
    } else {
      prev = r;
    }
  }
  for (uproxy_req_t **pp = &g_inflight; *pp;) {
    uproxy_req_t *r = *pp;
    if (r->deadline != UPROXY_NO_DEADLINE && now > r->deadline) {
      *pp = r->next;
      r->next = failed;
      failed = r;
    } else {
      pp = &r->next;
    }
  }
  spin_unlock_irqrestore(&g_lock, fl);

  int n = 0;
  for (uproxy_req_t *r = failed, *nx; r; r = nx) {
    nx = r->next; // leer antes de completar: el llamante puede liberar r
    uproxy_finish(r, -ETIMEDOUT);
    n++;
  }
  if (n)
    LOG_ERR("[UPROXY] %d peticiones sin respuesta: -ETIMEDOUT", n);
  return n;
}

// [FASE 6.2] Un driver que deja vencer peticiones está colgado: se le mata
// (una vez por conexión) para que el supervisor de aur_bus lo relance (6.3).
static void uproxy_kill_hung_driver(void) {
  unsigned long fl = spin_lock_irqsave(&g_lock);
  if (!g_open || g_wd_killed) {
    spin_unlock_irqrestore(&g_lock, fl);
    return;
  }
  g_wd_killed = 1;
  uint32_t pid = g_owner_pid;
  spin_unlock_irqrestore(&g_lock, fl);
  LOG_ERR("[UPROXY] driver pid=%u no responde: SIGKILL", pid);
  process_kill(pid, SIGKILL);
}

static bool uproxy_wd_never(void *arg) {
  (void)arg;
  return false;
}

// Corre mientras el .aur está conectado. Sale (y limpia g_wd_running bajo
// g_lock, atómico respecto a uproxy_open) cuando se desconecta.
static int uproxy_watchdog(void *arg) {
  (void)arg;
  for (;;) {
    wait_event_timeout(&g_wd_wq, uproxy_wd_never, NULL,
                       UPROXY_WATCHDOG_PERIOD_MS);
    unsigned long fl = spin_lock_irqsave(&g_lock);
    if (!g_open) {
      g_wd_running = 0;
      spin_unlock_irqrestore(&g_lock, fl);
      return 0;
    }
    spin_unlock_irqrestore(&g_lock, fl);
    if (uproxy_reap_expired(sched_get_ticks()) > 0)
      uproxy_kill_hung_driver();
  }
}

// ---------------------------------------------------------------------------
// Envío de una petición y espera
// ---------------------------------------------------------------------------
static int uproxy_call(uproxy_req_t *r) {
  completion_init(&r->done);
  r->status = 0;
  r->actual = r->out0 = r->out1 = 0;
  r->next = NULL;

  unsigned long fl = spin_lock_irqsave(&g_lock);
  if (!g_open || !g_ready) {
    spin_unlock_irqrestore(&g_lock, fl);
    return -ENODEV;
  }
  r->req.tag = ++g_next_tag;
  r->deadline = req_deadline(&r->req, sched_get_ticks());
  pend_push(r);
  spin_unlock_irqrestore(&g_lock, fl);

  wake_up_all(&g_wq);
  wait_for_completion_uninterruptible(&r->done);
  return r->status;
}

static int uproxy_simple(uint32_t op, uint32_t slot) {
  uproxy_req_t r;
  memset(&r, 0, sizeof(r));
  r.req.op = op;
  r.req.slot = slot;
  return uproxy_call(&r);
}

// ---------------------------------------------------------------------------
// usb_hcd_t
// ---------------------------------------------------------------------------
static uint8_t dev_slot(usb_device_t *dev) {
  return dev && dev->hc_priv ? ((uproxy_dev_t *)dev->hc_priv)->slot : 0;
}

static int uproxy_hcd_alloc_device(usb_device_t *dev) {
  if (!dev)
    return -EINVAL;
  if (dev->hc_priv)
    return 0;
  dev->hc_priv = kzalloc(sizeof(uproxy_dev_t));
  return dev->hc_priv ? 0 : -ENOMEM;
}

static void uproxy_hcd_free_device(usb_device_t *dev) {
  if (!dev || !dev->hc_priv)
    return;
  uint8_t slot = dev_slot(dev);
  if (slot)
    uproxy_simple(AUR_USB_OP_FREE_DEVICE, slot); // Disable Slot
  kfree(dev->hc_priv);
  dev->hc_priv = NULL;
}

static int uproxy_hcd_new_device(usb_device_t *parent_hub, uint8_t port,
                                 usb_speed_t speed, usb_device_t **out_dev) {
  if (!out_dev)
    return -EINVAL;
  *out_dev = NULL;

  usb_device_t *dev = usb_device_alloc();
  if (!dev)
    return -ENOMEM;
  dev->speed = speed;
  dev->parent_hub = parent_hub;
  dev->port_on_parent = port;
  dev->hcd = &g_uproxy_hcd;
  int rc = uproxy_hcd_alloc_device(dev);
  if (rc != 0) {
    usb_device_free(dev);
    return rc;
  }

  uproxy_req_t r;
  memset(&r, 0, sizeof(r));
  r.req.op = AUR_USB_OP_NEW_DEVICE;
  r.req.speed = (uint8_t)speed;
  r.req.port = port;
  r.req.route = usb_device_route(dev);
  r.req.parent_slot = dev_slot(parent_hub);
  r.req.parent_speed = parent_hub ? (uint8_t)parent_hub->speed : 0;
  const usb_device_t *top = parent_hub;
  while (top && top->parent_hub)
    top = top->parent_hub;
  r.req.root_port = top ? top->port_on_parent : port;

  rc = uproxy_call(&r);
  if (rc != 0 || r.out0 == 0) {
    usb_device_free(dev); // slot == 0: free_device no envía FREE_DEVICE
    return rc != 0 ? rc : -EIO;
  }
  ((uproxy_dev_t *)dev->hc_priv)->slot = (uint8_t)r.out0;
  dev->addr = (uint8_t)r.out0;
  dev->max_packet_size_ep0 = r.out1 > 0xFF ? 0xFF : (uint8_t)r.out1;

  LOG_INFO("[UPROXY] new device slot=%u route=0x%05x speed=%d parent=%s port=%u",
           dev->addr, r.req.route, (int)speed, parent_hub ? "hub" : "root",
           port);
  *out_dev = dev;
  return 0;
}

static int uproxy_hcd_set_hub_info(usb_device_t *dev, uint8_t num_ports) {
  uint8_t slot = dev_slot(dev);
  if (!slot)
    return -EINVAL;
  uproxy_req_t r;
  memset(&r, 0, sizeof(r));
  r.req.op = AUR_USB_OP_SET_HUB;
  r.req.slot = slot;
  r.req.num_ports = num_ports;
  return uproxy_call(&r);
}

static int uproxy_hcd_configure_ep(usb_device_t *dev, usb_endpoint_t *ep) {
  uint8_t slot = dev_slot(dev);
  if (!slot || !ep)
    return -EINVAL;
  // Streams (UAS) no están soportados en el xHCI de usuario (v1).
  if (ep->stream_id != 0)
    return -EOPNOTSUPP;
  uproxy_req_t r;
  memset(&r, 0, sizeof(r));
  r.req.op = AUR_USB_OP_CONFIGURE_EP;
  r.req.slot = slot;
  r.req.ep_addr = ep->addr;
  r.req.ep_type = ep->type;
  r.req.max_packet = ep->max_packet;
  r.req.interval = ep->interval;
  r.req.speed = (uint8_t)dev->speed;
  int rc = uproxy_call(&r);
  if (rc == 0)
    ep->hc_ep = dev->hc_priv; // marca "configurado"
  return rc;
}

static int uproxy_hcd_control_xfer(usb_device_t *dev, const uint8_t setup[8],
                                   void *data, int data_len, int is_in,
                                   int timeout_ms) {
  uint8_t slot = dev_slot(dev);
  if (!slot || !setup)
    return -EINVAL;
  int has_data = (data_len > 0 && data != NULL);
  if (has_data && (uint32_t)data_len > AUR_USB_MAX_XFER)
    return -EINVAL;

  uproxy_req_t r;
  memset(&r, 0, sizeof(r));
  r.req.op = AUR_USB_OP_CONTROL;
  r.req.slot = slot;
  memcpy(r.req.setup, setup, 8);
  r.req.is_in = is_in ? 1 : 0;
  r.req.len = has_data ? (uint32_t)data_len : 0;
  r.req.timeout_ms = timeout_ms > 0 ? (uint32_t)timeout_ms : 5000;
  if (has_data) {
    if (is_in)
      r.data_in = data;
    else
      r.data_out = data;
  }
  int rc = uproxy_call(&r);
  if (rc != 0)
    return rc;
  return has_data ? (int)r.actual : 0;
}

// Un trozo (<= AUR_USB_MAX_XFER) de bulk/interrupt.
static int uproxy_xfer_chunk(usb_device_t *dev, usb_endpoint_t *ep, void *buf,
                             int len, int timeout_ms, int *actual) {
  uint8_t slot = dev_slot(dev);
  int is_in = (ep->addr & 0x80) ? 1 : 0;

  uproxy_req_t r;
  memset(&r, 0, sizeof(r));
  r.req.op = AUR_USB_OP_XFER;
  r.req.slot = slot;
  r.req.ep_addr = ep->addr;
  r.req.ep_type = ep->type;
  r.req.max_packet = ep->max_packet;
  r.req.is_in = (uint8_t)is_in;
  r.req.len = (uint32_t)len;
  r.req.timeout_ms = timeout_ms > 0 ? (uint32_t)timeout_ms : 0;
  if (is_in)
    r.data_in = buf;
  else
    r.data_out = buf;
  int rc = uproxy_call(&r);
  if (rc != 0)
    return rc;
  *actual = r.actual > (uint32_t)len ? len : (int)r.actual;
  return 0;
}

static int uproxy_hcd_interrupt_xfer(usb_device_t *dev, usb_endpoint_t *ep,
                                     void *buf, int len, int timeout_ms,
                                     int *out_actual_len) {
  if (!dev_slot(dev) || !ep || !buf || len <= 0)
    return -EINVAL;
  int total = 0;
  while (total < len) {
    int n = len - total;
    if (n > (int)AUR_USB_MAX_XFER)
      n = (int)AUR_USB_MAX_XFER;
    int act = 0;
    int rc = uproxy_xfer_chunk(dev, ep, (uint8_t *)buf + total, n, timeout_ms,
                               &act);
    if (rc != 0)
      return rc;
    total += act;
    if (act < n)
      break; // paquete corto: fin de la transferencia
  }
  if (out_actual_len)
    *out_actual_len = total;
  return 0;
}

static int uproxy_hcd_bulk_xfer(usb_device_t *dev, usb_endpoint_t *ep,
                                void *buf, int len, int is_in,
                                int timeout_ms) {
  (void)is_in;
  int actual = 0;
  int rc = uproxy_hcd_interrupt_xfer(dev, ep, buf, len, timeout_ms, &actual);
  return rc == 0 ? actual : rc;
}

// bulk_submit / bulk_xfer_phys (UAS con streams) quedan a NULL: usb_uas.c
// exige ambos y simplemente no se activa con el xHCI de usuario.
usb_hcd_t g_uproxy_hcd = {
    .name = "xhci-user",
    .control_xfer = uproxy_hcd_control_xfer,
    .bulk_xfer = uproxy_hcd_bulk_xfer,
    .interrupt_xfer = uproxy_hcd_interrupt_xfer,
    .configure_ep = uproxy_hcd_configure_ep,
    .new_device = uproxy_hcd_new_device,
    .set_hub_info = uproxy_hcd_set_hub_info,
    .alloc_device = uproxy_hcd_alloc_device,
    .free_device = uproxy_hcd_free_device,
};

// ---------------------------------------------------------------------------
// Hotplug (política en el kernel; mecánica en el .aur)
// ---------------------------------------------------------------------------
static int attached_mark(uint32_t port) { // 1 si ya estaba marcado
  uint32_t bit = 1u << ((port - 1u) & 31u);
  uint32_t old =
      __atomic_fetch_or(&g_attached[(port - 1u) >> 5], bit, __ATOMIC_ACQ_REL);
  return (old & bit) != 0;
}

static void attached_clear(uint32_t port) {
  uint32_t bit = 1u << ((port - 1u) & 31u);
  __atomic_fetch_and(&g_attached[(port - 1u) >> 5], ~bit, __ATOMIC_ACQ_REL);
}

static int uproxy_port_op(uint32_t op, uint32_t port, int flag, uint32_t *out0,
                          uint32_t *out1) {
  uproxy_req_t r;
  memset(&r, 0, sizeof(r));
  r.req.op = op;
  r.req.port = (uint8_t)port;
  r.req.is_in = flag ? 1 : 0; // PORT_RESET: 1 = forzar reset (reintento)
  int rc = uproxy_call(&r);
  if (rc == 0) {
    if (out0)
      *out0 = r.out0;
    if (out1)
      *out1 = r.out1;
  }
  return rc;
}

static int uproxy_attach_one(uint32_t port, int force_reset) {
  uint32_t speed = 0;
  int rc = uproxy_port_op(AUR_USB_OP_PORT_RESET, port, force_reset, NULL, &speed);
  if (rc != 0)
    return rc;

  usb_device_t *dev = NULL;
  rc = g_uproxy_hcd.new_device(NULL, (uint8_t)port, (usb_speed_t)speed, &dev);
  if (rc != 0)
    return rc;
  rc = usb_enumerate(dev);
  if (rc != 0) {
    usb_device_free(dev); // free_device deshabilita el slot
    return rc;
  }
  return 0;
}

static bool never(void *arg) {
  (void)arg;
  return false;
}

static void uproxy_attach_all(void) {
  if (__atomic_exchange_n(&g_attach_busy, 1, __ATOMIC_ACQ_REL))
    return; // otro escaneo en curso

  int attached = 0;
  for (uint32_t p = 1; p <= g_max_ports && p <= 255; p++) {
    uint32_t connected = 0;
    if (uproxy_port_op(AUR_USB_OP_PORT_STATUS, p, 0, &connected, NULL) != 0)
      break; // driver caído
    if (!connected) {
      attached_clear(p);
      continue;
    }
    if (attached_mark(p))
      continue;

    // Reintentos en línea; si siguen fallando el puerto queda marcado hasta
    // que se desconecte (evita bucles de hotplug).
    int ok = 0, gone = 0;
    for (int attempt = 1; attempt <= 3; attempt++) {
      int rc = uproxy_attach_one(p, attempt > 1);
      if (rc == 0) {
        ok = 1;
        break;
      }
      if (rc == -ENODEV) {
        gone = 1;
        break;
      }
      LOG_WARN("[UPROXY] port %u: attach falló (%d), intento %d/3", p, rc,
               attempt);
      wait_event_timeout(&g_hub_wq, never, NULL, 100);
    }
    if (ok)
      attached++;
    else if (gone)
      attached_clear(p);
    else
      LOG_ERR("[UPROXY] port %u: se rinde hasta reconectar el dispositivo", p);
  }
  if (attached > 0)
    LOG_INFO("[UPROXY] %d device(s) attachados", attached);
  __atomic_store_n(&g_attach_busy, 0, __ATOMIC_RELEASE);
}

static bool has_hotplug(void *arg) {
  (void)arg;
  return __atomic_load_n(&g_hotplug_pending, __ATOMIC_ACQUIRE) != 0;
}

static int uproxy_hotplug_task(void *arg) {
  (void)arg;
  for (;;) {
    wait_event(&g_hub_wq, has_hotplug, NULL);
    (void)__atomic_exchange_n(&g_hotplug_pending, 0, __ATOMIC_ACQ_REL);
    if (g_open && g_ready) {
      LOG_INFO("[UPROXY] evento de puerto, re-escaneando root hub");
      uproxy_attach_all();
    }
  }
  return 0;
}

// ---------------------------------------------------------------------------
// ioctl
// ---------------------------------------------------------------------------
static int64_t uproxy_hello(uint64_t arg) {
  struct aur_usb_hello h;
  if (copy_from_user(&h, (void *)arg, sizeof(h)) < 0)
    return -EFAULT;
  if (h.max_ports == 0 || h.max_ports > 255)
    return -EINVAL;

  g_max_ports = h.max_ports;
  memset(g_attached, 0, sizeof(g_attached));

  unsigned long fl = spin_lock_irqsave(&g_lock);
  g_ready = 1;
  spin_unlock_irqrestore(&g_lock, fl);

  if (!g_thread_started) {
    if (!kthread_run(uproxy_hotplug_task, NULL, "usb-hotplug"))
      return -ENOMEM;
    g_thread_started = 1;
  }
  LOG_INFO("[UPROXY] controlador xHCI de usuario listo (%u puertos, %u slots)",
           h.max_ports, h.max_slots);
  return 0;
}

static bool uproxy_has_work(void *arg) {
  (void)arg;
  return g_pend_head != NULL;
}

static int64_t uproxy_fetch(uint64_t arg) {
  struct aur_usb_fetch f;
  if (copy_from_user(&f, (void *)arg, sizeof(f)) < 0)
    return -EFAULT;

  uproxy_req_t *r;
  for (;;) {
    unsigned long fl = spin_lock_irqsave(&g_lock);
    r = g_pend_head;
    if (r) {
      uint32_t need = 0;
      if ((r->req.op == AUR_USB_OP_CONTROL || r->req.op == AUR_USB_OP_XFER) &&
          !r->req.is_in && r->data_out)
        need = r->req.len;
      if (need > f.buf_size) {
        spin_unlock_irqrestore(&g_lock, fl);
        return -EMSGSIZE; // sin sacarla de la cola
      }
      pend_pop();
      r->deadline = req_deadline(&r->req, sched_get_ticks());
      inflight_add(r);
      spin_unlock_irqrestore(&g_lock, fl);
      break;
    }
    spin_unlock_irqrestore(&g_lock, fl);

    if (f.flags & AUR_USB_NOWAIT) {
      // Sin trabajo: éxito con req.op == 0 (no -EAGAIN: el trazador de
      // syscalls registraría cada error del bucle de sondeo del .aur).
      memset(&f.req, 0, sizeof(f.req));
      return copy_to_user((void *)arg, &f, sizeof(f)) < 0 ? -EFAULT : 0;
    }
    int rc = wait_event_interruptible(&g_wq, uproxy_has_work, NULL);
    if (rc < 0)
      return rc;
  }

  f.req = r->req;
  int err = 0;
  if (r->data_out && !r->req.is_in && r->req.len > 0 &&
      copy_to_user((void *)f.buf, r->data_out, r->req.len) < 0)
    err = -EFAULT;
  if (!err && copy_to_user((void *)arg, &f, sizeof(f)) < 0)
    err = -EFAULT;

  if (err) {
    // El .aur no verá esta petición: abortarla para no colgar al llamante.
    unsigned long fl = spin_lock_irqsave(&g_lock);
    uproxy_req_t *rm = inflight_remove(f.req.tag);
    spin_unlock_irqrestore(&g_lock, fl);
    if (rm)
      uproxy_finish(rm, -EIO);
    return err;
  }
  return 0;
}

static int64_t uproxy_complete(uint64_t arg) {
  struct aur_usb_done dn;
  if (copy_from_user(&dn, (void *)arg, sizeof(dn)) < 0)
    return -EFAULT;

  unsigned long fl = spin_lock_irqsave(&g_lock);
  uproxy_req_t *r = inflight_remove(dn.tag);
  spin_unlock_irqrestore(&g_lock, fl);
  if (!r)
    return -ENOENT;

  int status = dn.status;
  int64_t ret = 0;
  if (status == 0) {
    uint32_t n = dn.actual;
    if (n > r->req.len)
      n = r->req.len;
    if (r->data_in && n > 0) {
      if (dn.buf_size < n) {
        status = -EIO;
        ret = -EMSGSIZE;
      } else if (copy_from_user(r->data_in, (void *)dn.buf, n) < 0) {
        status = -EFAULT;
        ret = -EFAULT;
      }
    }
    r->actual = n;
    r->out0 = dn.out0;
    r->out1 = dn.out1;
  }
  uproxy_finish(r, status); // `r` ya no es válido
  return ret;
}

static int64_t uproxy_event(uint64_t arg) {
  if (arg != AUR_USB_EVT_PORT_CHANGE)
    return -EINVAL;
  __atomic_store_n(&g_hotplug_pending, 1, __ATOMIC_RELEASE);
  wake_up_all(&g_hub_wq);
  return 0;
}

static int64_t uproxy_ioctl(vfs_node_t *node, unsigned long req,
                            uint64_t arg) {
  (void)node;
  switch (req) {
  case UPROXY_HELLO:
    return uproxy_hello(arg);
  case UPROXY_FETCH:
    return uproxy_fetch(arg);
  case UPROXY_COMPLETE:
    return uproxy_complete(arg);
  case UPROXY_EVENT:
    return uproxy_event(arg);
  default:
    return -ENOTTY;
  }
}

// ---------------------------------------------------------------------------
// VFS: open / close
// ---------------------------------------------------------------------------
static int uproxy_open(vfs_node_t *node, int flags) {
  (void)flags;
  process_t *proc = process_current();
  if (!proc)
    return -EPERM;
  if (!(proc->aur.caps & AUR_CAP_USB_HCD)) {
    LOG_WARN("[UPROXY] pid=%u sin AUR_CAP_USB_HCD (caps=0x%x)", proc->pid,
             proc->aur.caps);
    return -EPERM;
  }
  unsigned long fl = spin_lock_irqsave(&g_lock);
  if (g_open) {
    spin_unlock_irqrestore(&g_lock, fl);
    return -EBUSY;
  }
  g_open = 1;
  g_owner = node;
  g_owner_pid = proc->pid;
  g_wd_killed = 0;
  int start_wd = !g_wd_running;
  g_wd_running = 1;
  spin_unlock_irqrestore(&g_lock, fl);
  if (start_wd && !kthread_run(uproxy_watchdog, NULL, "uproxy-wd")) {
    fl = spin_lock_irqsave(&g_lock);
    g_wd_running = 0;
    spin_unlock_irqrestore(&g_lock, fl);
    LOG_WARN("[UPROXY] sin watchdog de peticiones (kthread_run falló)");
  }
  LOG_INFO("[UPROXY] driver xHCI pid=%u conectado", proc->pid);
  return 0;
}

static int uproxy_close(vfs_node_t *node) {
  // El VFS llama a close() al destruir CUALQUIER nodo; solo el que hizo el
  // open con éxito desconecta (ver blkproxy).
  uproxy_req_t *failed = NULL;
  unsigned long fl = spin_lock_irqsave(&g_lock);
  if (!g_open || node != g_owner) {
    spin_unlock_irqrestore(&g_lock, fl);
    return 0;
  }
  g_open = 0;
  g_ready = 0;
  g_owner = NULL;
  for (uproxy_req_t *r = g_pend_head, *nx; r; r = nx) {
    nx = r->next;
    r->next = failed;
    failed = r;
  }
  g_pend_head = g_pend_tail = NULL;
  for (uproxy_req_t *r = g_inflight, *nx; r; r = nx) {
    nx = r->next;
    r->next = failed;
    failed = r;
  }
  g_inflight = NULL;
  spin_unlock_irqrestore(&g_lock, fl);

  int n = 0;
  for (uproxy_req_t *r = failed, *nx; r; r = nx) {
    nx = r->next; // leer antes de completar: el llamante puede liberar r
    uproxy_finish(r, -EIO);
    n++;
  }
  LOG_WARN("[UPROXY] driver xHCI desconectado (%d peticiones abortadas)", n);
  return 0;
}

static int uproxy_poll(vfs_node_t *node, short events) {
  (void)node;
  return ((events & 1) && g_pend_head) ? 1 : 0;
}

static wait_queue_t *uproxy_poll_wq(vfs_node_t *node) {
  (void)node;
  return &g_wq;
}

vfs_ops_t uproxy_dev_ops = {
    .open = uproxy_open,
    .close = uproxy_close,
    .ioctl = uproxy_ioctl,
    .poll = uproxy_poll,
    .poll_wq = uproxy_poll_wq,
};

void uproxy_init(void) {
  if (__atomic_exchange_n(&g_init_done, 1, __ATOMIC_ACQ_REL))
    return;
  wait_queue_init(&g_wq);
  wait_queue_init(&g_hub_wq);
  wait_queue_init(&g_wd_wq);
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------
// Los plazos dependen de la operación y el watchdog solo falla las vencidas.
static void test_uproxy_deadlines(void) {
  struct aur_usb_req q;
  memset(&q, 0, sizeof(q));
  q.op = AUR_USB_OP_XFER;
  q.timeout_ms = 0;
  TEST_ASSERT(req_deadline(&q, 1000) == UPROXY_NO_DEADLINE,
              "xfer sin timeout no caduca");
  q.timeout_ms = 2000;
  TEST_ASSERT(req_deadline(&q, 1000) == 1000 + 2000 + UPROXY_GRACE_MS,
              "xfer: timeout + gracia");
  q.op = AUR_USB_OP_CONTROL;
  TEST_ASSERT(req_deadline(&q, 0) == 2000 + UPROXY_GRACE_MS, "control");
  q.op = AUR_USB_OP_NEW_DEVICE;
  TEST_ASSERT(req_deadline(&q, 0) == UPROXY_DEFAULT_MS, "op sin timeout");
}

static void test_uproxy_reap_expired(void) {
  unsigned long fl = spin_lock_irqsave(&g_lock);
  int busy = g_open || g_pend_head || g_inflight;
  spin_unlock_irqrestore(&g_lock, fl);
  if (busy) {
    test_skip("usbproxy en uso");
    return;
  }

  uproxy_req_t old, young, fly, forever;
  memset(&old, 0, sizeof(old));
  young = fly = forever = old;
  uproxy_req_t *all[] = {&old, &young, &fly, &forever};
  for (int i = 0; i < 4; i++)
    completion_init(&all[i]->done);
  old.deadline = 1000;
  young.deadline = 5000;
  fly.deadline = 1000;
  fly.req.tag = 42;
  forever.deadline = UPROXY_NO_DEADLINE;

  fl = spin_lock_irqsave(&g_lock);
  pend_push(&old);
  pend_push(&young);
  pend_push(&forever);
  inflight_add(&fly);
  spin_unlock_irqrestore(&g_lock, fl);

  TEST_ASSERT_EQ(uproxy_reap_expired(1000), 0);
  TEST_ASSERT_EQ(uproxy_reap_expired(1001), 2); // old + fly
  TEST_ASSERT(completion_done(&old.done) && completion_done(&fly.done),
              "vencidas completadas");
  TEST_ASSERT(old.status == -ETIMEDOUT && fly.status == -ETIMEDOUT,
              "ETIMEDOUT");
  TEST_ASSERT(!completion_done(&young.done) && !completion_done(&forever.done),
              "las demás intactas");
  TEST_ASSERT(g_pend_head == &young && g_inflight == NULL, "colas");

  TEST_ASSERT_EQ(uproxy_reap_expired(UINT64_MAX - 1), 1); // young
  TEST_ASSERT(g_pend_head == &forever && g_pend_tail == &forever,
              "sin plazo sobrevive");

  // Limpieza: sacar `forever` (vive en la pila de este test).
  fl = spin_lock_irqsave(&g_lock);
  pend_pop();
  spin_unlock_irqrestore(&g_lock, fl);
}
REGISTER_TEST("usbproxy: plazos por operación", test_uproxy_deadlines);
REGISTER_TEST("usbproxy: watchdog expira peticiones", test_uproxy_reap_expired);
