// drivers/aur/xhci/xhci.c
//
// [FASE 5.4] xhci.aur — controlador USB xHCI en Ring 3.
//
// Puerto a userland de kernel/usb/xhci/*. El kernel conserva usb_core, hubs y
// los drivers de clase (HID, storage); este proceso implementa lo que hay por
// debajo de la interfaz usb_hcd_t:
//   - MMIO : mmap de /sys/bus/pci/devices/<BDF>/resource0
//   - DMA  : /dev/dma (anillos, contextos, scratchpad y buffers de datos)
//   - HCD  : /dev/usbproxy (ver kernel/usb/usb_proxy.c y el ABI en aur.h)
//
// Modelo: motor de peticiones ASÍNCRONO de un solo hilo. Las transferencias
// (control / bulk / interrupt) se encolan en su anillo, se toca el doorbell y
// se completan al llegar el evento; así un interrupt-IN de HID que tarda
// minutos no bloquea al resto de dispositivos. Las operaciones de gestión
// (NEW_DEVICE, CONFIGURE_EP, ...) se ejecutan síncronas, pero mientras esperan
// su comando siguen drenando el Event Ring.
//
// Con MSI (/dev/msi + /dev/irq/<vector>) el bucle principal duerme hasta el
// evento del controlador; sin capability MSI se hace polling del Event Ring
// (el controlador escribe los eventos igualmente).
//
// Restricciones de recursos de un .aur: 32 fds por proceso y cerrar un fd DMA
// libera sus páginas, por eso se piden pocas regiones grandes y nunca se
// cierran.
//
// Solo actúa si existe /usr/lib/aur/xhci.user (make XHCI_USER=1); sin él el
// driver de kernel xhci.c sigue siendo el dueño del controlador.

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

// ============================================================================
// ABI con el kernel (copia de kernel/dma_user.h y kernel/aur.h)
// ============================================================================
#define DMA_ALLOC 0x100
struct dma_alloc_req {
  uint64_t size, phys;
  uint32_t flags, _pad;
};

#define AUR_USB_OP_NEW_DEVICE 1
#define AUR_USB_OP_FREE_DEVICE 2
#define AUR_USB_OP_CONFIGURE_EP 3
#define AUR_USB_OP_SET_HUB 4
#define AUR_USB_OP_CONTROL 5
#define AUR_USB_OP_XFER 6
#define AUR_USB_OP_PORT_STATUS 7
#define AUR_USB_OP_PORT_RESET 8
#define AUR_USB_EVT_PORT_CHANGE 1
#define AUR_USB_NOWAIT (1u << 0)
#define AUR_USB_MAX_XFER 65536u
#define UPROXY_HELLO 0x500
#define UPROXY_FETCH 0x501
#define UPROXY_COMPLETE 0x502
#define UPROXY_EVENT 0x503

struct aur_usb_hello {
  uint32_t max_ports;
  uint32_t max_slots;
};
struct aur_usb_req {
  uint64_t tag;
  uint32_t op;
  uint32_t slot;
  uint32_t timeout_ms;
  uint32_t len;
  uint32_t route;
  uint16_t max_packet;
  uint8_t is_in;
  uint8_t ep_addr;
  uint8_t ep_type;
  uint8_t interval;
  uint8_t speed;
  uint8_t port;
  uint8_t root_port;
  uint8_t parent_slot;
  uint8_t parent_speed;
  uint8_t num_ports;
  uint8_t setup[8];
  uint8_t _pad[16];
};
struct aur_usb_fetch {
  struct aur_usb_req req;
  uint64_t buf;
  uint64_t buf_size;
  uint32_t flags;
  uint32_t _pad;
};
struct aur_usb_done {
  uint64_t tag;
  int32_t status;
  uint32_t actual;
  uint32_t out0;
  uint32_t out1;
  uint64_t buf;
  uint64_t buf_size;
};
_Static_assert(sizeof(struct aur_usb_req) == 64, "ABI aur_usb_req");
_Static_assert(sizeof(struct aur_usb_fetch) == 88, "ABI aur_usb_fetch");
_Static_assert(sizeof(struct aur_usb_done) == 40, "ABI aur_usb_done");

// usb_speed_t / USB_EP_* de kernel/usb/usb_core.h
#define SPEED_LOW 1
#define SPEED_FULL 2
#define SPEED_HIGH 3
#define SPEED_SUPER 4
#define SPEED_SUPER_PLUS 5
#define EP_CONTROL 0
#define EP_BULK 2
#define EP_INTERRUPT 3

#define MARKER_PATH "/usr/lib/aur/xhci.user"
#define PCI_SYSFS "/sys/bus/pci/devices"

// ============================================================================
// Registros y TRBs xHCI (kernel/usb/xhci/xhci.h)
// ============================================================================
#define CAP_HCSPARAMS1 0x04
#define CAP_HCSPARAMS2 0x08
#define CAP_HCCPARAMS1 0x10
#define CAP_DBOFF 0x14
#define CAP_RTSOFF 0x18

#define OP_USBCMD 0x00
#define OP_USBSTS 0x04
#define OP_PAGESIZE 0x08
#define OP_CRCR 0x18
#define OP_DCBAAP 0x30
#define OP_CONFIG 0x38
#define OP_PORTSC(n) (0x400 + ((n) - 1) * 0x10)

#define USBCMD_RS (1u << 0)
#define USBCMD_HCRST (1u << 1)
#define USBCMD_INTE (1u << 2)
#define USBSTS_HCH (1u << 0)
#define USBSTS_HSE (1u << 2)
#define USBSTS_EINT (1u << 3)
#define USBSTS_PCD (1u << 4)
#define USBSTS_CNR (1u << 11)
#define USBSTS_HCE (1u << 12)
#define CRCR_RCS (1u << 0)

#define PORTSC_CCS (1u << 0)
#define PORTSC_PED (1u << 1)
#define PORTSC_PR (1u << 4)
#define PORTSC_PLS_SHIFT 5
#define PORTSC_PLS_MASK (0xFu << 5)
#define PORTSC_PP (1u << 9)
#define PORTSC_CSC (1u << 17)
#define PORTSC_PEC (1u << 18)
#define PORTSC_WRC (1u << 19)
#define PORTSC_OCC (1u << 20)
#define PORTSC_PRC (1u << 21)
#define PORTSC_PLC (1u << 22)
#define PORTSC_CEC (1u << 23)
#define PORTSC_CHANGE_BITS                                                     \
  (PORTSC_CSC | PORTSC_PEC | PORTSC_WRC | PORTSC_OCC | PORTSC_PRC |            \
   PORTSC_PLC | PORTSC_CEC)
#define PORTSC_OCA (1u << 3)
#define PORTSC_SPEED_MASK (0xFu << 10)
#define PORTSC_WAKE_BITS (7u << 25)
#define PORTSC_DR (1u << 30)
#define PORTSC_PRESERVE                                                        \
  (PORTSC_CCS | PORTSC_OCA | PORTSC_PLS_MASK | PORTSC_PP | PORTSC_SPEED_MASK | \
   PORTSC_WAKE_BITS | PORTSC_DR)
#define PORTSC_WPR (1u << 31)
#define PLS_U0 0

#define RT_IMAN 0x20
#define RT_ERSTSZ 0x28
#define RT_ERSTBA 0x30
#define RT_ERDP 0x38
#define IMAN_IP (1u << 0)
#define IMAN_IE (1u << 1)
#define ERDP_EHB (1u << 3)

typedef struct {
  uint64_t param;
  uint32_t status;
  uint32_t control;
} __attribute__((packed)) trb_t;

#define TRB_NORMAL 1
#define TRB_SETUP 2
#define TRB_DATA 3
#define TRB_STATUS 4
#define TRB_LINK 6
#define TRB_ENABLE_SLOT 9
#define TRB_DISABLE_SLOT 10
#define TRB_ADDRESS_DEVICE 11
#define TRB_CONFIGURE_EP 12
#define TRB_EVAL_CTX 13
#define TRB_RESET_EP 14
#define TRB_STOP_EP 15
#define TRB_SET_TR_DEQUEUE 16
#define TRB_TRANSFER_EVT 32
#define TRB_CMD_COMPLETION 33
#define TRB_PORT_STATUS_CHG 34

#define TRB_CYCLE (1u << 0)
#define TRB_TC (1u << 1)
#define TRB_ISP (1u << 2)
#define TRB_CH (1u << 4)
#define TRB_IOC (1u << 5)
#define TRB_IDT (1u << 6)
#define TRB_DIR_IN (1u << 16)
#define TRB_TYPE(x) (((x) & 0x3Fu) << 10)
#define TRB_GET_TYPE(t) (((t).control >> 10) & 0x3F)
#define TRT_NONE (0u << 16)
#define TRT_OUT (2u << 16)
#define TRT_IN (3u << 16)

#define CC_SUCCESS 1
#define CC_SHORT_PACKET 13
#define CC_CTX_STATE_ERR 19
#define EVT_CC(t) (((t).status >> 24) & 0xFFu)
#define EVT_SLOT(t) (((t).control >> 24) & 0xFFu)
#define EVT_EP(t) (((t).control >> 16) & 0x1Fu)

#define EP_STATE_RUNNING 1
#define EP_STATE_HALTED 2
#define EP_STATE_ERROR 4

#define ICC_ADD_SLOT (1u << 0)
#define ICC_ADD_EP0 (1u << 1)

#define SLOT_DW0_ROUTE(s) ((s) & 0xFFFFFu)
#define SLOT_DW0_SPEED(s) (((s) & 0xFu) << 20)
#define SLOT_DW0_HUB (1u << 26)
#define SLOT_DW0_CTX_ENT(n) (((n) & 0x1Fu) << 27)
#define SLOT_DW1_RH_PORT(p) (((uint32_t)(p) & 0xFFu) << 16)
#define SLOT_DW1_NUM_PORTS(p) (((uint32_t)(p) & 0xFFu) << 24)
#define SLOT_DW2_TT_HUB_SLOT(s) ((s) & 0xFFu)
#define SLOT_DW2_TT_PORT(p) (((p) & 0xFFu) << 8)
#define SLOT_DW2_TTT(t) (((t) & 0x3u) << 16)

