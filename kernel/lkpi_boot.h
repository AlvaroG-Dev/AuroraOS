// kernel/lkpi_boot.h
#ifndef KERNEL_LKPI_BOOT_H
#define KERNEL_LKPI_BOOT_H

// Carga los .kur de `lkpi.load=` (linea del kernel). Devuelve cuantos cargo.
int lkpi_boot_load(void);

// ¿Cargo el arranque la capa (por lkpi.load= o por autodeteccion)? Si si, las
// pruebas que cargan/descargan la capa no deben ejecutarse.
int lkpi_boot_loaded(void);

#endif
