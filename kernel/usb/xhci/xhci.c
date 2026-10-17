// kernel/usb/xhci/xhci.c
//
// Driver xHCI (USB 3.x). Fase 1: bring-up del controlador.
// Referencia: xHCI spec 1.2 (Intel).

#include "xhci.h"
#include "../../driver.h"
#include "../../heap.h"
#include "../../klog.h"
#include "../../paging.h"
#include "../../pci.h"
#include "../../string.h"
#include "../../uaccess.h" // EINVAL, ENOMEM, EIO, ENODEV, ETIMEDOUT
#include "../usb_dma.h"
#include "xhci_ring.h"

xhci_controller_t g_xhci;

// ===========================================================================
// Accesos MMIO
// ===========================================================================
static inline uint32_t mmio_rd(uint32_t off) {
  return *(volatile uint32_t *)(g_xhci.mmio + off);
}
static inline void mmio_wr(uint32_t off, uint32_t v) {
  *(volatile uint32_t *)(g_xhci.mmio + off) = v;
}
static inline uint32_t op_rd(uint32_t off) {
  return *(volatile uint32_t *)(g_xhci.op + off);
}
static inline void op_wr(uint32_t off, uint32_t v) {
  *(volatile uint32_t *)(g_xhci.op + off) = v;
}
static inline uint32_t rt_rd(uint32_t off) {
  return *(volatile uint32_t *)(g_xhci.rt + off);
}
static inline void rt_wr(uint32_t off, uint32_t v) {
  *(volatile uint32_t *)(g_xhci.rt + off) = v;
}

