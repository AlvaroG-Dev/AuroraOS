// kernel/usb/xhci/xhci.c
#include "xhci.h"
#include "../../apic.h"
#include "../../driver.h"
#include "../../heap.h"
#include "../../klog.h"
#include "../../paging.h"
#include "../../pci.h"
#include "../../sched.h"
#include "../../string.h"
#include "../../uaccess.h"
#include "../usb_dma.h"
#include "xhci_ring.h"

xhci_controller_t g_xhci;

xhci_trb_t g_pending_cmds[XHCI_MAX_PENDING_CMDS];
int g_pending_cmd_count = 0;
xhci_trb_t g_pending_xfers[XHCI_MAX_PENDING_XFERS];
int g_pending_xfer_count = 0;
xhci_pending_urb_t g_async_urbs[XHCI_MAX_ASYNC_URBS];
int g_async_urb_count = 0;

static volatile int g_event_pending = 0;
static volatile int g_hotplug_pending = 0;

static inline uint32_t mmio_rd(uint32_t off) {
  return *(volatile uint32_t *)(g_xhci.mmio + off);
}
static inline void op_wr(uint32_t off, uint32_t v) {
  *(volatile uint32_t *)(g_xhci.op + off) = v;
}
static inline uint32_t op_rd(uint32_t off) {
  return *(volatile uint32_t *)(g_xhci.op + off);
}
static inline void rt_wr(uint32_t off, uint32_t v) {
  *(volatile uint32_t *)(g_xhci.rt + off) = v;
}

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

static inline int xhci_fls(uint32_t x) {
  if (x == 0)
    return 0;
  uint32_t r;
  __asm__ volatile("bsrl %1, %0" : "=r"(r) : "r"(x));
  return (int)r + 1;
}

int xhci_take_pending_cmd(uint64_t cmd_trb_phys, xhci_trb_t *out) {
  unsigned long flags = spin_lock_irqsave(&g_xhci.lock);
  for (int i = 0; i < g_pending_cmd_count; i++) {
    if (g_pending_cmds[i].param == cmd_trb_phys) {
      *out = g_pending_cmds[i];
      g_pending_cmds[i] = g_pending_cmds[--g_pending_cmd_count];
      spin_unlock_irqrestore(&g_xhci.lock, flags);
      return 0;
    }
  }
  spin_unlock_irqrestore(&g_xhci.lock, flags);
  return -1;
}

int xhci_take_pending_xfer(uint8_t slot, uint8_t ep_id, uint64_t trb_phys,
                           xhci_trb_t *out) {
  unsigned long flags = spin_lock_irqsave(&g_xhci.lock);
  for (int i = 0; i < g_pending_xfer_count; i++) {
    if (XHCI_EVT_SLOT(g_pending_xfers[i]) == slot &&
        XHCI_EVT_EP(g_pending_xfers[i]) == ep_id &&
        g_pending_xfers[i].param == trb_phys) {
      *out = g_pending_xfers[i];
      g_pending_xfers[i] = g_pending_xfers[--g_pending_xfer_count];
      spin_unlock_irqrestore(&g_xhci.lock, flags);
      return 0;
    }
  }
  spin_unlock_irqrestore(&g_xhci.lock, flags);
  return -1;
}

static bool has_events(void *arg) {
  (void)arg;
  return __atomic_load_n(&g_event_pending, __ATOMIC_ACQUIRE) != 0;
}
static bool has_hotplug(void *arg) {
  (void)arg;
  return __atomic_load_n(&g_hotplug_pending, __ATOMIC_ACQUIRE) != 0;
}

/*
 * Keep the hard IRQ short. USBSTS.EINT, ERDP.EHB and IMAN.IP must be
 * acknowledged after the Event Ring consumer has advanced ERDP; clearing
 * them here can lose a second event that arrives while the thread is running.
 */
static void xhci_irq_handler_msi(void) {
  __atomic_store_n(&g_event_pending, 1, __ATOMIC_RELEASE);
  wake_up_all(&g_xhci.irq_wq);
}