#define EP_DW1_CERR(c) (((c) & 0x3u) << 1)
#define EP_DW1_EPTYPE(t) (((t) & 0x7u) << 3)
#define EPTYPE_BULK_OUT 2
#define EPTYPE_INT_OUT 3
#define EPTYPE_CONTROL 4
#define EPTYPE_BULK_IN 6
#define EPTYPE_INT_IN 7
#define EP_DW1_MAX_PKT(p) (((uint32_t)(p) & 0xFFFFu) << 16)
#define EP_DEQ_DCS (1u << 0)

typedef struct {
  uint32_t drop, add, reserved[6];
} __attribute__((packed)) icc_t;
typedef struct {
  uint32_t dw0, dw1, dw2, dw3, reserved[4];
} __attribute__((packed)) ctx_t; // slot / endpoint context (32 B visibles)
typedef struct {
  uint64_t base;
  uint32_t size, reserved;
} __attribute__((packed)) erst_entry_t;

// ============================================================================
// Dimensionado
// ============================================================================
#define RING_TRBS 64      // anillos de comando / EP: 63 útiles + Link
#define EVT_TRBS 256      // Event Ring
#define MAX_SLOTS 30      // slots soportados (ids 1..30)
#define SLOT_STRIDE 40960 // 10 páginas por slot (ver slot_layout)
#define BUF_SIZE 65536    // buffer de datos por petición (= AUR_USB_MAX_XFER)
#define POOL_BUFS 32
#define MAX_REQS 32
#define TD_MAX_TRBS 8
#define SHORT_GRACE_MS 50

// ============================================================================
// Estado global
// ============================================================================
struct ring {
  trb_t *trbs;
  uint64_t phys;
  uint32_t size, enq, deq;
  uint8_t cycle, producer;
};

struct epst {
  int configured;
  uint8_t type, dir_in;
  uint16_t mps;
  struct ring ring;
  int req;          // índice de la petición en vuelo o -1
  int need_recover; // pendiente de Stop/Reset + Set TR Dequeue
};

struct slot {
  int used;
  uint8_t speed, root_port;
  uint8_t *in_ctx, *out_ctx;
  uint64_t in_phys, out_phys;
  struct epst ep[32]; // índice = DCI (1 = EP0)
};

enum { RQ_FREE = 0, RQ_QUEUED, RQ_ACTIVE };

struct req {
  int state;
  int internal; // 1 = petición interna (no se completa por ioctl)
  int finished; // para las internas
  int result;   // idem
  uint32_t result_actual;
  struct aur_usb_req r;
  int bidx;
  uint32_t dci;
  uint64_t deadline; // 0 = sin límite
  uint64_t t_short;
  int short_seen;
  uint32_t actual;
  uint32_t residual;
  // control
  int has_data;
  uint64_t data_phys, status_phys;
  // bulk / interrupt
  uint64_t td_phys[TD_MAX_TRBS];
  uint32_t td_len[TD_MAX_TRBS];
  int td_n;
  uint32_t td_total;
};

static volatile uint8_t *mmio, *opb, *rtb, *dbb;
static size_t mmio_len;
static uint32_t hcs1, hcs2, hcc1;
static uint16_t hciversion;
static uint8_t max_slots, max_ports, ctx_size;
static int fd_dma = -1, fd_up = -1;
static int ep_irq = -1;  // epoll sobre el vector MSI; -1 = polling
static int ep_both = -1; // lo mismo + /dev/usbproxy (peticiones nuevas)

static struct ring cmd_ring, evt_ring;
static uint64_t *dcbaa;
static uint64_t dcbaa_phys;
static erst_entry_t *erst;
static uint64_t erst_phys;

static uint8_t *pool_virt;
static uint64_t pool_phys;
static int pool_used[POOL_BUFS];

static uint8_t *slots_virt;
static uint64_t slots_phys;
static struct slot slots[MAX_SLOTS + 1];

static struct req reqs[MAX_REQS];
static trb_t cmd_evts[16];
static int ncmd_evts;
static volatile int g_pcd; // hay que avisar al kernel de un cambio de puerto
static int in_recovery;

// ============================================================================
// Utilidades
// ============================================================================
static inline uint32_t rd32(volatile uint8_t *base, uint32_t off) {
  return *(volatile uint32_t *)(base + off);
}
static inline void wr32(volatile uint8_t *base, uint32_t off, uint32_t v) {
  *(volatile uint32_t *)(base + off) = v;
}

static uint64_t now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static int fls32(uint32_t x) {
  int r = 0;
  while (x) {
    r++;
    x >>= 1;
  }
  return r;
}

static uint32_t speed_to_slot_code(unsigned s) {
  switch (s) {
  case SPEED_FULL:
    return 1;
  case SPEED_LOW:
    return 2;
  case SPEED_HIGH:
    return 3;
  case SPEED_SUPER:
    return 4;
  case SPEED_SUPER_PLUS:
    return 5;
  default:
    return 0;
  }
}

static unsigned portsc_speed(uint32_t portsc) {
  switch ((portsc >> 10) & 0xF) {
  case 1:
    return SPEED_FULL;
  case 2:
    return SPEED_LOW;
  case 3:
    return SPEED_HIGH;
  case 4:
    return SPEED_SUPER;
  case 5:
    return SPEED_SUPER_PLUS;
  default:
    return 0;
  }
}

static uint16_t default_ep0_mps(unsigned s) {
  switch (s) {
  case SPEED_HIGH:
    return 64;
  case SPEED_SUPER:
  case SPEED_SUPER_PLUS:
    return 512;
  default:
    return 8;
  }
}

static void *dma_alloc(size_t size, uint64_t *phys) {
  struct dma_alloc_req req;
  memset(&req, 0, sizeof(req));
  req.size = size;
  int fd = ioctl(fd_dma, DMA_ALLOC, &req);
  if (fd < 0)
    return NULL;
  void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (p == MAP_FAILED)
    return NULL;
  memset(p, 0, size);
  *phys = req.phys;
  return p;
}

// Sin HCCPARAMS1.AC64 el controlador solo direcciona los 4 GiB bajos.
static int dma_low_ok(uint64_t phys, size_t len) {
  return (hcc1 & 1u) || phys + len <= 0x100000000ULL;
}

static inline uint32_t portsc_neutral(uint32_t v) { return v & PORTSC_PRESERVE; }
static inline uint32_t portsc_rd(unsigned port) {
  return rd32(opb, OP_PORTSC(port));
}
static inline void portsc_wr(unsigned port, uint32_t v) {
  wr32(opb, OP_PORTSC(port), v);
}

static void pump_all(void);
static void check_timeouts(void);

// Duerme ms milisegundos sin dejar de atender el Event Ring.
static void delay_ms(uint32_t ms) {
  uint64_t t0 = now_ms();
  while (now_ms() - t0 < ms) {
    pump_all();
    usleep(500);
  }
}

// ============================================================================
// Anillos
// ============================================================================
static void ring_setup(struct ring *r, void *virt, uint64_t phys, uint32_t n,
                       int producer) {
  memset(virt, 0, (size_t)n * sizeof(trb_t));
  r->trbs = (trb_t *)virt;
  r->phys = phys;
  r->size = n;
  r->enq = r->deq = 0;
  r->cycle = 1;
  r->producer = (uint8_t)producer;
  if (producer) {
    trb_t *link = &r->trbs[n - 1];
    link->param = phys;
    link->control = TRB_TYPE(TRB_LINK) | TRB_TC | TRB_CYCLE;
  }
  __sync_synchronize();
}

// Vacía un anillo productor y lo deja consistente con DCS=1. Solo es seguro
// con el endpoint Stopped/Halted.
static void ring_reset(struct ring *r) {
  ring_setup(r, r->trbs, r->phys, r->size, 1);
}

static uint64_t ring_phys(const struct ring *r, uint32_t idx) {
  return r->phys + (uint64_t)idx * sizeof(trb_t);
}

static void ring_enqueue(struct ring *r, const trb_t *trb) {
  if (r->enq == r->size - 1) {
    // Wrap: el Link toma el cycle ACTUAL (antes del toggle) y copia CH del
    // TRB anterior; luego se vuelve a 0 y se invierte el cycle.
    trb_t *link = &r->trbs[r->size - 1];
    uint32_t chain = r->trbs[r->size - 2].control & TRB_CH;
    link->control = (link->control & ~(TRB_CYCLE | TRB_CH)) | r->cycle | chain;
    __sync_synchronize();
    r->enq = 0;
    r->cycle ^= 1;
  }
  trb_t *dst = &r->trbs[r->enq];
  *dst = *trb;
  dst->control = (dst->control & ~TRB_CYCLE) | r->cycle;
  __sync_synchronize();
  r->enq++;
}

// Event Ring (consumidor): 0 si hay evento, -1 si está vacío.
static int ring_dequeue(struct ring *r, trb_t *out) {
  trb_t *src = &r->trbs[r->deq];
  uint32_t ctl = *(volatile uint32_t *)&src->control;
  if ((ctl & TRB_CYCLE) != r->cycle)
    return -1;
  __sync_synchronize();
  *out = *src;
  r->deq++;
  if (r->deq == r->size) {
    r->deq = 0;
    r->cycle ^= 1;
  }
  return 0;
}

// ============================================================================
// Contexto de un slot (layout dentro de su región DMA)
//   +0     Input Context   (1 página)
//   +4096  Device Context  (1 página)
//   +8192  anillo de EP0   (64 TRB = 1 KiB)
//   +9216  anillos de EP   (DCI 2..31, 1 KiB cada uno)
// ============================================================================
static uint8_t *slot_base(unsigned slot) {
  return slots_virt + (size_t)(slot - 1) * SLOT_STRIDE;
}
static uint64_t slot_base_phys(unsigned slot) {
  return slots_phys + (uint64_t)(slot - 1) * SLOT_STRIDE;
}

