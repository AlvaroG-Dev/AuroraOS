// kernel/usb/xhci/xhci_port.c
#include "../../heap.h"
#include "../../klog.h"
#include "../../string.h"
#include "../../uaccess.h"
#include "../usb_core.h"
#include "xhci.h"
#include "xhci_ring.h"

#define PORT_RESET_TIMEOUT_MS 500
// xHCI admite hasta 255 puertos raíz. Una máscara uint8_t perdía los
// puertos 9+; ocho palabras cubren todo el rango sin shifts >= 32.
static uint32_t g_attached_ports_mask[8] = {0};

static int xhci_port_mark_attached(uint16_t port) {
  if (port == 0 || port > 255)
    return 0;
  uint32_t bit = 1u << ((port - 1u) & 31u);
  uint32_t old = __atomic_fetch_or(&g_attached_ports_mask[(port - 1u) >> 5],
                                   bit, __ATOMIC_ACQ_REL);
  return (old & bit) != 0;
}

static void xhci_port_mark_detached(uint16_t port) {
  if (port == 0 || port > 255)
    return;
  uint32_t bit = 1u << ((port - 1u) & 31u);
  __atomic_fetch_and(&g_attached_ports_mask[(port - 1u) >> 5], ~bit,
                     __ATOMIC_ACQ_REL);
}

// Consulta sin modificar el bitmap; se usa también para verificar
// que los puertos conectados sobreviven a un segundo escaneo del root hub.
int xhci_port_is_attached(uint8_t port) {
  if (port == 0 || port > g_xhci.max_ports)
    return 0;
  uint32_t bit = 1u << ((port - 1u) & 31u);
  uint32_t word = __atomic_load_n(&g_attached_ports_mask[(port - 1u) >> 5],
                                  __ATOMIC_ACQUIRE);
  return (word & bit) != 0;
}
static volatile int g_attach_busy = 0;

uint32_t xhci_port_status(uint8_t port) {
  if (!g_xhci.initialized || port < 1 || port > g_xhci.max_ports)
    return 0;
  return *(volatile uint32_t *)(g_xhci.op + XHCI_OP_PORTSC(port));
}

static void portsc_write(uint8_t port, uint32_t v) {
  *(volatile uint32_t *)(g_xhci.op + XHCI_OP_PORTSC(port)) = v;
}

static void xhci_delay_ms_local(uint32_t ms) {
  extern uint64_t klog_get_tsc_freq(void);
  uint64_t freq = klog_get_tsc_freq();
  if (freq == 0)
    freq = 2000000000ULL;
  uint64_t target = (freq / 1000) * ms;
  uint32_t lo, hi;
  __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
  uint64_t t0 = ((uint64_t)hi << 32) | lo;
  for (;;) {
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    if ((((uint64_t)hi << 32) | lo) - t0 >= target)
      break;
    __asm__ volatile("pause");
  }
}

int xhci_port_reset(uint8_t port) {
  if (!g_xhci.initialized || port < 1 || port > g_xhci.max_ports)
    return -EINVAL;
  uint32_t portsc = xhci_port_status(port);
  if (!(portsc & XHCI_PORTSC_CCS))
    return -ENODEV;

  /* [FIX HW REAL] Solo bits neutros: antes se reescribía PED=1 (desactiva el
   * puerto) y los change bits pendientes. */
  portsc_write(port, xhci_portsc_neutral(portsc) | XHCI_PORTSC_PP);
  // USB3 en Inactive/Compliance solo sale con Warm Reset (WPR, bit 31).
  uint32_t cur = xhci_port_status(port);
  uint32_t cur_pls = (cur & XHCI_PORTSC_PLS_MASK) >> XHCI_PORTSC_PLS_SHIFT;
  int ss_port = (((cur >> 10) & 0xF) >= 4);
  uint32_t rst_bit = (ss_port && (cur_pls == 6 || cur_pls == 10))
                         ? (1u << 31)
                         : XHCI_PORTSC_PR;
  portsc_write(port, xhci_portsc_neutral(cur) | rst_bit);

  uint32_t lo, hi;
  __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
  uint64_t t0 = ((uint64_t)hi << 32) | lo;
  extern uint64_t klog_get_tsc_freq(void);
  uint64_t freq = klog_get_tsc_freq();
  if (freq == 0)
    freq = 2000000000ULL;
  uint64_t max_cycles = (freq / 1000) * PORT_RESET_TIMEOUT_MS;

  int ok = 0;
  while (1) {
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    if ((((uint64_t)hi << 32) | lo) - t0 >= max_cycles)
      break;
    uint32_t s = xhci_port_status(port);
    // [FIX] Aceptar PRC (USB 2.0) o WRC (USB 3.0)
    if (s & (XHCI_PORTSC_PRC | XHCI_PORTSC_WRC)) {
      ok = 1;
      break;
    }
    xhci_delay_ms_local(1);
  }
  if (!ok)
    return -ETIMEDOUT;

  uint32_t s = xhci_port_status(port);
  // [FIX] Limpiar ambos bits de cambio
  /* [FIX HW REAL] Aquí s tenía PED=1: escribirlo desactivaba el puerto recién
   * reseteado y la función devolvía -EIO (puertos USB2 que "no aparecen"). */
  portsc_write(port,
               xhci_portsc_neutral(s) | XHCI_PORTSC_PRC | XHCI_PORTSC_WRC);
  s = xhci_port_status(port);
  if (!(s & XHCI_PORTSC_PED))
    return -EIO;
  return 0;
}