// Event thread: único consumidor del anillo.
void xhci_event_thread(void) {
  LOG_INFO("[xHCI] Event thread iniciado (MSI=%d)", g_xhci.using_msi);
  while (1) {
    if (g_xhci.using_msi) {
      wait_event(&g_xhci.irq_wq, has_events, NULL);
    } else {
      // Sin MSI: poll cada 1 tick (1 ms)
      wait_event_interruptible_timeout(&g_xhci.irq_wq, has_events, NULL, 1);
    }
    /*
     * Clear the notification before draining. An interrupt arriving during
     * the drain sets it again and cannot be overwritten by consumer bookkeeping.
     */
    (void)__atomic_exchange_n(&g_event_pending, 0, __ATOMIC_ACQ_REL);

    while (1) {
      xhci_trb_t evt;
      unsigned long flags = spin_lock_irqsave(&g_xhci.lock);
      int rc = xhci_ring_dequeue(&g_xhci.evt, &evt);
      spin_unlock_irqrestore(&g_xhci.lock, flags);

      if (rc != 0)
        break;

      uint8_t type = XHCI_TRB_GET_TYPE(evt);
      if (type == XHCI_TRB_CMD_COMPLETION) {
        flags = spin_lock_irqsave(&g_xhci.lock);
        if (g_pending_cmd_count < XHCI_MAX_PENDING_CMDS) {
          g_pending_cmds[g_pending_cmd_count++] = evt;
          wake_up_all_locked(&g_xhci.cmd_wq);
        } else {
          LOG_ERR("[xHCI] pending cmd queue full");
        }
        spin_unlock_irqrestore(&g_xhci.lock, flags);
      } else if (type == XHCI_TRB_TRANSFER_EVT) {
        // 1. Intentar casar con un URB asíncrono pendiente (bulk UAS).
        usb_bulk_urb_t *async_urb = NULL;

        flags = spin_lock_irqsave(&g_xhci.lock);
        for (int i = 0; i < g_async_urb_count; i++) {
          xhci_pending_urb_t *p = &g_async_urbs[i];
          if (p->slot_id == XHCI_EVT_SLOT(evt) &&
              p->ep_id == XHCI_EVT_EP(evt) && p->trb_phys == evt.param) {
            async_urb = p->urb;
            g_async_urbs[i] = g_async_urbs[--g_async_urb_count];
            break;
          }
        }
        spin_unlock_irqrestore(&g_xhci.lock, flags);

        if (async_urb) {
          LOG_DEBUG("[xHCI] async completion slot=%u ep=%u cc=%u",
                    XHCI_EVT_SLOT(evt), XHCI_EVT_EP(evt), XHCI_EVT_CC(evt));
          uint32_t cc = XHCI_EVT_CC(evt);
          uint32_t residual = evt.status & 0xFFFFFF;
          if (residual > (uint32_t)async_urb->len)
            residual = (uint32_t)async_urb->len;
          async_urb->actual_len = async_urb->len - (int)residual;
          async_urb->status =
              (cc == XHCI_CC_SUCCESS || cc == XHCI_CC_SHORT_PACKET) ? 0 : -EIO;

          // Determinar el buffer DMA origen (para IN) y liberar el
          // que haya allocado el HCD (si lo hay).
          usb_dma_buf_t *src_dma = NULL;
          if (async_urb->dma_buf) {
            src_dma = (usb_dma_buf_t *)async_urb->dma_buf;
          } else if (async_urb->hc_priv) {
            src_dma = (usb_dma_buf_t *)async_urb->hc_priv;
          }

          // Copiar el resultado al buf del caller si es IN y el buffer
          // del caller no coincide con el DMA.
          if (async_urb->is_in && src_dma && src_dma->virt != async_urb->buf &&
              async_urb->actual_len > 0 && async_urb->status == 0) {
            memcpy(async_urb->buf, src_dma->virt,
                   (size_t)async_urb->actual_len);
          }

          // Liberar el DMA si lo allocamos nosotros.
          if (async_urb->hc_priv) {
            usb_dma_buf_t *owned = (usb_dma_buf_t *)async_urb->hc_priv;
            if (owned->virt)
              usb_dma_free(owned);
            kfree(owned);
            async_urb->hc_priv = NULL;
          }

          if (async_urb->complete)
            async_urb->complete(async_urb);
          continue;
        }

        // 2. Sin match: evento para un waiter síncrono (HID, control).
        flags = spin_lock_irqsave(&g_xhci.lock);
        if (g_pending_xfer_count < XHCI_MAX_PENDING_XFERS) {
          g_pending_xfers[g_pending_xfer_count++] = evt;
          wake_up_all_locked(&g_xhci.xfer_wq);
        } else {
          LOG_ERR("[xHCI] pending xfer queue full");
        }
        spin_unlock_irqrestore(&g_xhci.lock, flags);
      } else if (type == XHCI_TRB_PORT_STATUS_CHG) {
        __atomic_store_n(&g_hotplug_pending, 1, __ATOMIC_RELEASE);
        wake_up_all(&g_xhci.hub_wq);
      }
    }

    /*
     * xHCI interrupt acknowledgement order:
     *   1. clear USBSTS status (including EINT),
     *   2. advance ERDP and write EHB=1,
     *   3. clear IMAN.IP while preserving IMAN.IE.
     */
    uint32_t sts = op_rd(XHCI_OP_USBSTS);
    uint32_t rw1c = sts & (XHCI_USBSTS_EINT | XHCI_USBSTS_PCD |
                           XHCI_USBSTS_HSE | XHCI_USBSTS_HCE);
    if (rw1c)
      op_wr(XHCI_OP_USBSTS, rw1c);

    unsigned long flags = spin_lock_irqsave(&g_xhci.lock);
    uint64_t next = xhci_ring_trb_phys(&g_xhci.evt, g_xhci.evt.dequeue);
    uint32_t *next_trb = (uint32_t *)&g_xhci.evt.trbs[g_xhci.evt.dequeue];
    int more_events =
        ((next_trb[3] & XHCI_TRB_CYCLE) == g_xhci.evt.cycle);
    spin_unlock_irqrestore(&g_xhci.lock, flags);

    rt_wr(XHCI_RT_IR0_ERDP + 4, (uint32_t)(next >> 32));
    rt_wr(XHCI_RT_IR0_ERDP, (uint32_t)(next & 0xFFFFFFF0u) | XHCI_ERDP_EHB);

    uint32_t iman = *(volatile uint32_t *)(g_xhci.rt + XHCI_RT_IR0_IMAN);
    if (iman & XHCI_IMAN_IP)
      rt_wr(XHCI_RT_IR0_IMAN, (iman & XHCI_IMAN_IE) | XHCI_IMAN_IP);

    /* Catch an event racing with the empty-ring check and acknowledgement. */
    flags = spin_lock_irqsave(&g_xhci.lock);
    next_trb = (uint32_t *)&g_xhci.evt.trbs[g_xhci.evt.dequeue];
    more_events =
        ((next_trb[3] & XHCI_TRB_CYCLE) == g_xhci.evt.cycle);
    spin_unlock_irqrestore(&g_xhci.lock, flags);
    if (more_events)
      __atomic_store_n(&g_event_pending, 1, __ATOMIC_RELEASE);
  }
}

