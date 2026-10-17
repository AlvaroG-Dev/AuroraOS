// kernel/usb/xhci/xhci_cmd.c
//
// Comandos xHCI: encolar en el command ring, ring doorbell 0, poll
// del event ring hasta ver el completion correspondiente.

#include "../../klog.h"
#include "../../spinlock.h"
#include "../../uaccess.h"
#include "xhci.h"
#include "xhci_ring.h"

// ===========================================================================
// Espera síncrona de un comando. El llamante rellena `trb`; nosotros
// añadimos IOC, encolamos, ringueamos doorbell 0, y polleamos el event
// ring hasta ver el completion cuyo `param` coincide con la dirección
// física del TRB que enqueuamos.
//
// Devuelve 0 en éxito y rellena `out_evt` (el evento completo) si != NULL.
// Devuelve -ETIMEDOUT si no llega el completion. Devuelve el completion
// code en `out_evt` para que el llamante lo inspeccione.
// ===========================================================================
static int xhci_cmd_wait(xhci_trb_t *trb, uint32_t timeout_ms,
                         xhci_trb_t *out_evt) {
  if (!g_xhci.initialized || !trb)
    return -EINVAL;

  unsigned long flags = spin_lock_irqsave(&g_xhci.lock);

  // Dirección física del TRB que vamos a escribir. Si el ring está
  // a punto de wrap, write_idx=0 (el enqueue real hará el wrap).
  uint32_t write_idx = g_xhci.cmd.enqueue;
  if (write_idx == g_xhci.cmd.size - 1)
    write_idx = 0;
  uint64_t cmd_trb_phys = xhci_ring_trb_phys(&g_xhci.cmd, write_idx);

  int rc = xhci_ring_enqueue(&g_xhci.cmd, trb);
  if (rc != 0) {
    spin_unlock_irqrestore(&g_xhci.lock, flags);
    return rc;
  }

  __asm__ volatile("mfence" ::: "memory");
  *(volatile uint32_t *)g_xhci.db = 0; // doorbell 0 = command ring

  // Timeout por TSC (no depende de IRQs)
  extern uint64_t klog_get_tsc_freq(void);
  uint64_t freq = klog_get_tsc_freq();
  if (freq == 0)
    freq = 2000000000ULL;
  uint64_t max_cycles = (freq / 1000) * timeout_ms;
  uint32_t lo, hi;
  __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
  uint64_t t0 = ((uint64_t)hi << 32) | lo;

  int ret = -ETIMEDOUT;
  while (1) {
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    if ((((uint64_t)hi << 32) | lo) - t0 >= max_cycles)
      break;

    xhci_trb_t evt;
    if (xhci_ring_dequeue(&g_xhci.evt, &evt) != 0) {
      for (volatile int i = 0; i < 100; i++)
        __asm__ volatile("pause");
      continue;
    }

    // Actualizar ERDP
    uint64_t next = xhci_ring_trb_phys(&g_xhci.evt, g_xhci.evt.dequeue);
    uint32_t e_lo = (uint32_t)(next & 0xFFFFFFFF);
    uint32_t e_hi = (uint32_t)(next >> 32);
    *(volatile uint32_t *)(g_xhci.rt + XHCI_RT_IR0_ERDP) = e_lo;
    *(volatile uint32_t *)(g_xhci.rt + XHCI_RT_IR0_ERDP + 4) = e_hi;

    uint8_t type = XHCI_TRB_GET_TYPE(evt);
    if (type == XHCI_TRB_CMD_COMPLETION) {
      if (evt.param == cmd_trb_phys) {
        if (out_evt)
          *out_evt = evt;
        ret = 0;
        break;
      }
      LOG_WARN("[xHCI] cmd completion huérfana (slot=%u cc=%u)",
               XHCI_EVT_SLOT(evt), XHCI_EVT_CC(evt));
    } else if (type == XHCI_TRB_PORT_STATUS_CHG) {
      LOG_DEBUG("[xHCI] PSCh (port=%u)", (uint32_t)(evt.param >> 24));
    } else if (type == XHCI_TRB_TRANSFER_EVT) {
      LOG_DEBUG("[xHCI] transfer event (slot=%u ep=%u cc=%u)",
                XHCI_EVT_SLOT(evt), XHCI_EVT_EP(evt), XHCI_EVT_CC(evt));
    } else {
      LOG_DEBUG("[xHCI] evento desconocido type=%u", type);
    }
  }

  spin_unlock_irqrestore(&g_xhci.lock, flags);
  return ret;
}

// ===========================================================================
// Comandos públicos
// ===========================================================================
int xhci_cmd_noop(void) {
  xhci_trb_t trb = {0};
  trb.control = XHCI_TRB_TYPE(XHCI_TRB_NO_OP_CMD) | XHCI_TRB_IOC;

  xhci_trb_t evt;
  int rc = xhci_cmd_wait(&trb, 2000, &evt);
  if (rc != 0)
    return rc;

  uint32_t cc = XHCI_EVT_CC(evt);
  if (cc != XHCI_CC_SUCCESS) {
    LOG_ERR("[xHCI] no-op completion code=%u", cc);
    return -EIO;
  }
  return 0;
}

