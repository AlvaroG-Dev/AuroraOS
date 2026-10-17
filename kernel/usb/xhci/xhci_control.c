// kernel/usb/xhci/xhci_control.c
//
// Control transfers sobre EP0. Encola setup + data + status en el
// transfer ring de EP0, toca el doorbell del slot, espera el Transfer
// Event con CC=Success o Short Packet.
//
// Referencia: xHCI spec 1.2 §4.11.2.2 y §6.4.1.
//
// NOTAS DE CORRECCIÓN IMPORTANTES:
//   - Setup Stage TRB lleva CH=1 (forma parte del mismo TD que data y
//     status). Sin CH, el HC trata cada TRB como TD independiente y el
//     comportamiento es indefinido (QEMU acaba reportando Ring Underrun
//     en el TRB posterior y nunca emite el SUCCESS del status).
//   - Data Stage TRB lleva CH=1 (siempre hay status después).
//   - Status Stage TRB lleva CH=0 y IOC=1 (es el último del TD y quien
//     dispara el Transfer Event de completitud).
//   - Dirección del status según spec §6.4.1.3:
//       Data IN  → Status OUT (DIR=0)
//       Data OUT → Status IN  (DIR=1)
//       No Data  → Status IN  (DIR=1)

#include "../../klog.h"
#include "../../spinlock.h"
#include "../../string.h"
#include "../../uaccess.h"
#include "../usb_core.h"
#include "xhci.h"
#include "xhci_ring.h"

// ===========================================================================
// Espera síncrona de un Transfer Event.
//
// Filtra por (slot, ep, param == último TRB del TD). El `param` de un
// Transfer Event generado por IOC apunta SIEMPRE al TRB del TD que
// lleva IOC (aquí, el status). Cualquier otro evento con el mismo
// (slot, ep) es ruido del ring (p.ej. Underrun del TRB siguiente,
// o eventos residuales de otro transfer).
// ===========================================================================
// Wait synchronously for a transfer event.
//
// The event ring is shared by all endpoints.  Events consumed by the wrong
// waiter are retained until their owning transfer consumes them.
// ===========================================================================
#define XHCI_PENDING_XFER_EVENTS 32

static xhci_trb_t g_pending_xfer_events[XHCI_PENDING_XFER_EVENTS];
static uint32_t g_pending_xfer_count;

static int xhci_transfer_event_matches(const xhci_trb_t *evt, uint8_t slot,
                                       uint8_t ep_id, uint64_t last_trb_phys) {
  return XHCI_TRB_GET_TYPE(*evt) == XHCI_TRB_TRANSFER_EVT &&
         XHCI_EVT_SLOT(*evt) == slot && XHCI_EVT_EP(*evt) == ep_id &&
         evt->param == last_trb_phys;
}

// Must be called with g_xhci.lock held.
static int xhci_take_pending_transfer_event(uint8_t slot, uint8_t ep_id,
                                            uint64_t last_trb_phys,
                                            xhci_trb_t *out_evt) {
  for (uint32_t i = 0; i < g_pending_xfer_count; i++) {
    if (!xhci_transfer_event_matches(&g_pending_xfer_events[i], slot, ep_id,
                                     last_trb_phys))
      continue;
    *out_evt = g_pending_xfer_events[i];
    g_pending_xfer_events[i] = g_pending_xfer_events[--g_pending_xfer_count];
    return 0;
  }
  return -ENOENT;
}

// Must be called with g_xhci.lock held.
static void xhci_save_pending_transfer_event(const xhci_trb_t *evt) {
  if (g_pending_xfer_count >= XHCI_PENDING_XFER_EVENTS) {
    LOG_ERR("[xHCI] pending transfer event queue full");
    return;
  }
  g_pending_xfer_events[g_pending_xfer_count++] = *evt;
}

static int xhci_wait_transfer(uint8_t slot, uint8_t ep_id,
                              uint64_t last_trb_phys, uint32_t timeout_ms,
                              uint32_t *out_residual_len) {
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
    unsigned long flags = spin_lock_irqsave(&g_xhci.lock);
    int from_pending = xhci_take_pending_transfer_event(
        slot, ep_id, last_trb_phys, &evt) == 0;
    int rc = from_pending ? 0 : xhci_ring_dequeue(&g_xhci.evt, &evt);
    if (rc == 0 && !from_pending) {
      uint64_t next = xhci_ring_trb_phys(&g_xhci.evt, g_xhci.evt.dequeue);
      *(volatile uint32_t *)(g_xhci.rt + XHCI_RT_IR0_ERDP) =
          (uint32_t)(next & 0xFFFFFFFF);
      *(volatile uint32_t *)(g_xhci.rt + XHCI_RT_IR0_ERDP + 4) =
          (uint32_t)(next >> 32);
    }
    spin_unlock_irqrestore(&g_xhci.lock, flags);

    if (rc != 0) {
      if ((++iter & 0x3F) == 0)
        sched_yield();
      else
        __asm__ volatile("pause");
      continue;
    }

    uint8_t type = XHCI_TRB_GET_TYPE(evt);
    if (type == XHCI_TRB_PORT_STATUS_CHG) {
      LOG_DEBUG("[xHCI] PSCh (port=%u)", (uint32_t)(evt.param >> 24));
      continue;
    }
    if (type == XHCI_TRB_CMD_COMPLETION)
      continue;
    if (type != XHCI_TRB_TRANSFER_EVT) {
      LOG_TRACE("[xHCI] unexpected event type=%u", type);
      continue;
    }
    if (!xhci_transfer_event_matches(&evt, slot, ep_id, last_trb_phys)) {
      flags = spin_lock_irqsave(&g_xhci.lock);
      xhci_save_pending_transfer_event(&evt);
      spin_unlock_irqrestore(&g_xhci.lock, flags);
      continue;
    }

    uint32_t cc = XHCI_EVT_CC(evt);
    uint32_t residual = evt.status & 0xFFFFFF;
    if (cc == XHCI_CC_SUCCESS || cc == XHCI_CC_SHORT_PACKET) {
      if (out_residual_len)
        *out_residual_len = residual;
      return 0;
    }
    LOG_ERR("[xHCI] transfer cc=%u (slot=%u ep=%u)", cc, slot, ep_id);
    return -EIO;
  }
}