void xhci_hotplug_thread(void) {
  LOG_INFO("[xHCI] Hotplug thread iniciado");
  while (1) {
    wait_event(&g_xhci.hub_wq, has_hotplug, NULL);
    (void)__atomic_exchange_n(&g_hotplug_pending, 0, __ATOMIC_ACQ_REL);
    LOG_INFO("[xHCI] Hotplug event detectado, re-escaneando root hub");
    xhci_attach_all_devices();
  }
}

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
        if (d.prog_if != 0x30)
          continue;
        *out = d;
        return 0;
      }
    }
  }
  return -ENODEV;
}

static int xhci_map_mmio(void) {
  uint32_t lo = pci_read_config_dword(g_xhci.pci.bus, g_xhci.pci.slot,
                                      g_xhci.pci.func, 0x10);
  if (lo & 0x01)
    return -ENODEV;
  uint64_t phys;
  if (((lo >> 1) & 0x03) == 0x02) {
    uint32_t hi = pci_read_config_dword(g_xhci.pci.bus, g_xhci.pci.slot,
                                        g_xhci.pci.func, 0x14);
    phys = (lo & ~0x0FULL) | ((uint64_t)hi << 32);
  } else {
    phys = lo & ~0x0FULL;
  }
  if (phys == 0)
    return -ENODEV;
  g_xhci.mmio_phys = phys;
  g_xhci.mmio_size = 0x10000;
  g_xhci.mmio = (volatile uint8_t *)mmio_map(
      phys, g_xhci.mmio_size, PTE_WRITABLE | PTE_NOCACHE | PTE_NX);
  if (!g_xhci.mmio)
    return -ENOMEM;
  return 0;
}

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
  if (g_xhci.max_slots == 0 || g_xhci.max_ports == 0)
    return -EIO;
  return 0;
}

/*
 * Transfer ownership from firmware before touching operational registers.
 * xECP is a dword offset from the capability base; NEXT is measured in dwords.
 */
static int xhci_legacy_handoff(void) {
  uint32_t xecp = (g_xhci.hccparams1 >> 16) & 0xFFFFu;
  if (xecp == 0)
    return 0;

  uint32_t off = xecp * 4u;
  for (uint32_t n = 0; n < 64 && off + 4 <= g_xhci.mmio_size; n++) {
    uint32_t cap = mmio_rd(off);
    uint8_t cap_id = (uint8_t)(cap & 0xFFu);
    uint8_t next = (uint8_t)((cap >> 8) & 0xFFu);

    if (cap_id == 1) { /* USB Legacy Support capability */
      const uint32_t bios_owned = 1u << 16;
      const uint32_t os_owned = 1u << 24;
      *(volatile uint32_t *)(g_xhci.mmio + off) = cap | os_owned;
      __asm__ volatile("mfence" ::: "memory");

      if (cap & bios_owned) {
        for (int i = 0; i < 1000; i++) {
          if (!(mmio_rd(off) & bios_owned)) {
            LOG_INFO("[xHCI] firmware relinquished USB ownership");
            return 0;
          }
          xhci_delay_ms(1);
        }
        LOG_ERR("[xHCI] timeout esperando USB Legacy Handoff (BIOS-owned)");
        return -ETIMEDOUT;
      }
      LOG_INFO("[xHCI] USB Legacy Support encontrado; OS-owned solicitado");
      return 0;
    }

    if (next == 0)
      break;
    uint32_t advance = (uint32_t)next * 4u;
    if (advance > g_xhci.mmio_size - off)
      break;
    off += advance;
  }
  return 0;
}

static int xhci_hc_reset(void) {
  if (op_rd(XHCI_OP_USBCMD) & XHCI_USBCMD_RS) {
    op_wr(XHCI_OP_USBCMD, op_rd(XHCI_OP_USBCMD) & ~XHCI_USBCMD_RS);
    int halted = 0;
    for (int i = 0; i < 100; i++) {
      if (op_rd(XHCI_OP_USBSTS) & XHCI_USBSTS_HCH) {
        halted = 1;
        break;
      }
      xhci_delay_ms(1);
    }
    if (!halted) {
      LOG_ERR("[xHCI] el controlador no se detuvo antes del reset");
      return -ETIMEDOUT;
    }
  }
  op_wr(XHCI_OP_USBCMD, op_rd(XHCI_OP_USBCMD) | XHCI_USBCMD_HCRST);
  for (int i = 0; i < 1000; i++) {
    if (!(op_rd(XHCI_OP_USBCMD) & XHCI_USBCMD_HCRST))
      break;
    xhci_delay_ms(1);
  }
  if (op_rd(XHCI_OP_USBCMD) & XHCI_USBCMD_HCRST)
    return -ETIMEDOUT;
  for (int i = 0; i < 1000; i++) {
    if (!(op_rd(XHCI_OP_USBSTS) & XHCI_USBSTS_CNR))
      break;
    xhci_delay_ms(1);
  }
  if (op_rd(XHCI_OP_USBSTS) & XHCI_USBSTS_CNR)
    return -ETIMEDOUT;
  op_wr(XHCI_OP_CONFIG, g_xhci.max_slots);
  return 0;
}

