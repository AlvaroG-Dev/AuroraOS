/* bootloader/bios/iso9660.c
 *
 * Lector minimo de ISO9660. Solo necesitamos:
 *   - PVD (LBA 16).
 *   - Root directory record del PVD.
 *   - Buscar un fichero por nombre 8.3 en el root directory.
 *
 * NO soportamos subdirectorios (no hace falta: /efi.img esta en la raiz).
 */
#include "bios.h"

#define ISO_SECTOR_SIZE 2048
#define ISO_PVD_LBA 16

/* On-disk structures (subset). */
typedef struct {
  uint8_t type;  /* 1=primary */
  uint8_t id[5]; /* "CD001" */
  uint8_t version;
  uint8_t _unused1;
  uint8_t sys_id[32];
  uint8_t vol_id[32];
  uint8_t _unused2[8];
  uint32_t vol_space_size_le;
  uint32_t vol_space_size_be;
  uint8_t _unused3[32];
  uint16_t vol_set_size_le;
  uint16_t vol_set_size_be;
  uint16_t vol_seq_num_le;
  uint16_t vol_seq_num_be;
  uint16_t logical_block_size_le;
  uint16_t logical_block_size_be;
  uint32_t path_table_size_le;
  uint32_t path_table_size_be;
  uint32_t path_table_lba_le;
  uint32_t path_table_opt_lba_le;
  /* ... resto ignorado ... */
} __attribute__((packed)) iso_pvd_t;

/* Directory record (subset). */
typedef struct {
  uint8_t length;
  uint8_t ext_attr_len;
  uint32_t extent_lba_le;
  uint32_t extent_lba_be;
  uint32_t data_len_le;
  uint32_t data_len_be;
  uint8_t recording_date[7];
  uint8_t flags;
  uint8_t file_unit_size;
  uint8_t interleave_gap;
  uint16_t vol_seq_num_le;
  uint16_t vol_seq_num_be;
  uint8_t name_len;
  uint8_t name[1]; /* variable */
} __attribute__((packed)) iso_dir_rec_t;

/* Estado del mount. */
static iso_pvd_t g_pvd;
static uint8_t g_pvd_buf[ISO_SECTOR_SIZE] __attribute__((aligned(16)));
static uint8_t g_dir_buf[ISO_SECTOR_SIZE] __attribute__((aligned(16)));
static uint32_t g_root_lba;
static uint32_t g_root_size;
static uint8_t g_drive;

int iso9660_mount(uint8_t drive) {
  g_drive = drive;

  bios_log("[ISO] antes de int13_read(LBA=%u)\n", ISO_PVD_LBA); // DEBUG

  if (int13_read(drive, ISO_PVD_LBA, 1, g_pvd_buf) != 0)
    return -1;

  bios_log("[ISO] int13_read OK\n"); // DEBUG

  /* Buscar el PVD entre los volume descriptors (LBA 16..). */
  uint8_t *p = g_pvd_buf;
  if (p[0] != 1 || !str_eq_n((char *)p + 1, "CD001", 5))
    return -1;

  copy_mem(&g_pvd, g_pvd_buf, sizeof(g_pvd));

  uint16_t lbs = g_pvd.logical_block_size_le;
  if (lbs != ISO_SECTOR_SIZE)
    return -1;

  /* Root directory record esta en offset 156 del PVD. */
  iso_dir_rec_t *root = (iso_dir_rec_t *)(g_pvd_buf + 156);
  g_root_lba = root->extent_lba_le;
  g_root_size = root->data_len_le;

  return 0;
}

/* Normaliza un nombre 8.3 al formato ISO9660: mayusculas, sin punto
 * separador, con ";1" al final para ficheros. */
static void iso_normalize(const char *name, char *out, uint32_t out_max) {
  uint32_t o = 0;
  uint32_t i = 0;
  while (name[i] && name[i] != '.' && o + 1 < out_max) {
    char c = name[i++];
    if (c >= 'a' && c <= 'z')
      c -= 32;
    out[o++] = c;
  }
  if (name[i] == '.') {
    i++;
    out[o++] = '.';
    while (name[i] && o + 2 < out_max) {
      char c = name[i++];
      if (c >= 'a' && c <= 'z')
        c -= 32;
      out[o++] = c;
    }
  }
  if (o + 2 < out_max) {
    out[o++] = ';';
    out[o++] = '1';
  }
  out[o] = 0;
}

int iso9660_find(const char *name_8_3_upper, iso_file_t *out) {
  if (!out)
    return -1;

  char want[32];
  iso_normalize(name_8_3_upper, want, sizeof(want));

  /* Recorrer el directorio raiz por sectores. */
  uint32_t remaining = g_root_size;
  uint32_t lba = g_root_lba;
  while (remaining > 0) {
    uint32_t chunk = remaining > ISO_SECTOR_SIZE ? ISO_SECTOR_SIZE : remaining;
    if (int13_read(g_drive, lba, 1, g_dir_buf) != 0)
      return -1;

    uint32_t off = 0;
    while (off < chunk) {
      iso_dir_rec_t *r = (iso_dir_rec_t *)(g_dir_buf + off);
      if (r->length == 0)
        break;

      if (r->name_len > 0 && r->name[0] != 0 && r->name[0] != 1) {
        char nm[64];
        uint32_t nl = r->name_len;
        if (nl >= sizeof(nm))
          nl = sizeof(nm) - 1;
        copy_mem(nm, r->name, nl);
        nm[nl] = 0;

        if (str_eq(nm, want)) {
          out->lba = r->extent_lba_le;
          out->size = r->data_len_le;
          return 0;
        }
      }
      off += r->length;
    }
    lba++;
    remaining -= chunk;
  }
  return -1;
}

int iso9660_read(uint8_t drive, const iso_file_t *f, uint32_t off, uint32_t len,
                 void *buf) {
  if (off >= f->size)
    return 0;
  if (off + len > f->size)
    len = f->size - off;

  uint32_t first_lba = f->lba + (off / ISO_SECTOR_SIZE);
  uint32_t intra = off % ISO_SECTOR_SIZE;
  uint32_t remaining = len;
  uint8_t *dst = (uint8_t *)buf;

  static uint8_t tmp[ISO_SECTOR_SIZE] __attribute__((aligned(16)));

  while (remaining > 0) {
    uint32_t to_read = ISO_SECTOR_SIZE - intra;
    if (to_read > remaining)
      to_read = remaining;

    if (int13_read(drive, first_lba, 1, tmp) != 0)
      return -1;

    copy_mem(dst, tmp + intra, to_read);
    dst += to_read;
    remaining -= to_read;
    intra = 0;
    first_lba++;
  }
  return (int)len;
}