static void slot_ring_init(struct slot *s, unsigned slot, unsigned dci) {
  size_t off = (dci == 1) ? 8192 : 9216 + (size_t)(dci - 2) * 1024;
  ring_setup(&s->ep[dci].ring, slot_base(slot) + off,
             slot_base_phys(slot) + off, RING_TRBS, 1);
}

static ctx_t *in_slot_ctx(struct slot *s) {
  return (ctx_t *)(s->in_ctx + ctx_size);
}
static ctx_t *in_ep_ctx(struct slot *s, unsigned dci) {
  return (ctx_t *)(s->in_ctx + (size_t)(dci + 1) * ctx_size);
}
static ctx_t *out_slot_ctx(struct slot *s) { return (ctx_t *)s->out_ctx; }
static ctx_t *out_ep_ctx(struct slot *s, unsigned dci) {
  return (ctx_t *)(s->out_ctx + (size_t)dci * ctx_size);
}

static void ring_doorbell(unsigned slot, unsigned target) {
  __sync_synchronize();
  wr32(dbb, slot * 4, target);
}

// ============================================================================
// Peticiones: completar / buscar
// ============================================================================
static void req_release(int qi);
static void ep_release(unsigned slot, unsigned dci, int qi);

static void req_finish(int qi, int status, uint32_t actual, uint32_t out0,
                       uint32_t out1) {
  struct req *q = &reqs[qi];
  if (q->internal) {
    q->result = status;
    q->result_actual = actual;
    q->finished = 1;
    return;
  }
  struct aur_usb_done dn;
  memset(&dn, 0, sizeof(dn));
  dn.tag = q->r.tag;
  dn.status = status;
  dn.actual = actual;
  dn.out0 = out0;
  dn.out1 = out1;
  if (q->bidx >= 0) {
    dn.buf = (uint64_t)(uintptr_t)(pool_virt + (size_t)q->bidx * BUF_SIZE);
    dn.buf_size = BUF_SIZE;
  }
  if (ioctl(fd_up, UPROXY_COMPLETE, &dn) != 0 && errno != ENOENT)
    perror("[xhci.aur] UPROXY_COMPLETE");
  req_release(qi);
}

static void req_release(int qi) {
  struct req *q = &reqs[qi];
  if (q->bidx >= 0)
    pool_used[q->bidx] = 0;
  q->state = RQ_FREE;
}

static uint8_t *req_buf(struct req *q) {
  return pool_virt + (size_t)q->bidx * BUF_SIZE;
}
static uint64_t req_buf_phys(struct req *q) {
  return pool_phys + (uint64_t)q->bidx * BUF_SIZE;
}

static void req_start(int qi);

// Libera el endpoint y, si no hay recuperación pendiente, arranca la
// siguiente petición encolada para él.
static void ep_release(unsigned slot, unsigned dci, int qi) {
  struct epst *e = &slots[slot].ep[dci];
  if (e->req == qi)
    e->req = -1;
  if (e->need_recover || e->req != -1)
    return;
  // Siguiente en cola para este endpoint (la de menor tag = más antigua).
  int best = -1;
  for (int i = 0; i < MAX_REQS; i++) {
    struct req *q = &reqs[i];
    if (q->state == RQ_QUEUED && q->r.slot == slot && q->dci == dci &&
        (best < 0 || q->r.tag < reqs[best].r.tag))
      best = i;
  }
  if (best >= 0)
    req_start(best);
}

// Falla una petición activa: libera el endpoint y marca la recuperación.
static void req_fail(int qi, int status, int recover) {
  struct req *q = &reqs[qi];
  unsigned slot = q->r.slot, dci = q->dci;
  if (recover)
    slots[slot].ep[dci].need_recover = 1;
  req_finish(qi, status, 0, 0, 0);
  ep_release(slot, dci, qi);
}

// ============================================================================
// Eventos
// ============================================================================
static void ctrl_event(int qi, const trb_t *e) {
  struct req *q = &reqs[qi];
  uint32_t cc = EVT_CC(*e), res = e->status & 0xFFFFFF;

  if (e->param == q->status_phys) {
    if (cc != CC_SUCCESS) {
      fprintf(stderr, "[xhci.aur] control: Status Stage cc=%u (slot %u)\n", cc,
              q->r.slot);
      req_fail(qi, -EIO, 1);
      return;
    }
    uint32_t transferred = 0;
    if (q->has_data)
      transferred = q->short_seen ? (q->r.len > q->residual
                                         ? q->r.len - q->residual
                                         : 0)
                                  : q->r.len;
    unsigned slot = q->r.slot, dci = q->dci;
    req_finish(qi, 0, transferred, 0, 0);
    ep_release(slot, dci, qi);
    return;
  }
  if (q->has_data && e->param == q->data_phys) {
    if (cc == CC_SHORT_PACKET) {
      q->short_seen = 1; // el Status Stage llegará después
      q->residual = res;
      return;
    }
    if (cc != CC_SUCCESS) {
      fprintf(stderr, "[xhci.aur] control: Data Stage cc=%u (slot %u)\n", cc,
              q->r.slot);
      req_fail(qi, -EIO, 1);
    }
    return;
  }
  // Evento de un TD anterior: ignorar.
}

static void xfer_done(int qi, uint32_t actual) {
  struct req *q = &reqs[qi];
  if (actual > q->r.len)
    actual = q->r.len;
  unsigned slot = q->r.slot, dci = q->dci;
  req_finish(qi, 0, actual, 0, 0);
  ep_release(slot, dci, qi);
}

static void xfer_event(int qi, const trb_t *e) {
  struct req *q = &reqs[qi];
  int idx = -1;
  for (int i = 0; i < q->td_n; i++)
    if (q->td_phys[i] == e->param) {
      idx = i;
      break;
    }
  if (idx < 0)
    return; // evento viejo de otro TD
  uint32_t cc = EVT_CC(*e), res = e->status & 0xFFFFFF;

  if (cc == CC_SUCCESS) {
    if (idx == q->td_n - 1)
      xfer_done(qi, q->short_seen ? q->actual : q->td_total);
    return;
  }
  if (cc == CC_SHORT_PACKET) {
    uint32_t before = 0;
    for (int i = 0; i < idx; i++)
      before += q->td_len[i];
    if (res > q->td_len[idx])
      res = q->td_len[idx];
    if (!q->short_seen)
      q->actual = before + q->td_len[idx] - res;
    if (idx == q->td_n - 1) {
      xfer_done(qi, q->actual);
      return;
    }
    q->short_seen = 1;
    q->t_short = now_ms();
    return;
  }
  fprintf(stderr, "[xhci.aur] transfer cc=%u (slot %u ep %u)\n", cc,
          q->r.slot, q->dci);
  req_fail(qi, -EIO, 1);
}

static void dispatch_event(const trb_t *e) {
  switch (TRB_GET_TYPE(*e)) {
  case TRB_CMD_COMPLETION:
    if (ncmd_evts >= (int)(sizeof(cmd_evts) / sizeof(cmd_evts[0]))) {
      // Eventos huérfanos (comandos con timeout): descartar el más viejo.
      memmove(&cmd_evts[0], &cmd_evts[1], sizeof(cmd_evts) - sizeof(trb_t));
      ncmd_evts--;
    }
    cmd_evts[ncmd_evts++] = *e;
    break;
  case TRB_TRANSFER_EVT: {
    unsigned slot = EVT_SLOT(*e), dci = EVT_EP(*e);
    if (slot < 1 || slot > MAX_SLOTS || dci < 1 || dci > 31)
      break;
    int qi = slots[slot].ep[dci].req;
    if (qi < 0 || reqs[qi].state != RQ_ACTIVE)
      break;
    if (reqs[qi].r.op == AUR_USB_OP_CONTROL)
      ctrl_event(qi, e);
    else
      xfer_event(qi, e);
    break;
  }
  case TRB_PORT_STATUS_CHG:
    g_pcd = 1;
    break;
  default:
    break;
  }
}

static int pump_events(void) {
  int n = 0;
  trb_t e;
  while (ring_dequeue(&evt_ring, &e) == 0) {
    n++;
    dispatch_event(&e);
  }
  if (n) {
    uint64_t next = ring_phys(&evt_ring, evt_ring.deq);
    wr32(rtb, RT_ERDP + 4, (uint32_t)(next >> 32));
    wr32(rtb, RT_ERDP, (uint32_t)(next & 0xFFFFFFF0u) | ERDP_EHB);
    uint32_t iman = rd32(rtb, RT_IMAN);
    if (iman & IMAN_IP)
      wr32(rtb, RT_IMAN, (iman & IMAN_IE) | IMAN_IP);
  }
  uint32_t sts = rd32(opb, OP_USBSTS);
  static int fatal_logged;
  if ((sts & (USBSTS_HSE | USBSTS_HCE)) && !fatal_logged) {
    fatal_logged = 1;
    fprintf(stderr, "[xhci.aur] error fatal del controlador: USBSTS=0x%08x\n",
            sts);
  }
  uint32_t rw1c = sts & (USBSTS_EINT | USBSTS_PCD | USBSTS_HSE | USBSTS_HCE);
  if (rw1c)
    wr32(opb, OP_USBSTS, rw1c);
  return n;
}

static void pump_all(void) {
  pump_events();
  check_timeouts();
}

// ============================================================================
// Comandos
// ============================================================================
static int cmd_wait(trb_t *trb, uint32_t timeout_ms, trb_t *out) {
  uint32_t idx = cmd_ring.enq;
  if (idx == cmd_ring.size - 1)
    idx = 0;
  uint64_t trb_phys = ring_phys(&cmd_ring, idx);
  ring_enqueue(&cmd_ring, trb);
  ring_doorbell(0, 0);

  uint64_t t0 = now_ms();
  for (;;) {
    pump_events();
    for (int i = 0; i < ncmd_evts; i++) {
      if (cmd_evts[i].param == trb_phys) {
        *out = cmd_evts[i];
        cmd_evts[i] = cmd_evts[--ncmd_evts];
        return 0;
      }
    }
    check_timeouts();
    if (now_ms() - t0 > timeout_ms) {
      fprintf(stderr, "[xhci.aur] cmd timeout (trb=0x%llx)\n",
              (unsigned long long)trb_phys);
      return -ETIMEDOUT;
    }
    usleep(100);
  }
}

