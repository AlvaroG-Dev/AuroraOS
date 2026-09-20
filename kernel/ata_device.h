// kernel/ata_device.h
#ifndef KERNEL_ATA_DEVICE_H
#define KERNEL_ATA_DEVICE_H

#include "ata_common.h"
#include "block.h"
#include <stdint.h>

// ---------------------------------------------------------------------------
// Estructura de un disco ATA.
//
// Vive en ata_pio.c pero se expone aquí para que ata_dma.c y (en el
// futuro) otros drivers puedan acceder a sus campos.
//
// El "propietario" del struct es el driver ata_pio. Los demás drivers
// (ata_dma) solo leen campos. Si en el futuro algún driver necesita
// modificar campos, debe coordinarse con ata_pio.
// ---------------------------------------------------------------------------
typedef struct ata_device {
  // Identidad (del IDENTIFY DEVICE).
  char model[41];
  char serial[21];
  char firmware[9];

  // Ubicación.
  ata_channel_t *channel;
  uint8_t drive; // 0 = master, 1 = slave
  uint8_t is_atapi;

  // Capacidades detectadas.
  uint8_t supports_lba;
  uint8_t supports_lba48;
  uint8_t supports_dma;
  uint8_t supports_flush;
  uint8_t supports_ncq;
  uint16_t pio_modes;
  uint16_t dma_modes;
  uint16_t udma_modes;
  uint32_t sector_size;
  uint64_t lba28_sectors;
  uint64_t lba48_sectors;

  // Modo activo.
  uint8_t use_lba48;
  uint8_t pio_mode;
  uint8_t use_dma;
  uint8_t dma_mode;

  // Estado en runtime.
  uint64_t num_sectors;
  uint32_t error_count;
  uint32_t retry_count;
  uint8_t dma_fail_streak; // fallos de DMA consecutivos (ver ata_submit)

  // Enlace al block layer.
  block_device_t *bdev;

  // Lista para debug.
  struct ata_device *next;
} ata_device_t;

#endif