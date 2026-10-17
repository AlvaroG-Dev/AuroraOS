/* bootloader/bios/fat16.c
 *
 * Lector FAT16 con doble backend:
 *   - ISO: el sistema FAT16 vive dentro de un fichero ISO9660 (efi.img).
 *   - DISK: el sistema FAT16 vive en una partición MBR (arranque USB).
 *
 * Todas las lecturas se hacen por `g_reader(byte_off, len, buf)`, que
 * devuelve bytes del sistema FAT16 como si fuese un fichero plano.
 */
#include "fat16.h"
#include "bios.h"

typedef struct {
  uint16_t bytes_per_sector;
  uint8_t sectors_per_cluster;
  uint16_t reserved_sectors;
  uint8_t num_fats;
  uint16_t root_entries;
  uint16_t total_sectors_16;
  uint16_t fatsz16;
  uint32_t fat_start_lba; /* LBA relativo al inicio del FS, 512 B */
  uint32_t root_start_lba;
  uint32_t data_start_lba;
  uint32_t root_dir_sectors;
  uint32_t cluster_size;
} fat16_ctx_t;

static fat16_ctx_t g_fat;
static uint8_t g_drive;

/* Reader activo. */
static int (*g_reader)(uint32_t byte_off, uint32_t len, void *buf) = NULL;

/* Estado para el reader ISO. */
static iso_file_t g_efi_img;

/* Estado para el reader DISK. */
static uint32_t g_part_lba_512 = 0;
static uint32_t g_part_size = 0;

/* ---------------------------------------------------------------------
 * Readers
 * --------------------------------------------------------------------- */
static int reader_iso(uint32_t byte_off, uint32_t len, void *buf) {
  int r = iso9660_read(g_drive, &g_efi_img, byte_off, len, buf);
  return (r == (int)len) ? 0 : -1;
}

static int reader_disk(uint32_t byte_off, uint32_t len, void *buf) {
  if (len == 0)
    return 0;

  /* Clamp al tamaño de la partición (por si el MBR miente). */
  if (g_part_size && byte_off + len > g_part_size) {
    if (byte_off >= g_part_size)
      return -1;
    len = g_part_size - byte_off;
  }

  uint32_t sectors = (len + 511) / 512;
  uint32_t total_bytes = sectors * 512;
  uint32_t lba = g_part_lba_512 + byte_off / 512;

  /* Lectura larga: partir en trozos si hace falta. */
  if (sectors > 0xFFFF) {
    uint32_t done = 0;
    while (done < len) {
      uint32_t chunk = len - done;
      if (chunk > 0xFFFF * 512)
        chunk = 0xFFFF * 512;
      uint32_t secs = (chunk + 511) / 512;
      if (int13_read_512(g_drive, g_part_lba_512 + (byte_off + done) / 512,
                         (uint16_t)secs, (uint8_t *)buf + done) != 0)
        return -1;
      done += chunk;
    }
    return 0;
  }

  /* Caso común: leemos exactamente lo pedido (múltiplos de 512). */
  if (total_bytes == len)
    return int13_read_512(g_drive, lba, (uint16_t)sectors, buf);

  /* Caso peligroso: len NO es múltiplo de 512. int13h siempre escribe
   * sectors*512 bytes, así que hay que hacer bounce. Es el caso de
   * fat_next_cluster() que pide 2 bytes. */
  static uint8_t bounce[8192] __attribute__((aligned(16))); /* 16 sectores */
  if (sectors > sizeof(bounce) / 512) {
    /* Demasiado grande para bounce; poco probable aquí. */
    return -1;
  }

  if (int13_read_512(g_drive, lba, (uint16_t)sectors, bounce) != 0)
    return -1;

  /* byte_off dentro del sector ya fue restado por el caller: byte_off/512
   * es el LBA, y el offset exacto dentro del sector es byte_off & 511. */
  uint32_t intra = byte_off & 511;
  if (intra + len > total_bytes)
    return -1;
  copy_mem(buf, bounce + intra, len);
  return 0;
}

/* Lee 512 B por offset absoluto en el FS. Wrapper de conveniencia. */
static int fat_blk_read(uint32_t byte_off, uint32_t len, void *buf) {
  if (!g_reader)
    return -1;
  return g_reader(byte_off, len, buf);
}

/* ---------------------------------------------------------------------
 * Mount
 * --------------------------------------------------------------------- */