static int cmd_simple(uint32_t control, uint64_t param, uint32_t timeout_ms,
                      trb_t *evt_out) {
  trb_t trb = {0};
  trb.param = param;
  trb.control = control;
  trb_t evt;
  int rc = cmd_wait(&trb, timeout_ms, &evt);
  if (rc != 0)
    return rc;
  if (evt_out)
    *evt_out = evt;
  return EVT_CC(evt) == CC_SUCCESS ? 0 : -EIO;
}

static int cmd_enable_slot(uint8_t *slot) {
  trb_t evt;
  int rc = cmd_simple(TRB_TYPE(TRB_ENABLE_SLOT) | TRB_IOC, 0, 5000, &evt);
  if (rc != 0)
    return rc;
  *slot = (uint8_t)EVT_SLOT(evt);
  return 0;
}

static int cmd_disable_slot(unsigned slot) {
  return cmd_simple(TRB_TYPE(TRB_DISABLE_SLOT) | (slot << 24) | TRB_IOC, 0,
                    5000, NULL);
}

static int cmd_ctx(uint32_t type, unsigned slot, uint64_t in_phys) {
  int rc = cmd_simple(TRB_TYPE(type) | (slot << 24) | TRB_IOC, in_phys, 5000,
                      NULL);
  if (rc == -EIO)
    fprintf(stderr, "[xhci.aur] comando %u (slot %u) falló\n", type, slot);
  return rc;
}

// Stop / Reset Endpoint toleran CTX_STATE_ERR (el EP ya estaba en otro estado).
static int cmd_ep_state(uint32_t type, unsigned slot, unsigned dci) {
  trb_t evt;
  trb_t trb = {0};
  trb.control = TRB_TYPE(type) | (slot << 24) | (dci << 16);
  int rc = cmd_wait(&trb, 2000, &evt);
  if (rc != 0)
    return rc;
  uint32_t cc = EVT_CC(evt);
  return (cc == CC_SUCCESS || cc == CC_CTX_STATE_ERR) ? 0 : -EIO;
}

static int cmd_set_tr_dequeue(unsigned slot, unsigned dci, uint64_t deq,
                              int dcs) {
  return cmd_simple(TRB_TYPE(TRB_SET_TR_DEQUEUE) | (slot << 24) | (dci << 16),
                    (deq & ~0xFULL) | (dcs ? 1u : 0u), 2000, NULL);
}

// ============================================================================
// Recuperación de endpoints (Stop/Reset + Set TR Dequeue)
// ============================================================================
static void recover_endpoint(unsigned slot, unsigned dci) {
  struct slot *s = &slots[slot];
  struct epst *e = &s->ep[dci];
  __sync_synchronize();
  uint32_t st = out_ep_ctx(s, dci)->dw0 & 7u;
  fprintf(stderr, "[xhci.aur] slot %u dci %u: recuperando endpoint (state=%u)\n",
          slot, dci, st);
  if (st == EP_STATE_RUNNING)
    cmd_ep_state(TRB_STOP_EP, slot, dci);
  else if (st == EP_STATE_HALTED || st == EP_STATE_ERROR)
    cmd_ep_state(TRB_RESET_EP, slot, dci);

  ring_reset(&e->ring);
  if (cmd_set_tr_dequeue(slot, dci, ring_phys(&e->ring, 0), 1) != 0)
    fprintf(stderr, "[xhci.aur] slot %u dci %u: set_tr_dequeue falló\n", slot,
            dci);
}

static void service_recoveries(void) {
  if (in_recovery)
    return;
  in_recovery = 1;
  for (unsigned sl = 1; sl <= MAX_SLOTS; sl++) {
    if (!slots[sl].used)
      continue;
    for (unsigned d = 1; d < 32; d++) {
      struct epst *e = &slots[sl].ep[d];
      if (!e->need_recover)
        continue;
      recover_endpoint(sl, d);
      e->need_recover = 0;
      ep_release(sl, d, -1); // arranca lo que hubiera encolado
    }
  }
  in_recovery = 0;
}

// ============================================================================
// Transferencias
// ============================================================================
// TD de Normal TRBs partido en límites de 64 KiB (un buffer de TRB no puede
// cruzarlos).
static int td_enqueue(struct epst *e, struct req *q, uint64_t phys,
                      uint32_t len) {
  uint16_t mps = e->mps ? e->mps : 8;
  uint32_t segs = (uint32_t)(((phys & 0xFFFFu) + len + 0xFFFFu) >> 16);
  if (segs > TD_MAX_TRBS || segs + 2 > e->ring.size)
    return -EINVAL;

  uint32_t total_pk = (len + mps - 1) / mps;
  uint32_t done = 0;
  int n = 0;
  while (done < len) {
    uint64_t addr = phys + done;
    uint32_t chunk = len - done;
    uint32_t to_boundary = 0x10000u - (uint32_t)(addr & 0xFFFFu);
    if (chunk > to_boundary)
      chunk = to_boundary;
    int last = (done + chunk == len);

    uint32_t td_size = 0;
    if (!last) {
      uint32_t rem = (hciversion < 0x100) ? (len - (done + chunk)) >> 10
                                          : total_pk - (done + chunk) / mps;
      td_size = rem > 31 ? 31 : rem;
    }
    trb_t trb = {0};
    trb.param = addr;
    trb.status = chunk | (td_size << 17);
    trb.control = TRB_TYPE(TRB_NORMAL) | TRB_ISP | (last ? TRB_IOC : TRB_CH);
    ring_enqueue(&e->ring, &trb);
    q->td_phys[n] = ring_phys(&e->ring, e->ring.enq - 1);
    q->td_len[n] = chunk;
    n++;
    done += chunk;
  }
  q->td_n = n;
  q->td_total = len;
  return 0;
}

static int control_enqueue(struct epst *e, struct req *q) {
  struct ring *r = &e->ring;
  int has_data = q->r.len > 0;
  int is_in = q->r.is_in;
  q->has_data = has_data;

  uint64_t setup_val;
  memcpy(&setup_val, q->r.setup, 8);
  trb_t setup = {0};
  setup.param = setup_val;
  setup.status = 8;
  setup.control = TRB_TYPE(TRB_SETUP) | TRB_IDT |
                  (!has_data ? TRT_NONE : (is_in ? TRT_IN : TRT_OUT));

  trb_t data = {0};
  if (has_data) {
    data.param = req_buf_phys(q);
    data.status = q->r.len;
    data.control = TRB_TYPE(TRB_DATA) | (is_in ? (TRB_DIR_IN | TRB_ISP) : 0);
  }
  int status_in = !has_data ? 1 : (is_in ? 0 : 1);
  trb_t status = {0};
  status.control = TRB_TYPE(TRB_STATUS) | TRB_IOC | (status_in ? TRB_DIR_IN : 0);

  // Direcciones físicas de los TRB que generarán eventos (misma regla de
  // salto del Link que ring_enqueue).
  uint32_t idx = r->enq;
  if (idx >= r->size - 1)
    idx = 0;
  uint32_t next = idx + 1;
  if (next >= r->size - 1)
    next = 0;
  if (has_data) {
    q->data_phys = ring_phys(r, next);
    next++;
    if (next >= r->size - 1)
      next = 0;
  }
  q->status_phys = ring_phys(r, next);

  ring_enqueue(r, &setup);
  if (has_data)
    ring_enqueue(r, &data);
  ring_enqueue(r, &status);
  return 0;
}

// Arranca (o encola) una petición CONTROL / XFER ya registrada.
static void req_start(int qi) {
  struct req *q = &reqs[qi];
  unsigned slot = q->r.slot, dci = q->dci;

  if (slot < 1 || slot > MAX_SLOTS || !slots[slot].used ||
      !slots[slot].ep[dci].configured) {
    req_finish(qi, -ENODEV, 0, 0, 0);
    return;
  }
  struct epst *e = &slots[slot].ep[dci];
  if (e->req != -1 || e->need_recover) {
    q->state = RQ_QUEUED;
    return;
  }

  int rc;
  q->short_seen = 0;
  q->actual = q->residual = 0;
  if (q->r.op == AUR_USB_OP_CONTROL)
    rc = control_enqueue(e, q);
  else
    rc = td_enqueue(e, q, req_buf_phys(q),
                    q->r.len); // len ya ajustado (múltiplo de MPS si IN int.)
  if (rc != 0) {
    req_finish(qi, rc, 0, 0, 0);
    return;
  }
  q->state = RQ_ACTIVE;
  e->req = qi;
  q->deadline = q->r.timeout_ms ? now_ms() + q->r.timeout_ms : 0;
  ring_doorbell(slot, dci);
}

static void check_timeouts(void) {
  uint64_t now = now_ms();
  for (int i = 0; i < MAX_REQS; i++) {
    struct req *q = &reqs[i];
    if (q->state != RQ_ACTIVE || q->finished)
      continue;
    if (q->r.op == AUR_USB_OP_XFER && q->short_seen &&
        now - q->t_short >= SHORT_GRACE_MS) {
      xfer_done(i, q->actual);
      continue;
    }
    if (q->deadline && now >= q->deadline) {
      if (q->r.op == AUR_USB_OP_XFER && q->short_seen) {
        xfer_done(i, q->actual);
        continue;
      }
      fprintf(stderr, "[xhci.aur] timeout (slot %u dci %u op %u)\n", q->r.slot,
              q->dci, q->r.op);
      req_fail(i, -ETIMEDOUT, 1);
    }
  }
}