int xhci_cmd_enable_slot(uint8_t *slot_id) {
  if (!slot_id)
    return -EINVAL;

  xhci_trb_t trb = {0};
  trb.control = XHCI_TRB_TYPE(XHCI_TRB_ENABLE_SLOT) | XHCI_TRB_IOC;

  xhci_trb_t evt;
  int rc = xhci_cmd_wait(&trb, 5000, &evt);
  if (rc != 0)
    return rc;

  uint32_t cc = XHCI_EVT_CC(evt);
  if (cc != XHCI_CC_SUCCESS) {
    LOG_ERR("[xHCI] enable_slot cc=%u", cc);
    return -EIO;
  }
  *slot_id = XHCI_EVT_SLOT(evt);
  return 0;
}

int xhci_cmd_disable_slot(uint8_t slot_id) {
  xhci_trb_t trb = {0};
  trb.control = XHCI_TRB_TYPE(XHCI_TRB_DISABLE_SLOT) |
                ((uint32_t)slot_id << 24) | XHCI_TRB_IOC;

  xhci_trb_t evt;
  int rc = xhci_cmd_wait(&trb, 5000, &evt);
  if (rc != 0)
    return rc;

  uint32_t cc = XHCI_EVT_CC(evt);
  if (cc != XHCI_CC_SUCCESS) {
    LOG_WARN("[xHCI] disable_slot(%u) cc=%u", slot_id, cc);
    return -EIO;
  }
  return 0;
}

// ===========================================================================
// Address Device (Fase 3)
// ===========================================================================
int xhci_cmd_address_device(uint8_t slot, uint8_t port, usb_speed_t speed,
                            uint16_t ep0_mps, uint64_t input_ctx_phys) {
  if (slot == 0 || slot > g_xhci.max_slots || port == 0)
    return -EINVAL;
  (void)speed;
  (void)ep0_mps;

  xhci_trb_t trb = {0};
  trb.param = input_ctx_phys;
  trb.control = XHCI_TRB_TYPE(XHCI_TRB_ADDRESS_DEVICE) |
                ((uint32_t)slot << 24) | XHCI_TRB_IOC;

  xhci_trb_t evt;
  int rc = xhci_cmd_wait(&trb, 5000, &evt);
  if (rc != 0)
    return rc;

  uint32_t cc = XHCI_EVT_CC(evt);
  if (cc != XHCI_CC_SUCCESS) {
    LOG_ERR("[xHCI] address_device(slot=%u) cc=%u", slot, cc);
    return -EIO;
  }
  LOG_INFO("[xHCI] slot %u direccionado (port=%u mps=%u)", slot, port, ep0_mps);
  return 0;
}

// ===========================================================================
// Configure Endpoint (spec xHCI 1.2 §4.6.6).
// ===========================================================================
int xhci_cmd_configure_endpoint(uint8_t slot, uint64_t input_ctx_phys) {
  if (slot == 0 || slot > g_xhci.max_slots)
    return -EINVAL;

  xhci_trb_t trb = {0};
  trb.param = input_ctx_phys;
  trb.control = XHCI_TRB_TYPE(XHCI_TRB_CONFIGURE_EP) | ((uint32_t)slot << 24) |
                XHCI_TRB_IOC;

  xhci_trb_t evt;
  int rc = xhci_cmd_wait(&trb, 5000, &evt);
  if (rc != 0)
    return rc;

  uint32_t cc = XHCI_EVT_CC(evt);
  if (cc != XHCI_CC_SUCCESS) {
    LOG_ERR("[xHCI] configure_endpoint(slot=%u) cc=%u", slot, cc);
    return -EIO;
  }
  return 0;
}

// ===========================================================================
// Evaluate Context (spec xHCI 1.2 §4.6.7).
//
// Se usa, por ejemplo, para actualizar bMaxPacketSize0 de EP0 tras leer
// el device descriptor real (Fase 2 lo hará para FS/LS que arrancan con
// mps=8 provisional).
// ===========================================================================
int xhci_cmd_evaluate_context(uint8_t slot, uint64_t input_ctx_phys) {
  if (slot == 0 || slot > g_xhci.max_slots)
    return -EINVAL;

  xhci_trb_t trb = {0};
  trb.param = input_ctx_phys;
  trb.control =
      XHCI_TRB_TYPE(XHCI_TRB_EVAL_CTX) | ((uint32_t)slot << 24) | XHCI_TRB_IOC;

  xhci_trb_t evt;
  int rc = xhci_cmd_wait(&trb, 5000, &evt);
  if (rc != 0)
    return rc;

  uint32_t cc = XHCI_EVT_CC(evt);
  if (cc != XHCI_CC_SUCCESS) {
    LOG_ERR("[xHCI] evaluate_context(slot=%u) cc=%u", slot, cc);
    return -EIO;
  }
  return 0;
}