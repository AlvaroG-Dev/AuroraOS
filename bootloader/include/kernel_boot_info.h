/* bootloader/include/kernel_boot_info.h */
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
  uint64_t initrd_base;
  uint64_t initrd_size;
  uint32_t splash_phase;      /* 0=off, 1=titulo, 2=spinner */
  uint32_t splash_elapsed_ms; /* Tiempo que lleva corriendo la splash */
  uint32_t edid_len;          /* 0 = sin EDID; 128 o 256 */
  uint8_t edid[256];          /* EDID del monitor (EFI_EDID_ACTIVE/DISCOVERED) */
};

_Static_assert(
    sizeof(struct kernel_boot_info) == 408,
    "kernel_boot_info cambio de tamano: sincroniza con kernel/main.c");

#endif