// ============================================================================
// Control transfer síncrono interno (para NEW_DEVICE)
// ============================================================================
static int ctrl_sync(unsigned slot, const uint8_t setup[8], void *buf,
                     uint32_t len, int is_in, uint32_t *actual) {
  int qi = -1, bi = -1;
  for (int i = 0; i < MAX_REQS; i++)
    if (reqs[i].state == RQ_FREE) {
      qi = i;
      break;
    }
  for (int i = 0; i < POOL_BUFS; i++)
    if (!pool_used[i]) {
      bi = i;
      break;
    }
  if (qi < 0 || bi < 0)
    return -EBUSY;

  struct req *q = &reqs[qi];
  memset(q, 0, sizeof(*q));
  q->internal = 1;
  q->bidx = bi;
  pool_used[bi] = 1;
  q->r.op = AUR_USB_OP_CONTROL;
  q->r.slot = slot;
  q->r.len = len;
  q->r.is_in = (uint8_t)is_in;
  q->r.timeout_ms = 2000;
  q->r.tag = ~0ull; // las internas son las últimas en la cola
  memcpy(q->r.setup, setup, 8);
  q->dci = 1;
  q->state = RQ_QUEUED;
  req_start(qi);

  while (!q->finished) {
    pump_all();
    service_recoveries();
    if (q->state == RQ_QUEUED && slots[slot].ep[1].req == -1 &&
        !slots[slot].ep[1].need_recover)
      req_start(qi);
    usleep(100);
  }
  int rc = q->result;
  if (rc == 0) {
    if (is_in && buf && q->result_actual)
      memcpy(buf, req_buf(q), q->result_actual < len ? q->result_actual : len);
    if (actual)
      *actual = q->result_actual;
  }
  req_release(qi);
  return rc;
}

// ============================================================================
// Operaciones de gestión
// ============================================================================
static int op_new_device(const struct aur_usb_req *rq, uint32_t *out_slot,
                         uint32_t *out_mps) {
  uint8_t slot = 0;
  int rc = cmd_enable_slot(&slot);
  if (rc != 0)
    return rc;
  if (slot < 1 || slot > MAX_SLOTS) {
    cmd_disable_slot(slot);
    return -EIO;
  }

  struct slot *s = &slots[slot];
  memset(s, 0, sizeof(*s));
  s->used = 1;
  s->speed = rq->speed;
  s->root_port = rq->root_port;
  s->in_ctx = slot_base(slot);
  s->in_phys = slot_base_phys(slot);
  s->out_ctx = slot_base(slot) + 4096;
  s->out_phys = slot_base_phys(slot) + 4096;
  memset(s->in_ctx, 0, 4096);
  memset(s->out_ctx, 0, 4096);
  for (unsigned d = 1; d < 32; d++)
    s->ep[d].req = -1;
  slot_ring_init(s, slot, 1);
  s->ep[1].configured = 1;
  s->ep[1].type = EP_CONTROL;
  s->ep[1].mps = default_ep0_mps(rq->speed);

  dcbaa[slot] = s->out_phys;
  __sync_synchronize();

  uint16_t mps = default_ep0_mps(rq->speed);
  uint32_t dw2 = 0;
  if (rq->parent_slot != 0 &&
      (rq->parent_speed == SPEED_HIGH || rq->parent_speed == SPEED_SUPER ||
       rq->parent_speed == SPEED_SUPER_PLUS) &&
      (rq->speed == SPEED_FULL || rq->speed == SPEED_LOW)) {
    dw2 = SLOT_DW2_TT_HUB_SLOT(rq->parent_slot) | SLOT_DW2_TT_PORT(rq->port) |
          SLOT_DW2_TTT(1);
    printf("[xhci.aur] slot %u: TT (hub_slot=%u port=%u)\n", slot,
           rq->parent_slot, rq->port);
  }

  ((icc_t *)s->in_ctx)->add = ICC_ADD_SLOT | ICC_ADD_EP0;
  ctx_t *sc = in_slot_ctx(s);
  sc->dw0 = SLOT_DW0_ROUTE(rq->route) |
            SLOT_DW0_SPEED(speed_to_slot_code(rq->speed)) |
            SLOT_DW0_CTX_ENT(1);
  sc->dw1 = SLOT_DW1_RH_PORT(rq->root_port);
  sc->dw2 = dw2;
  sc->dw3 = 0;
  ctx_t *e0 = in_ep_ctx(s, 1);
  e0->dw0 = 0;
  e0->dw1 = EP_DW1_CERR(3) | EP_DW1_EPTYPE(EPTYPE_CONTROL) | EP_DW1_MAX_PKT(mps);
  uint64_t deq = ring_phys(&s->ep[1].ring, 0) | EP_DEQ_DCS;
  e0->dw2 = (uint32_t)deq;
  e0->dw3 = (uint32_t)(deq >> 32);
  e0->reserved[0] = 8; // Average TRB Length
  __sync_synchronize();

  rc = cmd_ctx(TRB_ADDRESS_DEVICE, slot, s->in_phys);
  if (rc != 0) {
    cmd_disable_slot(slot);
    dcbaa[slot] = 0;
    s->used = 0;
    return rc;
  }
  delay_ms(20); // SET_ADDRESS settle time

  // Full-speed: bMaxPacketSize0 no se conoce hasta leer el descriptor. Se leen
  // 8 bytes y se ajusta el MPS con Evaluate Context (si no, un GET_DESCRIPTOR
  // de 18 bytes con MPS real de 64 provoca Babble).
  if (rq->speed == SPEED_FULL) {
    uint8_t d8[8] = {0};
    const uint8_t setup[8] = {0x80, 0x06, 0x00, 0x01, 0x00, 0x00, 0x08, 0x00};
    uint32_t got = 0;
    int crc = ctrl_sync(slot, setup, d8, 8, 1, &got);
    if (crc == 0 && got >= 8 && d8[7] != 0 && d8[7] != mps) {
      memset(s->in_ctx, 0, 4096);
      ((icc_t *)s->in_ctx)->add = ICC_ADD_EP0;
      ctx_t *ie0 = in_ep_ctx(s, 1);
      memcpy(ie0, out_ep_ctx(s, 1), sizeof(*ie0));
      ie0->dw1 = (ie0->dw1 & 0xFFFFu) | EP_DW1_MAX_PKT(d8[7]);
      __sync_synchronize();
      if (cmd_ctx(TRB_EVAL_CTX, slot, s->in_phys) == 0) {
        printf("[xhci.aur] slot %u: EP0 MPS %u -> %u\n", slot, mps, d8[7]);
        mps = d8[7];
        s->ep[1].mps = mps;
      }
    }
  }

  *out_slot = slot;
  *out_mps = mps;
  return 0;
}

static void kill_slot_requests(unsigned slot) {
  for (int i = 0; i < MAX_REQS; i++) {
    struct req *q = &reqs[i];
    if ((q->state == RQ_ACTIVE || q->state == RQ_QUEUED) && q->r.slot == slot &&
        !q->internal) {
      unsigned dci = q->dci;
      int was_active = (q->state == RQ_ACTIVE);
      req_finish(i, -ENODEV, 0, 0, 0);
      if (was_active && slots[slot].ep[dci].req == i)
        slots[slot].ep[dci].req = -1;
    }
  }
}

static int op_free_device(unsigned slot) {
  if (slot < 1 || slot > MAX_SLOTS || !slots[slot].used)
    return 0;
  kill_slot_requests(slot);
  cmd_disable_slot(slot);
  dcbaa[slot] = 0;
  __sync_synchronize();
  slots[slot].used = 0;
  return 0;
}

static int op_configure_ep(const struct aur_usb_req *rq) {
  unsigned slot = rq->slot;
  if (slot < 1 || slot > MAX_SLOTS || !slots[slot].used)
    return -ENODEV;
  struct slot *s = &slots[slot];
  unsigned ep_num = rq->ep_addr & 0x0F;
  int dir_in = (rq->ep_addr & 0x80) ? 1 : 0;
  if (ep_num == 0 || ep_num > 15)
    return -EINVAL;
  unsigned dci = 2 * ep_num + (dir_in ? 1 : 0);
  struct epst *e = &s->ep[dci];
  if (e->configured)
    return 0;

  uint8_t eptype;
  switch (rq->ep_type) {
  case EP_INTERRUPT:
    eptype = dir_in ? EPTYPE_INT_IN : EPTYPE_INT_OUT;
    break;
  case EP_BULK:
    eptype = dir_in ? EPTYPE_BULK_IN : EPTYPE_BULK_OUT;
    break;
  default:
    return -EINVAL;
  }

  slot_ring_init(s, slot, dci);
  memset(s->in_ctx, 0, 4096);
  ((icc_t *)s->in_ctx)->add = ICC_ADD_SLOT | (1u << dci);
  // El Slot Context de entrada parte del estado actual del slot (route, hub,
  // TT...) y conserva los endpoints ya configurados (Context Entries).
  ctx_t *sc = in_slot_ctx(s);
  memcpy(sc, out_slot_ctx(s), sizeof(*sc));
  uint32_t cur_ent = (sc->dw0 >> 27) & 0x1Fu;
  if (cur_ent < dci)
    cur_ent = dci;
  sc->dw0 = (sc->dw0 & ~(0x1Fu << 27)) | SLOT_DW0_CTX_ENT(cur_ent);

  ctx_t *ec = in_ep_ctx(s, dci);
  ec->dw0 = 0;
  ec->dw1 = EP_DW1_CERR(3) | EP_DW1_EPTYPE(eptype) |
            EP_DW1_MAX_PKT(rq->max_packet);
  uint64_t deq = ring_phys(&e->ring, 0) | EP_DEQ_DCS;
  ec->dw2 = (uint32_t)deq;
  ec->dw3 = (uint32_t)(deq >> 32);

  // Endpoints periódicos: Interval y Max ESIT Payload (si no, el HC no
  // programa bien el teclado/ratón).
  uint32_t max_esit = 0;
  if (rq->ep_type == EP_INTERRUPT) {
    uint32_t interval_field;
    if (rq->speed == SPEED_LOW || rq->speed == SPEED_FULL) {
      int b = rq->interval;
      if (b < 1)
        b = 1;
      interval_field = (uint32_t)(fls32((uint32_t)b * 8u) - 1);
      if (interval_field < 3)
        interval_field = 3;
      if (interval_field > 10)
        interval_field = 10;
    } else {
      interval_field = rq->interval;
      if (interval_field < 1)
        interval_field = 1;
      if (interval_field > 16)
        interval_field = 16;
      interval_field -= 1; // período = 2^(bInterval-1) microframes
    }
    ec->dw0 |= (interval_field & 0xFFu) << 16;
    max_esit = rq->max_packet;
    ec->dw0 |= ((max_esit >> 16) & 0xFFu) << 24;
  }
  uint32_t avg = rq->max_packet > 0 ? rq->max_packet : 8;
  if (avg > 1024)
    avg = 1024;
  ec->reserved[0] = (avg & 0xFFFFu) | ((max_esit & 0xFFFFu) << 16);
  __sync_synchronize();

  int rc = cmd_ctx(TRB_CONFIGURE_EP, slot, s->in_phys);
  if (rc != 0)
    return rc;
  e->configured = 1;
  e->type = rq->ep_type;
  e->dir_in = (uint8_t)dir_in;
  e->mps = rq->max_packet;
  e->req = -1;
  return 0;
}

