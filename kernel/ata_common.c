// kernel/ata_common.c
//
// Helpers compartidos entre ATA, ATAPI, ATA DMA y AHCI.

#include "ata_common.h"
#include "io.h"
#include "klog.h"

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
  return freq * ATA_TIMEOUT_SECONDS;
}

// ===========================================================================
// Polling
// ===========================================================================

int ata_wait_not_busy(uint16_t io_base, uint64_t max_iters) {
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

int ata_soft_reset(ata_channel_t *ch) {
  uint16_t ctrl = ch->ctrl_base;
  uint16_t io = ch->io_base;

  // SRST=1.
  outb(ctrl + ATA_CTRL_DEV_CTRL, ATA_DEVCTRL_SRST);
  // Esperar 5 µs mínimo.
  for (volatile int i = 0; i < 1000; i++) {
    __asm__ volatile("pause");
  }
  // SRST=0.
  outb(ctrl + ATA_CTRL_DEV_CTRL, 0);

  // Esperar BSY=0.
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
//
// Flujo:
//   1. Esperar BSY=0.
//   2. Seleccionar drive.
//   3. Escribir Features (DMA bit: 0 para PIO).
//   4. Escribir la longitud esperada de transferencia en LBA1/LBA2.
//   5. Enviar PACKET (0xA0).
//   6. Esperar DRQ=1.
//   7. Escribir 12 bytes de comando SCSI en Data (6 palabras).
//   8. Transferir datos (si los hay).
//   9. Esperar BSY=0.
//  10. Comprobar ERR/DF.
// ===========================================================================

int ata_issue_packet(ata_channel_t *ch, uint8_t drive, const uint8_t *cmd12,
                     uint32_t data_len, int write) {
  uint16_t io = ch->io_base;
  uint64_t timeout = ata_timeout_iters();
  int rc;

  // 1. Esperar BSY=0.
  rc = ata_wait_not_busy(io, timeout);
  if (rc != ATA_OK)
    return rc;

  // 2. Seleccionar drive.
  ata_select_drive(ch, drive);

  rc = ata_wait_not_busy(io, timeout);
  if (rc != ATA_OK)
    return rc;

  // 3. Features: 0 para PIO, 1 para DMA.
  outb(io + ATA_REG_FEATURES, 0);

  // 4. Longitud de transferencia en LBA1/LBA2 (big-endian: LBA2 alto, LBA1
  // bajo). El byte alto de la longitud en LBA2, el byte bajo en LBA1. El byte
  // más alto (bits 16-23) va en LBA0 (poco común).
  uint8_t len_lo = (uint8_t)(data_len & 0xFF);
  uint8_t len_hi = (uint8_t)((data_len >> 8) & 0xFF);
  uint8_t len_xhi = (uint8_t)((data_len >> 16) & 0xFF);
  outb(io + ATA_REG_LBA0, len_xhi);
  outb(io + ATA_REG_LBA1, len_lo);
  outb(io + ATA_REG_LBA2, len_hi);

  // 5. Enviar PACKET.
  outb(io + ATA_REG_COMMAND, ATA_CMD_PACKET);

  // 6. Esperar DRQ=1.
  rc = ata_wait_drq(io, timeout);
  if (rc != ATA_OK)
    return rc;

  // 7. Escribir 12 bytes del comando SCSI en Data (6 palabras de 16 bits).
  const uint16_t *cmd16 = (const uint16_t *)cmd12;
  for (int i = 0; i < 6; i++) {
    outw(io + ATA_REG_DATA, cmd16[i]);
  }

  // 8. Transferir datos si hay.
  if (data_len > 0) {
    uint32_t words = (data_len + 1) / 2;         // redondeo
    uint16_t *ptr = (uint16_t *)(uintptr_t)NULL; // placeholder
    (void)ptr;
    (void)words;
    (void)write;
    // La transferencia de datos la hace el llamante, porque necesita
    // acceder al buffer. En atapi_read, después de enviar el comando,
    // se llama a una función específica de transferencia.
    // Este helper solo envía el comando.
  }

  return ATA_OK;
}