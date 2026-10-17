/* bootloader/include/kernel_boot_info.h
 *
 * Estructura compartida entre el bootloader (EFI y BIOS) y el kernel.
 * CUALQUIER cambio aquí DEBE replicarse en kernel/main.c.
 * El _Static_assert detecta desalineaciones en build time.
 */
#ifndef AURORA_KERNEL_BOOT_INFO_H
#define AURORA_KERNEL_BOOT_INFO_H

#include <stdint.h>

struct __attribute__((packed)) kernel_boot_info {
  uint64_t fb_base;
  uint64_t fb_size;
  uint32_t fb_width;
  uint32_t fb_height;
  uint32_t fb_pitch;
  uint32_t fb_bpp;
  uint64_t memmap;
  uint64_t memmap_size;
  uint64_t memmap_desc_size;
  uint32_t memmap_desc_ver;
  uint8_t acpi_rsdp[64];
  uint64_t initrd_base; /* direccion FISICA del initrd */
  uint64_t initrd_size;
};

_Static_assert(
    sizeof(struct kernel_boot_info) == 140,
    "kernel_boot_info cambio de tamano: sincroniza con kernel/main.c");

#endif