static int fat16_mount_common(void) {
  uint8_t vbr[512];
  if (fat_blk_read(0, 512, vbr) != 0)
    return -1;

  if (vbr[510] != 0x55 || vbr[511] != 0xAA)
    return -1;

  uint16_t bps = *(uint16_t *)(vbr + 11);
  uint8_t spc = vbr[13];
  uint16_t rsvd = *(uint16_t *)(vbr + 14);
  uint8_t nfats = vbr[16];
  uint16_t root_entries = *(uint16_t *)(vbr + 17);
  uint16_t ts16 = *(uint16_t *)(vbr + 19);
  uint16_t fatsz = *(uint16_t *)(vbr + 22);

  if (bps != 512 || spc == 0 || fatsz == 0)
    return -1;

  g_fat.bytes_per_sector = bps;
  g_fat.sectors_per_cluster = spc;
  g_fat.reserved_sectors = rsvd;
  g_fat.num_fats = nfats;
  g_fat.root_entries = root_entries;
  g_fat.total_sectors_16 = ts16;
  g_fat.fatsz16 = fatsz;
  g_fat.fat_start_lba = rsvd;
  g_fat.root_dir_sectors = (root_entries * 32 + bps - 1) / bps;
  g_fat.root_start_lba = rsvd + nfats * fatsz;
  g_fat.data_start_lba = g_fat.root_start_lba + g_fat.root_dir_sectors;
  g_fat.cluster_size = bps * spc;
  return 0;
}

int fat16_mount_iso(uint8_t drive, uint32_t iso_lba_2048,
                    uint32_t iso_size_bytes) {
  g_drive = drive;
  g_efi_img.lba = iso_lba_2048;
  g_efi_img.size = iso_size_bytes;
  g_reader = reader_iso;
  g_part_size = 0;
  return fat16_mount_common();
}

int fat16_mount_disk(uint8_t drive, uint32_t part_lba_512,
                     uint32_t part_size_bytes) {
  g_drive = drive;
  g_part_lba_512 = part_lba_512;
  g_part_size = part_size_bytes;
  g_reader = reader_disk;
  return fat16_mount_common();
}

/* ---------------------------------------------------------------------
 * Navegación FAT
 * --------------------------------------------------------------------- */
static uint16_t fat_next_cluster(uint16_t cluster) {
  uint32_t fat_off = g_fat.fat_start_lba * 512u + (uint32_t)cluster * 2u;
  uint16_t next;
  if (fat_blk_read(fat_off, 2, &next) != 0)
    return 0xFFFF;
  return next;
}

/* Convierte "AURORA/KERNEL.ELF" o "/AURORA/KERNEL.ELF" a "AURORA  KERNEL  " */
static void fat_make_8_3(const char *name, uint8_t out[11]) {
  for (int i = 0; i < 11; i++)
    out[i] = ' ';
  while (*name == '/' || *name == '\\')
    name++;
  uint32_t i = 0, o = 0;
  while (name[i] && name[i] != '.' && name[i] != '/' && name[i] != '\\' &&
         o < 8) {
    char c = name[i++];
    if (c >= 'a' && c <= 'z')
      c -= 32;
    out[o++] = c;
  }
  if (name[i] == '.') {
    i++;
    o = 8;
    while (name[i] && name[i] != '/' && name[i] != '\\' && o < 11) {
      char c = name[i++];
      if (c >= 'a' && c <= 'z')
        c -= 32;
      out[o++] = c;
    }
  }
}