void xhci_port_poll_changes(void) {
  if (!g_xhci.initialized)
    return;
  for (uint16_t p = 1; p <= (uint16_t)g_xhci.max_ports; p++) {
    uint32_t s = xhci_port_status((uint8_t)p);
    // Si hay algún bit de cambio pendiente...
    if (s & XHCI_PORTSC_CHANGE_BITS) {
      // ACK de todos los change bits en UNA escritura (RW1C) y sin tocar
      // PED/PR: antes se escribía `s` entero, lo que deshabilitaba el puerto.
      portsc_write((uint8_t)p,
                   xhci_portsc_neutral(s) | (s & XHCI_PORTSC_CHANGE_BITS));
    }
  }
}

static int xhci_attach_one(uint8_t port) {
  uint32_t portsc = xhci_port_status(port);
  if (!(portsc & XHCI_PORTSC_CCS))
    return -ENODEV;

  usb_speed_t speed = xhci_speed_to_usb(portsc);
  LOG_INFO("[xHCI] attach: port=%u speed=%d PORTSC=0x%08x", port, (int)speed,
           portsc);

  uint32_t pls = (portsc & XHCI_PORTSC_PLS_MASK) >> XHCI_PORTSC_PLS_SHIFT;
  // Solo SuperSpeed se habilita solo por link training. Un puerto USB2 con
  // PED=1 puede tener un device que YA tiene dirección (BIOS o intento previo
  // fallido): sin reset, Address Device falla con cc=4 (Transaction Error).
  int is_ss = (speed == USB_SPEED_SUPER || speed == USB_SPEED_SUPER_PLUS);
  int already_enabled =
      is_ss && (portsc & XHCI_PORTSC_PED) && (pls == XHCI_PLS_U0);
  if (is_ss && !already_enabled) {
    // El link SuperSpeed entrena solo: dar hasta ~300 ms antes de resetear.
    for (int i = 0; i < 30 && !already_enabled; i++) {
      xhci_delay_ms_local(10);
      portsc = xhci_port_status(port);
      pls = (portsc & XHCI_PORTSC_PLS_MASK) >> XHCI_PORTSC_PLS_SHIFT;
      already_enabled = (portsc & XHCI_PORTSC_CCS) &&
                        (portsc & XHCI_PORTSC_PED) && (pls == XHCI_PLS_U0);
    }
    if (!(portsc & XHCI_PORTSC_CCS))
      return -ENODEV;
    if (already_enabled)
      speed = xhci_speed_to_usb(portsc);
  }

  if (!already_enabled) {
    int rc = xhci_port_reset(port);
    if (rc != 0)
      return rc;
    portsc = xhci_port_status(port);
    speed = xhci_speed_to_usb(portsc);
    xhci_delay_ms_local(20); // TRSTRCY: recuperación tras reset antes de hablar
  }

  usb_device_t *dev = NULL;
  int rc = g_xhci_hcd.new_device(NULL, port, speed, &dev);
  if (rc != 0)
    return rc;

  rc = usb_enumerate(dev);
  if (rc != 0) {
    usb_device_free(dev); // free_device hace Disable Slot + limpia el DCBAA
    return rc;
  }
  return 0;
}

int xhci_attach_all_devices(void) {
  if (!g_xhci.initialized)
    return -ENODEV;

  // Solo un scan a la vez. Si otro hilo/hotplug ya está escaneando,
  // salimos: el hub está siendo enumerado, no hace falta duplicar.
  if (__atomic_exchange_n(&g_attach_busy, 1, __ATOMIC_ACQ_REL))
    return 0;

  xhci_port_poll_changes();
  int attached = 0;

  for (uint16_t p = 1; p <= (uint16_t)g_xhci.max_ports; p++) {
    uint32_t s = xhci_port_status((uint8_t)p);
    if (!(s & XHCI_PORTSC_CCS)) {
      xhci_port_mark_detached(p);
      continue;
    }
    // Reservar antes de attachar. El escaneo se serializa mediante
    // g_attach_busy; el bitmap atómico evita perder los puertos 9+.
    if (xhci_port_mark_attached(p))
      continue;

    // Reintentos en línea. Si siguen fallando el puerto queda marcado hasta
    // que se desconecte: antes se hacía rollback y cada fallo generaba otro
    // evento de hotplug -> bucle infinito que gastaba un slot por intento.
    int ok = 0, gone = 0;
    for (int attempt = 1; attempt <= 3; attempt++) {
      int rc = xhci_attach_one((uint8_t)p);
      if (rc == 0) {
        ok = 1;
        break;
      }
      if (rc == -ENODEV) {
        gone = 1;
        break;
      }
      LOG_WARN("[xHCI] port %u: attach falló (%d), intento %d/3", p, rc,
               attempt);
      xhci_delay_ms_local(100);
    }
    if (ok)
      attached++;
    else if (gone)
      xhci_port_mark_detached(p);
    else
      LOG_ERR("[xHCI] port %u: se rinde hasta reconectar el dispositivo", p);
  }

  if (attached > 0)
    LOG_INFO("[xHCI] %d device(s) attachados", attached);

  __atomic_store_n(&g_attach_busy, 0, __ATOMIC_RELEASE);
  return attached;
}