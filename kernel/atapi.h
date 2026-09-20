// kernel/atapi.h
#ifndef KERNEL_ATAPI_H
#define KERNEL_ATAPI_H

#include "driver.h"

// ---------------------------------------------------------------------------
// Driver ATAPI (CD/DVD sobre PATA).
//
// Detecta unidades ATAPI en los canales IDE (primario y secundario),
// las identifica con IDENTIFY PACKET DEVICE e INQUIRY, y las registra
// en el block layer como sr0, sr1, ...
//
// Soporta:
//   - TEST UNIT READY, REQUEST SENSE, INQUIRY, READ CAPACITY (10),
//     READ (10).
//   - Detección de medio (presente/ausente).
//   - Detección de cambio de medio (media change).
//   - Lectura de bloques de 2048 bytes (CD) o 4096 (DVD).
//
// Los CDs/DVDs son READ-ONLY. bdev_write devuelve -EROFS.
//
// API pública: solo el struct driver y atapi_dump.
// ---------------------------------------------------------------------------

extern struct driver atapi_driver;

// Debug: imprime el estado de las unidades ATAPI.
void atapi_dump(void);

#endif