static void xhci_debug_ep0(xhci_dev_priv_t *p) {
  uint32_t sts = *(volatile uint32_t *)(g_xhci.op + XHCI_OP_USBSTS);
  uint32_t *ep = (uint32_t *)((uint8_t *)p->out_ctx.virt + g_xhci.ctx_size);
  LOG_ERR("[xHCI-DBG] USBSTS=0x%08x EP0state=%u deq=0x%08x%08x "
          "ring.enq=%u ring.cyc=%u",
          sts, ep[0] & 7, ep[3], ep[2] & ~0xFu, p->ep0_ring.enqueue,
          p->ep0_ring.cycle);
  for (uint32_t i = 0; i < p->ep0_ring.size; i++) {
    xhci_trb_t *t = &p->ep0_ring.trbs[i];
    LOG_ERR("[xHCI-DBG]  trb[%u] %08x%08x %08x %08x", i,
            (uint32_t)(t->param >> 32), (uint32_t)t->param, t->status,
            t->control);
  }
}

// ===========================================================================
// Control transfer sobre EP0.
//
// Estructura del TD (spec xHCI 1.2 §4.11.2.2 y §6.4.1):
//   Setup TRB:  CH=1, IDT=1, DIR=dir(data), TRT en status
//   Data TRB:   CH=1, DIR=dir(data), SIN ISP (solo interrupt/bulk lo usan)
//   Status TRB: CH=0, IOC=1, DIR=opuesto al data (IN si no hay data)
//
// ISP en el data TRB de un control transfer confunde a QEMU: puede
// interpretar la TD como "completada por short packet" y saltarse el
// status, provocando Ring Underrun en el siguiente TRB.
// ===========================================================================
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

  // ------------------------------------------------------------------
  // 1. Setup TRB.
  // ------------------------------------------------------------------
  uint64_t setup_val;
  memcpy(&setup_val, setup, 8);

  uint32_t trt = !has_data ? XHCI_SETUP_TRT_NONE
                           : (is_in ? XHCI_SETUP_TRT_IN : XHCI_SETUP_TRT_OUT);

  // Setup Stage TRB (xHCI 1.2 §6.4.1.2.1):
  //   DW2: TRB Transfer Length [16:0] = 8 (obligatorio)
  //   DW3: TRT [17:16] (junto a IDT, TYPE, cycle). NO es el bit DIR.
  xhci_trb_t setup_trb = {0};
  setup_trb.param = setup_val;
  setup_trb.status = 8;
  setup_trb.control = XHCI_TRB_TYPE(XHCI_TRB_SETUP_STAGE) | XHCI_TRB_IDT | trt;

  // ------------------------------------------------------------------
  // 2. Data TRB. CH=1 (siempre hay status detrás). SIN ISP.
  // ------------------------------------------------------------------
  usb_dma_buf_t data_dma = {0};
  if (has_data) {
    if (usb_dma_alloc((size_t)data_len, &data_dma) != 0)
      return -ENOMEM;
  }

  xhci_trb_t data_trb = {0};
  if (has_data) {
    data_trb.param = data_dma.phys;
    data_trb.status = (uint32_t)data_len;
    data_trb.control = XHCI_TRB_TYPE(XHCI_TRB_DATA_STAGE) | XHCI_TRB_CH |
                       (is_in ? XHCI_TRB_DIR_IN : 0);
  }

  // ------------------------------------------------------------------
  // 3. Status TRB. CH=0 (último del TD). IOC=1. DIR opuesto al data.
  // ------------------------------------------------------------------
  int status_dir_in = !has_data ? 1 : (is_in ? 0 : 1);

  xhci_trb_t status_trb = {0};
  status_trb.control = XHCI_TRB_TYPE(XHCI_TRB_STATUS_STAGE) | XHCI_TRB_IOC;
  if (status_dir_in)
    status_trb.control |= XHCI_TRB_DIR_IN;

  // ------------------------------------------------------------------
  // 4. Dirección física del status TRB ANTES de encolar.
  // ------------------------------------------------------------------
  // Se encolan 2 TRBs (setup + status) o 3 (setup + data + status). Se
  // simula el enqueue (con el salto del Link TRB) para saber dónde caerá
  // el status.
  uint32_t n_trbs = has_data ? 3 : 2;
  uint32_t status_idx = p->ep0_ring.enqueue;
  for (uint32_t i = 0; i < n_trbs; i++) {
    if (status_idx == p->ep0_ring.size - 1)
      status_idx = 0; // el Link TRB no es usable: el enqueue real salta a 0
    if (i + 1 < n_trbs)
      status_idx++;
  }
  uint64_t status_phys = xhci_ring_trb_phys(&p->ep0_ring, status_idx);

  // ------------------------------------------------------------------
  // 5. Encolar TRBs y ring doorbell, bajo lock.
  // ------------------------------------------------------------------
  unsigned long flags = spin_lock_irqsave(&g_xhci.lock);

  int rc = xhci_ring_enqueue(&p->ep0_ring, &setup_trb);
  if (rc == 0 && has_data)
    rc = xhci_ring_enqueue(&p->ep0_ring, &data_trb);
  if (rc == 0)
    rc = xhci_ring_enqueue(&p->ep0_ring, &status_trb);

  __asm__ volatile("mfence" ::: "memory");
  *(volatile uint32_t *)(g_xhci.db + p->slot_id * 4) = 1;

  spin_unlock_irqrestore(&g_xhci.lock, flags);

  if (rc != 0) {
    if (has_data)
      usb_dma_free(&data_dma);
    return rc;
  }

  // ------------------------------------------------------------------
  // 6. Esperar Transfer Event del status TRB.
  // ------------------------------------------------------------------
  uint32_t actual = 0;
  int werr = xhci_wait_transfer(p->slot_id, 1 /* EP0 */, status_phys,
                                timeout_ms, &actual);
  // El Transfer Event que esperamos es el del Status Stage (único con IOC):
  // su campo de longitud es el residuo de ESE TRB (0 bytes), no los bytes
  // recibidos en el Data Stage. Como el buffer DMA está a cero, copiar
  // data_len es seguro aunque el dispositivo haya enviado menos (short).
  if (werr == 0)
    actual = has_data ? (uint32_t)data_len : 0;

  if (werr == -ETIMEDOUT)
    xhci_debug_ep0(p);

  // ------------------------------------------------------------------
  // 7. Copiar datos de vuelta al llamante (device-to-host).
  // ------------------------------------------------------------------
  if (werr == 0 && has_data && is_in) {
    uint32_t to_copy =
        actual < (uint32_t)data_len ? actual : (uint32_t)data_len;
    memcpy(data, data_dma.virt, to_copy);
  }

  if (werr == 0) {
    LOG_DEBUG("[xHCI] control OK slot=%u %s len=%d actual=%u", p->slot_id,
              is_in ? "IN" : "OUT", data_len, actual);
  }

  if (has_data)
    usb_dma_free(&data_dma);

  return werr;
}

