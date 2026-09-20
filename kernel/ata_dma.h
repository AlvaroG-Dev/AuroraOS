// kernel/ata_dma.h
#ifndef KERNEL_ATA_DMA_H
#define KERNEL_ATA_DMA_H

#include "ata_common.h"
#include "wait.h"
#include <stdint.h>


// ---------------------------------------------------------------------------
// Soporte de DMA para ATA (Bus Master IDE).
// ...

// Bits de BM Command.
#define BM_CMD_START_STOP (1 << 0)
#define BM_CMD_READ (1 << 3)

// Bits de BM Status.
#define BM_STATUS_ACTIVE (1 << 0)
#define BM_STATUS_ERROR (1 << 1)
#define BM_STATUS_IRQ (1 << 2)
#define BM_STATUS_DRIVE0 (1 << 5)
#define BM_STATUS_DRIVE1 (1 << 6)

typedef struct __attribute__((packed)) {
  uint32_t base;
  uint16_t count;
  uint16_t flags;
} ata_dma_prd_t;

#define ATA_DMA_PRD_EOT (1 << 15)

// Timeout para operaciones DMA.
#define ATA_DMA_TIMEOUT_SECONDS 5

typedef struct {
  uint16_t bmide_base;
  uint8_t irq;
  uint8_t channel_idx;
  uint8_t initialized;
  spinlock_t lock;

  ata_dma_prd_t *prdt;
  uint64_t prdt_phys;

  // Completion.
  wait_queue_t irq_wq;
  volatile int irq_pending;
  volatile int irq_error;

  // Contadores (debug).
  uint32_t irq_count;
  uint32_t timeout_count;
  uint32_t error_count;
} ata_dma_state_t;

int ata_dma_init_channel(int channel_idx, uint8_t pci_bus, uint8_t pci_slot,
                         uint8_t pci_func);

struct ata_device;
int ata_dma_read(struct ata_device *dev, uint64_t lba, uint32_t count,
                 void *buf);
int ata_dma_write(struct ata_device *dev, uint64_t lba, uint32_t count,
                  const void *buf);

void ata_dma_irq_handler(int channel_idx);
int ata_dma_is_ready(int channel_idx);
void ata_dma_dump(void);

#endif