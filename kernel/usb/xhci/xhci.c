// kernel/usb/xhci/xhci.c
#include "xhci.h"
#include "../../apic.h"
#include "../../heap.h"
#include "../../klog.h"
#include "../../paging.h"
#include "../../pci.h"
#include "../../sched.h"
#include "../../string.h"
#include "../../uaccess.h"
#include "../usb_dma.h"
#include "../usb_proxy.h"
#include "xhci_ring.h"
#include "xhci_scratch.h"

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

void xhci_delay_ms(uint32_t ms) {
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
        (trb_phys == 0 || g_pending_xfers[i].param == trb_phys)) {
      *out = g_pending_xfers[i];
      g_pending_xfers[i] = g_pending_xfers[--g_pending_xfer_count];
      spin_unlock_irqrestore(&g_xhci.lock, flags);
      return 0;
    }
  }
  spin_unlock_irqrestore(&g_xhci.lock, flags);
  return -1;
}

void xhci_flush_pending_xfers(uint8_t slot, uint8_t ep_id) {
  unsigned long flags = spin_lock_irqsave(&g_xhci.lock);
  for (int i = 0; i < g_pending_xfer_count;) {
    if (XHCI_EVT_SLOT(g_pending_xfers[i]) == slot &&
        XHCI_EVT_EP(g_pending_xfers[i]) == ep_id)
      g_pending_xfers[i] = g_pending_xfers[--g_pending_xfer_count];
    else
      i++;
  }
  spin_unlock_irqrestore(&g_xhci.lock, flags);
}

static bool has_events(void *arg) {
  (void)arg;
  return __atomic_load_n(&g_event_pending, __ATOMIC_ACQUIRE) != 0;
}
static bool has_hotplug(void *arg) {
  (void)arg;
  return __atomic_load_n(&g_hotplug_pending, __ATOMIC_ACQUIRE) != 0;
}

static void xhci_irq_handler_msi(void) {
  __atomic_store_n(&g_event_pending, 1, __ATOMIC_RELEASE);
  wake_up_all(&g_xhci.irq_wq);
}

