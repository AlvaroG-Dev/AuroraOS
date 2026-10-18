// kernel/usb/xhci/xhci_cmd.c
#include "../../klog.h"
#include "../../spinlock.h"
#include "../../uaccess.h"
#include "../../wait.h"
#include "xhci.h"
#include "xhci_ring.h"

static bool has_pending_cmd(void *arg) { return g_pending_cmd_count > 0; }

static int xhci_cmd_wait(xhci_trb_t *trb, uint32_t timeout_ms,
                         xhci_trb_t *out_evt) {
  if (!g_xhci.initialized || !trb)
    return -EINVAL;

  unsigned long flags = spin_lock_irqsave(&g_xhci.lock);

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
  *(volatile uint32_t *)g_xhci.db = 0;
  spin_unlock_irqrestore(&g_xhci.lock, flags);

  // Espera por TSC con timeout absoluto (sin depender de has_pending_cmd).
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
        LOG_ERR("[xHCI] cmd timeout (trb=0x%llx)",
                (unsigned long long)cmd_trb_phys);
        return -ETIMEDOUT;
      }
    }

    xhci_trb_t evt;
    if (xhci_take_pending_cmd(cmd_trb_phys, &evt) == 0) {
      if (out_evt)
        *out_evt = evt;
      return 0;
    }

    if ((++iter & 0x3F) == 0)
      sched_yield();
    else
      __asm__ volatile("pause");
  }
}

int xhci_cmd_noop(void) {
  xhci_trb_t trb = {0};
  trb.control = XHCI_TRB_TYPE(XHCI_TRB_NO_OP_CMD) | XHCI_TRB_IOC;
  xhci_trb_t evt;
  int rc = xhci_cmd_wait(&trb, 2000, &evt);
  if (rc != 0)
    return rc;
  if (XHCI_EVT_CC(evt) != XHCI_CC_SUCCESS)
    return -EIO;
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
  if (XHCI_EVT_CC(evt) != XHCI_CC_SUCCESS)
    return -EIO;
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
  if (XHCI_EVT_CC(evt) != XHCI_CC_SUCCESS)
    return -EIO;
  return 0;
}

int xhci_cmd_address_device(uint8_t slot, uint8_t port, usb_speed_t speed,
                            uint16_t ep0_mps, uint64_t input_ctx_phys) {
  if (slot == 0 || slot > g_xhci.max_slots || port == 0)
    return -EINVAL;
  xhci_trb_t trb = {0};
  trb.param = input_ctx_phys;
  trb.control = XHCI_TRB_TYPE(XHCI_TRB_ADDRESS_DEVICE) |
                ((uint32_t)slot << 24) | XHCI_TRB_IOC;
  xhci_trb_t evt;
  int rc = xhci_cmd_wait(&trb, 5000, &evt);
  if (rc != 0)
    return rc;
  if (XHCI_EVT_CC(evt) != XHCI_CC_SUCCESS) {
    LOG_ERR("[xHCI] address_device(slot=%u) cc=%u", slot, XHCI_EVT_CC(evt));
    return -EIO;
  }
  return 0;
}

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
  if (XHCI_EVT_CC(evt) != XHCI_CC_SUCCESS) {
    LOG_ERR("[xHCI] configure_endpoint(slot=%u) cc=%u", slot, XHCI_EVT_CC(evt));
    return -EIO;
  }
  return 0;
}

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
  if (XHCI_EVT_CC(evt) != XHCI_CC_SUCCESS)
    return -EIO;
  return 0;
}