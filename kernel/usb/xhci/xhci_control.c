// kernel/usb/xhci/xhci_control.c
#include "../../klog.h"
#include "../../spinlock.h"
#include "../../string.h"
#include "../../uaccess.h"
#include "../../wait.h"
#include "../usb_core.h"
#include "xhci.h"
#include "xhci_ring.h"

static bool has_pending_xfer(void *arg) { return g_pending_xfer_count > 0; }

int xhci_wait_transfer(uint8_t slot, uint8_t ep_id, uint64_t last_trb_phys,
                       uint32_t timeout_ms, uint32_t *out_residual_len,
                       xhci_trb_t *out_event) {
  if (!g_xhci.initialized)
    return -ENODEV;

  extern uint64_t klog_get_tsc_freq(void);
  extern void sched_yield(void);

  uint64_t freq = klog_get_tsc_freq();
  if (freq == 0)
    freq = 2000000000ULL;

  uint64_t max_cycles = (timeout_ms == 0) ? 0 : (freq / 1000) * timeout_ms;

  uint32_t lo, hi;
  __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
  uint64_t t0 = ((uint64_t)hi << 32) | lo;

  int iter = 0;
  while (1) {
    if (max_cycles != 0) {
      __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
      if ((((uint64_t)hi << 32) | lo) - t0 >= max_cycles) {
        LOG_ERR("[xHCI] transfer timeout (slot=%u ep=%u)", slot, ep_id);
        return -ETIMEDOUT;
      }
    }

    xhci_trb_t evt;
    if (xhci_take_pending_xfer(slot, ep_id, last_trb_phys, &evt) == 0) {
      uint32_t cc = XHCI_EVT_CC(evt);
      if (out_residual_len)
        *out_residual_len = evt.status & 0xFFFFFF;
      if (out_event)
        *out_event = evt;
      if (cc == XHCI_CC_SUCCESS || cc == XHCI_CC_SHORT_PACKET)
        return 0;
      LOG_ERR("[xHCI] transfer cc=%u (slot=%u ep=%u)", cc, slot, ep_id);
      return -EIO;
    }

    if ((++iter & 0x3F) == 0)
      sched_yield();
    else
      __asm__ volatile("pause");
  }
}

static void xhci_diag_timeout(xhci_dev_priv_t *p) {
  uint32_t sts = *(volatile uint32_t *)(g_xhci.op + XHCI_OP_USBSTS);
  uint32_t psc = xhci_port_status(p->root_hub_port);
  uint32_t ep0_dw0 = 0;
  if (p->out_ctx.virt)
    ep0_dw0 =
        *(volatile uint32_t *)((uint8_t *)p->out_ctx.virt + g_xhci.ctx_size);
  LOG_ERR(
      "[xHCI] diag slot=%u: USBSTS=0x%08x%s%s PORTSC[%u]=0x%08x "
      "(CCS=%d PED=%d PLS=%u) EP0.state=%u (0=off 1=run 2=halt 3=stop 4=err)",
      p->slot_id, sts, (sts & XHCI_USBSTS_HSE) ? " HSE!" : "",
      (sts & XHCI_USBSTS_HCH) ? " HALTED!" : "", p->root_hub_port, psc,
      !!(psc & XHCI_PORTSC_CCS), !!(psc & XHCI_PORTSC_PED),
      (psc >> XHCI_PORTSC_PLS_SHIFT) & 0xF, ep0_dw0 & 7u);
}

int xhci_cmd_reset_endpoint(uint8_t slot, uint32_t dci) {
  xhci_trb_t trb = {0};
  trb.control = XHCI_TRB_TYPE(XHCI_TRB_RESET_EP) | ((uint32_t)slot << 24) |
                ((uint32_t)dci << 16);

  unsigned long flags = spin_lock_irqsave(&g_xhci.lock);
  uint32_t idx = g_xhci.cmd.enqueue;
  if (idx == g_xhci.cmd.size - 1)
    idx = 0;
  uint64_t phys = xhci_ring_trb_phys(&g_xhci.cmd, idx);
  int rc = xhci_ring_enqueue(&g_xhci.cmd, &trb);
  if (rc == 0) {
    __asm__ volatile("mfence" ::: "memory");
    *(volatile uint32_t *)(g_xhci.db + 0) = 0;
  }
  spin_unlock_irqrestore(&g_xhci.lock, flags);
  return rc;
}