void xhci_event_thread(void) {
  LOG_INFO("[xHCI] Event thread iniciado (MSI=%d)", g_xhci.using_msi);
  while (1) {
    wait_event_interruptible_timeout(&g_xhci.irq_wq, has_events, NULL, 10);
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
        if (g_pending_cmd_count >= XHCI_MAX_PENDING_CMDS) {
          // Eventos huérfanos (comandos con timeout): descartar el más viejo
          // en vez de perder el nuevo, o todo comando posterior hace timeout.
          LOG_WARN("[xHCI] pending cmd queue full: descartando el más viejo");
          g_pending_cmds[0] = g_pending_cmds[--g_pending_cmd_count];
        }
        g_pending_cmds[g_pending_cmd_count++] = evt;
        wake_up_all_locked(&g_xhci.cmd_wq);
        spin_unlock_irqrestore(&g_xhci.lock, flags);
      } else if (type == XHCI_TRB_TRANSFER_EVT) {
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

          usb_dma_buf_t *src_dma = NULL;
          if (async_urb->dma_buf) {
            src_dma = (usb_dma_buf_t *)async_urb->dma_buf;
          } else if (async_urb->hc_priv) {
            src_dma = (usb_dma_buf_t *)async_urb->hc_priv;
          }

          if (async_urb->is_in && src_dma && src_dma->virt != async_urb->buf &&
              async_urb->actual_len > 0 && async_urb->status == 0) {
            memcpy(async_urb->buf, src_dma->virt,
                   (size_t)async_urb->actual_len);
          }

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

        flags = spin_lock_irqsave(&g_xhci.lock);
        if (g_pending_xfer_count >= XHCI_MAX_PENDING_XFERS) {
          LOG_WARN("[xHCI] pending xfer queue full: descartando el más viejo");
          g_pending_xfers[0] = g_pending_xfers[--g_pending_xfer_count];
        }
        g_pending_xfers[g_pending_xfer_count++] = evt;
        wake_up_all_locked(&g_xhci.xfer_wq);
        spin_unlock_irqrestore(&g_xhci.lock, flags);
      } else if (type == XHCI_TRB_PORT_STATUS_CHG) {
        __atomic_store_n(&g_hotplug_pending, 1, __ATOMIC_RELEASE);
        wake_up_all(&g_xhci.hub_wq);
      }
    }

    uint32_t sts = op_rd(XHCI_OP_USBSTS);
    static int s_fatal_logged;
    if ((sts & (XHCI_USBSTS_HSE | XHCI_USBSTS_HCE)) && !s_fatal_logged) {
      s_fatal_logged = 1;
      LOG_ERR("[xHCI] error fatal del controlador: USBSTS=0x%08x%s%s", sts,
              (sts & XHCI_USBSTS_HSE) ? " HSE" : "",
              (sts & XHCI_USBSTS_HCE) ? " HCE" : "");
    }
    uint32_t rw1c = sts & (XHCI_USBSTS_EINT | XHCI_USBSTS_PCD |
                           XHCI_USBSTS_HSE | XHCI_USBSTS_HCE);
    if (rw1c)
      op_wr(XHCI_OP_USBSTS, rw1c);

    unsigned long flags = spin_lock_irqsave(&g_xhci.lock);
    uint64_t next = xhci_ring_trb_phys(&g_xhci.evt, g_xhci.evt.dequeue);
    spin_unlock_irqrestore(&g_xhci.lock, flags);

    rt_wr(XHCI_RT_IR0_ERDP + 4, (uint32_t)(next >> 32));
    rt_wr(XHCI_RT_IR0_ERDP, (uint32_t)(next & 0xFFFFFFF0u) | XHCI_ERDP_EHB);

    uint32_t iman = *(volatile uint32_t *)(g_xhci.rt + XHCI_RT_IR0_IMAN);
    if (iman & XHCI_IMAN_IP)
      rt_wr(XHCI_RT_IR0_IMAN, (iman & XHCI_IMAN_IE) | XHCI_IMAN_IP);

    flags = spin_lock_irqsave(&g_xhci.lock);
    volatile uint32_t *next_trb =
        (volatile uint32_t *)&g_xhci.evt.trbs[g_xhci.evt.dequeue];
    int more_events = ((next_trb[3] & XHCI_TRB_CYCLE) == g_xhci.evt.cycle);
    spin_unlock_irqrestore(&g_xhci.lock, flags);
    if (more_events)
      __atomic_store_n(&g_event_pending, 1, __ATOMIC_RELEASE);
  }
}

void xhci_hotplug_thread(void) {
  LOG_INFO("[xHCI] Hotplug thread iniciado");
  // Debounce de conexión (USB2 tPDDEBOUNCE ~100 ms) y link training USB3
  // tras alimentar los puertos: sin esperar, CCS/PED aún no son fiables.
  xhci_delay_ms(120);
  while (1) {
    wait_event(&g_xhci.hub_wq, has_hotplug, NULL);
    (void)__atomic_exchange_n(&g_hotplug_pending, 0, __ATOMIC_ACQ_REL);
    LOG_INFO("[xHCI] Hotplug event detectado, re-escaneando root hub");
    xhci_attach_all_devices();
  }
}

static int xhci_map_mmio(void) {
  uint32_t lo = pci_read_config_dword(g_xhci.pci.bus, g_xhci.pci.slot,
                                      g_xhci.pci.func, 0x10);
  if (lo & 0x01)
    return -ENODEV;
  uint64_t phys;
  uint32_t hi = 0;
  if (((lo >> 1) & 0x03) == 0x02) {
    hi = pci_read_config_dword(g_xhci.pci.bus, g_xhci.pci.slot, g_xhci.pci.func,
                               0x14);
    phys = (lo & ~0x0FULL) | ((uint64_t)hi << 32);
  } else {
    phys = lo & ~0x0FULL;
  }
  LOG_INFO("[xHCI] BAR0 raw=0x%08x hi=0x%08x phys=0x%016lx", lo, hi,
           (unsigned long)phys);
  if (phys == 0)
    return -ENODEV;
  g_xhci.mmio_phys = phys;
  g_xhci.mmio_size = 0x10000;
  g_xhci.mmio = (volatile uint8_t *)mmio_map(
      phys, g_xhci.mmio_size, PTE_WRITABLE | PTE_NOCACHE | PTE_NX);
  if (!g_xhci.mmio) {
    LOG_ERR("[xHCI] no se pudo mapear BAR0 phys=0x%016lx size=0x%lx",
            (unsigned long)phys, (unsigned long)g_xhci.mmio_size);
    return -ENOMEM;
  }
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
  if (!(g_xhci.hccparams1 & XHCI_HCC1_AC64))
    LOG_WARN("[xHCI] sin direccionamiento de 64 bits: el DMA debe estar por "
             "debajo de 4 GiB");
  return 0;
}

