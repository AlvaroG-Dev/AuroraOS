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

uint32_t xhci_port_status(uint8_t port) {
  if (!g_xhci.initialized || port < 1 || port > g_xhci.max_ports)
    return 0;
  return *(volatile uint32_t *)(g_xhci.op + XHCI_OP_PORTSC(port));
}

static void portsc_write(uint8_t port, uint32_t v) {
  *(volatile uint32_t *)(g_xhci.op + XHCI_OP_PORTSC(port)) = v;
}

// ===========================================================================
// Port reset
// ===========================================================================
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

  LOG_INFO("[xHCI] port %u: reset (PORTSC=0x%08x)", port, portsc);

  // Limpiar change bits + poner PP
  uint32_t v = portsc & ~XHCI_PORTSC_CHANGE_BITS;
  v |= XHCI_PORTSC_PP;
  portsc_write(port, v);

  // PR=1
  v = xhci_port_status(port) & ~XHCI_PORTSC_CHANGE_BITS;
  portsc_write(port, v | XHCI_PORTSC_PR);

  // Poll PRC (Port Reset Change)
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

  if (!ok) {
    LOG_ERR("[xHCI] port %u: timeout tras PR (PORTSC=0x%08x)", port,
            xhci_port_status(port));
    return -ETIMEDOUT;
  }

  // Limpiar PRC + otros change bits
  uint32_t s = xhci_port_status(port);
  portsc_write(port, (s & ~XHCI_PORTSC_CHANGE_BITS) | XHCI_PORTSC_PRC);

  // Verificar PED (Port Enabled)
  s = xhci_port_status(port);
  if (!(s & XHCI_PORTSC_PED)) {
    LOG_WARN("[xHCI] port %u: PED=0 tras reset (PORTSC=0x%08x)", port, s);
    return -EIO;
  }

  LOG_INFO("[xHCI] port %u: reset OK (PORTSC=0x%08x speed=%u)", port, s,
           (s >> 10) & 0xF);
  return 0;
}

// ===========================================================================
// Drain de eventos PSCh (Port Status Change)
//
// Se llama al arrancar para que el attach inicial vea los puertos
// recién conectados. Para detección en caliente, se puede llamar
// periódicamente desde un timer.
// ===========================================================================
void xhci_port_poll_changes(void) {
  if (!g_xhci.initialized)
    return;
  for (;;) {
    xhci_trb_t evt;
    unsigned long flags = spin_lock_irqsave(&g_xhci.lock);
    int rc = xhci_ring_dequeue(&g_xhci.evt, &evt);
    if (rc == 0) {
      uint64_t next = xhci_ring_trb_phys(&g_xhci.evt, g_xhci.evt.dequeue);
      *(volatile uint32_t *)(g_xhci.rt + XHCI_RT_IR0_ERDP) =
          (uint32_t)(next & 0xFFFFFFFF);
      *(volatile uint32_t *)(g_xhci.rt + XHCI_RT_IR0_ERDP + 4) =
          (uint32_t)(next >> 32);
    }
    spin_unlock_irqrestore(&g_xhci.lock, flags);
    if (rc != 0)
      break;

    uint8_t type = XHCI_TRB_GET_TYPE(evt);
    if (type == XHCI_TRB_PORT_STATUS_CHG) {
      uint8_t port = (uint8_t)(evt.param >> 24);
      if (port >= 1 && port <= g_xhci.max_ports) {
        uint32_t s = xhci_port_status(port);
        portsc_write(port, (s & ~XHCI_PORTSC_CHANGE_BITS) |
                               (s & XHCI_PORTSC_CHANGE_BITS));
      }
    }
  }
}

