// kernel/lkpi_boot.h
#ifndef KERNEL_LKPI_BOOT_H
#define KERNEL_LKPI_BOOT_H

// Carga los .kur de `lkpi.load=` (linea del kernel). Devuelve cuantos cargo.
int lkpi_boot_load(void);

#endif
