// kernel/efi_rt.h
//
// Servicios runtime de UEFI (solo ResetSystem), Plan_Power.md P3b.
//
// El bootloader no llama a SetVirtualAddressMap, asi que el firmware espera
// ejecutarse con virtual == fisico. En efi_rt_init() se guardan las regiones
// con atributo RUNTIME del mapa de memoria y se prepara un PML4 propio que
// las mapea en identidad; efi_rt_reset() cambia a el solo durante la llamada.
#ifndef KERNEL_EFI_RT_H
#define KERNEL_EFI_RT_H

#include <stdint.h>

// Tipos de EFI_RESET_TYPE.
#define EFI_RESET_COLD 0
#define EFI_RESET_WARM 1
#define EFI_RESET_SHUTDOWN 2

// Paso 1, antes de paging_init() (el mapa de memoria del bootloader aun es
// accesible): copia el puntero a EFI_RUNTIME_SERVICES y las regiones RUNTIME.
int efi_rt_collect(uint64_t runtime_services_phys, const void *memmap,
                   uint64_t memmap_size, uint64_t desc_size);
// Paso 2, tras paging_init() y pmm_init(): construye el PML4 con identidad.
// Devuelve 0 si ResetSystem queda disponible.
int efi_rt_init(void);

// 1 si hay ResetSystem utilizable (UEFI, no desactivado con efi=noruntime).
int efi_rt_available(void);

// Llama a ResetSystem(type). Con interrupciones desactivadas. Vuelve solo si
// el firmware no hizo nada.
void efi_rt_reset(int type);

#endif
