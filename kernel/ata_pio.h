// kernel/ata_pio.h
#ifndef KERNEL_ATA_PIO_H
#define KERNEL_ATA_PIO_H

#include "driver.h"

// Driver ATA PIO. Detecta discos ATA en los canales primario y secundario,
// y los registra en el block layer como hda, hdb, hdc, hdd.
//
// En Fase 1 solo hace detección (IDENTIFY DEVICE). La lectura/escritura
// se añade en la parte 2.
//
// ATAPI (CD/DVD): se detecta pero no se registra. Fase 2 lo hace.
extern struct driver ata_pio_driver;

#endif