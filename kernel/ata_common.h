// kernel/ata_common.h
#ifndef KERNEL_ATA_COMMON_H
#define KERNEL_ATA_COMMON_H

#include "spinlock.h"
#include <stdint.h>

// ---------------------------------------------------------------------------
// Helpers y constantes comunes a ATA, ATAPI, ATA DMA y AHCI.
//
// ATA y ATAPI comparten:
//   - Registros del task file (Data, Error, Sector Count, LBA0-2, Drive,
//     Status/Command).
//   - Timing (400 ns delay, wait BSY=0, wait DRQ=1).
//   - Selección de drive (master/slave).
//   - Soft reset del canal.
//   - SET FEATURES.
//
// Este archivo centraliza esos helpers. Los drivers específicos (ata_pio,
// atapi, ata_dma, ahci) los usan.
// ---------------------------------------------------------------------------

// ===========================================================================
// Puertos y registros
// ===========================================================================

#define ATA_PRIMARY_IO 0x1F0
#define ATA_PRIMARY_CTRL 0x3F6
#define ATA_PRIMARY_IRQ 14
#define ATA_SECONDARY_IO 0x170
#define ATA_SECONDARY_CTRL 0x376
#define ATA_SECONDARY_IRQ 15

// Offsets desde io_base.
#define ATA_REG_DATA 0
#define ATA_REG_ERROR 1
#define ATA_REG_FEATURES 1
#define ATA_REG_SECCOUNT 2
#define ATA_REG_LBA0 3
#define ATA_REG_LBA1 4
#define ATA_REG_LBA2 5
#define ATA_REG_DRIVE 6
#define ATA_REG_STATUS 7
#define ATA_REG_COMMAND 7

// Offsets desde ctrl_base.
#define ATA_CTRL_ALT_STATUS 0
#define ATA_CTRL_DEV_CTRL 2

// ===========================================================================
// Bits del status
// ===========================================================================
#define ATA_SR_BSY 0x80
#define ATA_SR_DRDY 0x40
#define ATA_SR_DF 0x20
#define ATA_SR_DSC 0x10
#define ATA_SR_DRQ 0x08
#define ATA_SR_CORR 0x04
#define ATA_SR_IDX 0x02
#define ATA_SR_ERR 0x01

// Bits del registro Error.
#define ATA_ER_ABRT 0x04
#define ATA_ER_UNC 0x40
#define ATA_ER_IDNF 0x10
#define ATA_ER_MC 0x20

// ===========================================================================
// Comandos
// ===========================================================================
#define ATA_CMD_READ_SECTORS 0x20
#define ATA_CMD_WRITE_SECTORS 0x30
#define ATA_CMD_READ_SECTORS_EXT 0x24
#define ATA_CMD_WRITE_SECTORS_EXT 0x34
#define ATA_CMD_READ_DMA 0xC8
#define ATA_CMD_WRITE_DMA 0xCA
#define ATA_CMD_READ_DMA_EXT 0x25
#define ATA_CMD_WRITE_DMA_EXT 0x35
#define ATA_CMD_IDENTIFY 0xEC
#define ATA_CMD_IDENTIFY_PACKET 0xA1
#define ATA_CMD_FLUSH_CACHE 0xE7
#define ATA_CMD_FLUSH_CACHE_EXT 0xEA
#define ATA_CMD_SET_FEATURES 0xEF
#define ATA_CMD_PACKET 0xA0

// Features para SET FEATURES.
#define ATA_FEATURE_TRANSFER_MODE 0x03

// Bits del Drive/Head.
#define ATA_DRIVE_MASTER 0xA0
#define ATA_DRIVE_SLAVE 0xB0
#define ATA_DRIVE_LBA 0x40

// Bits del Device Control.
#define ATA_DEVCTRL_SRST 0x04
#define ATA_DEVCTRL_NIEN 0x02

// ===========================================================================
// Timeout y reintentos
// ===========================================================================
#define ATA_TIMEOUT_SECONDS 5
#define ATA_MAX_RETRIES 3

// ===========================================================================
// Errores semánticos (compartidos)
// ===========================================================================
enum ata_err {
  ATA_OK = 0,
  ATA_ERR_ABRT,    // comando abortado
  ATA_ERR_UNC,     // error de datos
  ATA_ERR_IDNF,    // sector no encontrado
  ATA_ERR_MC,      // media change
  ATA_ERR_DF,      // device fault
  ATA_ERR_BSY,     // timeout en BSY
  ATA_ERR_DRQ,     // timeout en DRQ
  ATA_ERR_TIMEOUT, // timeout general
  ATA_ERR_UNKNOWN,
  ATA_ERR_NODEV, // no hay dispositivo
};

// ===========================================================================
// Canal IDE (compartido entre ATA y ATAPI)
// ===========================================================================
typedef struct ata_channel {
  uint16_t io_base;
  uint16_t ctrl_base;
  uint8_t irq;
  const char *name; // "primario" o "secundario"
  spinlock_t lock;  // protege operaciones del canal
  struct ata_device *master;
  struct ata_device *slave;
} ata_channel_t;

// ===========================================================================
// Helpers de bajo nivel
// ===========================================================================

// Delay de 400 ns leyendo el alternate status 4 veces.
void ata_io_delay(uint16_t ctrl_base);

// Devuelve el número de iteraciones de polling para ATA_TIMEOUT_SECONDS.
uint64_t ata_timeout_iters(void);

// Espera a BSY=0. Devuelve ATA_OK o ATA_ERR_BSY.
int ata_wait_not_busy(uint16_t io_base, uint64_t max_iters);

// Espera a BSY=0 && DRQ=1. Comprueba ERR y DF. Devuelve ATA_OK o error.
int ata_wait_drq(uint16_t io_base, uint64_t max_iters);

// Selecciona un drive (master/slave). Aplica el delay.
void ata_select_drive(ata_channel_t *ch, uint8_t drive);

// Soft reset del canal.
int ata_soft_reset(ata_channel_t *ch);

// SET FEATURES.
int ata_set_features(ata_channel_t *ch, uint8_t drive, uint8_t feature,
                     uint8_t value);

// Envía un comando PACKET (ATAPI) con 12 bytes de comando SCSI.
// data_len: bytes a transferir (0 si no hay datos).
// write: 0 para leer del drive, 1 para escribir al drive.
// Devuelve ATA_OK o error.
int ata_issue_packet(ata_channel_t *ch, uint8_t drive, const uint8_t *cmd12,
                     uint32_t data_len, int write);

// Lee el registro Error y lo traduce a código semántico.
int ata_decode_error(uint16_t io_base);

#endif