int xhci_cmd_set_tr_dequeue(uint8_t slot, uint32_t dci, uint64_t deq_ptr,
                            int dcs) {
  xhci_trb_t trb = {0};
  trb.param = deq_ptr;
  trb.status = 0;
  trb.control = XHCI_TRB_TYPE(XHCI_TRB_SET_TR_DEQUEUE) |
                ((uint32_t)slot << 24) | ((uint32_t)dci << 16);
  if (dcs)
    trb.param |= 1;

  unsigned long flags = spin_lock_irqsave(&g_xhci.lock);
  uint32_t idx = g_xhci.cmd.enqueue;
  if (idx == g_xhci.cmd.size - 1)
    idx = 0;
  uint64_t phys = xhci_ring_trb_phys(&g_xhci.cmd, idx);
  int rc = xhci_ring_enqueue(&g_xhci.cmd, &trb);
  if (rc == 0) {
    __asm__ volatile("mfence" ::: "memory");
    *(volatile uint32_t *)(g_xhci.db + 0) = 0;
  }
  spin_unlock_irqrestore(&g_xhci.lock, flags);
  return rc;
}

void xhci_recover_ep0(xhci_dev_priv_t *p) {
  LOG_INFO("[xHCI] slot %u: recuperando EP0 tras error", p->slot_id);
  xhci_cmd_reset_endpoint(p->slot_id, 1);
  xhci_delay_ms(10);
  p->ep0_ring.enqueue = 0;
  p->ep0_ring.dequeue = 0;
  p->ep0_ring.cycle = 1;
  uint64_t deq = xhci_ring_trb_phys(&p->ep0_ring, 0) | XHCI_EP_DEQ_DCS;
  xhci_cmd_set_tr_dequeue(p->slot_id, 1, deq, 1);
  xhci_delay_ms(10);
}

