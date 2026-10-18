// kernel/usb/xhci/xhci_port.c
#include "../../heap.h"
#include "../../klog.h"
#include "../../string.h"
#include "../../uaccess.h"
#include "../usb_core.h"
#include "xhci.h"
#include "xhci_ring.h"

#define PORT_RESET_TIMEOUT_MS 500
static uint8_t g_attached_ports_mask = 0;
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

  uint32_t v = portsc & ~XHCI_PORTSC_CHANGE_BITS;
  v |= XHCI_PORTSC_PP;
  portsc_write(port, v);

  v = xhci_port_status(port) & ~XHCI_PORTSC_CHANGE_BITS;
  portsc_write(port, v | XHCI_PORTSC_PR);

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
    if (s & XHCI_PORTSC_PRC) {
      ok = 1;
      break;
    }
    xhci_delay_ms_local(1);
  }
  if (!ok)
    return -ETIMEDOUT;

  uint32_t s = xhci_port_status(port);
  portsc_write(port, (s & ~XHCI_PORTSC_CHANGE_BITS) | XHCI_PORTSC_PRC);
  s = xhci_port_status(port);
  if (!(s & XHCI_PORTSC_PED))
    return -EIO;
  return 0;
}

void xhci_port_poll_changes(void) {
  if (!g_xhci.initialized)
    return;
  for (uint8_t p = 1; p <= g_xhci.max_ports; p++) {
    uint32_t s = xhci_port_status(p);
    if (s & XHCI_PORTSC_CHANGE_BITS) {
      portsc_write(p, (s & ~XHCI_PORTSC_CHANGE_BITS) |
                          (s & XHCI_PORTSC_CHANGE_BITS));
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
  int already_enabled = (portsc & XHCI_PORTSC_PED) && (pls == XHCI_PLS_U0);

  if (!already_enabled) {
    int rc = xhci_port_reset(port);
    if (rc != 0)
      return rc;
    portsc = xhci_port_status(port);
    speed = xhci_speed_to_usb(portsc);
  }

  usb_device_t *dev = NULL;
  int rc = g_xhci_hcd.new_device(NULL, port, speed, &dev);
  if (rc != 0)
    return rc;

  rc = usb_enumerate(dev);
  if (rc != 0) {
    xhci_cmd_disable_slot(dev->addr);
    g_xhci.dcbaa[dev->addr] = 0;
    usb_device_free(dev);
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

  for (uint8_t p = 1; p <= g_xhci.max_ports; p++) {
    uint32_t s = xhci_port_status(p);
    if (!(s & XHCI_PORTSC_CCS)) {
      g_attached_ports_mask &= ~(1u << (p - 1));
      continue;
    }
    // Reservar antes de attachar: si hotplug entra mientras tanto,
    // ve el bit ya puesto y no intenta duplicar.
    uint8_t old = __atomic_fetch_or(&g_attached_ports_mask, 1u << (p - 1),
                                    __ATOMIC_ACQ_REL);
    if (old & (1u << (p - 1)))
      continue;

    if (xhci_attach_one(p) == 0) {
      attached++;
    } else {
      // Rollback si falló
      __atomic_fetch_and(&g_attached_ports_mask, ~(1u << (p - 1)),
                         __ATOMIC_ACQ_REL);
    }
  }

  if (attached > 0)
    LOG_INFO("[xHCI] %d device(s) attachados", attached);

  g_attach_busy = 0;
  return attached;
}