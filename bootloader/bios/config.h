/* bootloader/bios/config.h */
#ifndef AURORA_BIOS_CONFIG_H
#define AURORA_BIOS_CONFIG_H
#include "bios.h"

#define CFG_PATH_MAX 128
#define CFG_CMDLINE_MAX 256
#define CFG_TIMEOUT_DEFAULT 3

typedef struct {
  char kernel_path[CFG_PATH_MAX]; /* normalizado a /AURORA/KERNEL.ELF */
  char initrd_path[CFG_PATH_MAX];
  char cmdline[CFG_CMDLINE_MAX];
  int have_kernel;
  int have_initrd;
  int have_cmdline;
  int timeout;
} bios_config_t;

/* Carga /AURORA/AURORA.CONF (y variantes) de efi.img. Si no existe o
 * falla, deja la estructura con los defaults y devuelve 0. */
int bios_config_load(uint8_t drive, const fat_file_t *efi_img_root_unused,
                     bios_config_t *out);

#endif