int xhci_hcd_control_xfer(usb_device_t *dev, const uint8_t setup[8], void *data,
                          int data_len, int is_in, int timeout_ms) {
  if (!dev || !dev->hc_priv || !setup)
    return -EINVAL;
  xhci_dev_priv_t *p = (xhci_dev_priv_t *)dev->hc_priv;
  if (!p)
    return -EINVAL;
  if (timeout_ms <= 0)
    timeout_ms = 5000;
  int has_data = (data_len > 0 && data != NULL);

  uint64_t setup_val;
  memcpy(&setup_val, setup, 8);
  uint32_t trt = !has_data ? XHCI_SETUP_TRT_NONE
                           : (is_in ? XHCI_SETUP_TRT_IN : XHCI_SETUP_TRT_OUT);

  xhci_trb_t setup_trb = {0};
  setup_trb.param = setup_val;
  setup_trb.status = 8;
  // Setup Stage TRB bits 4:1 are reserved. CH is not valid on this
  // stage: Setup, optional Data, and Status are separate control-stage TDs.
  setup_trb.control =
      XHCI_TRB_TYPE(XHCI_TRB_SETUP_STAGE) | XHCI_TRB_IDT | trt;

  usb_dma_buf_t data_dma = {0};
  if (has_data) {
    if (usb_dma_alloc((size_t)data_len, &data_dma) != 0)
      return -ENOMEM;
    if (!is_in)
      memcpy(data_dma.virt, data, (size_t)data_len);
    usb_dma_sync(&data_dma, is_in ? USB_DMA_FROM_DEVICE : USB_DMA_TO_DEVICE);
  }

  xhci_trb_t data_trb = {0};
  if (has_data) {
    data_trb.param = data_dma.phys;
    data_trb.status = (uint32_t)data_len;
    // This Data Stage TD contains exactly one TRB, so CH must be clear.
    // On a short IN packet, ISP reports the Data Stage residual; the following
    // Status Stage TRB then reports completion of the control request.
    data_trb.control =
        XHCI_TRB_TYPE(XHCI_TRB_DATA_STAGE) |
        (is_in ? (XHCI_TRB_DIR_IN | XHCI_TRB_ISP) : 0);
  }

  int status_dir_in = !has_data ? 1 : (is_in ? 0 : 1);
  xhci_trb_t status_trb = {0};
  status_trb.control = XHCI_TRB_TYPE(XHCI_TRB_STATUS_STAGE) | XHCI_TRB_IOC;
  if (status_dir_in)
    status_trb.control |= XHCI_TRB_DIR_IN;

  uint64_t data_phys = 0;
  uint64_t status_phys = 0;
  int rc;

  unsigned long flags = spin_lock_irqsave(&g_xhci.lock);
  // Predict the physical TRB addresses using the same Link-TRB skip rule as
  // xhci_ring_enqueue(). The ring's enqueue index can currently point at Link.
  uint32_t setup_idx = p->ep0_ring.enqueue;
  if (setup_idx >= p->ep0_ring.size - 1)
    setup_idx = 0;
  uint32_t next_idx = setup_idx + 1;
  if (next_idx >= p->ep0_ring.size - 1)
    next_idx = 0;
  if (has_data) {
    data_phys = xhci_ring_trb_phys(&p->ep0_ring, next_idx);
    next_idx++;
    if (next_idx >= p->ep0_ring.size - 1)
      next_idx = 0;
  }
  status_phys = xhci_ring_trb_phys(&p->ep0_ring, next_idx);

  rc = xhci_ring_enqueue(&p->ep0_ring, &setup_trb);
  if (rc == 0 && has_data)
    rc = xhci_ring_enqueue(&p->ep0_ring, &data_trb);
  if (rc == 0)
    rc = xhci_ring_enqueue(&p->ep0_ring, &status_trb);

  if (rc == 0) {
    __asm__ volatile("mfence" ::: "memory");
    *(volatile uint32_t *)(g_xhci.db + p->slot_id * 4) = 1;
  }
  spin_unlock_irqrestore(&g_xhci.lock, flags);

  if (rc != 0) {
    if (has_data)
      usb_dma_free(&data_dma);
    return rc;
  }

  uint32_t actual = 0;
  xhci_trb_t first_evt = {0};
  // En una lectura IN corta puede llegar primero el evento SHORT_PACKET del
  // Data Stage; después debe completarse el Status Stage.
  int werr = xhci_wait_transfer(p->slot_id, 1, 0, timeout_ms, &actual,
                                &first_evt);

  if (werr == 0) {
    if (has_data && first_evt.param == data_phys) {
      if (XHCI_EVT_CC(first_evt) != XHCI_CC_SHORT_PACKET) {
        LOG_ERR("[xHCI] evento inesperado en Data Stage: cc=%u param=0x%lx",
                XHCI_EVT_CC(first_evt), (unsigned long)first_evt.param);
        werr = -EIO;
      } else {
        // Este residuo corresponde al búfer del Data Stage.
        xhci_trb_t status_evt = {0};
        uint32_t status_residual = 0;
        werr = xhci_wait_transfer(p->slot_id, 1, status_phys, timeout_ms,
                                  &status_residual, &status_evt);
        if (werr == 0 && XHCI_EVT_CC(status_evt) != XHCI_CC_SUCCESS) {
          LOG_ERR("[xHCI] Status Stage terminó con cc=%u",
                  XHCI_EVT_CC(status_evt));
          werr = -EIO;
        }
      }
    } else if (first_evt.param != status_phys ||
               XHCI_EVT_CC(first_evt) != XHCI_CC_SUCCESS) {
      LOG_ERR("[xHCI] evento inesperado en Control TD: cc=%u param=0x%lx "
              "(data=0x%lx status=0x%lx)",
              XHCI_EVT_CC(first_evt), (unsigned long)first_evt.param,
              (unsigned long)data_phys, (unsigned long)status_phys);
      werr = -EIO;
    } else {
      // Si Status es el primer evento, no hubo un evento intermedio de
      // paquete corto: el Data Stage completó la longitud solicitada.
      actual = 0;
    }
  }

  if (werr != 0) {
    xhci_diag_timeout(p);
    xhci_recover_ep0(p);
    if (has_data)
      usb_dma_free(&data_dma);
    return werr;
  }

  int transferred = 0;
  if (has_data) {
    if (first_evt.param == data_phys) {
      if (actual > (uint32_t)data_len)
        actual = (uint32_t)data_len;
      transferred = data_len - (int)actual;
    } else {
      // El contador del Status Stage es de su etapa, no del búfer IN.
      transferred = data_len;
    }
    if (is_in) {
      usb_dma_sync(&data_dma, USB_DMA_FROM_DEVICE);
      memcpy(data, data_dma.virt, (size_t)transferred);
    }
    usb_dma_free(&data_dma);
  }
  return transferred;
}