static int op_set_hub(const struct aur_usb_req *rq) {
  unsigned slot = rq->slot;
  if (slot < 1 || slot > MAX_SLOTS || !slots[slot].used)
    return -ENODEV;
  struct slot *s = &slots[slot];
  memset(s->in_ctx, 0, 4096);
  ((icc_t *)s->in_ctx)->add = ICC_ADD_SLOT;
  // Hub y Number of Ports se aplican con Configure Endpoint (Evaluate Context
  // solo evalúa Interrupter Target / Max Exit Latency).
  ctx_t *sc = in_slot_ctx(s);
  memcpy(sc, out_slot_ctx(s), sizeof(*sc));
  sc->dw0 |= SLOT_DW0_HUB;
  sc->dw1 = (sc->dw1 & ~(0xFFu << 24)) | SLOT_DW1_NUM_PORTS(rq->num_ports);
  __sync_synchronize();
  int rc = cmd_ctx(TRB_CONFIGURE_EP, slot, s->in_phys);
  if (rc == 0)
    printf("[xhci.aur] slot %u: marcado como hub (%u puertos)\n", slot,
           rq->num_ports);
  return rc;
}

// ============================================================================
// Puertos
// ============================================================================
#define PORT_RESET_TIMEOUT_MS 500

static int port_reset_hw(unsigned port) {
  uint32_t portsc = portsc_rd(port);
  if (!(portsc & PORTSC_CCS))
    return -ENODEV;
  // Solo bits neutros: reescribir PED=1 desactiva el puerto.
  portsc_wr(port, portsc_neutral(portsc) | PORTSC_PP);
  uint32_t cur = portsc_rd(port);
  uint32_t cur_pls = (cur & PORTSC_PLS_MASK) >> PORTSC_PLS_SHIFT;
  int ss_port = (((cur >> 10) & 0xF) >= 4);
  // USB3 en Inactive/Compliance solo sale con Warm Reset.
  uint32_t rst = (ss_port && (cur_pls == 6 || cur_pls == 10)) ? PORTSC_WPR
                                                              : PORTSC_PR;
  portsc_wr(port, portsc_neutral(cur) | rst);

  uint64_t t0 = now_ms();
  int ok = 0;
  while (now_ms() - t0 < PORT_RESET_TIMEOUT_MS) {
    if (portsc_rd(port) & (PORTSC_PRC | PORTSC_WRC)) {
      ok = 1;
      break;
    }
    delay_ms(1);
  }
  if (!ok)
    return -ETIMEDOUT;
  uint32_t s = portsc_rd(port);
  portsc_wr(port, portsc_neutral(s) | PORTSC_PRC | PORTSC_WRC);
  s = portsc_rd(port);
  return (s & PORTSC_PED) ? 0 : -EIO;
}

// Equivale a xhci_attach_one() del kernel salvo el alta del dispositivo:
// deja el puerto habilitado y devuelve la velocidad.
static int op_port_reset(unsigned port, int force, uint32_t *speed_out) {
  if (port < 1 || port > max_ports)
    return -EINVAL;
  uint32_t portsc = portsc_rd(port);
  if (!(portsc & PORTSC_CCS))
    return -ENODEV;

  unsigned speed = portsc_speed(portsc);
  uint32_t pls = (portsc & PORTSC_PLS_MASK) >> PORTSC_PLS_SHIFT;
  int is_ss = (speed == SPEED_SUPER || speed == SPEED_SUPER_PLUS);
  if (force)
    is_ss = 0; // reintento: resetear siempre (ver xhci_attach_one del kernel)
  int enabled = !force && is_ss && (portsc & PORTSC_PED) && pls == PLS_U0;
  printf("[xhci.aur] attach: port=%u speed=%u PORTSC=0x%08x\n", port, speed,
         portsc);

  if (is_ss && !enabled) {
    // El link SuperSpeed entrena solo: dar hasta ~300 ms antes de resetear.
    for (int i = 0; i < 30 && !enabled; i++) {
      delay_ms(10);
      portsc = portsc_rd(port);
      pls = (portsc & PORTSC_PLS_MASK) >> PORTSC_PLS_SHIFT;
      enabled = (portsc & PORTSC_CCS) && (portsc & PORTSC_PED) && pls == PLS_U0;
    }
    if (!(portsc & PORTSC_CCS))
      return -ENODEV;
    if (enabled)
      speed = portsc_speed(portsc);
  }
  // Reset siempre, aunque el link SS ya esté en U0 (como Linux y como
  // xhci_attach_one del kernel): sin él, VirtualBox da cc=4 en el primer
  // control transfer. `enabled` solo evita esperar el link training.
  {
    int rc = port_reset_hw(port);
    if (rc != 0)
      return rc;
    speed = portsc_speed(portsc_rd(port));
    delay_ms(20); // TRSTRCY
  }
  *speed_out = speed;
  return 0;
}

static void op_port_status(unsigned port, uint32_t *connected,
                           uint32_t *speed) {
  if (port < 1 || port > max_ports) {
    *connected = *speed = 0;
    return;
  }
  uint32_t s = portsc_rd(port);
  // ACK de los change bits (RW1C) sin tocar PED/PR.
  if (s & PORTSC_CHANGE_BITS) {
    portsc_wr(port, portsc_neutral(s) | (s & PORTSC_CHANGE_BITS));
    s = portsc_rd(port);
  }
  *connected = (s & PORTSC_CCS) ? 1 : 0;
  *speed = portsc_speed(s);
}

// ============================================================================
// Inicialización del controlador
// ============================================================================
static int find_controller(char *bdf, size_t len) {
  DIR *d = opendir(PCI_SYSFS);
  if (!d)
    return -1;
  struct dirent *e;
  int found = -1;
  while ((e = readdir(d)) != NULL) {
    if (e->d_name[0] == '.' || strlen(e->d_name) >= len)
      continue;
    char path[512], buf[32];
    snprintf(path, sizeof(path), PCI_SYSFS "/%s/class", e->d_name);
    int fd = open(path, O_RDONLY);
    if (fd < 0)
      continue;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
      continue;
    buf[n] = '\0';
    if (strtoul(buf, NULL, 16) == 0x0C0330UL) { // USB / xHCI
      strcpy(bdf, e->d_name);
      found = 0;
      break;
    }
  }
  closedir(d);
  return found;
}

static void wait_bus_master(const char *bdf) {
  char path[512];
  snprintf(path, sizeof(path), PCI_SYSFS "/%s/config", bdf);
  for (int i = 0; i < 300; i++) {
    int fd = open(path, O_RDONLY);
    if (fd >= 0) {
      uint8_t cfg[8];
      ssize_t n = read(fd, cfg, sizeof(cfg));
      close(fd);
      if (n == 8 && (cfg[4] & 0x06) == 0x06)
        return;
    }
    usleep(10000);
  }
  fprintf(stderr, "[xhci.aur] %s: bus mastering no habilitado tras 3 s\n", bdf);
}

static int map_mmio(const char *bdf) {
  char path[512];
  snprintf(path, sizeof(path), PCI_SYSFS "/%s/resource0", bdf);
  int fd = open(path, O_RDWR | O_SYNC);
  if (fd < 0) {
    perror("[xhci.aur] open resource0");
    return -1;
  }
  static const size_t sizes[] = {0x10000, 0x8000, 0x4000, 0x2000};
  for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
    void *m = mmap(NULL, sizes[i], PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (m != MAP_FAILED) {
      mmio = (volatile uint8_t *)m;
      mmio_len = sizes[i];
      return 0;
    }
  }
  perror("[xhci.aur] mmap BAR0");
  return -1;
}

// Handoff BIOS -> OS (USB Legacy Support, capability id 1).
static int legacy_handoff(void) {
  uint32_t xecp = (hcc1 >> 16) & 0xFFFFu;
  if (xecp == 0)
    return 0;
  uint32_t off = xecp * 4u;
  for (uint32_t n = 0; n < 64 && off + 8 <= mmio_len; n++) {
    uint32_t cap = rd32(mmio, off);
    uint8_t id = cap & 0xFF, next = (cap >> 8) & 0xFF;
    if (id == 1) {
      const uint32_t bios_owned = 1u << 16, os_owned = 1u << 24;
      wr32(mmio, off, cap | os_owned);
      __sync_synchronize();
      if (cap & bios_owned) {
        for (int i = 0; i < 1000; i++) {
          if (!(rd32(mmio, off) & bios_owned))
            break;
          usleep(1000);
        }
        if (rd32(mmio, off) & bios_owned) {
          fprintf(stderr, "[xhci.aur] timeout en el USB Legacy Handoff\n");
          return -ETIMEDOUT;
        }
      }
      // Apagar los SMI de la BIOS y limpiar sus flags RW1C.
      uint32_t ctl = rd32(mmio, off + 4);
      ctl &= ~((0x3u << 1) | (0xFFu << 5) | (0x7u << 17));
      ctl |= (0x7u << 29);
      wr32(mmio, off + 4, ctl);
      return 0;
    }
    if (next == 0)
      break;
    off += (uint32_t)next * 4u;
  }
  return 0;
}

