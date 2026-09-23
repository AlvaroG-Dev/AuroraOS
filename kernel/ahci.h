// kernel/ahci.h
#ifndef KERNEL_AHCI_H
#define KERNEL_AHCI_H

#include "ata_common.h"
#include "block.h"
#include "driver.h"
#include "pci.h"
#include "spinlock.h"
#include "wait.h"
#include <stdint.h>

// ===========================================================================
// Constantes
// ===========================================================================
#define AHCI_CMD_LIST_ENTRIES 32
#define AHCI_CMD_HEADER_SIZE 32
#define AHCI_CMD_TABLE_SIZE (0x80 + 16 * AHCI_PRDT_ENTRIES)
#define AHCI_RX_FIS_SIZE 256
#define AHCI_PRDT_ENTRIES 32

#define AHCI_PRDT_IOC (1u << 31)
#define AHCI_PRD_MAX_BYTES (4u * 1024 * 1024)
#define AHCI_MAX_XFER_BYTES (64u * 1024)

#define AHCI_CMD_WRITE (1u << 6)

// PxCMD
#define AHCI_PxCMD_ST (1u << 0)
#define AHCI_PxCMD_SUD (1u << 1)
#define AHCI_PxCMD_POD (1u << 2)
#define AHCI_PxCMD_CLO (1u << 3)
#define AHCI_PxCMD_FRE (1u << 4)
#define AHCI_PxCMD_CCS (0x1Fu << 8)
#define AHCI_PxCMD_FR (1u << 14)
#define AHCI_PxCMD_CR (1u << 15)

// PxIS
#define AHCI_PxIS_DHRS (1u << 0)
#define AHCI_PxIS_PSS (1u << 1)
#define AHCI_PxIS_DSS (1u << 2)
#define AHCI_PxIS_SDBS (1u << 3)
#define AHCI_PxIS_UFS (1u << 4)
#define AHCI_PxIS_DPS (1u << 5)
#define AHCI_PxIS_PCS (1u << 6)
#define AHCI_PxIS_DMPS (1u << 7)
#define AHCI_PxIS_PRCS (1u << 22)
#define AHCI_PxIS_IPMS (1u << 23)
#define AHCI_PxIS_OFS (1u << 24)
#define AHCI_PxIS_INFS (1u << 26)
#define AHCI_PxIS_IFS (1u << 27)
#define AHCI_PxIS_HBDS (1u << 28)
#define AHCI_PxIS_HBFS (1u << 29)
#define AHCI_PxIS_TFES (1u << 30)
#define AHCI_PxIS_CPDS (1u << 31)

#define AHCI_PxIS_FATAL                                                        \
  (AHCI_PxIS_TFES | AHCI_PxIS_HBFS | AHCI_PxIS_HBDS | AHCI_PxIS_IFS |          \
   AHCI_PxIS_OFS)

// PxSSTS
#define AHCI_SSTS_DET_MASK 0x0F
#define AHCI_SSTS_DET_NONE 0x00
#define AHCI_SSTS_DET_PHY 0x01
#define AHCI_SSTS_DET_READY 0x03
#define AHCI_SSTS_DET_OFF 0x04

// FIS
#define FIS_TYPE_REG_H2D 0x27
#define FIS_TYPE_REG_D2H 0x34
#define FIS_TYPE_DMA_ACT 0x39
#define FIS_TYPE_DMA_SETUP 0x41
#define FIS_TYPE_DATA 0x46
#define FIS_TYPE_BIST 0x58
#define FIS_TYPE_PIO_SETUP 0x5F
#define FIS_TYPE_DEV_BITS 0xA1

#define CAP_S64A (1u << 31)

// ===========================================================================
// [AHCI] Umbrales de robustez.
// ===========================================================================

// Número máximo de IRQs espurias consecutivas (HBA_IS == 0) antes de
// considerar que la IRQ está compartida con otro dispositivo que
// mantiene la línea asertada. Al superarlo, la IRQ se queda enmascarada
// y el polling se encarga. Se recupera reiniciando el sistema.
#define AHCI_SPURIOUS_LIMIT 32

// Ticks de scheduler (1 ms cada uno) que esperamos antes de que el
// polling rescate un comando. El IRQ handler en producción tarda
// <100 us; 2 ms es margen de sobra y minimiza la latencia cuando la
// IRQ no llega (enmascarada, perdida, storm).
#define AHCI_POLL_DELAY_TICKS 2

