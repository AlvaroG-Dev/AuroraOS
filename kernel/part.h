// kernel/part.h
#ifndef KERNEL_PART_H
#define KERNEL_PART_H

#include "block.h"

// ---------------------------------------------------------------------------
// Partition layer.
//
// Detecta tablas de particiones (MBR y GPT) en discos registrados y crea
// un block_device hijo por cada partición. Se llama UNA VEZ después de
// que todos los drivers de bloque hayan registrado sus discos.
//
// Los dispositivos de partición se registran con is_partition = 1,
// parent apuntando al disco padre y start_lba igual al LBA inicial de
// la partición en el disco físico. El nombre sigue el convenio Linux:
//   sda → sda1, sda2, ...
//   hda → hda1, hda2, ...
//
// Convenio de numeración:
//   - MBR: primarias ocupan índices 1-4 (matching el slot del MBR),
//     lógicas empiezan en 5 en orden del EBR chain.
//   - GPT: índice = entry index + 1.
//
// No soportado (todavía):
//   - Híbrido MBR+GPT real: si el protective MBR es válido y GPT
//     parsea, se ignora MBR.
//   - Discos con sector_size != 512 para la tabla MBR (la firma 0x55AA
//     sigue estando en bytes 510-511 en cualquier caso).
// ---------------------------------------------------------------------------

// Escanea la tabla de particiones de `disk` y registra cada partición.
// Devuelve el número de particiones registradas (>= 0), o <0 en error
// de I/O. Idempotente: llamarla dos veces sobre el mismo disco no
// duplica particiones.
int part_scan(block_device_t *disk);

// Escanea todos los discos registrados que no sean particiones y no
// sean read-only. Idempotente.
void part_scan_all(void);

#endif