static int xhci_init_rings(void) {
  int rc = xhci_ring_init(&g_xhci.cmd, 64, 1);
  if (rc != 0)
    return rc;
  rc = xhci_ring_init(&g_xhci.evt, 64, 0);
  if (rc != 0) {
    xhci_ring_destroy(&g_xhci.cmd);
    return rc;
  }

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

  op_wr(XHCI_OP_DCBAAP, (uint32_t)(g_xhci.dcbaa_phys & 0xFFFFFFFF));
  op_wr(XHCI_OP_DCBAAP + 4, (uint32_t)(g_xhci.dcbaa_phys >> 32));
  op_wr(XHCI_OP_CRCR, (uint32_t)(g_xhci.cmd.phys & 0xFFFFFFFF) | XHCI_CRCR_RCS);
  op_wr(XHCI_OP_CRCR + 4, (uint32_t)(g_xhci.cmd.phys >> 32));

  rt_wr(XHCI_RT_IR0_ERSTSZ, 1);
  rt_wr(XHCI_RT_IR0_ERSTBA, (uint32_t)(g_xhci.erst_phys & 0xFFFFFFFF));
  rt_wr(XHCI_RT_IR0_ERSTBA + 4, (uint32_t)(g_xhci.erst_phys >> 32));
  rt_wr(XHCI_RT_IR0_ERDP + 4, (uint32_t)(g_xhci.evt.phys >> 32));
  rt_wr(XHCI_RT_IR0_ERDP,
        (uint32_t)(g_xhci.evt.phys & 0xFFFFFFF0u) | XHCI_ERDP_EHB);

  /* Interrupts stay disabled until the handler and consumer threads exist. */
  rt_wr(XHCI_RT_IR0_IMAN, 0);

  op_wr(XHCI_OP_USBCMD, XHCI_USBCMD_RS);
  for (int i = 0; i < 100; i++) {
    if (op_rd(XHCI_OP_USBCMD) & XHCI_USBCMD_RS)
      break;
    xhci_delay_ms(1);
  }
  if (!(op_rd(XHCI_OP_USBCMD) & XHCI_USBCMD_RS))
    return -EIO;
  return 0;
}

static int xhci_power_on_ports(void) {
  int ppc = !!(g_xhci.hccparams1 & XHCI_HCC1_PPC);
  if (!ppc)
    return 0;
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

int xhci_hcd_alloc_device(usb_device_t *dev) {
  if (!dev)
    return -EINVAL;
  if (dev->hc_priv)
    return 0;
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
  for (int i = 0; i < 15; i++) {
    if (p->eps[i].stream_ctx.virt)
      usb_dma_free(&p->eps[i].stream_ctx);
    if (p->eps[i].configured)
      xhci_ring_destroy(&p->eps[i].ring);
  }
  if (p->input_ctx.virt)
    usb_dma_free(&p->input_ctx);
  if (p->out_ctx.virt)
    usb_dma_free(&p->out_ctx);
  xhci_ring_destroy(&p->ep0_ring);
  kfree(p);
  dev->hc_priv = NULL;
}

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
  uint32_t dci = 2 * ep_num + (dir_in ? 1 : 0);
  xhci_input_ctx_t *in = (xhci_input_ctx_t *)p->input_ctx.virt;
  memset(in, 0, sizeof(*in));
  in->icc.add = XHCI_ICC_ADD_SLOT | (1u << dci);
  in->slot.dw0 = XHCI_SLOT_DW0_ROUTE(0) |
                 XHCI_SLOT_DW0_SPEED(usb_speed_to_xhci_slot_code(p->speed)) |
                 XHCI_SLOT_DW0_CTX_ENT(dci);
  in->slot.dw1 = XHCI_SLOT_DW1_RH_PORT(p->root_hub_port);
  in->slot.dw2 = (((uint32_t)p->slot_id) << 8);
  in->slot.dw3 = 0;

  xhci_ep_ctx_t *ec = xhci_ictx_ep(in, ep_num, dir_in);
  ec->dw0 = 0;
  ec->dw1 = XHCI_EP_DW1_CERR(3) | XHCI_EP_DW1_EPTYPE(st->ep_type) |
            XHCI_EP_DW1_MAX_PKT(ep->max_packet);
  uint64_t ep_deq = xhci_ring_trb_phys(&st->ring, 0) | XHCI_EP_DEQ_DCS;
  ec->dw2 = (uint32_t)(ep_deq & 0xFFFFFFFF);
  ec->dw3 = (uint32_t)(ep_deq >> 32);
  if (ep->stream_id != 0 && ep->type == USB_EP_BULK) {
    rc = usb_dma_alloc(XHCI_UAS_STREAM_COUNT * sizeof(xhci_trb_t),
                       &st->stream_ctx);
    if (rc != 0) {
      xhci_ring_destroy(&st->ring);
      memset(st, 0, sizeof(*st));
      return rc;
    }
    xhci_trb_t *sc = (xhci_trb_t *)st->stream_ctx.virt;
    memset(sc, 0, XHCI_UAS_STREAM_COUNT * sizeof(*sc));
    uint64_t stream_deq = xhci_ring_trb_phys(&st->ring, 0);
    for (uint32_t sid = 1; sid < XHCI_UAS_STREAM_COUNT; sid++) {
      sc[sid].param = XHCI_EP_DEQ_DCS | XHCI_STREAM_CTX_SCT_PRIMARY;
      sc[sid].status = (uint32_t)(stream_deq & 0xFFFFFFFFu);
      sc[sid].control = (uint32_t)(stream_deq >> 32);
    }
    usb_dma_sync(&st->stream_ctx, USB_DMA_TO_DEVICE);
    st->has_streams = 1;
    st->max_streams = XHCI_UAS_STREAM_COUNT - 1;
    ec->dw0 |= XHCI_EP_DW0_LSA | XHCI_EP_DW0_MAXPSTR(XHCI_UAS_MAX_PSTREAMS);
    ec->dw2 = (uint32_t)(st->stream_ctx.phys & 0xFFFFFFFFu);
    ec->dw3 = (uint32_t)(st->stream_ctx.phys >> 32);
  }

  if (ep->type == USB_EP_INTERRUPT) {
    uint32_t interval_field;
    if (p->speed == USB_SPEED_LOW || p->speed == USB_SPEED_FULL) {
      int b = ep->interval;
      if (b < 1)
        b = 1;
      if (b > 255)
        b = 255;
      interval_field = (uint32_t)(xhci_fls((uint32_t)b) + 3);
    } else {
      interval_field = (uint32_t)ep->interval;
      if (interval_field < 1)
        interval_field = 1;
      if (interval_field > 255)
        interval_field = 255;
    }
    ec->dw0 |= (interval_field & 0xFF) << 16;
  }
  uint32_t avg = (ep->max_packet > 0) ? ep->max_packet : 8;
  if (avg > 1024)
    avg = 1024;
  ec->reserved[0] = avg & 0xFFFF;
  usb_dma_sync(&p->input_ctx, USB_DMA_TO_DEVICE);
  rc = xhci_cmd_configure_endpoint(p->slot_id, p->input_ctx.phys);
  if (rc != 0) {
    xhci_ring_destroy(&st->ring);
    memset(st, 0, sizeof(*st));
    return rc;
  }
  st->configured = 1;
  ep->hc_ep = st;
  return 0;
}