// ===========================================================================
// Estructuras de memoria compartida
// ===========================================================================
typedef struct __attribute__((packed)) {
  uint16_t flags;
  uint16_t prdtl;
  volatile uint32_t prdbc;
  uint32_t ctba;
  uint32_t ctbau;
  uint32_t reserved[4];
} ahci_cmd_header_t;
_Static_assert(sizeof(ahci_cmd_header_t) == 32, "ahci_cmd_header_t != 32 B");

typedef struct __attribute__((packed)) {
  uint8_t cfis[64];
  uint8_t acmd[32];
  uint8_t reserved[32];
  struct {
    uint32_t dba;
    uint32_t dbau;
    uint32_t reserved;
    uint32_t flags;
  } prdt[AHCI_PRDT_ENTRIES];
} ahci_cmd_table_t;

typedef struct __attribute__((packed)) {
  uint8_t dsfis[0x1C];
  uint8_t reserved0[0x04];
  uint8_t psfis[0x14];
  uint8_t reserved1[0x0C];
  uint8_t rfis[0x14];
  uint8_t reserved2[0x04];
  uint8_t sdbfis[0x08];
  uint8_t ufis[0x40];
  uint8_t reserved3[0x60];
} ahci_rx_fis_t;

typedef struct __attribute__((packed)) {
  uint8_t fis_type;
  uint8_t pmport : 4;
  uint8_t reserved0 : 3;
  uint8_t c : 1;
  uint8_t command;
  uint8_t featurel;
  uint8_t lba0;
  uint8_t lba1;
  uint8_t lba2;
  uint8_t device;
  uint8_t lba3;
  uint8_t lba4;
  uint8_t lba5;
  uint8_t featureh;
  uint8_t countl;
  uint8_t counth;
  uint8_t icc;
  uint8_t control;
  uint8_t reserved1[4];
} fis_reg_h2d_t;

// ===========================================================================
// Puerto AHCI
// ===========================================================================
typedef struct ahci_port {
  uint8_t port_num;
  volatile uint8_t *regs;
  uint8_t implemented;
  uint8_t initialized;
  uint8_t has_device;
  uint8_t dead;

  uint64_t cmd_list_phys;
  ahci_cmd_header_t *cmd_list;

  uint64_t rx_fis_phys;
  ahci_rx_fis_t *rx_fis;

  uint64_t cmd_table_phys;
  ahci_cmd_table_t *cmd_table;

  uint64_t identify_phys;
  uint16_t *identify;

  spinlock_t lock;
  volatile int busy;
  wait_queue_t slot_wq;

  wait_queue_t irq_wq;
  volatile int irq_pending;
  volatile uint32_t irq_status;

  uint32_t cmd_timeout_ms;

  uint8_t eh_level;

  uint8_t ncq_depth;
  uint8_t ncq_enabled;

  // [FASE 3] Bio en vuelo
  struct bio *inflight_bio;
  uint64_t xfer_lba;
  uint32_t xfer_left;
  uint8_t *xfer_buf;
  int xfer_is_read;
  int xfer_need_flush;
  int xfer_error;
  int xfer_started;
  uint8_t xfer_state;

  uint64_t num_sectors;
  uint32_t sector_size;

  block_device_t *bdev;

  uint32_t cmd_count;
  uint32_t error_count;
  uint32_t timeout_count;
  uint64_t
      last_cmd_tick; // tick del scheduler cuando se lanzó el último comando
} ahci_port_t;

// ===========================================================================
// HBA AHCI
// ===========================================================================
typedef struct ahci_hba {
  pci_device_t pci;
  volatile uint8_t *abar;
  uint64_t abar_phys;

  uint32_t cap;
  uint32_t cap2;
  uint32_t pi;
  uint32_t version;
  uint32_t bohc;

  ahci_port_t ports[32];
  uint8_t num_ports_impl;

  uint8_t initialized;

  // IRQ del controlador.
  uint8_t irq;

  // [NUEVO] Modo de interrupción activo.
  uint8_t using_msi;  // 1 si MSI, 0 si IRQ legacy
  uint8_t irq_masked; // 1 si la IRQ legacy está enmascarada

  // [NUEVO] Detección de storm.
  uint32_t irq_spurious_count;
  uint32_t irq_count;

  // [NUEVO] Vector MSI asignado.
  uint8_t msi_vector;
} ahci_hba_t;

extern struct driver ahci_driver;
void ahci_dump(void);
int ahci_disk_count(void);
void ahci_poll_ports(void);

#endif