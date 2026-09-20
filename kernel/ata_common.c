// kernel/ata_common.c
//
// Helpers compartidos entre ATA, ATAPI, ATA DMA y AHCI.

#include "ata_common.h"
#include "io.h"
#include "klog.h"
#include "sched.h"

// ===========================================================================
// Delay y timeout
// ===========================================================================

void ata_io_delay(uint16_t ctrl_base) {
  inb(ctrl_base + ATA_CTRL_ALT_STATUS);
  inb(ctrl_base + ATA_CTRL_ALT_STATUS);
  inb(ctrl_base + ATA_CTRL_ALT_STATUS);
  inb(ctrl_base + ATA_CTRL_ALT_STATUS);
}

uint64_t ata_timeout_iters(void) {
  extern uint64_t klog_get_tsc_freq(void);
  uint64_t freq = klog_get_tsc_freq();
  if (freq == 0)
    return 1000000000ULL;
  // [FIX] Cap a 5e9 iteraciones. Si el TSC está mal calibrado (hemos
  // visto casos de 11 GHz en QEMU por un bug en klog_calibrate_tsc),
  // ata_timeout_iters() devolvía 56e9 y los bucles de polling tardaban
  // minutos en expirar. Con 5e9 son ~1.3 segundos reales.
  uint64_t iters = freq * ATA_TIMEOUT_SECONDS;
  if (iters > 5000000000ULL)
    iters = 5000000000ULL;
  return iters;
}

// ===========================================================================
// Polling
// ===========================================================================

int ata_wait_not_busy(uint16_t io_base, uint64_t max_iters) {
  // [FIX] Cap defensivo por si el llamante pasa un valor enorme.
  if (max_iters > 5000000000ULL)
    max_iters = 5000000000ULL;
  for (uint64_t i = 0; i < max_iters; i++) {
    uint8_t status = inb(io_base + ATA_REG_STATUS);
    if ((status & ATA_SR_BSY) == 0)
      return ATA_OK;
    __asm__ volatile("pause");
  }
  return ATA_ERR_BSY;
}

int ata_decode_error(uint16_t io_base) {
  uint8_t err = inb(io_base + ATA_REG_ERROR);
  if (err & ATA_ER_ABRT)
    return ATA_ERR_ABRT;
  if (err & ATA_ER_UNC)
    return ATA_ERR_UNC;
  if (err & ATA_ER_IDNF)
    return ATA_ERR_IDNF;
  if (err & ATA_ER_MC)
    return ATA_ERR_MC;
  return ATA_ERR_UNKNOWN;
}

int ata_wait_drq(uint16_t io_base, uint64_t max_iters) {
  if (max_iters > 5000000000ULL)
    max_iters = 5000000000ULL;
  for (uint64_t i = 0; i < max_iters; i++) {
    uint8_t status = inb(io_base + ATA_REG_STATUS);
    if (status & ATA_SR_BSY) {
      __asm__ volatile("pause");
      continue;
    }
    if (status & ATA_SR_ERR)
      return ata_decode_error(io_base);
    if (status & ATA_SR_DF)
      return ATA_ERR_DF;
    if (status & ATA_SR_DRQ)
      return ATA_OK;
    __asm__ volatile("pause");
  }
  return ATA_ERR_DRQ;
}

// ===========================================================================
// Selección y reset
// ===========================================================================

void ata_select_drive(ata_channel_t *ch, uint8_t drive) {
  uint8_t val = drive ? ATA_DRIVE_SLAVE : ATA_DRIVE_MASTER;
  outb(ch->io_base + ATA_REG_DRIVE, val);
  ata_io_delay(ch->ctrl_base);
}

static void ata_delay_us(uint32_t us) {
  extern uint64_t klog_get_tsc_freq(void);
  uint64_t freq = klog_get_tsc_freq();
  if (freq == 0) {
    for (volatile uint64_t i = 0; i < (uint64_t)us * 1000; i++)
      __asm__ volatile("pause");
    return;
  }
  uint64_t cycles = (freq / 1000000ULL) * us;
  uint32_t lo, hi;
  __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
  uint64_t start = ((uint64_t)hi << 32) | lo;
  for (;;) {
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    if ((((uint64_t)hi << 32) | lo) - start >= cycles)
      break;
    __asm__ volatile("pause");
  }
}

