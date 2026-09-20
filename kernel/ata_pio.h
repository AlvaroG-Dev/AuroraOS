// kernel/ata_pio.h
#ifndef KERNEL_ATA_PIO_H
#define KERNEL_ATA_PIO_H

#include "driver.h"

// Driver ATA PIO. Detecta discos ATA y los registra en el block layer.
// Lee/escribe con PIO (LBA28 + LBA48).
extern struct driver ata_pio_driver;

// Debug: imprime el estado de todos los discos ATA.
void ata_dump(void);

#endif