int fat16_find(const char *path_8_3, fat_file_t *out) {
  if (!out || !path_8_3)
    return -1;

  const char *slash = 0;
  for (const char *p = path_8_3; *p; p++)
    if (*p == '/')
      slash = p;

  uint16_t dir_cluster = 0;
  uint8_t want[11], dir_want[11];

  if (slash) {
    char dirname[32];
    uint32_t dl = (uint32_t)(slash - path_8_3);
    if (dl >= sizeof(dirname))
      return -1;
    copy_mem(dirname, path_8_3, dl);
    dirname[dl] = 0;
    fat_make_8_3(dirname, dir_want);
    fat_make_8_3(slash + 1, want);
  } else {
    fat_make_8_3(path_8_3, want);
  }

  uint32_t root_bytes = g_fat.root_entries * 32u;
  uint32_t root_byte_off = g_fat.root_start_lba * 512u;
  uint8_t buf[512];
  uint32_t found_cluster = 0;

  if (slash) {
    for (uint32_t off = 0; off < root_bytes; off += 512) {
      if (fat_blk_read(root_byte_off + off, 512, buf) != 0)
        return -1;
      for (int i = 0; i < 16; i++) {
        uint8_t *e = buf + i * 32;
        if (e[0] == 0)
          goto root_done;
        if (e[0] == 0xE5)
          continue;
        if (e[11] & 0x08)
          continue;
        if (e[11] & 0x10) {
          if (compare_mem(e, dir_want, 11) == 0) {
            found_cluster = *(uint16_t *)(e + 26);
            goto root_done;
          }
        }
      }
    }
  root_done:
    if (found_cluster == 0)
      return -1;
    dir_cluster = found_cluster;
  }

  uint32_t cur = dir_cluster;
  uint32_t cluster_bytes = g_fat.cluster_size;
  uint8_t *cluster_buf = (uint8_t *)0x28000;

  for (int depth = 0; depth < 256; depth++) {
    if (dir_cluster == 0) {
      uint32_t byte_off = g_fat.root_start_lba * 512u;
      for (uint32_t off = 0; off < root_bytes; off += 512) {
        if (fat_blk_read(byte_off + off, 512, buf) != 0)
          return -1;
        for (int i = 0; i < 16; i++) {
          uint8_t *e = buf + i * 32;
          if (e[0] == 0)
            return -1;
          if (e[0] == 0xE5)
            continue;
          if (e[11] & 0x08)
            continue;
          if (compare_mem(e, want, 11) == 0) {
            out->first_cluster = *(uint16_t *)(e + 26);
            out->size = *(uint32_t *)(e + 28);
            return 0;
          }
        }
      }
      return -1;
    }

    uint32_t byte_off =
        g_fat.data_start_lba * 512u + (uint32_t)(cur - 2) * cluster_bytes;
    if (fat_blk_read(byte_off, cluster_bytes, cluster_buf) != 0)
      return -1;

    uint32_t entries = cluster_bytes / 32;
    for (uint32_t i = 0; i < entries; i++) {
      uint8_t *e = cluster_buf + i * 32;
      if (e[0] == 0)
        return -1;
      if (e[0] == 0xE5)
        continue;
      if (e[11] & 0x08)
        continue;
      if (compare_mem(e, want, 11) == 0) {
        out->first_cluster = *(uint16_t *)(e + 26);
        out->size = *(uint32_t *)(e + 28);
        return 0;
      }
    }

    cur = fat_next_cluster(cur);
    if (cur >= 0xFFF8)
      return -1;
  }
  return -1;
}

int fat16_read(uint8_t drive, const fat_file_t *f, uint32_t off, uint32_t len,
               void *buf) {
  (void)drive;
  if (off >= f->size)
    return 0;
  if (off + len > f->size)
    len = f->size - off;

  uint32_t cluster_size = g_fat.cluster_size;
  uint32_t cluster_idx = off / cluster_size;
  uint32_t intra = off % cluster_size;

  uint16_t cur = (uint16_t)f->first_cluster;
  for (uint32_t i = 0; i < cluster_idx; i++) {
    cur = fat_next_cluster(cur);
    if (cur >= 0xFFF8)
      return -1;
  }

  uint8_t *dst = (uint8_t *)buf;
  uint32_t remaining = len;

  while (remaining > 0) {
    uint32_t byte_off =
        g_fat.data_start_lba * 512u + (uint32_t)(cur - 2) * cluster_size;
    uint32_t to_read = cluster_size - intra;
    if (to_read > remaining)
      to_read = remaining;

    uint8_t *tmp = (uint8_t *)0x2C000;
    if (fat_blk_read(byte_off, cluster_size, tmp) != 0)
      return -1;
    copy_mem(dst, tmp + intra, to_read);

    dst += to_read;
    remaining -= to_read;
    intra = 0;

    if (remaining > 0) {
      cur = fat_next_cluster(cur);
      if (cur >= 0xFFF8)
        return -1;
    }
  }
  return (int)len;
}

uint32_t fat16_data_start_lba(void) { return g_fat.data_start_lba; }
uint32_t fat16_cluster_size(void) { return g_fat.cluster_size; }