int xhci_hcd_new_device(usb_device_t *parent_hub, uint8_t port,
                        usb_speed_t speed, usb_device_t **out_dev) {
  if (!out_dev)
    return -EINVAL;
  *out_dev = NULL;

  uint8_t slot = 0;
  int rc = xhci_cmd_enable_slot(&slot);
  if (rc != 0)
    return rc;

  usb_device_t *dev = usb_device_alloc();
  if (!dev) {
    xhci_cmd_disable_slot(slot);
    return -ENOMEM;
  }

  dev->speed = speed;
  dev->parent_hub = parent_hub;
  dev->port_on_parent = port;
  uint16_t mps = xhci_default_ep0_mps(speed);
  dev->max_packet_size_ep0 = (mps > 0xFFu) ? 0xFFu : (uint8_t)mps;
  dev->hcd = &g_xhci_hcd;
  dev->addr = slot;

  rc = xhci_hcd_alloc_device(dev);
  if (rc != 0) {
    usb_device_free(dev);
    xhci_cmd_disable_slot(slot);
    return rc;
  }

  xhci_dev_priv_t *p = (xhci_dev_priv_t *)dev->hc_priv;
  p->slot_id = slot;
  p->root_hub_port = port;
  if (parent_hub) {
    const usb_device_t *top_hub = parent_hub;
    while (top_hub->parent_hub)
      top_hub = top_hub->parent_hub;
    p->root_hub_port = top_hub->port_on_parent;
  }
  p->speed = speed;

  g_xhci.dcbaa[slot] = p->out_ctx.phys;
  __asm__ volatile("mfence" ::: "memory");

  xhci_input_ctx_t *icc = (xhci_input_ctx_t *)p->input_ctx.virt;
  memset(icc, 0, sizeof(*icc));
  icc->icc.add = XHCI_ICC_ADD_SLOT | XHCI_ICC_ADD_EP0;

  uint32_t route = usb_device_route(dev);

  // ------------------------------------------------------------------
  // Transaction Translator (TT) para dispositivos FS/LS detrás de un hub HS/SS.
  // Si el padre es un hub de alta velocidad y el hijo es de baja/full speed,
  // el xHCI necesita saber qué TT usar (el del hub padre).
  // ------------------------------------------------------------------
  uint32_t dw2 = 0;
  if (parent_hub != NULL) {
    if ((parent_hub->speed == USB_SPEED_HIGH ||
         parent_hub->speed == USB_SPEED_SUPER ||
         parent_hub->speed == USB_SPEED_SUPER_PLUS) &&
        (speed == USB_SPEED_FULL || speed == USB_SPEED_LOW)) {

      uint8_t tt_hub_slot = parent_hub->addr; // Slot ID del hub
      uint8_t tt_port = port;                 // Puerto en el hub
      uint8_t ttt = 1;                        // Think Time (1 = 8 FS bit times)

      dw2 = XHCI_SLOT_DW2_TT_HUB_SLOT(tt_hub_slot) |
            XHCI_SLOT_DW2_TT_PORT(tt_port) | XHCI_SLOT_DW2_TTT(ttt);

      LOG_INFO("[xHCI] slot %u: TT configurado (hub_slot=%u port=%u ttt=%u)",
               slot, tt_hub_slot, tt_port, ttt);
    }
  }

  icc->slot.dw0 = XHCI_SLOT_DW0_ROUTE(route) |
                  XHCI_SLOT_DW0_SPEED(usb_speed_to_xhci_slot_code(speed)) |
                  XHCI_SLOT_DW0_CTX_ENT(1);
  icc->slot.dw1 = XHCI_SLOT_DW1_RH_PORT(p->root_hub_port);
  icc->slot.dw2 = dw2;
  icc->slot.dw3 = 0;

  xhci_ep_ctx_t *ep0 = xhci_ictx_ep0(icc);
  ep0->dw0 = 0;
  ep0->dw1 = XHCI_EP_DW1_CERR(3) | XHCI_EP_DW1_EPTYPE(XHCI_EPTYPE_CONTROL) |
             XHCI_EP_DW1_MAX_PKT(mps);
  uint64_t deq = xhci_ring_trb_phys(&p->ep0_ring, 0) | XHCI_EP_DEQ_DCS;
  ep0->dw2 = (uint32_t)(deq & 0xFFFFFFFF);
  ep0->dw3 = (uint32_t)(deq >> 32);
  ep0->reserved[0] = XHCI_EP_DW4_AVG_TRB_LEN(8);

  usb_dma_sync(&p->input_ctx, USB_DMA_TO_DEVICE);

  rc = xhci_cmd_address_device(slot, port, speed, mps, p->input_ctx.phys);
  if (rc != 0) {
    g_xhci.dcbaa[slot] = 0;
    usb_device_free(dev);
    xhci_cmd_disable_slot(slot);
    return rc;
  }

  LOG_INFO("[xHCI] new device slot=%u route=0x%05x speed=%d parent=%s port=%u",
           slot, route, (int)speed, parent_hub ? "hub" : "root", port);
  *out_dev = dev;
  return 0;
}