// ===========================================================================
// Attach (Fase 3)
// ===========================================================================
static int xhci_attach_one(uint8_t port) {
  uint32_t portsc = xhci_port_status(port);
  if (!(portsc & XHCI_PORTSC_CCS))
    return -ENODEV;

  usb_speed_t speed = xhci_speed_to_usb(portsc);
  LOG_INFO("[xHCI] attach: port=%u speed=%d", port, (int)speed);

  int rc = xhci_port_reset(port);
  if (rc != 0)
    return rc;

  portsc = xhci_port_status(port);
  speed = xhci_speed_to_usb(portsc);
  uint16_t mps = xhci_default_ep0_mps(speed);

  // ------------------------------------------------------------------
  // 1. Enable Slot
  // ------------------------------------------------------------------
  uint8_t slot = 0;
  rc = xhci_cmd_enable_slot(&slot);
  if (rc != 0) {
    LOG_ERR("[xHCI] port %u: enable_slot falló: %d", port, rc);
    return rc;
  }
  LOG_INFO("[xHCI] port %u: slot_id=%u", port, slot);

  // ------------------------------------------------------------------
  // 2. Allocate usb_device_t + buffers DMA
  // ------------------------------------------------------------------
  usb_device_t *dev = usb_device_alloc();
  if (!dev) {
    xhci_cmd_disable_slot(slot);
    return -ENOMEM;
  }
  dev->speed = speed;
  dev->max_packet_size_ep0 = (mps > 0xFFu) ? 0xFFu : (uint8_t)mps;
  dev->hcd = &g_xhci_hcd;
  dev->addr = slot;

  rc = xhci_hcd_alloc_device(dev);
  if (rc != 0) {
    LOG_ERR("[xHCI] port %u: alloc_device falló: %d", port, rc);
    usb_device_free(dev);
    xhci_cmd_disable_slot(slot);
    return rc;
  }
  xhci_dev_priv_t *p = (xhci_dev_priv_t *)dev->hc_priv;
  p->slot_id = slot;
  p->port = port;
  p->speed = speed;

  // ------------------------------------------------------------------
  // 3. CRÍTICO: registrar el Output Device Context en la DCBAA.
  //
  // QEMU valida DCBAA[slot] != 0 al procesar Address Device y devuelve
  // TRB Error (cc=5) si es cero. Linux hace lo mismo en xhci_alloc_dev
  // (xhci_write_64 en &dcbaa->dev_context_ptrs[slot_id]).
  // ------------------------------------------------------------------
  if (p->out_ctx.phys == 0 || (p->out_ctx.phys & 0x3F) != 0) {
    LOG_ERR("[xHCI] port %u: out_ctx.phys inválido (0x%llx)", port,
            (unsigned long long)p->out_ctx.phys);
    usb_device_free(dev);
    xhci_cmd_disable_slot(slot);
    return -EIO;
  }
  g_xhci.dcbaa[slot] = p->out_ctx.phys;
  __asm__ volatile("mfence" ::: "memory");

  LOG_INFO("[xHCI] port %u: DCBAA[%u] = 0x%llx", port, slot,
           (unsigned long long)g_xhci.dcbaa[slot]);

  // ------------------------------------------------------------------
  // 4. Rellenar el input context (ICC + slot + EP0)
  // ------------------------------------------------------------------
  xhci_input_ctx_t *icc = (xhci_input_ctx_t *)p->input_ctx.virt;
  memset(icc, 0, sizeof(*icc));

  icc->icc.add = XHCI_ICC_ADD_SLOT | XHCI_ICC_ADD_EP0;

  // Slot context. El USB Device Address (DW3) lo rellena el HC en Address
  // Device; en el input context debe ir a 0.
  icc->slot.dw0 = XHCI_SLOT_DW0_ROUTE(0) |
                  XHCI_SLOT_DW0_SPEED(usb_speed_to_xhci_slot_code(speed)) |
                  XHCI_SLOT_DW0_CTX_ENT(1);    // 1 endpoint context (EP0)
  icc->slot.dw1 = XHCI_SLOT_DW1_RH_PORT(port); // Root Hub Port Number [23:16]
  icc->slot.dw2 = 0;
  icc->slot.dw3 = 0;

  // EP0 context (índice 0 del array ep[] del input context).
  xhci_ep_ctx_t *ep0 = xhci_ictx_ep0(icc);
  ep0->dw0 = 0; // EP State = Disabled (lo pone el HC a Running)
  ep0->dw1 = XHCI_EP_DW1_CERR(3) | XHCI_EP_DW1_EPTYPE(XHCI_EPTYPE_CONTROL) |
             XHCI_EP_DW1_MAX_PKT(mps);
  uint64_t deq = xhci_ring_trb_phys(&p->ep0_ring, 0) | XHCI_EP_DEQ_DCS;
  ep0->dw2 = (uint32_t)(deq & 0xFFFFFFFF);
  ep0->dw3 = (uint32_t)(deq >> 32);
  ep0->reserved[0] = XHCI_EP_DW4_AVG_TRB_LEN(8); // DW4: avg TRB length

  usb_dma_sync(&p->input_ctx, USB_DMA_TO_DEVICE);

  // ------------------------------------------------------------------
  // 5. Address Device
  // ------------------------------------------------------------------
  rc = xhci_cmd_address_device(slot, port, speed, mps, p->input_ctx.phys);
  if (rc != 0) {
    LOG_ERR("[xHCI] port %u: address_device falló: %d", port, rc);
    g_xhci.dcbaa[slot] = 0;
    __asm__ volatile("mfence" ::: "memory");
    usb_device_free(dev);
    xhci_cmd_disable_slot(slot);
    return rc;
  }

  // ------------------------------------------------------------------
  // 6. Enumeración genérica
  // ------------------------------------------------------------------
  rc = usb_enumerate(dev);
  if (rc != 0) {
    LOG_ERR("[xHCI] port %u: enumerate falló: %d", port, rc);
    g_xhci.dcbaa[slot] = 0;
    __asm__ volatile("mfence" ::: "memory");
    usb_device_free(dev);
    xhci_cmd_disable_slot(slot);
    return rc;
  }

  LOG_INFO("[xHCI] port %u: device %04x:%04x listo (slot=%u)", port,
           dev->vendor_id, dev->product_id, slot);
  // Nota: no liberamos `dev` para que Fase 2 pueda usarlo.
  return 0;
}

int xhci_attach_all_devices(void) {
  if (!g_xhci.initialized)
    return -ENODEV;
  xhci_port_poll_changes();
  int attached = 0;
  for (uint8_t p = 1; p <= g_xhci.max_ports; p++) {
    if (g_attached_ports_mask & (1u << (p - 1))) {
      LOG_DEBUG("[xHCI] port %u: ya enumerado, skip", p);
      continue;
    }
    uint32_t s = xhci_port_status(p);
    if (!(s & XHCI_PORTSC_CCS))
      continue;
    if (xhci_attach_one(p) == 0) {
      g_attached_ports_mask |= (1u << (p - 1));
      attached++;
    }
  }
  LOG_INFO("[xHCI] %d device(s) attachados", attached);
  return attached;
}