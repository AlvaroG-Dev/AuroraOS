// kernel/fsprobe.h — detección del tipo de FS de un block device por su
// firma (no por el tipo de partición, que miente a menudo).
#ifndef KERNEL_FSPROBE_H
#define KERNEL_FSPROBE_H

#include "block.h"

enum fs_kind {
  FS_UNKNOWN = 0,
  FS_FAT32,
  FS_EXT2, // familia ext2/3/4 (magia 0xEF53); el driver decide qué acepta
  FS_ISO9660,
};

enum fs_kind fs_probe(block_device_t *bdev);
const char *fs_kind_name(enum fs_kind k);

#endif