static int hc_reset(void) {
  if (rd32(opb, OP_USBCMD) & USBCMD_RS) {
    wr32(opb, OP_USBCMD, rd32(opb, OP_USBCMD) & ~USBCMD_RS);
    int halted = 0;
    for (int i = 0; i < 100; i++) {
      if (rd32(opb, OP_USBSTS) & USBSTS_HCH) {
        halted = 1;
        break;
      }
      usleep(1000);
    }
    if (!halted)
      return -ETIMEDOUT;
  }
  wr32(opb, OP_USBCMD, rd32(opb, OP_USBCMD) | USBCMD_HCRST);
  for (int i = 0; i < 1000 && (rd32(opb, OP_USBCMD) & USBCMD_HCRST); i++)
    usleep(1000);
  if (rd32(opb, OP_USBCMD) & USBCMD_HCRST)
    return -ETIMEDOUT;
  for (int i = 0; i < 1000 && (rd32(opb, OP_USBSTS) & USBSTS_CNR); i++)
    usleep(1000);
  if (rd32(opb, OP_USBSTS) & USBSTS_CNR)
    return -ETIMEDOUT;
  wr32(opb, OP_CONFIG, max_slots);
  return 0;
}

static int setup_scratchpad(void *ctl_virt, uint64_t ctl_phys, size_t *off) {
  uint32_t n = (((hcs2 >> 21) & 0x1Fu) << 5) | ((hcs2 >> 27) & 0x1Fu);
  if (n == 0)
    return 0;
  uint32_t psreg = rd32(opb, OP_PAGESIZE);
  size_t pg = 4096;
  if (psreg) {
    unsigned shift = 12;
    while (!(psreg & 1u) && shift < 31) {
      psreg >>= 1;
      shift++;
    }
    pg = (size_t)1 << shift;
  }
  size_t bytes = (size_t)n * pg + pg;
  size_t alloc = (bytes + 4095) & ~(size_t)4095;
  uint64_t pphys;
  uint8_t *pages = dma_alloc(alloc, &pphys);
  if (!pages || !dma_low_ok(pphys, alloc)) {
    fprintf(stderr,
            "[xhci.aur] scratchpad: DMA falló o fuera de 4 GiB (%u x %zu B)\n",
            n, pg);
    return -ENOMEM;
  }
  size_t poff = (size_t)((pg - (pphys & (pg - 1))) & (pg - 1));

  // Array de punteros dentro de la región de control (alineado a 64).
  *off = (*off + 63) & ~(size_t)63;
  uint64_t *arr = (uint64_t *)((uint8_t *)ctl_virt + *off);
  uint64_t arr_phys = ctl_phys + *off;
  *off += (size_t)n * 8;
  for (uint32_t i = 0; i < n; i++)
    arr[i] = pphys + poff + (uint64_t)i * pg;
  dcbaa[0] = arr_phys;
  __sync_synchronize();
  printf("[xhci.aur] scratchpad: %u buffers x %zu B\n", n, pg);
  return 0;
}

static int init_controller(const char *bdf) {
  if (map_mmio(bdf) != 0)
    return -1;

  uint32_t cap0 = rd32(mmio, 0);
  uint8_t caplen = cap0 & 0xFF;
  hciversion = (uint16_t)(cap0 >> 16);
  hcs1 = rd32(mmio, CAP_HCSPARAMS1);
  hcs2 = rd32(mmio, CAP_HCSPARAMS2);
  hcc1 = rd32(mmio, CAP_HCCPARAMS1);
  uint32_t dboff = rd32(mmio, CAP_DBOFF) & ~0x3u;
  uint32_t rtsoff = rd32(mmio, CAP_RTSOFF) & ~0x1Fu;
  max_slots = hcs1 & 0xFF;
  max_ports = (hcs1 >> 24) & 0xFF;
  ctx_size = (hcc1 & (1u << 2)) ? 64 : 32;
  opb = mmio + caplen;
  dbb = mmio + dboff;
  rtb = mmio + rtsoff;
  if (max_slots == 0 || max_ports == 0 || rtsoff + 0x40 > mmio_len ||
      dboff + 4 > mmio_len) {
    fprintf(stderr,
            "[xhci.aur] capacidades inválidas o BAR demasiado corto: cap0=%08x "
            "hcs1=%08x dboff=%x rtsoff=%x mmio_len=%zx\n",
            cap0, hcs1, dboff, rtsoff, mmio_len);
    return -1;
  }
  if (max_slots > MAX_SLOTS)
    max_slots = MAX_SLOTS;
  printf("[xhci.aur] %s ver=%x.%02x slots=%u ports=%u ctx=%uB\n", bdf,
         hciversion >> 8, hciversion & 0xFF, max_slots, max_ports, ctx_size);

  if (legacy_handoff() != 0)
    return -1;
  if (hc_reset() != 0) {
    fprintf(stderr, "[xhci.aur] reset del controlador falló\n");
    return -1;
  }

  // Región de control: cmd ring, event ring, ERST, DCBAA, array scratchpad.
  size_t ctl_size = 65536;
  uint64_t ctl_phys;
  uint8_t *ctl = dma_alloc(ctl_size, &ctl_phys);
  if (!ctl) {
    fprintf(stderr, "[xhci.aur] sin memoria DMA\n");
    return -1;
  }
  size_t off = 0;
  ring_setup(&cmd_ring, ctl + off, ctl_phys + off, RING_TRBS, 1);
  off += 4096;
  ring_setup(&evt_ring, ctl + off, ctl_phys + off, EVT_TRBS, 0);
  off += 4096;
  erst = (erst_entry_t *)(ctl + off);
  erst_phys = ctl_phys + off;
  erst[0].base = evt_ring.phys;
  erst[0].size = EVT_TRBS;
  off += 4096;
  dcbaa = (uint64_t *)(ctl + off);
  dcbaa_phys = ctl_phys + off;
  off += 4096;
  if (!dma_low_ok(ctl_phys, ctl_size)) {
    fprintf(stderr, "[xhci.aur] sin AC64 y DMA por encima de 4 GiB\n");
    return -1;
  }
  if (setup_scratchpad(ctl, ctl_phys, &off) != 0)
    return -1;

  // Buffers de datos (alineados a 64 KiB para que ningún TRB cruce frontera).
  size_t pool_bytes = (size_t)(POOL_BUFS + 1) * BUF_SIZE;
  uint64_t pphys;
  uint8_t *praw = dma_alloc(pool_bytes, &pphys);
  if (!praw || !dma_low_ok(pphys, pool_bytes)) {
    fprintf(stderr, "[xhci.aur] sin memoria DMA (<4 GiB) para buffers\n");
    return -1;
  }
  size_t poff = (size_t)((BUF_SIZE - (pphys & (BUF_SIZE - 1))) & (BUF_SIZE - 1));
  pool_virt = praw + poff;
  pool_phys = pphys + poff;

  slots_virt = dma_alloc((size_t)MAX_SLOTS * SLOT_STRIDE, &slots_phys);
  if (!slots_virt ||
      !dma_low_ok(slots_phys, (size_t)MAX_SLOTS * SLOT_STRIDE)) {
    fprintf(stderr, "[xhci.aur] sin memoria DMA (<4 GiB) para slots\n");
    return -1;
  }

  wr32(opb, OP_DCBAAP, (uint32_t)dcbaa_phys);
  wr32(opb, OP_DCBAAP + 4, (uint32_t)(dcbaa_phys >> 32));
  wr32(opb, OP_CRCR, (uint32_t)cmd_ring.phys | CRCR_RCS);
  wr32(opb, OP_CRCR + 4, (uint32_t)(cmd_ring.phys >> 32));

  // Orden de la spec: ERSTSZ, ERDP y por último ERSTBA (activa el Event Ring).
  wr32(rtb, RT_ERSTSZ, 1);
  wr32(rtb, RT_ERDP + 4, (uint32_t)(evt_ring.phys >> 32));
  wr32(rtb, RT_ERDP, (uint32_t)(evt_ring.phys & 0xFFFFFFF0u) | ERDP_EHB);
  wr32(rtb, RT_ERSTBA, (uint32_t)erst_phys);
  wr32(rtb, RT_ERSTBA + 4, (uint32_t)(erst_phys >> 32));
  wr32(rtb, RT_IMAN, 0); // sin interrupciones: polling del Event Ring

  wr32(opb, OP_USBCMD, USBCMD_RS);
  for (int i = 0; i < 100 && !(rd32(opb, OP_USBCMD) & USBCMD_RS); i++)
    usleep(1000);
  if (!(rd32(opb, OP_USBCMD) & USBCMD_RS))
    return -1;

  // Alimentar los puertos si el HC lo requiere (PPC).
  if (hcc1 & (1u << 3)) {
    for (unsigned p = 1; p <= max_ports; p++) {
      uint32_t ps = portsc_rd(p);
      if (ps & PORTSC_PP)
        continue;
      portsc_wr(p, portsc_neutral(ps) | PORTSC_PP);
      for (int i = 0; i < 100 && !(portsc_rd(p) & PORTSC_PP); i++)
        usleep(1000);
    }
  }
  return 0;
}

// ============================================================================
// Bucle principal
// ============================================================================
static int find_free_req(void) {
  for (int i = 0; i < MAX_REQS; i++)
    if (reqs[i].state == RQ_FREE)
      return i;
  return -1;
}
static int find_free_buf(void) {
  for (int i = 0; i < POOL_BUFS; i++)
    if (!pool_used[i])
      return i;
  return -1;
}

