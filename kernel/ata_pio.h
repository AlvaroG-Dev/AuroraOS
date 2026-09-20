// kernel/ata_pio.h
#ifndef KERNEL_ATA_PIO_H
#define KERNEL_ATA_PIO_H

#include "driver.h"

// ---------------------------------------------------------------------------
// Driver ATA completo (PATA).
//
// Detecta discos ATA en los canales primario y secundario, identifica sus
// capacidades (LBA28, LBA48, DMA, PIO modes, sector size), selecciona el
// mejor modo, y los registra en el block layer como hda, hdb, hdc, hdd.
//
// Soporta:
//   - IDENTIFY DEVICE con parseo completo.
//   - LBA28 y LBA48 (selección automática).
//   - PIO con timing estricto del estándar ATA.
//   - Manejo de errores con reintentos y reset de canal.
//   - Fallback LBA48 → LBA28 si el disco lo soporta.
//
// NO soporta (por ahora):
//   - DMA (Fase 3).
//   - ATAPI (CD/DVD, Fase 2).
//   - Hot-plug (PATA no lo soporta).
//
// API pública: solo el struct driver. Todo lo demás es privado.
// ---------------------------------------------------------------------------

extern struct driver ata_pio_driver;

// Debug: imprime el estado de todos los discos ATA.
void ata_dump(void);

#endif