int ata_soft_reset(ata_channel_t *ch) {
  uint16_t ctrl = ch->ctrl_base;
  uint16_t io = ch->io_base;

  outb(ctrl + ATA_CTRL_DEV_CTRL, ATA_DEVCTRL_SRST);
  for (volatile int i = 0; i < 1000; i++) {
    __asm__ volatile("pause");
  }
  outb(ctrl + ATA_CTRL_DEV_CTRL, 0);

  ata_delay_us(2000);

  return ata_wait_not_busy(io, ata_timeout_iters());
}

// ===========================================================================
// SET FEATURES
// ===========================================================================

int ata_set_features(ata_channel_t *ch, uint8_t drive, uint8_t feature,
                     uint8_t value) {
  uint16_t io = ch->io_base;
  uint64_t timeout = ata_timeout_iters();

  int rc = ata_wait_not_busy(io, timeout);
  if (rc != ATA_OK)
    return rc;

  ata_select_drive(ch, drive);
  rc = ata_wait_not_busy(io, timeout);
  if (rc != ATA_OK)
    return rc;

  outb(io + ATA_REG_FEATURES, feature);
  outb(io + ATA_REG_SECCOUNT, value);
  outb(io + ATA_REG_COMMAND, ATA_CMD_SET_FEATURES);

  rc = ata_wait_not_busy(io, timeout);
  if (rc != ATA_OK)
    return rc;

  uint8_t status = inb(io + ATA_REG_STATUS);
  if (status & ATA_SR_ERR)
    return ata_decode_error(io);
  return ATA_OK;
}

// ===========================================================================
// Comando PACKET (ATAPI)
// ===========================================================================

int ata_issue_packet(ata_channel_t *ch, uint8_t drive, const uint8_t *cmd12,
                     uint32_t data_len, int write) {
  uint16_t io = ch->io_base;
  uint64_t timeout = ata_timeout_iters();
  int rc;

  rc = ata_wait_not_busy(io, timeout);
  if (rc != ATA_OK)
    return rc;

  ata_select_drive(ch, drive);

  rc = ata_wait_not_busy(io, timeout);
  if (rc != ATA_OK)
    return rc;

  outb(io + ATA_REG_FEATURES, 0);

  uint8_t len_lo = (uint8_t)(data_len & 0xFF);
  uint8_t len_hi = (uint8_t)((data_len >> 8) & 0xFF);
  uint8_t len_xhi = (uint8_t)((data_len >> 16) & 0xFF);
  outb(io + ATA_REG_LBA0, len_xhi);
  outb(io + ATA_REG_LBA1, len_lo);
  outb(io + ATA_REG_LBA2, len_hi);

  outb(io + ATA_REG_COMMAND, ATA_CMD_PACKET);

  rc = ata_wait_drq(io, timeout);
  if (rc != ATA_OK)
    return rc;

  const uint16_t *cmd16 = (const uint16_t *)cmd12;
  for (int i = 0; i < 6; i++) {
    outw(io + ATA_REG_DATA, cmd16[i]);
  }

  (void)write;
  return ATA_OK;
}

// ===========================================================================
// Exclusion de canal
//
// [FIX] Ahora es defensiva contra contextos con preempt_count > 0.
// En esos contextos, sched_yield() es un no-op (el scheduler no puede
// cambiar de tarea), así que el bucle se colgaba indefinidamente si el
// canal estaba ocupado. La solución es hacer polling con pause.
// ===========================================================================

void ata_chan_acquire(ata_channel_t *ch) {
  uint64_t iterations = 0;
  int warned = 0;
  for (;;) {
    unsigned long f = spin_lock_irqsave(&ch->lock);
    if (!ch->busy) {
      ch->busy = 1;
      spin_unlock_irqrestore(&ch->lock, f);
      return;
    }
    spin_unlock_irqrestore(&ch->lock, f);

    // [FIX] Si no podemos dormir, hacer polling en vez de sched_yield.
    if (preempt_count() > 0) {
      for (volatile int i = 0; i < 1000; i++) {
        __asm__ volatile("pause");
      }
    } else {
      sched_yield();
    }

    if (!warned && ++iterations == 500000) {
      LOG_ERR("[ATA] canal %s ocupado durante demasiado tiempo (%lu "
              "iteraciones, preempt=%d)",
              ch->name ? ch->name : "?", (unsigned long)iterations,
              preempt_count());
      warned = 1;
    }
  }
}

void ata_chan_release(ata_channel_t *ch) {
  unsigned long f = spin_lock_irqsave(&ch->lock);
  ch->busy = 0;
  spin_unlock_irqrestore(&ch->lock, f);
}