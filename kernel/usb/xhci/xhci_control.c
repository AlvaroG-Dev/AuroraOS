// kernel/usb/xhci/xhci_control.c
#include "../../klog.h"
#include "../../spinlock.h"
#include "../../string.h"
#include "../../uaccess.h"
#include "../../wait.h"
#include "../usb_core.h"
#include "xhci.h"
#include "xhci_ring.h"

extern void sched_yield(void);

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

// ===========================================================================
// TD de bulk/interrupt (Normal TRBs encadenados)
// ===========================================================================
int xhci_td_enqueue(xhci_ep_state_t *st, uint16_t mps, uint64_t phys,
                    uint32_t len, int isp_all, xhci_td_t *td) {
  if (!st || !td || len == 0 || !st->ring.is_producer)
    return -EINVAL;
  if (mps == 0)
    mps = 8;
  uint32_t segs = (uint32_t)(((phys & 0xFFFFu) + len + 0xFFFFu) >> 16);
  if (segs > XHCI_TD_MAX_TRBS || segs + 2 > st->ring.size)
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
      uint32_t rem;
      if (g_xhci.hciversion < 0x100)
        rem = (len - (done + chunk)) >> 10;
      else
        rem = total_pk - (done + chunk) / mps;
      td_size = rem > 31 ? 31 : rem;
    }

    xhci_trb_t trb = {0};
    trb.param = addr;
    trb.status = chunk | (td_size << 17);
    trb.control = XHCI_TRB_TYPE(XHCI_TRB_NORMAL) | XHCI_TRB_ISP |
                  (last ? XHCI_TRB_IOC : XHCI_TRB_CH);
    if (!last && !isp_all)
      trb.control &= ~XHCI_TRB_ISP;
    int rc = xhci_ring_enqueue(&st->ring, &trb);
    if (rc != 0)
      return rc;
    td->phys[n] = xhci_ring_trb_phys(&st->ring, st->ring.enqueue - 1);
    td->len[n] = chunk;
    n++;
    done += chunk;
  }
  td->n = n;
  td->total = len;
  return 0;
}

// Espera el final de un TD. Un paquete corto en un TRB intermedio genera un
// evento Short Packet para ESE TRB y luego otro (IOC) para el último.
static int xhci_wait_td(uint8_t slot, uint8_t dci, const xhci_td_t *td,
                        uint32_t timeout_ms, uint32_t *out_actual) {
  if (!g_xhci.initialized)
    return -ENODEV;
  extern uint64_t klog_get_tsc_freq(void);
  uint64_t freq = klog_get_tsc_freq();
  if (freq == 0)
    freq = 2000000000ULL;
  uint64_t max_cycles = (timeout_ms == 0) ? 0 : (freq / 1000) * timeout_ms;
  uint64_t short_grace = (freq / 1000) * 50;
  uint32_t lo, hi;
  __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
  uint64_t t0 = ((uint64_t)hi << 32) | lo;
  uint64_t t_short = 0;

  uint32_t actual = td->total;
  int short_seen = 0;
  int iter = 0;
  while (1) {
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    uint64_t now = ((uint64_t)hi << 32) | lo;
    if (short_seen && now - t_short >= short_grace) {
      *out_actual = actual;
      return 0;
    }
    if (max_cycles != 0 && now - t0 >= max_cycles) {
      if (short_seen) {
        *out_actual = actual;
        return 0;
      }
      LOG_ERR("[xHCI] transfer timeout (slot=%u ep=%u)", slot, dci);
      return -ETIMEDOUT;
    }

    xhci_trb_t evt;
    if (xhci_take_pending_xfer(slot, dci, 0, &evt) == 0) {
      int idx = -1;
      for (int i = 0; i < td->n; i++) {
        if (td->phys[i] == evt.param) {
          idx = i;
          break;
        }
      }
      if (idx < 0)
        continue; // evento viejo de otro TD
      uint32_t cc = XHCI_EVT_CC(evt);
      uint32_t res = evt.status & 0xFFFFFF;
      if (cc == XHCI_CC_SUCCESS) {
        if (idx == td->n - 1) {
          *out_actual = short_seen ? actual : td->total;
          return 0;
        }
        continue;
      }
      if (cc == XHCI_CC_SHORT_PACKET) {
        uint32_t before = 0;
        for (int i = 0; i < idx; i++)
          before += td->len[i];
        if (res > td->len[idx])
          res = td->len[idx];
        if (!short_seen)
          actual = before + td->len[idx] - res;
        if (idx == td->n - 1) {
          *out_actual = actual;
          return 0;
        }
        short_seen = 1;
        t_short = now;
        continue;
      }
      LOG_ERR("[xHCI] transfer cc=%u (slot=%u ep=%u)", cc, slot, dci);
      return -EIO;
    }
    if ((++iter & 0x3F) == 0)
      sched_yield();
    else
      __asm__ volatile("pause");
  }
}

