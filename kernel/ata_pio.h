// kernel/ata_pio.h
#ifndef KERNEL_ATA_PIO_H
#define KERNEL_ATA_PIO_H

#include "ata_common.h"
#include "driver.h"

struct ata_device;

// [FIX] Devuelve el otro drive del mismo canal, o NULL si no hay.
struct ata_device *ata_dev_pair(struct ata_device *dev);

extern struct driver ata_pio_driver;

// [FIX] Fase C: llamar DESPUÉS de atapi_init(). Selecciona modo (con
// compatibilidad de par master/slave) y registra los discos ATA en el
// block layer.
int ata_pio_finalize(void);

void ata_dump(void);

#endif