// ===========================================================================
// Delay (TSC) — mismo patrón que ahci.c
// ===========================================================================
static void xhci_delay_ms(uint32_t ms) {
  extern uint64_t klog_get_tsc_freq(void);
  uint64_t freq = klog_get_tsc_freq();
  if (freq == 0) {
    for (volatile uint64_t i = 0; i < (uint64_t)ms * 1000000ULL; i++)
      __asm__ volatile("pause");
    return;
  }
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

// ===========================================================================
// 1. Detección PCI
// ===========================================================================
static int xhci_find_controller(pci_device_t *out) {
  for (uint16_t bus = 0; bus < 256; bus++) {
    for (uint8_t slot = 0; slot < 32; slot++) {
      uint16_t vendor = pci_read_config_word((uint8_t)bus, slot, 0, 0);
      if (vendor == 0xFFFF)
        continue;

      uint8_t hdr = pci_read_config_byte((uint8_t)bus, slot, 0, 0x0E);
      int nfuncs = (hdr & 0x80) ? 8 : 1;

      for (uint8_t func = 0; func < nfuncs; func++) {
        pci_device_t d;
        if (pci_get_device((uint8_t)bus, slot, func, &d) != 0)
          continue;
        if (d.class_id != 0x0C || d.subclass_id != 0x03)
          continue;
        if (d.prog_if != 0x30) {
          LOG_DEBUG("[xHCI] %02x:%02x.%x prog_if=0x%02x (no xHCI)", d.bus,
                    d.slot, d.func, d.prog_if);
          continue;
        }
        *out = d;
        return 0;
      }
    }
  }
  return -ENODEV;
}

// ===========================================================================
// 2. Mapear MMIO (BAR0, 64-bit)
// ===========================================================================
static int xhci_map_mmio(void) {
  // Leer BAR0 crudo para preservar los 64 bits.
  uint32_t lo = pci_read_config_dword(g_xhci.pci.bus, g_xhci.pci.slot,
                                      g_xhci.pci.func, 0x10);
  if (lo & 0x01) {
    LOG_ERR("[xHCI] BAR0 es I/O, no MMIO");
    return -ENODEV;
  }
  uint64_t phys;
  if (((lo >> 1) & 0x03) == 0x02) {
    uint32_t hi = pci_read_config_dword(g_xhci.pci.bus, g_xhci.pci.slot,
                                        g_xhci.pci.func, 0x14);
    phys = (lo & ~0x0FULL) | ((uint64_t)hi << 32);
  } else {
    phys = lo & ~0x0FULL;
  }
  if (phys == 0) {
    LOG_ERR("[xHCI] BAR0 = 0");
    return -ENODEV;
  }

  g_xhci.mmio_phys = phys;
  g_xhci.mmio_size = 0x10000; // 64 KB cubre CAP + OP + DB + RT
  g_xhci.mmio = (volatile uint8_t *)mmio_map(
      phys, g_xhci.mmio_size, PTE_WRITABLE | PTE_NOCACHE | PTE_NX);
  if (!g_xhci.mmio) {
    LOG_ERR("[xHCI] mmio_map(0x%lx) falló", (unsigned long)phys);
    return -ENOMEM;
  }
  LOG_INFO("[xHCI] MMIO phys=0x%lx -> virt=%p", (unsigned long)phys,
           (void *)g_xhci.mmio);
  return 0;
}

// ===========================================================================
// 3. Leer capacidades
// ===========================================================================
static int xhci_read_caps(void) {
  uint32_t cap0 = mmio_rd(XHCI_CAP_CAPLENGTH);
  g_xhci.caplen = (uint8_t)(cap0 & 0xFF);
  g_xhci.hciversion = (uint16_t)(cap0 >> 16);

  g_xhci.hcsparams1 = mmio_rd(XHCI_CAP_HCSPARAMS1);
  g_xhci.hcsparams2 = mmio_rd(XHCI_CAP_HCSPARAMS2);
  g_xhci.hcsparams3 = mmio_rd(XHCI_CAP_HCSPARAMS3);
  g_xhci.hccparams1 = mmio_rd(XHCI_CAP_HCCPARAMS1);
  g_xhci.dboff = mmio_rd(XHCI_CAP_DBOFF) & ~0x3u;
  g_xhci.rtsoff = mmio_rd(XHCI_CAP_RTSOFF) & ~0x1Fu;

  g_xhci.max_slots = (uint8_t)XHCI_HCS1_MAX_SLOTS(g_xhci.hcsparams1);
  g_xhci.max_ports = (uint8_t)XHCI_HCS1_MAX_PORTS(g_xhci.hcsparams1);
  g_xhci.ctx_size = (g_xhci.hccparams1 & XHCI_HCC1_CSZ) ? 64 : 32;

  g_xhci.op = g_xhci.mmio + g_xhci.caplen;
  g_xhci.db = g_xhci.mmio + g_xhci.dboff;
  g_xhci.rt = g_xhci.mmio + g_xhci.rtsoff;

  if (g_xhci.max_slots == 0 || g_xhci.max_ports == 0) {
    LOG_ERR("[xHCI] hcsparams inválidos (slots=%u ports=%u)", g_xhci.max_slots,
            g_xhci.max_ports);
    return -EIO;
  }

  LOG_INFO("[xHCI] ver=%04x CAPLEN=0x%x slots=%u ports=%u ctx=%uB",
           g_xhci.hciversion, g_xhci.caplen, g_xhci.max_slots, g_xhci.max_ports,
           g_xhci.ctx_size);
  LOG_INFO("[xHCI] DBOFF=0x%x RTSOFF=0x%x HCCPARAMS1=0x%08x", g_xhci.dboff,
           g_xhci.rtsoff, g_xhci.hccparams1);
  return 0;
}

// ===========================================================================
// 4. HCRST
// ===========================================================================
static int xhci_hc_reset(void) {
  // Asegurar HCH=1 (halted)
  if (op_rd(XHCI_OP_USBCMD) & XHCI_USBCMD_RS) {
    op_wr(XHCI_OP_USBCMD, op_rd(XHCI_OP_USBCMD) & ~XHCI_USBCMD_RS);
    for (int i = 0; i < 100; i++) {
      if (op_rd(XHCI_OP_USBSTS) & XHCI_USBSTS_HCH)
        break;
      xhci_delay_ms(1);
    }
  }

  // HCRST
  op_wr(XHCI_OP_USBCMD, op_rd(XHCI_OP_USBCMD) | XHCI_USBCMD_HCRST);

  for (int i = 0; i < 1000; i++) {
    if (!(op_rd(XHCI_OP_USBCMD) & XHCI_USBCMD_HCRST))
      break;
    xhci_delay_ms(1);
  }
  if (op_rd(XHCI_OP_USBCMD) & XHCI_USBCMD_HCRST) {
    LOG_ERR("[xHCI] HCRST no se limpió");
    return -ETIMEDOUT;
  }

  for (int i = 0; i < 1000; i++) {
    if (!(op_rd(XHCI_OP_USBSTS) & XHCI_USBSTS_CNR))
      break;
    xhci_delay_ms(1);
  }
  if (op_rd(XHCI_OP_USBSTS) & XHCI_USBSTS_CNR) {
    LOG_ERR("[xHCI] CNR no se limpió tras HCRST");
    return -ETIMEDOUT;
  }

  // CONFIG.MaxSlotsEn
  op_wr(XHCI_OP_CONFIG, g_xhci.max_slots);

  LOG_DEBUG("[xHCI] HCRST completado; USBCMD=0x%08x USBSTS=0x%08x",
            op_rd(XHCI_OP_USBCMD), op_rd(XHCI_OP_USBSTS));
  return 0;
}

// ===========================================================================
// 5. Rings + DCBAA + ERST + arranque
// ===========================================================================
static int xhci_init_rings(void) {
  int rc;

  rc = xhci_ring_init(&g_xhci.cmd, 64, 1);
  if (rc != 0)
    return rc;

  rc = xhci_ring_init(&g_xhci.evt, 64, 0);
  if (rc != 0) {
    xhci_ring_destroy(&g_xhci.cmd);
    return rc;
  }

  // ERST (una entrada)
  usb_dma_buf_t erst_buf;
  rc = usb_dma_alloc(sizeof(xhci_erst_entry_t), &erst_buf);
  if (rc != 0) {
    xhci_ring_destroy(&g_xhci.evt);
    xhci_ring_destroy(&g_xhci.cmd);
    return rc;
  }
  g_xhci.erst = (xhci_erst_entry_t *)erst_buf.virt;
  g_xhci.erst_phys = erst_buf.phys;
  g_xhci.erst[0].base = g_xhci.evt.phys;
  g_xhci.erst[0].size = g_xhci.evt.size;
  g_xhci.erst[0].reserved = 0;

  // DCBAA
  size_t dcbaa_bytes = (size_t)(g_xhci.max_slots + 1) * 8;
  usb_dma_buf_t dcbaa_buf;
  rc = usb_dma_alloc(dcbaa_bytes, &dcbaa_buf);
  if (rc != 0) {
    usb_dma_free(&erst_buf);
    xhci_ring_destroy(&g_xhci.evt);
    xhci_ring_destroy(&g_xhci.cmd);
    return rc;
  }
  g_xhci.dcbaa = (uint64_t *)dcbaa_buf.virt;
  g_xhci.dcbaa_phys = dcbaa_buf.phys;

  // Programar registros operacionales
  op_wr(XHCI_OP_DCBAAP, (uint32_t)(g_xhci.dcbaa_phys & 0xFFFFFFFF));
  op_wr(XHCI_OP_DCBAAP + 4, (uint32_t)(g_xhci.dcbaa_phys >> 32));

  op_wr(XHCI_OP_CRCR, (uint32_t)(g_xhci.cmd.phys & 0xFFFFFFFF) | XHCI_CRCR_RCS);
  op_wr(XHCI_OP_CRCR + 4, (uint32_t)(g_xhci.cmd.phys >> 32));

  // Interrupter 0
  rt_wr(XHCI_RT_IR0_ERSTSZ, 1);
  rt_wr(XHCI_RT_IR0_ERSTBA, (uint32_t)(g_xhci.erst_phys & 0xFFFFFFFF));
  rt_wr(XHCI_RT_IR0_ERSTBA + 4, (uint32_t)(g_xhci.erst_phys >> 32));
  // ERDP con EHB=1 inicialmente
  rt_wr(XHCI_RT_IR0_ERDP, (uint32_t)(g_xhci.evt.phys & 0xFFFFFFFF) | 0x8);
  rt_wr(XHCI_RT_IR0_ERDP + 4, (uint32_t)(g_xhci.evt.phys >> 32));

  // Arrancar
  op_wr(XHCI_OP_USBCMD, XHCI_USBCMD_RS | XHCI_USBCMD_INTE);
  for (int i = 0; i < 100; i++) {
    if (op_rd(XHCI_OP_USBCMD) & XHCI_USBCMD_RS)
      break;
    xhci_delay_ms(1);
  }
  if (!(op_rd(XHCI_OP_USBCMD) & XHCI_USBCMD_RS)) {
    LOG_ERR("[xHCI] RS no se activó");
    return -EIO;
  }

  LOG_DEBUG("[xHCI] cmd@0x%lx evt@0x%lx dcbaa@0x%lx erst@0x%lx",
            (unsigned long)g_xhci.cmd.phys, (unsigned long)g_xhci.evt.phys,
            (unsigned long)g_xhci.dcbaa_phys, (unsigned long)g_xhci.erst_phys);
  return 0;
}

// ===========================================================================
// 6. Power-on de puertos (si PPC=1)
// ===========================================================================
static int xhci_power_on_ports(void) {
  int ppc = !!(g_xhci.hccparams1 & XHCI_HCC1_PPC);
  if (!ppc)
    return 0; // ports always powered

  for (uint8_t p = 1; p <= g_xhci.max_ports; p++) {
    uint32_t portsc = op_rd(XHCI_OP_PORTSC(p));
    if (portsc & XHCI_PORTSC_PP)
      continue;

    uint32_t val = portsc & ~XHCI_PORTSC_CHANGE_BITS;
    val |= XHCI_PORTSC_PP;
    op_wr(XHCI_OP_PORTSC(p), val);

    for (int i = 0; i < 100; i++) {
      if (op_rd(XHCI_OP_PORTSC(p)) & XHCI_PORTSC_PP)
        break;
      xhci_delay_ms(1);
    }
  }
  return 0;
}

// ===========================================================================
// API pública
// ===========================================================================

int xhci_hcd_alloc_device(usb_device_t *dev) {
  if (!dev)
    return -EINVAL;
  if (dev->hc_priv)
    return 0; // ya allocado

  xhci_dev_priv_t *p = (xhci_dev_priv_t *)kzalloc(sizeof(*p));
  if (!p)
    return -ENOMEM;

  int rc = xhci_ring_init(&p->ep0_ring, 16, 1);
  if (rc != 0) {
    kfree(p);
    return rc;
  }

  rc = usb_dma_alloc(sizeof(xhci_input_ctx_t), &p->input_ctx);
  if (rc != 0) {
    xhci_ring_destroy(&p->ep0_ring);
    kfree(p);
    return rc;
  }

  // Output Device Context. El HC lo rellena internamente durante
  // Address Device, pero la memoria debe existir y estar apuntada
  // por DCBAA[slot] (spec xHCI 1.2 §6.1).
  //
  // Tamaño: 33 contextos (ICC + slot + 31 EP) × ctx_size bytes.
  // (El HC no lo usa para ICC; usamos el mismo tamaño que input_ctx
  // por simetría y para cubrir todos los endpoints futuros.)
  size_t out_ctx_size = (size_t)(1 + 1 + 31) * g_xhci.ctx_size;
  rc = usb_dma_alloc(out_ctx_size, &p->out_ctx);
  if (rc != 0) {
    usb_dma_free(&p->input_ctx);
    xhci_ring_destroy(&p->ep0_ring);
    kfree(p);
    return rc;
  }

  dev->hc_priv = p;
  return 0;
}

void xhci_hcd_free_device(usb_device_t *dev) {
  if (!dev || !dev->hc_priv)
    return;
  xhci_dev_priv_t *p = (xhci_dev_priv_t *)dev->hc_priv;
  if (p->input_ctx.virt)
    usb_dma_free(&p->input_ctx);
  if (p->out_ctx.virt)
    usb_dma_free(&p->out_ctx);
  xhci_ring_destroy(&p->ep0_ring);
  kfree(p);
  dev->hc_priv = NULL;
}

// ===========================================================================
// Configura un endpoint no-EP0 en `dev`.
//
// Flujo:
//   1. Allocar un transfer ring para el EP.
//   2. Rellenar el input context: slot + EP0 + el nuevo EP.
//   3. Add = slot | ep0 | nuevo_ep  (para no "perder" los ya configurados).
//   4. Issue Configure Endpoint.
//
// Tras esto, `ep->hc_ep` apunta a `xhci_ep_state_t`.
// ===========================================================================
int xhci_hcd_configure_ep(usb_device_t *dev, usb_endpoint_t *ep) {
  if (!dev || !dev->hc_priv || !ep)
    return -EINVAL;
  xhci_dev_priv_t *p = (xhci_dev_priv_t *)dev->hc_priv;

  int ep_num = ep->addr & 0x0F;
  int dir_in = (ep->addr & 0x80) ? 1 : 0;
  if (ep_num == 0 || ep_num > 15)
    return -EINVAL;

  xhci_ep_state_t *st = &p->eps[ep_num - 1];
  if (st->configured) {
    ep->hc_ep = st;
    return 0;
  }

  // 1. Ring para el endpoint.
  memset(st, 0, sizeof(*st));
  int rc = xhci_ring_init(&st->ring, 16, 1);
  if (rc != 0)
    return rc;

  st->ep_num = (uint8_t)ep_num;
  st->dir_in = (uint8_t)dir_in;

  switch (ep->type) {
  case USB_EP_INTERRUPT:
    st->ep_type = dir_in ? XHCI_EPTYPE_INT_IN : XHCI_EPTYPE_INT_OUT;
    break;
  case USB_EP_BULK:
    st->ep_type = dir_in ? XHCI_EPTYPE_BULK_IN : XHCI_EPTYPE_BULK_OUT;
    break;
  default:
    xhci_ring_destroy(&st->ring);
    memset(st, 0, sizeof(*st));
    return -EINVAL;
  }

  // DCI (Device Context Index): EP1 OUT=2, EP1 IN=3, EP2 OUT=4, ...
  uint32_t dci = 2 * ep_num + (dir_in ? 1 : 0);

  // 2. Input context: SOLO slot + nuevo EP. NO incluir EP0
  //    (Configure Endpoint no puede tocar EP0, spec §4.6.6).
  xhci_input_ctx_t *in = (xhci_input_ctx_t *)p->input_ctx.virt;
  memset(in, 0, sizeof(*in));

  in->icc.add = XHCI_ICC_ADD_SLOT | (1u << dci);

  // 3. Slot Context. Context Entries = DCI del nuevo endpoint
  //    (highest valid DCI). Con EP0+EP1 IN configurados, es 3.
  in->slot.dw0 = XHCI_SLOT_DW0_ROUTE(0) |
                 XHCI_SLOT_DW0_SPEED(usb_speed_to_xhci_slot_code(p->speed)) |
                 XHCI_SLOT_DW0_CTX_ENT(dci);
  in->slot.dw1 = XHCI_SLOT_DW1_RH_PORT(p->port);
  in->slot.dw2 = XHCI_SLOT_DW2_USB_ADDR(p->slot_id);
  in->slot.dw3 = 0;

  // 4. Nuevo endpoint context. EP State = Disabled (el HC lo pone
  //    a Running tras procesar el comando). CErr va en DW1.
  xhci_ep_ctx_t *ec = xhci_ictx_ep(in, ep_num, dir_in);
  ec->dw0 = 0;
  ec->dw1 = XHCI_EP_DW1_CERR(3) | XHCI_EP_DW1_EPTYPE(st->ep_type) |
            XHCI_EP_DW1_MAX_PKT(ep->max_packet);
  uint64_t ep_deq = xhci_ring_trb_phys(&st->ring, 0) | XHCI_EP_DEQ_DCS;
  ec->dw2 = (uint32_t)(ep_deq & 0xFFFFFFFF);
  ec->dw3 = (uint32_t)(ep_deq >> 32);

  // Interval para interrupt endpoints (DW0 bits 23:16).
  if (ep->type == USB_EP_INTERRUPT) {
    ec->dw0 |= ((uint32_t)ep->interval & 0xFF) << 16;
  }

  // Average TRB Length (DW4 bits 15:0). Requerido para periodic EPs.
  uint32_t avg = (ep->max_packet > 0) ? ep->max_packet : 8;
  if (avg > 1024)
    avg = 1024;
  ec->reserved[0] = avg & 0xFFFF;

  usb_dma_sync(&p->input_ctx, USB_DMA_TO_DEVICE);

  // 5. Configure Endpoint.
  rc = xhci_cmd_configure_endpoint(p->slot_id, p->input_ctx.phys);
  if (rc != 0) {
    LOG_ERR("[xHCI] slot %u: configure_ep(EP%d %s) falló: %d", p->slot_id,
            ep_num, dir_in ? "IN" : "OUT", rc);
    xhci_ring_destroy(&st->ring);
    memset(st, 0, sizeof(*st));
    return rc;
  }

  st->configured = 1;
  ep->hc_ep = st;
  LOG_INFO("[xHCI] slot %u: EP%d %s configurado (mps=%u interval=%u)",
           p->slot_id, ep_num, dir_in ? "IN" : "OUT", ep->max_packet,
           ep->interval);
  return 0;
}

// El HCD que se pasa a los usb_device_t.
usb_hcd_t g_xhci_hcd = {
    .name = "xhci",
    .priv = &g_xhci,
    .control_xfer = xhci_hcd_control_xfer,
    .bulk_xfer = NULL,
    .interrupt_xfer = xhci_hcd_interrupt_xfer,
    .configure_ep = xhci_hcd_configure_ep,
    .alloc_device = xhci_hcd_alloc_device,
    .free_device = xhci_hcd_free_device,
};

int xhci_probe(void) {
  memset(&g_xhci, 0, sizeof(g_xhci));
  spin_init(&g_xhci.lock);

  int rc = xhci_find_controller(&g_xhci.pci);
  if (rc != 0) {
    LOG_INFO("[xHCI] no hay controlador xHCI en el bus PCI");
    return rc;
  }
  LOG_INFO("[xHCI] detectado en %02x:%02x.%x (vendor=0x%04x dev=0x%04x)",
           g_xhci.pci.bus, g_xhci.pci.slot, g_xhci.pci.func,
           g_xhci.pci.vendor_id, g_xhci.pci.device_id);

  pci_enable_bus_mastering(&g_xhci.pci);
  pci_enable_io_mem(&g_xhci.pci);

  rc = xhci_map_mmio();
  if (rc != 0)
    return rc;

  rc = xhci_read_caps();
  if (rc != 0)
    return rc;

  rc = xhci_hc_reset();
  if (rc != 0)
    return rc;

  rc = xhci_init_rings();
  if (rc != 0)
    return rc;

  rc = xhci_power_on_ports();
  if (rc != 0)
    return rc;

  g_xhci.initialized = 1;

  // Attach inicial. Para detección en caliente, se puede llamar a
  // xhci_attach_all_devices() desde el polling del scheduler (Fase 3+).
  xhci_attach_all_devices();

  LOG_INFO("[xHCI] inicializado");
  return 0;
}

void xhci_dump(void) {
  if (!g_xhci.initialized) {
    LOG_INFO("[xHCI] no inicializado");
    return;
  }
  LOG_INFO("[xHCI] CAPLEN=0x%x ver=%04x slots=%u ports=%u ctx=%uB",
           g_xhci.caplen, g_xhci.hciversion, g_xhci.max_slots, g_xhci.max_ports,
           g_xhci.ctx_size);
  LOG_INFO("[xHCI] cmd@0x%lx (%u TRBs enq=%u cyc=%u) evt@0x%lx (%u TRBs deq=%u "
           "cyc=%u)",
           (unsigned long)g_xhci.cmd.phys, g_xhci.cmd.size, g_xhci.cmd.enqueue,
           g_xhci.cmd.cycle, (unsigned long)g_xhci.evt.phys, g_xhci.evt.size,
           g_xhci.evt.dequeue, g_xhci.evt.cycle);
  LOG_INFO("[xHCI] USBCMD=0x%08x USBSTS=0x%08x", op_rd(XHCI_OP_USBCMD),
           op_rd(XHCI_OP_USBSTS));
}

// ===========================================================================
// Debug: volcar el estado de todos los puertos.
// ===========================================================================
void xhci_port_dump_all(void) {
  if (!g_xhci.initialized) {
    LOG_INFO("[xHCI] port dump: no inicializado");
    return;
  }
  for (uint8_t p = 1; p <= g_xhci.max_ports; p++) {
    uint32_t portsc = xhci_port_status(p);
    int ccs = !!(portsc & XHCI_PORTSC_CCS);
    int ped = !!(portsc & XHCI_PORTSC_PED);
    int pp = !!(portsc & XHCI_PORTSC_PP);
    uint32_t speed = (portsc >> 10) & 0xF;
    uint32_t pls = (portsc >> 5) & 0xF;
    LOG_INFO("[xHCI] port %u: PORTSC=0x%08x CCS=%d PED=%d PP=%d "
             "PLS=%u speed=%u",
             p, portsc, ccs, ped, pp, pls, speed);
  }
}

// ===========================================================================
// Driver
// ===========================================================================
static int xhci_init_impl(void) { return xhci_probe(); }

struct driver xhci_driver = {
    .name = "xhci",
    .init = xhci_init_impl,
    .shutdown = NULL,
};