// ===========================================================================
// Interrupt/bulk transfer síncrono (Fase 2).
//
// Encola un único TRB (normal o interrupt, según el endpoint) con
// IOC=1, ringuea el doorbell del slot (target = DCI del endpoint), y
// espera el Transfer Event.
//
// `timeout_ms == 0` = infinito (para HID: dormimos hasta que llegue
// un report). El kthread del HID lo usará así.
// ===========================================================================
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

  // DCI (Device Context Index) para el doorbell: EP_IN_n DCI = 2n+1,
  // EP_OUT_n DCI = 2n. EP0 DCI = 1.
  uint32_t dci = dir_in ? (2 * ep_num + 1) : (2 * ep_num);

  // DMA buffer para el llamante.
  usb_dma_buf_t dma = {0};
  if (usb_dma_alloc((size_t)len, &dma) != 0)
    return -ENOMEM;

  if (!dir_in && ep->type != USB_EP_INTERRUPT) {
    // OUT: copiar el payload al buffer DMA antes de submit.
    memcpy(dma.virt, buf, (size_t)len);
    usb_dma_sync(&dma, USB_DMA_TO_DEVICE);
  }

  // Calcular la dirección física del TRB antes del enqueue.
  uint32_t wrap_limit = st->ring.size - 1;
  uint32_t trb_idx = st->ring.enqueue;
  if (trb_idx == wrap_limit)
    trb_idx = 0;
  uint64_t trb_phys = xhci_ring_trb_phys(&st->ring, trb_idx);

  // Construir TRB normal (usado tanto por interrupt como bulk).
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

  // timeout_ms == 0 means an infinite wait.
  uint32_t residual = 0;
  int werr = xhci_wait_transfer(p->slot_id, (uint8_t)dci, trb_phys,
                                (uint32_t)timeout_ms, &residual);

  if (werr == 0 && residual > (uint32_t)len) {
    LOG_ERR("[xHCI] invalid residual %u (len=%d, slot=%u ep=%u)",
            residual, len, p->slot_id, dci);
    werr = -EIO;
  }

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