int xhci_hcd_set_hub_info(usb_device_t *dev, uint8_t num_ports) {
  if (!dev || !dev->hc_priv)
    return -EINVAL;
  xhci_dev_priv_t *p = (xhci_dev_priv_t *)dev->hc_priv;

  xhci_input_ctx_t *icc = (xhci_input_ctx_t *)p->input_ctx.virt;
  memset(icc, 0, sizeof(*icc));

  icc->icc.add = XHCI_ICC_ADD_SLOT;

  uint32_t route = usb_device_route(dev);

  // DW0: Route, Speed, Hub=1, Context Entries
  icc->slot.dw0 = XHCI_SLOT_DW0_ROUTE(route) |
                  XHCI_SLOT_DW0_SPEED(usb_speed_to_xhci_slot_code(p->speed)) |
                  XHCI_SLOT_DW0_CTX_ENT(1) | XHCI_SLOT_DW0_HUB;

  // DW1: Root Hub Port, Number of Ports
  icc->slot.dw1 = XHCI_SLOT_DW1_RH_PORT(p->root_hub_port) |
                  XHCI_SLOT_DW1_NUM_PORTS(num_ports);

  icc->slot.dw2 = 0;
  icc->slot.dw3 = 0;

  usb_dma_sync(&p->input_ctx, USB_DMA_TO_DEVICE);

  int rc = xhci_cmd_evaluate_context(p->slot_id, p->input_ctx.phys);
  if (rc != 0) {
    LOG_ERR("[xHCI] set_hub_info(slot=%u) falló: %d", p->slot_id, rc);
    return rc;
  }

  LOG_INFO("[xHCI] slot %u: marcado como hub (%u puertos)", p->slot_id,
           num_ports);
  return 0;
}

int xhci_hcd_bulk_submit(usb_device_t *dev, usb_endpoint_t *ep,
                         usb_bulk_urb_t *urb) {
  if (!dev || !dev->hc_priv || !ep || !urb || !urb->buf || urb->len <= 0)
    return -EINVAL;

  xhci_dev_priv_t *p = (xhci_dev_priv_t *)dev->hc_priv;
  xhci_ep_state_t *st = (xhci_ep_state_t *)ep->hc_ep;
  if (!st || !st->configured)
    return -ENODEV;

  int dir_in = (ep->addr & 0x80) ? 1 : 0;
  int ep_num = ep->addr & 0x0F;
  uint32_t dci = dir_in ? (2 * ep_num + 1) : (2 * ep_num);
  uint16_t stream_id = urb->stream_id;
  if (st->has_streams) {
    if (stream_id == 0 || stream_id > st->max_streams)
      return -EINVAL;
  } else if (stream_id != 0) {
    return -EINVAL;
  }

  // Elegir buffer DMA: el del caller si lo aporta, o uno propio.
  usb_dma_buf_t dma_local;
  usb_dma_buf_t *dma = NULL;
  int dma_allocated = 0;
  memset(&dma_local, 0, sizeof(dma_local));

  if (urb->dma_buf) {
    dma = urb->dma_buf;
    // Aseguramos que el write-back al físico esté hecho si es OUT.
    if (!dir_in)
      usb_dma_sync(dma, USB_DMA_TO_DEVICE);
  } else {
    if (usb_dma_alloc((size_t)urb->len, &dma_local) != 0)
      return -ENOMEM;
    dma = &dma_local;
    dma_allocated = 1;
    if (!dir_in) {
      memcpy(dma->virt, urb->buf, (size_t)urb->len);
      usb_dma_sync(dma, USB_DMA_TO_DEVICE);
    }
  }

  // Reservar slot en la cola antes de tocar el ring.
  unsigned long flags = spin_lock_irqsave(&g_xhci.lock);
  if (g_async_urb_count >= XHCI_MAX_ASYNC_URBS) {
    spin_unlock_irqrestore(&g_xhci.lock, flags);
    if (dma_allocated)
      usb_dma_free(&dma_local);
    return -ENOMEM;
  }

  uint32_t wrap_limit = st->ring.size - 1;
  uint32_t trb_idx = st->ring.enqueue;
  if (trb_idx == wrap_limit)
    trb_idx = 0;
  uint64_t trb_phys = xhci_ring_trb_phys(&st->ring, trb_idx);

  xhci_trb_t trb = {0};
  trb.param = dma->phys;
  trb.status = (uint32_t)urb->len;
  trb.control = XHCI_TRB_TYPE(XHCI_TRB_NORMAL) | XHCI_TRB_ISP | XHCI_TRB_IOC |
                (dir_in ? XHCI_TRB_DIR_IN : 0);

  int rc = xhci_ring_enqueue(&st->ring, &trb);
  if (rc != 0) {
    spin_unlock_irqrestore(&g_xhci.lock, flags);
    if (dma_allocated)
      usb_dma_free(&dma_local);
    return rc;
  }

  xhci_pending_urb_t *pu = &g_async_urbs[g_async_urb_count++];
  pu->urb = urb;
  pu->trb_phys = trb_phys;
  pu->slot_id = p->slot_id;
  pu->ep_id = (uint8_t)dci;

  __asm__ volatile("mfence" ::: "memory");
  *(volatile uint32_t *)(g_xhci.db + p->slot_id * 4) =
      dci | ((uint32_t)stream_id * 65536u);
  spin_unlock_irqrestore(&g_xhci.lock, flags);

  // Guardar el buffer DMA allocado (solo si lo hemos allocado nosotros)
  // para liberarlo en el completion. Si urb->dma_buf es del caller,
  // dejamos hc_priv = NULL como marcador.
  if (dma_allocated) {
    usb_dma_buf_t *saved = kmalloc(sizeof(*saved));
    if (saved)
      memcpy(saved, &dma_local, sizeof(dma_local));
    urb->hc_priv = saved;
  } else {
    urb->hc_priv = NULL;
  }

  return 0;
}

