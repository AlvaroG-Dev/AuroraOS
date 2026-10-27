// kernel/ata_dma.h
#ifndef KERNEL_ATA_DMA_H
#define KERNEL_ATA_DMA_H

#include "ata_common.h"
#include "wait.h"
#include <stdint.h>

struct bio;

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

#define ATA_DMA_TIMEOUT_SECONDS 1

// [FASE 1.4] Polling rescue: si el bio lleva más de N ticks en vuelo
// sin IRQ, el scheduler tick intenta completarlo leyendo BM_STATUS.
// 2 ms es margen suficiente para que el IRQ handler normal (que tarda
// <100 us) haga su trabajo.
#define ATA_DMA_POLL_DELAY_TICKS 2

// [FASE 1.4] Estados del path async.
#define XFS_IDLE 0     // Sin bio en vuelo.
#define XFS_RUNNING 1  // Transferencia DMA de datos en curso.
#define XFS_FLUSHING 2 // FLUSH CACHE en curso tras último chunk.

typedef struct {
  uint16_t bmide_base;
  uint8_t irq;
  uint8_t channel_idx;
  uint8_t initialized;
  spinlock_t lock;

  ata_dma_prd_t *prdt;
  uint64_t prdt_phys;

  // Completion para el path síncrono (ata_dma_xfer espera aquí).
  wait_queue_t irq_wq;
  volatile int irq_pending;
  volatile int irq_error;

  // Contadores (debug).
  uint32_t irq_count;
  uint32_t timeout_count;
  uint32_t error_count;

  // [FASE 1.4] Estado async.
  //
  // Protegido por `lock`. Solo se usan en el path async (submit -> IRQ).
  // El path síncrono NO toca estos campos: usa los de arriba + espera
  // en irq_wq.
  //
  // `inflight_bio` es la señal de "hay un async en curso". Cuando está
  // a NULL, o no hay async, o el async ya terminó.
  struct bio *inflight_bio;
  // Disco y tamano de sector del bio en vuelo, copiados en submit: si el bio
  // va a una particion, part_submit() le devuelve su bdev (la particion) en
  // cuanto submit() retorna, asi que la IRQ no puede leerlos de bio->bdev.
  struct ata_device *inflight_dev;
  uint32_t inflight_sector_size;
  uint64_t xfer_lba;
  uint32_t xfer_left;
  uint8_t *xfer_buf;
  int xfer_is_read;
  int xfer_need_flush;
  int xfer_error;
  int xfer_state;
  uint32_t xfer_chunk_sectors;
  uint64_t last_cmd_tick;
} ata_dma_state_t;

int ata_dma_init_channel(int channel_idx, uint8_t pci_bus, uint8_t pci_slot,
                         uint8_t pci_func);

struct ata_device;
int ata_dma_read(struct ata_device *dev, uint64_t lba, uint32_t count,
                 void *buf);
int ata_dma_write(struct ata_device *dev, uint64_t lba, uint32_t count,
                  const void *buf);

// [FASE 1.4] Envío asíncrono. Devuelve:
//   0             → error al arrancar (bio NO en vuelo).
//   -EINPROGRESS  → en vuelo, bio_endio() se llamará desde IRQ/polling.
// El bio debe tener buf alineado a 2 bytes; si no, el llamante debe
// usar el path síncrono.
int ata_dma_submit_async(struct ata_device *dev, struct bio *bio);

void ata_dma_irq_handler(int channel_idx);
int ata_dma_is_ready(int channel_idx);
void ata_dma_dump(void);

// [FASE 1.4] Polling rescue. Llamar ~100 Hz desde sched_tick (BSP).
// Si hay un bio async en vuelo y el hardware parece haber terminado
// (BM_STATUS sin ACTIVE, o FLUSH con BSY=0) pero la IRQ no llegó,
// completa el bio.
void ata_dma_poll_ports(void);

#endif