int xhci_hcd_interrupt_xfer(usb_device_t *dev, usb_endpoint_t *ep, void *buf,
                            int len, int timeout_ms, int *out_actual_len) {
  if (!dev || !dev->hc_priv || !ep || !buf || len <= 0)
    return -EINVAL;
  xhci_dev_priv_t *p = (xhci_dev_priv_t *)dev->hc_priv;
  xhci_ep_state_t *st = (xhci_ep_state_t *)ep->hc_ep;
  if (!st || !st->configured)
    return -ENODEV;
  int dir_in = (ep->addr & 0x80) ? 1 : 0;
  int ep_num = ep->addr & 0x0F;
  uint32_t dci = dir_in ? (2 * ep_num + 1) : (2 * ep_num);

  usb_dma_buf_t dma = {0};
  if (usb_dma_alloc((size_t)len, &dma) != 0)
    return -ENOMEM;
  if (!dir_in && ep->type != USB_EP_INTERRUPT) {
    memcpy(dma.virt, buf, (size_t)len);
    usb_dma_sync(&dma, USB_DMA_TO_DEVICE);
  }

  uint32_t wrap_limit = st->ring.size - 1;
  uint32_t trb_idx = st->ring.enqueue;
  if (trb_idx == wrap_limit)
    trb_idx = 0;
  uint64_t trb_phys = xhci_ring_trb_phys(&st->ring, trb_idx);

  xhci_trb_t trb = {0};
  trb.param = dma.phys;
  trb.status = (uint32_t)len;
  trb.control = XHCI_TRB_TYPE(XHCI_TRB_NORMAL) | XHCI_TRB_ISP | XHCI_TRB_IOC |
                (dir_in ? XHCI_TRB_DIR_IN : 0);

  unsigned long flags = spin_lock_irqsave(&g_xhci.lock);
  int rc = xhci_ring_enqueue(&st->ring, &trb);
  __asm__ volatile("mfence" ::: "memory");
  *(volatile uint32_t *)(g_xhci.db + p->slot_id * 4) = dci;
  spin_unlock_irqrestore(&g_xhci.lock, flags);

  if (rc != 0) {
    usb_dma_free(&dma);
    return rc;
  }

  uint32_t residual = 0;
  int werr = xhci_wait_transfer(p->slot_id, (uint8_t)dci, trb_phys,
                                (uint32_t)timeout_ms, &residual, NULL);
  if (werr == 0 && residual > (uint32_t)len)
    werr = -EIO;
  if (werr == 0) {
    uint32_t actual = (uint32_t)len - residual;
    if (dir_in)
      memcpy(buf, dma.virt, actual);
    if (out_actual_len)
      *out_actual_len = (int)actual;
  }
  usb_dma_free(&dma);
  return werr;
}

int xhci_hcd_bulk_xfer(usb_device_t *dev, usb_endpoint_t *ep, void *buf,
                       int len, int is_in, int timeout_ms) {
  (void)is_in;
  int actual = 0;
  int rc = xhci_hcd_interrupt_xfer(dev, ep, buf, len, timeout_ms, &actual);
  return rc == 0 ? actual : rc;
}