// ===========================================================================
// Bulk transfer síncrono usando un buffer DMA ya preparado por el caller.
//
// `phys` es la dirección física del buffer (el caller la obtuvo con
// usb_dma_alloc). `virt` es la misma memoria mapeada vía PHYS_MAP_BASE,
// solo se usa para devolver el data en IN. NO se hace memcpy: el HC
// lee/escribe directamente en `phys`.
//
// Esto evita el problema de coherencia caché<->DMA que se da con
// usb-uas (QEMU lee el buffer con address_space_read sin pasar por
// la caché del CPU guest, y el memcpy a un DMA nuevo deja los bytes
// en caché sin write-back visible al emulador).
// ===========================================================================
int xhci_hcd_bulk_xfer_phys(usb_device_t *dev, usb_endpoint_t *ep,
                            uint64_t phys, void *virt, int len, int is_in,
                            int timeout_ms) {
  if (!dev || !dev->hc_priv || !ep || !phys || !virt || len <= 0)
    return -EINVAL;

  xhci_dev_priv_t *p = (xhci_dev_priv_t *)dev->hc_priv;
  xhci_ep_state_t *st = (xhci_ep_state_t *)ep->hc_ep;
  if (!st || !st->configured)
    return -ENODEV;
  uint16_t stream_id = ep->stream_id;
  if (st->has_streams) {
    if (stream_id == 0 || stream_id > st->max_streams)
      return -EINVAL;
  } else if (stream_id != 0) {
    return -EINVAL;
  }

  int dir_in = (ep->addr & 0x80) ? 1 : 0;
  int ep_num = ep->addr & 0x0F;
  uint32_t dci = dir_in ? (2 * ep_num + 1) : (2 * ep_num);

  // Write-back del rango antes de DMA (no-op en x86 coherente, pero
  // necesario para QEMU's usb-uas que lee del buffer físico sin pasar
  // por la caché del guest).
  if (!dir_in) {
    uintptr_t start = (uintptr_t)virt & ~(uintptr_t)63;
    uintptr_t end = ((uintptr_t)virt + (uintptr_t)len + 63) & ~(uintptr_t)63;
    for (uintptr_t a = start; a < end; a += 64)
      __asm__ volatile("clflush (%0)" : : "r"((const void *)a) : "memory");
    __asm__ volatile("mfence" ::: "memory");
  }

  uint32_t wrap_limit = st->ring.size - 1;
  uint32_t trb_idx = st->ring.enqueue;
  if (trb_idx == wrap_limit)
    trb_idx = 0;
  uint64_t trb_phys = xhci_ring_trb_phys(&st->ring, trb_idx);

  xhci_trb_t trb = {0};
  trb.param = phys;
  trb.status = (uint32_t)len;
  trb.control = XHCI_TRB_TYPE(XHCI_TRB_NORMAL) | XHCI_TRB_ISP | XHCI_TRB_IOC |
                (dir_in ? XHCI_TRB_DIR_IN : 0);

  unsigned long flags = spin_lock_irqsave(&g_xhci.lock);
  int rc = xhci_ring_enqueue(&st->ring, &trb);
  __asm__ volatile("mfence" ::: "memory");
  *(volatile uint32_t *)(g_xhci.db + p->slot_id * 4) =
      dci | ((uint32_t)stream_id * 65536u);
  spin_unlock_irqrestore(&g_xhci.lock, flags);

  if (rc != 0)
    return rc;

  uint32_t residual = 0;
  int werr = xhci_wait_transfer(p->slot_id, (uint8_t)dci, trb_phys,
                                (uint32_t)timeout_ms, &residual);

  if (werr == 0) {
    if (dir_in) {
      uintptr_t flush_base = (uintptr_t)virt & ~(uintptr_t)63;
      uintptr_t flush_end =
          ((uintptr_t)virt + (uintptr_t)len + 63) & ~(uintptr_t)63;
      for (uintptr_t a = flush_base; a < flush_end; a += 64)
        __asm__ volatile("clflush (%0)" : : "r"((const void *)a) : "memory");
      __asm__ volatile("mfence" ::: "memory");
    }
    if (residual > (uint32_t)len)
      residual = (uint32_t)len;
    return len - (int)residual;
  }
  return werr;
}