// Como Linux: apagar los SMI de la BIOS (USBLEGCTLSTS = cap+4) y limpiar sus
// flags RW1C; si no, la BIOS puede seguir interceptando el xHC.
static void xhci_disable_bios_smi(uint32_t off) {
  uint32_t ctl = mmio_rd(off + 4);
  ctl &= ~((0x3u << 1) | (0xFFu << 5) | (0x7u << 17));
  ctl |= (0x7u << 29);
  *(volatile uint32_t *)(g_xhci.mmio + off + 4) = ctl;
}

static int xhci_legacy_handoff(void) {
  uint32_t xecp = (g_xhci.hccparams1 >> 16) & 0xFFFFu;
  if (xecp == 0)
    return 0;

  uint32_t off = xecp * 4u;
  for (uint32_t n = 0; n < 64 && off + 4 <= g_xhci.mmio_size; n++) {
    uint32_t cap = mmio_rd(off);
    uint8_t cap_id = (uint8_t)(cap & 0xFFu);
    uint8_t next = (uint8_t)((cap >> 8) & 0xFFu);

    if (cap_id == 1) {
      const uint32_t bios_owned = 1u << 16;
      const uint32_t os_owned = 1u << 24;
      *(volatile uint32_t *)(g_xhci.mmio + off) = cap | os_owned;
      __asm__ volatile("mfence" ::: "memory");

      if (cap & bios_owned) {
        for (int i = 0; i < 1000; i++) {
          if (!(mmio_rd(off) & bios_owned)) {
            LOG_INFO("[xHCI] firmware relinquished USB ownership");
            xhci_disable_bios_smi(off);
            return 0;
          }
          xhci_delay_ms(1);
        }
        LOG_ERR("[xHCI] timeout esperando USB Legacy Handoff (BIOS-owned)");
        return -ETIMEDOUT;
      }
      LOG_INFO("[xHCI] USB Legacy Support encontrado; OS-owned solicitado");
      xhci_disable_bios_smi(off);
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
    if (!halted)
      return -ETIMEDOUT;
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

static usb_dma_buf_t g_sp_array_raw, g_sp_pages_raw;

static int xhci_setup_scratchpad(void) {
  uint32_t n = xhci_scratch_count(g_xhci.hcsparams2);
  if (n == 0) {
    LOG_INFO("[xHCI] scratchpad: no requerido");
    return 0;
  }
  size_t pg = xhci_page_size(op_rd(0x08 /* PAGESIZE */));

  if (usb_dma_alloc((size_t)n * 8 + 64, &g_sp_array_raw) != 0)
    return -ENOMEM;
  if (usb_dma_alloc((size_t)n * pg + pg, &g_sp_pages_raw) != 0) {
    usb_dma_free(&g_sp_array_raw);
    return -ENOMEM;
  }
  memset(g_sp_array_raw.virt, 0, (size_t)n * 8 + 64);
  memset(g_sp_pages_raw.virt, 0, (size_t)n * pg + pg);

  size_t aoff = xhci_align_off(g_sp_array_raw.phys, 64);
  size_t poff = xhci_align_off(g_sp_pages_raw.phys, pg);
  uint64_t *arr = (uint64_t *)((uint8_t *)g_sp_array_raw.virt + aoff);
  uint64_t arr_phys = g_sp_array_raw.phys + aoff;
  uint64_t pages_phys = g_sp_pages_raw.phys + poff;
  for (uint32_t i = 0; i < n; i++)
    arr[i] = pages_phys + (uint64_t)i * pg;

  g_xhci.dcbaa[0] = arr_phys;
  __asm__ volatile("mfence" ::: "memory");
  LOG_INFO("[xHCI] scratchpad: %u buffers x %u B", n, (unsigned)pg);
  return 0;
}

static int xhci_init_rings(void) {
  int rc = xhci_ring_init(&g_xhci.cmd, 64, 1);
  if (rc != 0)
    return rc;
  rc = xhci_ring_init(&g_xhci.evt, 256, 0);
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
  memset(g_xhci.dcbaa, 0, dcbaa_bytes);

  rc = xhci_setup_scratchpad();
  if (rc != 0) {
    LOG_ERR("[xHCI] scratchpad falló: %d", rc);
    usb_dma_free(&dcbaa_buf);
    usb_dma_free(&erst_buf);
    xhci_ring_destroy(&g_xhci.evt);
    xhci_ring_destroy(&g_xhci.cmd);
    return rc;
  }

  op_wr(XHCI_OP_DCBAAP, (uint32_t)(g_xhci.dcbaa_phys & 0xFFFFFFFF));
  op_wr(XHCI_OP_DCBAAP + 4, (uint32_t)(g_xhci.dcbaa_phys >> 32));
  op_wr(XHCI_OP_CRCR, (uint32_t)(g_xhci.cmd.phys & 0xFFFFFFFF) | XHCI_CRCR_RCS);
  op_wr(XHCI_OP_CRCR + 4, (uint32_t)(g_xhci.cmd.phys >> 32));

  // Orden de la spec (4.2): ERSTSZ, ERDP y por último ERSTBA (su escritura
  // activa el event ring).
  rt_wr(XHCI_RT_IR0_ERSTSZ, 1);
  rt_wr(XHCI_RT_IR0_ERDP + 4, (uint32_t)(g_xhci.evt.phys >> 32));
  rt_wr(XHCI_RT_IR0_ERDP,
        (uint32_t)(g_xhci.evt.phys & 0xFFFFFFF0u) | XHCI_ERDP_EHB);
  rt_wr(XHCI_RT_IR0_ERSTBA, (uint32_t)(g_xhci.erst_phys & 0xFFFFFFFF));
  rt_wr(XHCI_RT_IR0_ERSTBA + 4, (uint32_t)(g_xhci.erst_phys >> 32));

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
    uint32_t val = xhci_portsc_neutral(portsc) | XHCI_PORTSC_PP;
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
  size_t input_ctx_size = (size_t)33 * g_xhci.ctx_size;
  rc = usb_dma_alloc(input_ctx_size, &p->input_ctx);
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
  memset(p->out_ctx.virt, 0, out_ctx_size);
  dev->hc_priv = p;
  return 0;
}

void xhci_hcd_free_device(usb_device_t *dev) {
  if (!dev || !dev->hc_priv)
    return;
  xhci_dev_priv_t *p = (xhci_dev_priv_t *)dev->hc_priv;
  // El slot debe deshabilitarse ANTES de liberar contextos/rings (el HC aún
  // puede escribir en ellos). Así ningún camino de error filtra slots.
  if (p->slot_id != 0 && g_xhci.initialized) {
    xhci_cmd_disable_slot(p->slot_id);
    g_xhci.dcbaa[p->slot_id] = 0;
    __asm__ volatile("mfence" ::: "memory");
  }
  for (int i = 0; i < 30; i++) {
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
  xhci_ep_state_t *st = &p->eps[2 * (ep_num - 1) + dir_in];
  if (st->configured) {
    ep->hc_ep = st;
    return 0;
  }
  memset(st, 0, sizeof(*st));
  int rc = xhci_ring_init(&st->ring, 256, 1);
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
  uint8_t *in = (uint8_t *)p->input_ctx.virt;
  memset(in, 0, p->input_ctx.size);
  xhci_ictx_control(in)->add = XHCI_ICC_ADD_SLOT | (1u << dci);
  // El Slot Context de entrada DEBE partir del estado actual del slot (route,
  // hub/num_ports, TT...). Antes se rellenaba a mano: route=0, sin flag Hub
  // y TT port = slot_id; además CTX_ENT=dci recortaba endpoints ya
  // configurados con un DCI mayor.
  xhci_slot_ctx_t *slot_ctx = xhci_ictx_slot(in, g_xhci.ctx_size);
  memcpy(slot_ctx, p->out_ctx.virt, sizeof(*slot_ctx));
  uint32_t cur_ent = (slot_ctx->dw0 >> 27) & 0x1Fu;
  if (cur_ent < dci)
    cur_ent = dci;
  slot_ctx->dw0 =
      (slot_ctx->dw0 & ~(0x1Fu << 27)) | XHCI_SLOT_DW0_CTX_ENT(cur_ent);

  xhci_ep_ctx_t *ec = xhci_ictx_ep(in, g_xhci.ctx_size, ep_num, dir_in);
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
      sc[sid].param =
          stream_deq | XHCI_STREAM_CTX_SCT_PRIMARY | XHCI_EP_DEQ_DCS;
      sc[sid].status = 0;
      sc[sid].control = 0;
    }
    usb_dma_sync(&st->stream_ctx, USB_DMA_TO_DEVICE);
    st->has_streams = 1;
    st->max_streams = XHCI_UAS_STREAM_COUNT - 1;
    ec->dw0 |= XHCI_EP_DW0_LSA | XHCI_EP_DW0_MAXPSTR(XHCI_UAS_MAX_PSTREAMS);
    ec->dw2 = (uint32_t)(st->stream_ctx.phys & 0xFFFFFFFFu);
    ec->dw3 = (uint32_t)(st->stream_ctx.phys >> 32);
  }

  // Para endpoints periódicos (interrupt), xHCI exige Max ESIT Payload.
  // Si queda a cero, el controlador puede aceptar Configure Endpoint pero no
  // programar correctamente las transferencias del teclado/ratón HID.
  uint32_t max_esit_payload = 0;
  if (ep->type == USB_EP_INTERRUPT) {
    uint32_t interval_field;
    if (p->speed == USB_SPEED_LOW || p->speed == USB_SPEED_FULL) {
      int b = ep->interval;
      if (b < 1)
        b = 1;
      if (b > 255)
        b = 255;
      interval_field = (uint32_t)(xhci_fls((uint32_t)b * 8u) - 1);
      if (interval_field < 3)
        interval_field = 3;
      if (interval_field > 10)
        interval_field = 10;
    } else {
      interval_field = (uint32_t)ep->interval;
      if (interval_field < 1)
        interval_field = 1;
      if (interval_field > 16)
        interval_field = 16;
      interval_field -= 1; // período = 2^(bInterval-1) microframes
    }
    ec->dw0 |= (interval_field & 0xFFu) << 16;

    // Para los endpoints HID usados aquí, el máximo por ESIT es un paquete
    // (wMaxPacketSize). El campo es de 24 bits: 8 en DW0[31:24] y 16 en
    // DW4[31:16].
    max_esit_payload = ep->max_packet;
    ec->dw0 |= ((max_esit_payload >> 16) & 0xFFu) << 24;
  }
  uint32_t avg = (ep->max_packet > 0) ? ep->max_packet : 8;
  if (avg > 1024)
    avg = 1024;
  ec->reserved[0] = (avg & 0xFFFFu) | ((max_esit_payload & 0xFFFFu) << 16);
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
    xhci_cmd_disable_slot(slot);
    usb_device_free(dev);
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

  uint8_t *input_ctx = (uint8_t *)p->input_ctx.virt;
  memset(input_ctx, 0, p->input_ctx.size);
  xhci_ictx_control(input_ctx)->add = XHCI_ICC_ADD_SLOT | XHCI_ICC_ADD_EP0;

  uint32_t route = usb_device_route(dev);
  uint32_t dw2 = 0;
  if (parent_hub != NULL) {
    if ((parent_hub->speed == USB_SPEED_HIGH ||
         parent_hub->speed == USB_SPEED_SUPER ||
         parent_hub->speed == USB_SPEED_SUPER_PLUS) &&
        (speed == USB_SPEED_FULL || speed == USB_SPEED_LOW)) {
      uint8_t tt_hub_slot = parent_hub->addr;
      uint8_t tt_port = port;
      uint8_t ttt = 1;
      dw2 = XHCI_SLOT_DW2_TT_HUB_SLOT(tt_hub_slot) |
            XHCI_SLOT_DW2_TT_PORT(tt_port) | XHCI_SLOT_DW2_TTT(ttt);
      LOG_INFO("[xHCI] slot %u: TT configurado (hub_slot=%u port=%u ttt=%u)",
               slot, tt_hub_slot, tt_port, ttt);
    }
  }

  xhci_slot_ctx_t *slot_ctx = xhci_ictx_slot(input_ctx, g_xhci.ctx_size);
  slot_ctx->dw0 = XHCI_SLOT_DW0_ROUTE(route) |
                  XHCI_SLOT_DW0_SPEED(usb_speed_to_xhci_slot_code(speed)) |
                  XHCI_SLOT_DW0_CTX_ENT(1);
  slot_ctx->dw1 = XHCI_SLOT_DW1_RH_PORT(p->root_hub_port);
  slot_ctx->dw2 = dw2;
  slot_ctx->dw3 = 0;

  xhci_ep_ctx_t *ep0 = xhci_ictx_ep(input_ctx, g_xhci.ctx_size, 0, 0);
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
    usb_device_free(dev); // free_device hace Disable Slot + limpia el DCBAA
    return rc;
  }

  xhci_delay_ms(20); // SET_ADDRESS settle time

  // Full-speed: bMaxPacketSize0 puede ser 8/16/32/64 y no se conoce hasta leer
  // el descriptor. Con el contexto a 8 y un device de 64, el GET_DESCRIPTOR de
  // 18 bytes provocaba Babble (cc=3) y EP0 halteado. Se leen 8 bytes, se
  // ajusta el MPS con Evaluate Context y el resto sigue normal.
  if (speed == USB_SPEED_FULL) {
    uint8_t d8[8] = {0};
    const uint8_t setup[8] = {0x80, 0x06, 0x00, 0x01, 0x00, 0x00, 0x08, 0x00};
    int got = xhci_hcd_control_xfer(dev, setup, d8, 8, 1, 2000);
    if (got >= 8 && d8[7] != 0 && d8[7] != mps) {
      uint8_t *in = (uint8_t *)p->input_ctx.virt;
      memset(in, 0, p->input_ctx.size);
      xhci_ictx_control(in)->add = XHCI_ICC_ADD_EP0;
      xhci_ep_ctx_t *e0 = xhci_ictx_ep(in, g_xhci.ctx_size, 0, 0);
      memcpy(e0, (uint8_t *)p->out_ctx.virt + g_xhci.ctx_size, sizeof(*e0));
      e0->dw1 = (e0->dw1 & 0xFFFFu) | XHCI_EP_DW1_MAX_PKT(d8[7]);
      usb_dma_sync(&p->input_ctx, USB_DMA_TO_DEVICE);
      if (xhci_cmd_evaluate_context(slot, p->input_ctx.phys) == 0) {
        LOG_INFO("[xHCI] slot %u: EP0 MPS %u -> %u", slot, mps, d8[7]);
        dev->max_packet_size_ep0 = d8[7];
      } else {
        LOG_WARN("[xHCI] slot %u: evaluate_context(EP0 MPS=%u) falló", slot,
                 d8[7]);
      }
    } else if (got < 8) {
      LOG_WARN("[xHCI] slot %u: lectura de 8 bytes del device descriptor "
               "falló: %d",
               slot, got);
    }
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
  uint8_t *input_ctx = (uint8_t *)p->input_ctx.virt;
  memset(input_ctx, 0, p->input_ctx.size);
  xhci_ictx_control(input_ctx)->add = XHCI_ICC_ADD_SLOT;
  // Evaluate Context solo evalúa Interrupter Target / Max Exit Latency: el
  // flag Hub y Number of Ports se aplican con Configure Endpoint (Linux:
  // xhci_update_hub_device). Se parte del slot context actual.
  xhci_slot_ctx_t *slot_ctx = xhci_ictx_slot(input_ctx, g_xhci.ctx_size);
  memcpy(slot_ctx, p->out_ctx.virt, sizeof(*slot_ctx));
  slot_ctx->dw0 |= XHCI_SLOT_DW0_HUB;
  slot_ctx->dw1 =
      (slot_ctx->dw1 & ~(0xFFu << 24)) | XHCI_SLOT_DW1_NUM_PORTS(num_ports);
  usb_dma_sync(&p->input_ctx, USB_DMA_TO_DEVICE);
  int rc = xhci_cmd_configure_endpoint(p->slot_id, p->input_ctx.phys);
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

  usb_dma_buf_t dma_local;
  usb_dma_buf_t *dma = NULL;
  int dma_allocated = 0;
  memset(&dma_local, 0, sizeof(dma_local));

  if (urb->dma_buf) {
    dma = urb->dma_buf;
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

  unsigned long flags = spin_lock_irqsave(&g_xhci.lock);
  if (g_async_urb_count >= XHCI_MAX_ASYNC_URBS) {
    spin_unlock_irqrestore(&g_xhci.lock, flags);
    if (dma_allocated)
      usb_dma_free(&dma_local);
    return -ENOMEM;
  }

  // TD partido en límites de 64 KiB; ISP solo en el último TRB (la
  // completion asíncrona se resuelve por el último TRB del TD).
  xhci_td_t td;
  int rc = xhci_td_enqueue(st, ep->max_packet, dma->phys, (uint32_t)urb->len, 0,
                           &td);
  if (rc != 0) {
    spin_unlock_irqrestore(&g_xhci.lock, flags);
    if (dma_allocated)
      usb_dma_free(&dma_local);
    return rc;
  }

  xhci_pending_urb_t *pu = &g_async_urbs[g_async_urb_count++];
  pu->urb = urb;
  pu->trb_phys = td.phys[td.n - 1];
  pu->slot_id = p->slot_id;
  pu->ep_id = (uint8_t)dci;

  __asm__ volatile("mfence" ::: "memory");
  *(volatile uint32_t *)(g_xhci.db + p->slot_id * 4) =
      dci | ((uint32_t)stream_id * 65536u);
  spin_unlock_irqrestore(&g_xhci.lock, flags);

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

int xhci_hcd_bulk_xfer_phys(usb_device_t *dev, usb_endpoint_t *ep,
                            uint64_t phys, void *virt, int len, int is_in,
                            int timeout_ms) {
  (void)is_in;
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

  if (!dir_in) {
    uintptr_t start = (uintptr_t)virt & ~(uintptr_t)63;
    uintptr_t end = ((uintptr_t)virt + (uintptr_t)len + 63) & ~(uintptr_t)63;
    for (uintptr_t a = start; a < end; a += 64)
      __asm__ volatile("clflush (%0)" : : "r"((const void *)a) : "memory");
    __asm__ volatile("mfence" ::: "memory");
  }

  // Trozos de 1 MiB: caben en el ring y cada TD se parte en límites de 64 KiB.
  int total = 0;
  while (total < len) {
    uint32_t n = (uint32_t)(len - total);
    if (n > XHCI_TD_CHUNK)
      n = XHCI_TD_CHUNK;
    uint32_t actual = 0;
    int rc = xhci_td_xfer(p, st, dci, ep->max_packet, stream_id,
                          phys + (uint64_t)total, n, timeout_ms, &actual);
    if (rc != 0)
      return rc;
    total += (int)actual;
    if (actual < n)
      break;
  }

  if (dir_in) {
    uintptr_t flush_base = (uintptr_t)virt & ~(uintptr_t)63;
    uintptr_t flush_end =
        ((uintptr_t)virt + (uintptr_t)len + 63) & ~(uintptr_t)63;
    for (uintptr_t a = flush_base; a < flush_end; a += 64)
      __asm__ volatile("clflush (%0)" : : "r"((const void *)a) : "memory");
    __asm__ volatile("mfence" ::: "memory");
  }
  return total;
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

// Chipsets Intel con EHCI + xHCI (Panther Point 7-series, Lynx Point 8-series,
// Wildcat Point...): hasta que el SO lo pida, el firmware deja los puertos
// USB2 de las tomas compartidas enrutados al EHCI, y un pendrive USB2 no se
// ve desde el xHCI. Como Linux (usb_enable_intel_xhci_ports): activar los
// puertos SuperSpeed (USB3_PSSEN) y pasar los USB2 al xHCI (XUSB2PR) segun
// lo que el chipset declara disponible (USB3PRM / USB2PRM).
#define INTEL_XUSB2PR 0xD0
#define INTEL_USB2PRM 0xD4
#define INTEL_USB3_PSSEN 0xD8
#define INTEL_USB3PRM 0xDC

static void xhci_intel_route_ports(pci_device_t *d) {
  if (d->vendor_id != 0x8086)
    return;
  switch (d->device_id) {
  case 0x1E31: // Panther Point
  case 0x8C31: // Lynx Point
  case 0x9C31: // Lynx Point-LP
  case 0x8CB1: // Wildcat Point
  case 0x9CB1: // Wildcat Point-LP
  case 0x9DED: // Cannon Point-LP (no hace dano si no aplica)
    break;
  default:
    return;
  }
  uint32_t usb3 = pci_read_config_dword(d->bus, d->slot, d->func, INTEL_USB3PRM);
  pci_write_config_dword(d->bus, d->slot, d->func, INTEL_USB3_PSSEN, usb3);
  uint32_t usb2 = pci_read_config_dword(d->bus, d->slot, d->func, INTEL_USB2PRM);
  pci_write_config_dword(d->bus, d->slot, d->func, INTEL_XUSB2PR, usb2);
  LOG_INFO("[xHCI] Intel %04x: puertos enrutados al xHCI (USB3PRM=%08x "
           "USB2PRM=%08x, XUSB2PR=%08x)",
           d->device_id, usb3, usb2,
           pci_read_config_dword(d->bus, d->slot, d->func, INTEL_XUSB2PR));
}

static int xhci_probe_inner(pci_device_t *pci_dev, const pci_device_id_t *id) {
  (void)id;
  if (!pci_dev)
    return -EINVAL;
  if (pci_dev->prog_if != 0x30) {
    LOG_INFO("[xHCI] %02x:%02x.%x: progif=0x%02x != 0x30, no es xHCI",
             pci_dev->bus, pci_dev->slot, pci_dev->func, pci_dev->prog_if);
    return -ENODEV;
  }
  xhci_intel_route_ports(pci_dev);
  if (xhci_user_mode()) {
    LOG_INFO("[xHCI] %02x:%02x.%x: modo usuario activo, cediendo a xhci.aur",
             pci_dev->bus, pci_dev->slot, pci_dev->func);
    return -ENODEV;
  }
  if (g_xhci.initialized) {
    LOG_INFO("[xHCI] %02x:%02x.%x: ya hay xHCI activo en %02x:%02x.%x, "
             "ignorando este controlador",
             pci_dev->bus, pci_dev->slot, pci_dev->func, g_xhci.pci.bus,
             g_xhci.pci.slot, g_xhci.pci.func);
    return -EBUSY;
  }

  memset(&g_xhci, 0, sizeof(g_xhci));
  spin_init(&g_xhci.lock);
  wait_queue_init(&g_xhci.irq_wq);
  wait_queue_init(&g_xhci.cmd_wq);
  wait_queue_init(&g_xhci.xfer_wq);
  wait_queue_init(&g_xhci.hub_wq);

  g_xhci.pci = *pci_dev;
  pci_enable_bus_mastering(&g_xhci.pci);
  pci_enable_io_mem(&g_xhci.pci);

  int rc = xhci_map_mmio();
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

  uint8_t msi_vec = 0x62;
  msi_install_handler(msi_vec, xhci_irq_handler_msi);
  if (pci_enable_msi_any(g_xhci.pci.bus, g_xhci.pci.slot, g_xhci.pci.func,
                         msi_vec, (uint8_t)lapic_get_bsp_id()) == 0) {
    g_xhci.using_msi = 1;
    g_xhci.msi_vector = msi_vec;
    LOG_INFO("[xHCI] MSI preparado en vector 0x%02x", msi_vec);
  } else {
    msi_install_handler(msi_vec, NULL);
    LOG_WARN("[xHCI] MSI no disponible; usando polling del Event Ring");
  }

  g_xhci.initialized = 1;

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

  // [FIX] No enumerar sincrónicamente aquí para no bloquear pci_bus_probe_all.
  // Encolamos el escaneo en el hotplug thread.
  __atomic_store_n(&g_hotplug_pending, 1, __ATOMIC_RELEASE);
  wake_up_all(&g_xhci.hub_wq);

  LOG_INFO("[xHCI] inicializado");
  return 0;
}

static const pci_device_id_t xhci_id_table[] = {{
  .vendor_id = PCI_ANY_ID,
  .device_id = PCI_ANY_ID,
  .subvendor = PCI_ANY_ID,
  .subdevice = PCI_ANY_ID,
  .class_id = 0x0C,
  .subclass_id = 0x03,
  .class_mask = 0xFF,
}, {0}};

static int xhci_probe_generic(struct device *dev) {
return xhci_probe_inner((pci_device_t *)dev->priv,
                        (const pci_device_id_t *)dev->matched_id);
}

struct driver xhci_driver = {
  .name = "xhci",
  .bus = &pci_bus,
  .id_table = xhci_id_table,
  .probe = xhci_probe_generic,
  .remove = NULL,
};

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