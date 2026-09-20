// kernel/ahci.h
#ifndef KERNEL_AHCI_H
#define KERNEL_AHCI_H

#include "ata_common.h" // reutilizamos ATA_OK, ATA_ERR_*, ATA_CMD_*
#include "block.h"
#include "driver.h"
#include "pci.h"
#include "spinlock.h"
#include "wait.h"
#include <stdint.h>

// ---------------------------------------------------------------------------
// AHCI 1.3.1 — Advanced Host Controller Interface
//
// Controlador SATA nativo. Un HBA gestiona hasta 32 puertos; cada puerto
// tiene:
//   - Una Command List (32 slots de Command Header, 32 B cada uno).
//   - Una Received FIS Structure (256 B mínimo) donde el HBA escribe los
//     FIS que devuelve el dispositivo.
//   - Una Command Table por slot (con FIS H2D + PRDT).
//
// El driver actual usa el slot 0 de la Command List de cada puerto y espera
// la IRQ del HBA para completar. Sin NCQ por ahora.
//
// Referencias:
//   - AHCI 1.3.1 spec (Intel, 2012).
//   - Serial ATA 3.0 (AHCI usa FIS de SATA).
// ---------------------------------------------------------------------------

// ===========================================================================
// Constantes
// ===========================================================================

#define AHCI_CMD_LIST_ENTRIES 32
#define AHCI_CMD_HEADER_SIZE 32
#define AHCI_CMD_TABLE_SIZE (0x80 + 16 * AHCI_PRDT_ENTRIES)
#define AHCI_RX_FIS_SIZE 256
#define AHCI_PRDT_ENTRIES 32

// Flags del PRDT. El bit 31 es IOC (Interrupt On Completion), los
// bits 0..21 son el DBC (Data Byte Count - 1).
#define AHCI_PRDT_IOC (1u << 31)

// Bytes máximos por entrada PRD (spec: 4 MB) y por comando (limitamos a
// 64 KB para que el PRDT nunca se llene aunque el buffer no esté
// físicamente contiguo).
#define AHCI_PRD_MAX_BYTES (4u * 1024 * 1024)
#define AHCI_MAX_XFER_BYTES (64u * 1024)

// Bits de DW0 del Command Header (campo `flags`).
//   bits 4:0 = CFL (longitud del FIS en dwords), bit 6 = W (H2D data).
#define AHCI_CMD_WRITE (1u << 6)

// Puerto: bits de PxCMD.
#define AHCI_PxCMD_ST (1u << 0)
#define AHCI_PxCMD_SUD (1u << 1)
#define AHCI_PxCMD_POD (1u << 2)
#define AHCI_PxCMD_CLO (1u << 3)
#define AHCI_PxCMD_FRE (1u << 4)
#define AHCI_PxCMD_CCS (0x1Fu << 8)
#define AHCI_PxCMD_FR (1u << 14)
#define AHCI_PxCMD_CR (1u << 15)

// Puerto: bits de PxIS.
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

// Errores fatales: el HBA para el puerto (CR=0) y hay que recuperarlo.
#define AHCI_PxIS_FATAL                                                        \
  (AHCI_PxIS_TFES | AHCI_PxIS_HBFS | AHCI_PxIS_HBDS | AHCI_PxIS_IFS |          \
   AHCI_PxIS_OFS)

// Puerto: bits de PxSSTS.
#define AHCI_SSTS_DET_MASK 0x0F
#define AHCI_SSTS_DET_NONE 0x00
#define AHCI_SSTS_DET_PHY 0x01
#define AHCI_SSTS_DET_READY 0x03
#define AHCI_SSTS_DET_OFF 0x04

// Tipo de FIS.
#define FIS_TYPE_REG_H2D 0x27
#define FIS_TYPE_REG_D2H 0x34
#define FIS_TYPE_DMA_ACT 0x39
#define FIS_TYPE_DMA_SETUP 0x41
#define FIS_TYPE_DATA 0x46
#define FIS_TYPE_BIST 0x58
#define FIS_TYPE_PIO_SETUP 0x5F
#define FIS_TYPE_DEV_BITS 0xA1

// ===========================================================================
// Estructuras de memoria compartida con el HBA
// ===========================================================================

// Command Header (AHCI 1.3.1, 4.2.2). 32 bytes.
//   DW0: bits 4:0 CFL, 5 A, 6 W, 7 P, 8 R, 9 B, 10 C, 15:12 PMP,
//        bits 31:16 PRDTL (16 bits).
//   DW1: PRDBC (lo escribe el HBA).
//   DW2/DW3: CTBA / CTBAU (Command Table, alineada a 128 B).
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

  // Command List (1 página, 32 entradas de 32 B).
  uint64_t cmd_list_phys;
  ahci_cmd_header_t *cmd_list;

  // Received FIS (1 página, primeros 256 B usados).
  uint64_t rx_fis_phys;
  ahci_rx_fis_t *rx_fis;

  // Command Table (1 página, 256 B usados para slot 0).
  uint64_t cmd_table_phys;
  ahci_cmd_table_t *cmd_table;

  // Buffer del IDENTIFY. 1 página propia, físicamente contigua y
  // alineada a 4 KB. El PRDT lo describe con una sola entrada.
  uint64_t identify_phys;
  uint16_t *identify;

  // Serialización del slot 0: `busy` se protege con `lock` (spinlock, solo
  // para el flag; NUNCA se duerme con él cogido). Los que esperan el slot
  // duermen en `slot_wq`.
  spinlock_t lock;
  volatile int busy;
  wait_queue_t slot_wq;

  // Comando en vuelo (slot 0): la IRQ despierta a `irq_wq`.
  wait_queue_t irq_wq;
  volatile int irq_pending;
  volatile uint32_t irq_status;

  // Identidad del dispositivo.
  uint64_t num_sectors;
  uint32_t sector_size;

  // Enlace al block layer.
  block_device_t *bdev;

  // Debug.
  uint32_t cmd_count;
  uint32_t error_count;
  uint32_t timeout_count;
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
} ahci_hba_t;

// ===========================================================================
// API pública
// ===========================================================================
extern struct driver ahci_driver;

void ahci_dump(void);
int ahci_disk_count(void);

#endif