usb_hcd_t g_xhci_hcd = {
    .name = "xhci",
    .priv = &g_xhci,
    .control_xfer = xhci_hcd_control_xfer,
    .bulk_xfer = xhci_hcd_bulk_xfer,
    .interrupt_xfer = xhci_hcd_interrupt_xfer,
    .configure_ep = xhci_hcd_configure_ep,
    .new_device = xhci_hcd_new_device,
    .set_hub_info = xhci_hcd_set_hub_info,
    .bulk_submit = xhci_hcd_bulk_submit,
    .bulk_xfer_phys = xhci_hcd_bulk_xfer_phys,
    .alloc_device = xhci_hcd_alloc_device,
    .free_device = xhci_hcd_free_device,
};

int xhci_probe(void) {
  memset(&g_xhci, 0, sizeof(g_xhci));
  spin_init(&g_xhci.lock);
  wait_queue_init(&g_xhci.irq_wq);
  wait_queue_init(&g_xhci.cmd_wq);
  wait_queue_init(&g_xhci.xfer_wq);
  wait_queue_init(&g_xhci.hub_wq);

  int rc = xhci_find_controller(&g_xhci.pci);
  if (rc != 0)
    return rc;

  pci_enable_bus_mastering(&g_xhci.pci);
  pci_enable_io_mem(&g_xhci.pci);
  rc = xhci_map_mmio();
  if (rc != 0)
    return rc;
  rc = xhci_read_caps();
  if (rc != 0)
    return rc;
  rc = xhci_legacy_handoff();
  if (rc != 0)
    return rc;
  rc = xhci_hc_reset();
  if (rc != 0)
    return rc;
  rc = xhci_init_rings();
  if (rc != 0)
    return rc;
  xhci_power_on_ports();

  /*
   * Register the vector before enabling MSI in PCI config space. The HC is
   * running, but USBCMD.INTE and IMAN.IE remain disabled until this is ready.
   */
  uint8_t msi_vec = 0x62;
  msi_install_handler(msi_vec, xhci_irq_handler_msi);
  if (pci_enable_msi(g_xhci.pci.bus, g_xhci.pci.slot, g_xhci.pci.func, msi_vec,
                     (uint8_t)lapic_get_bsp_id()) == 0) {
    g_xhci.using_msi = 1;
    g_xhci.msi_vector = msi_vec;
    LOG_INFO("[xHCI] MSI preparado en vector 0x%02x", msi_vec);
  } else {
    msi_install_handler(msi_vec, NULL);
    LOG_WARN("[xHCI] MSI no disponible; usando polling del Event Ring");
  }

  g_xhci.initialized = 1;

  /* Create the consumer before allowing the controller to generate MSI. */
  if (!sched_create_task(xhci_event_thread)) {
    LOG_ERR("[xHCI] no se pudo crear Event Thread; abortando");
    g_xhci.initialized = 0;
    return -ENOMEM;
  }
  if (!sched_create_task(xhci_hotplug_thread))
    LOG_WARN("[xHCI] no se pudo crear Hotplug Thread");

  if (g_xhci.using_msi) {
    rt_wr(XHCI_RT_IR0_IMAN, XHCI_IMAN_IE);
    op_wr(XHCI_OP_USBCMD, op_rd(XHCI_OP_USBCMD) | XHCI_USBCMD_INTE);
  }

  xhci_attach_all_devices();
  LOG_INFO("[xHCI] inicializado");
  return 0;
}

void xhci_dump(void) {
  if (!g_xhci.initialized)
    return;
  LOG_INFO("[xHCI] CAPLEN=0x%x ver=%04x slots=%u ports=%u ctx=%uB",
           g_xhci.caplen, g_xhci.hciversion, g_xhci.max_slots, g_xhci.max_ports,
           g_xhci.ctx_size);
  LOG_INFO("[xHCI] cmd@0x%lx evt@0x%lx dcbaa@0x%lx",
           (unsigned long)g_xhci.cmd.phys, (unsigned long)g_xhci.evt.phys,
           (unsigned long)g_xhci.dcbaa_phys);
  LOG_INFO("[xHCI] USBCMD=0x%08x USBSTS=0x%08x", op_rd(XHCI_OP_USBCMD),
           op_rd(XHCI_OP_USBSTS));
}

void xhci_port_dump_all(void) {
  if (!g_xhci.initialized)
    return;
  for (uint8_t p = 1; p <= g_xhci.max_ports; p++) {
    uint32_t portsc = xhci_port_status(p);
    LOG_INFO(
        "[xHCI] port %u: PORTSC=0x%08x CCS=%d PED=%d PP=%d PLS=%u speed=%u", p,
        portsc, !!(portsc & XHCI_PORTSC_CCS), !!(portsc & XHCI_PORTSC_PED),
        !!(portsc & XHCI_PORTSC_PP), (portsc >> 5) & 0xF, (portsc >> 10) & 0xF);
  }
}

static int xhci_init_impl(void) { return xhci_probe(); }
struct driver xhci_driver = {
    .name = "xhci", .init = xhci_init_impl, .shutdown = NULL};