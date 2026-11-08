// kernel/fsprobe.c — ver fsprobe.h.
#include "fsprobe.h"

#include "heap.h"
#include "string.h"

const char *fs_kind_name(enum fs_kind k) {
  switch (k) {
  case FS_FAT32:
    return "fat32";
  case FS_EXT2:
    return "ext2";
  case FS_ISO9660:
    return "iso9660";
  default:
    return "desconocido";
  }
}

// Lee `len` bytes desde el byte `off` del dispositivo (sectores completos).
static int probe_read(block_device_t *bdev, uint64_t off, void *out,
                      uint32_t len) {
  uint32_t ss = bdev->sector_size;
  if (!ss || len > 4096 || off + len > bdev_size_bytes(bdev))
    return -1;
  uint64_t first = off / ss;
  uint64_t last = (off + len - 1) / ss;
  uint32_t n = (uint32_t)(last - first + 1);
  uint8_t *tmp = kmalloc((size_t)n * ss);
  if (!tmp)
    return -1;
  int rc = bdev_read(bdev, first, n, tmp);
  if (rc == 0)
    memcpy(out, tmp + (off - first * ss), len);
  kfree(tmp);
  return rc == 0 ? 0 : -1;
}

enum fs_kind fs_probe(block_device_t *bdev) {
  if (!bdev || bdev->sector_size == 0)
    return FS_UNKNOWN;
  uint8_t b[8];

  // ISO9660: "CD001" en el byte 32769 (descriptor primario, sector 16).
  if (probe_read(bdev, 32769, b, 5) == 0 && memcmp(b, "CD001", 5) == 0)
    return FS_ISO9660;

  // ext2/3/4: s_magic (0xEF53, LE) en el byte 1024 + 56.
  if (probe_read(bdev, 1024 + 56, b, 2) == 0 && b[0] == 0x53 && b[1] == 0xEF)
    return FS_EXT2;

  // FAT32: "FAT32   " en el offset 82 del boot sector y firma 0x55AA.
  uint8_t bs[512];
  if (bdev->sector_size <= 512 && probe_read(bdev, 0, bs, 512) == 0 &&
      bs[510] == 0x55 && bs[511] == 0xAA && memcmp(bs + 82, "FAT32   ", 8) == 0)
    return FS_FAT32;

  return FS_UNKNOWN;
}