int xhci_td_xfer(xhci_dev_priv_t *p, xhci_ep_state_t *st, uint32_t dci,
                 uint16_t mps, uint16_t stream_id, uint64_t phys, uint32_t len,
                 int timeout_ms, uint32_t *out_actual) {
  xhci_td_t td;
  uint32_t actual = 0;
  unsigned long flags = spin_lock_irqsave(&g_xhci.lock);
  int rc = xhci_td_enqueue(st, mps, phys, len, 1, &td);
  if (rc == 0) {
    __asm__ volatile("mfence" ::: "memory");
    *(volatile uint32_t *)(g_xhci.db + p->slot_id * 4) =
        dci | ((uint32_t)stream_id << 16);
  }
  spin_unlock_irqrestore(&g_xhci.lock, flags);
  if (rc != 0)
    return rc;

  int werr = xhci_wait_td(p->slot_id, (uint8_t)dci, &td,
                          (uint32_t)(timeout_ms < 0 ? 0 : timeout_ms), &actual);
  if (werr != 0) {
    if (st->has_streams)
      xhci_cmd_reset_endpoint(p->slot_id, dci); // anillos por stream: ver notas
    else
      xhci_recover_endpoint(p, dci, &st->ring);
    return werr;
  }
  if (out_actual)
    *out_actual = actual;
  return 0;
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

static uint32_t xhci_ep_state(xhci_dev_priv_t *p, uint32_t dci) {
  if (!p->out_ctx.virt)
    return XHCI_EP_STATE_DISABLED;
  __asm__ volatile("mfence" ::: "memory");
  volatile uint32_t *ep = (volatile uint32_t *)((uint8_t *)p->out_ctx.virt +
                                                (size_t)dci * g_xhci.ctx_size);
  return ep[0] & 7u;
}

// Recuperación completa de un endpoint tras error/timeout:
//   Running -> Stop Endpoint; Halted/Error -> Reset Endpoint;
//   después se vacía el ring y se reprograma el dequeue pointer.
// Todos los comandos esperan su evento (antes eran fire-and-forget y sus
// completions llenaban la cola de comandos pendientes).
void xhci_recover_endpoint(xhci_dev_priv_t *p, uint32_t dci,
                           xhci_ring_t *ring) {
  uint32_t st = xhci_ep_state(p, dci);
  LOG_INFO("[xHCI] slot %u dci %u: recuperando endpoint (state=%u)", p->slot_id,
           dci, st);
  if (st == XHCI_EP_STATE_RUNNING)
    xhci_cmd_stop_endpoint(p->slot_id, dci);
  else if (st == XHCI_EP_STATE_HALTED || st == XHCI_EP_STATE_ERROR)
    xhci_cmd_reset_endpoint(p->slot_id, dci);

  xhci_ring_reset(ring);
  uint64_t deq = xhci_ring_trb_phys(ring, 0);
  int rc = xhci_cmd_set_tr_dequeue(p->slot_id, dci, deq, 1);
  if (rc != 0)
    LOG_WARN("[xHCI] slot %u dci %u: set_tr_dequeue falló: %d", p->slot_id, dci,
             rc);
  xhci_flush_pending_xfers(p->slot_id, (uint8_t)dci);
}

void xhci_recover_ep0(xhci_dev_priv_t *p) {
  xhci_recover_endpoint(p, 1, &p->ep0_ring);
}

static int xhci_control_xfer_locked(usb_device_t *dev, const uint8_t setup[8],
                                    void *data, int data_len, int is_in,
                                    int timeout_ms) {
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
  setup_trb.control = XHCI_TRB_TYPE(XHCI_TRB_SETUP_STAGE) | XHCI_TRB_IDT | trt;

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
    data_trb.control = XHCI_TRB_TYPE(XHCI_TRB_DATA_STAGE) |
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

  // Descartar eventos viejos de un TD anterior fallido/timeout de este EP0.
  xhci_flush_pending_xfers(p->slot_id, 1);

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
  int werr =
      xhci_wait_transfer(p->slot_id, 1, 0, timeout_ms, &actual, &first_evt);

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

int xhci_hcd_control_xfer(usb_device_t *dev, const uint8_t setup[8], void *data,
                          int data_len, int is_in, int timeout_ms) {
  if (!dev || !dev->hc_priv || !setup)
    return -EINVAL;
  xhci_dev_priv_t *p = (xhci_dev_priv_t *)dev->hc_priv;
  // EP0 solo admite un control transfer a la vez: hub kthread, enumeración y
  // HID pueden coincidir sobre el mismo device.
  while (__atomic_exchange_n(&p->ctrl_busy, 1, __ATOMIC_ACQUIRE))
    sched_yield();
  int rc =
      xhci_control_xfer_locked(dev, setup, data, data_len, is_in, timeout_ms);
  __atomic_store_n(&p->ctrl_busy, 0, __ATOMIC_RELEASE);
  return rc;
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

  // Transferencias grandes de bulk: trozos de 1 MiB (múltiplo de cualquier
  // MPS).
  if (ep->type != USB_EP_INTERRUPT && (uint32_t)len > XHCI_TD_CHUNK) {
    int total = 0;
    while (total < len) {
      int n = len - total;
      if (n > (int)XHCI_TD_CHUNK)
        n = (int)XHCI_TD_CHUNK;
      int act = 0;
      int rc = xhci_hcd_interrupt_xfer(dev, ep, (uint8_t *)buf + total, n,
                                       timeout_ms, &act);
      if (rc != 0)
        return rc;
      total += act;
      if (act < n)
        break;
    }
    if (out_actual_len)
      *out_actual_len = total;
    return 0;
  }

  // Un interrupt IN debe pedir SIEMPRE un múltiplo de wMaxPacketSize: si el
  // dispositivo envía un informe más largo que el TRB (p.ej. ratón con varios
  // report IDs) el xHC devuelve Babble Detected (cc=3) y HALTEA el endpoint.
  uint32_t trb_len = (uint32_t)len;
  if (dir_in && ep->type == USB_EP_INTERRUPT && ep->max_packet > 0) {
    uint32_t mps = ep->max_packet;
    if (trb_len < mps)
      trb_len = mps;
    trb_len = ((trb_len + mps - 1) / mps) * mps;
  }

  usb_dma_buf_t dma = {0};
  if (usb_dma_alloc((size_t)trb_len, &dma) != 0)
    return -ENOMEM;
  if (!dir_in) {
    memcpy(dma.virt, buf, (size_t)len);
    usb_dma_sync(&dma, USB_DMA_TO_DEVICE);
  }

  uint32_t actual = 0;
  int werr = xhci_td_xfer(p, st, dci, ep->max_packet, 0, dma.phys, trb_len,
                          timeout_ms, &actual);
  if (werr == 0) {
    if (actual > (uint32_t)len)
      actual = (uint32_t)len;
    if (dir_in) {
      usb_dma_sync(&dma, USB_DMA_FROM_DEVICE);
      memcpy(buf, dma.virt, actual);
    }
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