static void handle_new(int qi) {
  struct req *q = &reqs[qi];
  const struct aur_usb_req *rq = &q->r;
  uint32_t out0 = 0, out1 = 0;
  int rc = 0;

  switch (rq->op) {
  case AUR_USB_OP_CONTROL:
  case AUR_USB_OP_XFER: {
    unsigned slot = rq->slot;
    if (rq->op == AUR_USB_OP_CONTROL) {
      q->dci = 1;
      if (rq->len > BUF_SIZE)
        rc = -EINVAL;
    } else {
      unsigned num = rq->ep_addr & 0x0F;
      q->dci = 2 * num + ((rq->ep_addr & 0x80) ? 1 : 0);
      if (num == 0 || rq->len == 0 || rq->len > BUF_SIZE)
        rc = -EINVAL;
      // Un interrupt IN debe pedir SIEMPRE un múltiplo de wMaxPacketSize: si
      // el dispositivo envía un informe más largo que el TRB, el xHC devuelve
      // Babble y HALTEA el endpoint.
      if (rc == 0 && rq->is_in && rq->ep_type == EP_INTERRUPT &&
          rq->max_packet > 0) {
        uint32_t mps = rq->max_packet, t = rq->len;
        if (t < mps)
          t = mps;
        t = ((t + mps - 1) / mps) * mps;
        if (t > BUF_SIZE)
          rc = -EINVAL;
        else
          q->r.len = t; // el kernel recibirá min(actual, len original)
      }
    }
    if (rc == 0 && (slot < 1 || slot > MAX_SLOTS))
      rc = -ENODEV;
    if (rc != 0) {
      req_finish(qi, rc, 0, 0, 0);
      return;
    }
    q->state = RQ_QUEUED;
    req_start(qi);
    return;
  }
  case AUR_USB_OP_NEW_DEVICE:
    rc = op_new_device(rq, &out0, &out1);
    break;
  case AUR_USB_OP_FREE_DEVICE:
    rc = op_free_device(rq->slot);
    break;
  case AUR_USB_OP_CONFIGURE_EP:
    rc = op_configure_ep(rq);
    break;
  case AUR_USB_OP_SET_HUB:
    rc = op_set_hub(rq);
    break;
  case AUR_USB_OP_PORT_STATUS:
    op_port_status(rq->port, &out0, &out1);
    break;
  case AUR_USB_OP_PORT_RESET:
    rc = op_port_reset(rq->port, rq->is_in, &out1);
    break;
  default:
    rc = -EINVAL;
  }
  req_finish(qi, rc, 0, out0, out1);
}

// ============================================================================
// MSI (/dev/msi + /dev/irq/<vector>)
// ============================================================================
#define AURMSI_ALLOC 0x700
struct aur_msi_alloc {
  uint8_t bus, slot, func, _pad;
  uint32_t vector;
};
_Static_assert(sizeof(struct aur_msi_alloc) == 8, "ABI aur_msi_alloc");

struct aur_irq_event {
  uint32_t irq;
  uint32_t count;
};

static int g_irq_fd = -1;

// "0000:00:03.0" -> bus/slot/func.
static int parse_bdf(const char *bdf, uint8_t *bus, uint8_t *slot,
                     uint8_t *func) {
  if (strlen(bdf) < 12 || bdf[4] != ':' || bdf[7] != ':' || bdf[10] != '.')
    return -1;
  *bus = (uint8_t)strtoul(bdf + 5, NULL, 16);
  *slot = (uint8_t)strtoul(bdf + 8, NULL, 16);
  *func = (uint8_t)(bdf[11] - '0');
  return 0;
}

// Pide MSI para el controlador y prepara ep_irq / ep_both. 0 = MSI activo;
// -1 = sin MSI (sin capability, sin permiso...): se sigue en polling.
static int msi_setup(const char *bdf) {
  struct aur_msi_alloc a;
  memset(&a, 0, sizeof(a));
  if (parse_bdf(bdf, &a.bus, &a.slot, &a.func) != 0)
    return -1;
  int fd_msi = open("/dev/msi", O_RDWR);
  if (fd_msi < 0 || ioctl(fd_msi, AURMSI_ALLOC, &a) != 0) {
    fprintf(stderr, "[xhci.aur] %s: sin MSI (%s), usando polling\n", bdf,
            strerror(errno));
    return -1;
  }
  // fd_msi se queda abierto: cerrarlo libera el vector.
  char path[32];
  snprintf(path, sizeof(path), "/dev/irq/%u", a.vector);
  g_irq_fd = open(path, O_RDONLY);
  ep_irq = epoll_create1(0);
  ep_both = epoll_create1(0);
  struct epoll_event ev = {.events = EPOLLIN, .data.fd = g_irq_fd};
  int ok = g_irq_fd >= 0 && ep_irq >= 0 && ep_both >= 0 &&
           epoll_ctl(ep_irq, EPOLL_CTL_ADD, g_irq_fd, &ev) == 0 &&
           epoll_ctl(ep_both, EPOLL_CTL_ADD, g_irq_fd, &ev) == 0;
  ev.data.fd = fd_up;
  ok = ok && epoll_ctl(ep_both, EPOLL_CTL_ADD, fd_up, &ev) == 0;
  if (!ok) {
    perror("[xhci.aur] /dev/irq");
    ep_irq = ep_both = -1;
    return -1;
  }
  printf("[xhci.aur] %s: MSI en vector 0x%02x\n", bdf, a.vector);
  return 0;
}

// Duerme hasta una IRQ (o petición nueva si `with_proxy`) o `ms`.
static void wait_event_ms(int with_proxy, int ms) {
  struct epoll_event ev[2];
  int n = epoll_wait(with_proxy ? ep_both : ep_irq, ev, 2, ms);
  for (int i = 0; i < n; i++) {
    if (ev[i].data.fd != g_irq_fd)
      continue;
    struct aur_irq_event e;
    if (read(g_irq_fd, &e, sizeof(e)) < 0 && errno != EINTR)
      perror("[xhci.aur] read irq");
  }
}

int main(int argc, char **argv) {
  if (access(MARKER_PATH, F_OK) != 0)
    return 0; // el driver de kernel es el dueño del xHCI

  // aur_bus lanza una instancia por controlador USB declinado; solo una
  // puede abrir /dev/usbproxy.
  fd_up = open("/dev/usbproxy", O_RDWR);
  if (fd_up < 0) {
    if (errno != EBUSY)
      perror("[xhci.aur] open /dev/usbproxy");
    return errno == EBUSY ? 0 : 1;
  }

  // aur_bus pasa el BDF del controlador en argv[1]; sin él se toma el primer
  // xHCI de sysfs.
  char bdf[64];
  if (argc > 1 && strlen(argv[1]) < sizeof(bdf)) {
    strcpy(bdf, argv[1]);
  } else if (find_controller(bdf, sizeof(bdf)) != 0) {
    // Controlador USB1/2 (UHCI/OHCI/EHCI) u otro: no es cosa nuestra.
    return 0;
  }
  printf("[xhci.aur] Iniciando driver xHCI en Ring 3 (%s)...\n", bdf);

  fd_dma = open("/dev/dma", O_RDWR);
  if (fd_dma < 0) {
    perror("[xhci.aur] open /dev/dma");
    return 1;
  }
  wait_bus_master(bdf);
  for (int i = 0; i < MAX_REQS; i++)
    reqs[i].bidx = -1;
  if (init_controller(bdf) != 0)
    return 1;

  // MSI: IMAN.IE + USBCMD.INTE. pump_events ya limpia IMAN.IP / USBSTS.EINT
  // y reescribe ERDP.EHB, que es lo que rearma la interrupción.
  if (msi_setup(bdf) == 0) {
    wr32(rtb, RT_IMAN, IMAN_IE | IMAN_IP);
    wr32(opb, OP_USBCMD, rd32(opb, OP_USBCMD) | USBCMD_INTE);
  }

  struct aur_usb_hello hello = {.max_ports = max_ports, .max_slots = max_slots};
  if (ioctl(fd_up, UPROXY_HELLO, &hello) != 0) {
    perror("[xhci.aur] UPROXY_HELLO");
    return 1;
  }
  printf("[xhci.aur] controlador listo; sirviendo peticiones\n");

  // Debounce de conexión (tPDDEBOUNCE ~100 ms) y link training USB3 antes de
  // pedir el primer escaneo del root hub.
  delay_ms(120);
  g_pcd = 1;

  for (;;) {
    int progress = 0;

    // 1. Peticiones nuevas del kernel (mientras haya petición y buffer libres).
    for (;;) {
      int qi = find_free_req(), bi = find_free_buf();
      if (qi < 0 || bi < 0)
        break;
      struct aur_usb_fetch f;
      memset(&f, 0, sizeof(f));
      f.buf = (uint64_t)(uintptr_t)(pool_virt + (size_t)bi * BUF_SIZE);
      f.buf_size = BUF_SIZE;
      f.flags = AUR_USB_NOWAIT;
      if (ioctl(fd_up, UPROXY_FETCH, &f) != 0) {
        if (errno == EINTR)
          break;
        perror("[xhci.aur] UPROXY_FETCH");
        return 1;
      }
      if (f.req.op == 0)
        break; // nada pendiente
      struct req *q = &reqs[qi];
      memset(q, 0, sizeof(*q));
      q->r = f.req;
      q->bidx = bi;
      pool_used[bi] = 1;
      q->state = RQ_QUEUED;
      progress = 1;
      handle_new(qi);
    }

    // 2. Eventos del controlador y timeouts.
    if (pump_events())
      progress = 1;
    check_timeouts();
    service_recoveries();

    // 3. Cambios de puerto: avisar al kernel para que reescanee.
    if (g_pcd) {
      g_pcd = 0;
      if (ioctl(fd_up, UPROXY_EVENT, AUR_USB_EVT_PORT_CHANGE) != 0)
        perror("[xhci.aur] UPROXY_EVENT");
    }

    // 4. Espera: sin trabajo, 1 ms; con bulk/control en vuelo, solo ceder la
    // CPU para bajar la latencia (los interrupt IN de HID no cuentan: están
    // siempre pendientes).
    if (!progress) {
      int busy = 0;
      for (int i = 0; i < MAX_REQS; i++)
        if (reqs[i].state == RQ_ACTIVE && reqs[i].r.ep_type != EP_INTERRUPT)
          busy = 1;
      if (ep_irq >= 0) {
        // Con MSI se duerme hasta el evento del controlador (o, si hay hueco
        // para más, hasta una petición nueva). El timeout cubre los plazos de
        // check_timeouts() y una IRQ perdida.
        wait_event_ms(find_free_req() >= 0 && find_free_buf() >= 0,
                      busy ? 2 : 10);
      } else if (busy) {
        sched_yield();
      } else {
        usleep(500);
      }
    }
  }
  return 0;
}
