// kernel/fat32.c
//
// Implementación de FAT32 con soporte read/write + LFN. Ver fat32.h
// para el contrato.
//
// Referencias:
//   - Microsoft "FAT32 File System Specification" (diciembre 2000)
//   - Linux fs/fat/ (para la validación y los offsets)
//   - OSDev wiki "FAT"

#include "fat32.h"
#include "heap.h"
#include "klog.h"
#include "string.h"
#include "uaccess.h" // EINVAL, EIO, ENOMEM, ENOENT, EISDIR, ENAMETOOLONG,
                     // EEXIST, ENOTEMPTY, ENOSPC
#include <stddef.h>

// ===========================================================================
// Estructuras on-disk
// ===========================================================================

struct __attribute__((packed)) fat32_bpb {
  uint8_t jump[3];             // 0
  uint8_t oem[8];              // 3
  uint16_t bytes_per_sector;   // 11
  uint8_t sectors_per_cluster; // 13
  uint16_t reserved_sectors;   // 14
  uint8_t num_fats;            // 16
  uint16_t root_entry_count;   // 17
  uint16_t total_sectors_16;   // 19
  uint8_t media;               // 21
  uint16_t fat_size_16;        // 22
  uint16_t sectors_per_track;  // 24
  uint16_t num_heads;          // 26
  uint32_t hidden_sectors;     // 28
  uint32_t total_sectors_32;   // 32
  uint32_t fat_size_32;        // 36
  uint16_t ext_flags;          // 40
  uint16_t fs_version;         // 42
  uint32_t root_cluster;       // 44
  uint16_t fs_info_sector;     // 48
  uint16_t backup_boot_sector; // 50
  uint8_t reserved[12];        // 52
  uint8_t drive_num;           // 64
  uint8_t reserved1;           // 65
  uint8_t boot_sig;            // 66
  uint32_t volume_id;          // 67
  uint8_t volume_label[11];    // 71
  uint8_t fs_type[8];          // 82 ("FAT32   ")
  uint8_t boot_code[420];      // 90
  uint16_t signature;          // 510 (0xAA55)
};
_Static_assert(sizeof(struct fat32_bpb) == 512, "fat32_bpb != 512 bytes");

// Directory entry (32 bytes, little-endian).
struct __attribute__((packed)) fat32_dirent {
  uint8_t name[11];
  uint8_t attr;
  uint8_t nt_reserved;
  uint8_t create_time_tenths;
  uint16_t create_time;
  uint16_t create_date;
  uint16_t last_access_date;
  uint16_t first_cluster_hi;
  uint16_t write_time;
  uint16_t write_date;
  uint16_t first_cluster_lo;
  uint32_t file_size;
};
_Static_assert(sizeof(struct fat32_dirent) == 32, "fat32_dirent != 32 bytes");

#define FAT_ATTR_READ_ONLY 0x01
#define FAT_ATTR_HIDDEN 0x02
#define FAT_ATTR_SYSTEM 0x04
#define FAT_ATTR_VOLUME_ID 0x08
#define FAT_ATTR_DIRECTORY 0x10
#define FAT_ATTR_ARCHIVE 0x20
#define FAT_ATTR_LFN 0x0F // READ_ONLY|HIDDEN|SYSTEM|VOLUME_ID

#define FAT_EOC_MIN 0x0FFFFFF8u
#define FAT_BAD 0x0FFFFFF7u
#define FAT_IS_VALID(c) ((c) >= 2 && (c) < FAT_EOC_MIN)

// [LFN] Límites de nombre largo. 255 chars como máximo según spec.
// 20 entries LFN + 1 short = 21 slots consecutivos necesarios.
#define FAT32_LFN_MAX_CHARS 255
#define FAT32_LFN_MAX_ENTRIES 20

// ===========================================================================
// Estado en memoria
// ===========================================================================
typedef struct {
  block_device_t *bdev;

  uint32_t bytes_per_sector;
  uint32_t sectors_per_cluster;
  uint32_t cluster_size;
  uint32_t reserved_sectors;
  uint32_t num_fats;
  uint32_t fat_size_sectors; // sectores por FAT
  uint32_t total_sectors;
  uint32_t root_cluster;
  uint32_t fat_start_sector;  // = reserved_sectors
  uint32_t data_start_sector; // = reserved + num_fats * fat_size
  uint32_t total_clusters;

  // [PR 3.1] Caché de la FAT#1 en memoria. NULL → path lento (bios por
  // consulta). Con caché, cada fat_get es un load de memoria.
  uint32_t *fat_cache;
  uint32_t fat_entries;

  // [PR 4] Estado de escritura.
  int fat_dirty;           // 1 si fat_cache difiere del disco
  int use_second_fat;      // 1 si ext_flags bit 7 no está puesto
  uint32_t fs_info_sector; // sector de FSInfo (no usado por ahora)
} fat32_fs_t;

typedef struct {
  fat32_fs_t *fs;
  uint32_t start_cluster;
  uint32_t size;
  int is_dir;

  // [PR 4.3] Ubicación del dirent en el directorio padre, capturada
  // al hacer lookup. Necesario para actualizar file_size y first_cluster
  // al escribir/truncar. dirent_lba == 0 significa "sin dirent"
  // (la raíz del FS o un nodo sintético).
  uint64_t dirent_lba;
  uint32_t dirent_off;
} fat32_node_priv_t;

// Resultado de iterar un directorio buscando un nombre.
typedef struct {
  const char *name;
  uint32_t cluster;
  uint32_t size;
  int is_dir;
  int found;
  uint64_t lba; // [PR 4.3] ubicación del dirent corto
  uint32_t off;
} fat32_lookup_ctx_t;

// [LFN] Posición física (LBA + offset) de una entry de 32 bytes.
typedef struct {
  uint64_t lba;
  uint32_t off;
} dirent_pos_t;

// Contexto para fat32_rename: localiza src y guarda su info completa
// (LFN chain + short + datos).
struct rename_src_ctx {
  const char *name;
  int found;
  uint64_t short_lba;
  uint32_t short_off;
  dirent_pos_t lfn[FAT32_LFN_MAX_ENTRIES];
  int lfn_count;
  uint32_t first_cluster;
  uint32_t size;
  uint8_t attr;
};

// ===========================================================================
// Forward declarations
// ===========================================================================
static vfs_fs_ops_t fat32_fs_ops;
static vfs_ops_t fat32_node_ops;
static int fat32_rename(fat32_fs_t *fs, uint32_t src_parent_cluster,
                        const char *src_name, uint32_t dst_parent_cluster,
                        const char *dst_name);

// ===========================================================================
// Helpers básicos
// ===========================================================================
static inline uint64_t cluster_to_sector(const fat32_fs_t *fs, uint32_t c) {
  return (uint64_t)fs->data_start_sector +
         (uint64_t)(c - 2) * fs->sectors_per_cluster;
}

// Lee el siguiente cluster de la FAT. Devuelve <0 en error.
static int64_t fat32_fat_get(const fat32_fs_t *fs, uint32_t cluster) {
  // [PR 3.1] Fast path con caché.
  if (fs->fat_cache) {
    if (cluster >= fs->fat_entries)
      return -EIO;
    return (int64_t)(fs->fat_cache[cluster] & 0x0FFFFFFFu);
  }

  // Fallback: lectura por sector.
  uint64_t byte_off = (uint64_t)cluster * 4;
  uint64_t sector = fs->fat_start_sector + byte_off / fs->bytes_per_sector;
  uint32_t within = (uint32_t)(byte_off % fs->bytes_per_sector);

  uint8_t sbuf[512];
  uint8_t *buf = sbuf;
  int heap = 0;
  if (fs->bytes_per_sector > sizeof(sbuf)) {
    buf = (uint8_t *)kmalloc(fs->bytes_per_sector);
    if (!buf)
      return -ENOMEM;
    heap = 1;
  }
  if (bdev_read(fs->bdev, sector, 1, buf) != 0) {
    if (heap)
      kfree(buf);
    return -EIO;
  }

  uint32_t val = (uint32_t)buf[within] | ((uint32_t)buf[within + 1] << 8) |
                 ((uint32_t)buf[within + 2] << 16) |
                 ((uint32_t)buf[within + 3] << 24);
  if (heap)
    kfree(buf);
  return (int64_t)(val & 0x0FFFFFFFu);
}

// Comparación de nombres FAT (case-insensitive ASCII).
static int fat32_name_eq(const char *a, const char *b) {
  while (*a && *b) {
    char ca = *a, cb = *b;
    if (ca >= 'A' && ca <= 'Z')
      ca += 32;
    if (cb >= 'A' && cb <= 'Z')
      cb += 32;
    if (ca != cb)
      return 0;
    a++;
    b++;
  }
  return *a == '\0' && *b == '\0';
}

// Convierte 8.3 (11 bytes space-padded) a string con '.' opcional.
// nt_reserved: bits 3/4 activan lowercase para base/ext.
static void fat32_format_short(const uint8_t *name11, uint8_t nt_reserved,
                               char *out, size_t outlen) {
  int base_low = (nt_reserved & 0x08) != 0;
  int ext_low = (nt_reserved & 0x10) != 0;

  size_t n = 0;
  for (int i = 0; i < 8 && n + 1 < outlen; i++) {
    uint8_t c = name11[i];
    if (c == ' ')
      break;
    if (c == 0x05)
      c = 0xE5;
    if (base_low && c >= 'A' && c <= 'Z')
      c += 32;
    out[n++] = (char)c;
  }
  int ext_len = 0;
  for (int i = 8; i < 11; i++)
    if (name11[i] != ' ')
      ext_len = i - 8 + 1;
  if (ext_len > 0 && n + 1 < outlen) {
    out[n++] = '.';
    for (int i = 0; i < ext_len && n + 1 < outlen; i++) {
      uint8_t c = name11[8 + i];
      if (ext_low && c >= 'A' && c <= 'Z')
        c += 32;
      out[n++] = (char)c;
    }
  }
  out[n] = '\0';
}

// Checksum de un 8.3 name para validar LFN entries.
static uint8_t fat32_lfn_checksum(const uint8_t *name11) {
  uint8_t sum = 0;
  for (int i = 0; i < 11; i++)
    sum = (uint8_t)(((sum & 1) ? 0x80 : 0) + (sum >> 1) + name11[i]);
  return sum;
}

// ===========================================================================
// [LFN] Helpers de nombres largos
// ===========================================================================

// Chars prohibidos en FAT. También aplica a LFN.
static int fat32_char_forbidden(uint8_t c) {
  if (c < 0x20)
    return 1;
  switch (c) {
  case '"':
  case '*':
  case '+':
  case ',':
  case '/':
  case ':':
  case ';':
  case '<':
  case '=':
  case '>':
  case '?':
  case '[':
  case '\\':
  case ']':
  case '|':
    return 1;
  default:
    return 0;
  }
}

// ¿Es "name" un 8.3 puro (todo mayúsculas, base<=8, ext<=3, sin raros)?
// 1 = sí, 0 = no (necesita LFN), -EINVAL = inválido de cualquier forma.
static int fat32_is_pure_83(const char *name) {
  if (!name || !name[0] || name[0] == '.')
    return -EINVAL;

  const char *dot = NULL;
  int base_len = 0, ext_len = 0;
  for (const char *p = name; *p; p++) {
    if (*p == '.') {
      if (dot)
        return 0;
      dot = p;
      continue;
    }
    uint8_t c = (uint8_t)*p;
    if (fat32_char_forbidden(c))
      return -EINVAL;
    if (c >= 'a' && c <= 'z')
      return 0;
    if (dot) {
      ext_len++;
      if (ext_len > 3)
        return 0;
    } else {
      base_len++;
      if (base_len > 8)
        return 0;
    }
  }
  if (base_len == 0)
    return -EINVAL;
  if (dot && ext_len == 0)
    return 0;
  return 1;
}

// Valida un nombre largo (>8.3 o con lowercase). Devuelve 0 si OK.
static int fat32_validate_long_name(const char *name) {
  if (!name || !name[0] || name[0] == '.')
    return -EINVAL;
  size_t n = strlen(name);
  if (n > FAT32_LFN_MAX_CHARS)
    return -ENAMETOOLONG;
  for (const char *p = name; *p; p++) {
    if (fat32_char_forbidden((uint8_t)*p))
      return -EINVAL;
  }
  return 0;
}

// Extrae base y ext de un nombre para construir el alias 8.3.
// Base hasta 8 chars, ext hasta 3. Convierte a mayúsculas y sustituye
// chars prohibidos por '_'.
static void fat32_extract_parts(const char *name, uint8_t *base_out,
                                int *base_len_out, uint8_t *ext_out,
                                int *ext_len_out) {
  const char *dot = NULL;
  for (const char *p = name; *p; p++) {
    if (*p == '.') {
      dot = p;
      break;
    }
  }

  int blen = 0;
  for (const char *p = name; p != dot && *p && blen < 8; p++) {
    uint8_t c = (uint8_t)*p;
    if (fat32_char_forbidden(c))
      c = '_';
    if (c >= 'a' && c <= 'z')
      c -= 32;
    base_out[blen++] = c;
  }
  *base_len_out = blen;

  int elen = 0;
  if (dot) {
    for (const char *p = dot + 1; *p && elen < 3; p++) {
      uint8_t c = (uint8_t)*p;
      if (fat32_char_forbidden(c))
        c = '_';
      if (c >= 'a' && c <= 'z')
        c -= 32;
      ext_out[elen++] = c;
    }
  }
  *ext_len_out = elen;
}

// ¿Existe ya un dirent con este nombre corto (11 bytes) en el dir?
// 1 = existe, 0 = libre, <0 = error.
static int fat32_short_name_exists(fat32_fs_t *fs, uint32_t dir_cluster,
                                   const uint8_t *name11) {
  uint8_t *cbuf = (uint8_t *)kmalloc(fs->cluster_size);
  if (!cbuf)
    return -ENOMEM;

  uint32_t cluster = dir_cluster;
  int guard = 0;
  while (FAT_IS_VALID(cluster) && guard < 0x100000) {
    guard++;
    uint64_t lba = cluster_to_sector(fs, cluster);
    if (bdev_read(fs->bdev, lba, fs->sectors_per_cluster, cbuf) != 0) {
      kfree(cbuf);
      return -EIO;
    }
    for (uint32_t off = 0; off + 32 <= fs->cluster_size; off += 32) {
      uint8_t first = cbuf[off];
      if (first == 0x00) {
        kfree(cbuf);
        return 0;
      }
      if (first == 0xE5)
        continue;
      if ((cbuf[off + 11] & 0x0F) == 0x0F)
        continue; // LFN
      if (memcmp(cbuf + off, name11, 11) == 0) {
        kfree(cbuf);
        return 1;
      }
    }
    int64_t next = fat32_fat_get(fs, cluster);
    if (next < 0 || (uint32_t)next >= FAT_EOC_MIN)
      break;
    cluster = (uint32_t)next;
  }
  kfree(cbuf);
  return 0;
}

// Genera un alias 8.3 para un nombre largo. Si cabe exacto y el alias
// natural no colisiona, lo usa. Si no, genera BASEPART~N.
static int fat32_gen_short_alias(fat32_fs_t *fs, uint32_t parent_cluster,
                                 const char *long_name, uint8_t *out11,
                                 uint8_t *out_nt) {
  uint8_t base[8];
  uint8_t ext[3];
  int blen, elen;
  fat32_extract_parts(long_name, base, &blen, ext, &elen);
  if (blen == 0)
    return -EINVAL;

  // Contar la forma natural (sin truncar).
  const char *dot = NULL;
  int raw_blen = 0, raw_elen = 0;
  for (const char *p = long_name; *p; p++) {
    if (*p == '.') {
      dot = p;
      break;
    }
    raw_blen++;
  }
  if (dot) {
    for (const char *p = dot + 1; *p; p++)
      raw_elen++;
  }

  // Intento 1: si cabe exacto (raw_blen<=8 && raw_elen<=3), usar eso.
  if (raw_blen <= 8 && raw_elen <= 3) {
    memset(out11, ' ', 11);
    for (int i = 0; i < blen; i++)
      out11[i] = base[i];
    for (int i = 0; i < elen; i++)
      out11[8 + i] = ext[i];
    if (!fat32_short_name_exists(fs, parent_cluster, out11)) {
      *out_nt = 0;
      return 0;
    }
  }

  // Intento 2: BASEPART~N.
  for (uint32_t n = 1; n < 1000000; n++) {
    char digits[8];
    int dlen = 0;
    uint32_t tmp = n;
    while (tmp > 0) {
      digits[dlen++] = '0' + (tmp % 10);
      tmp /= 10;
    }
    if (dlen == 0)
      digits[dlen++] = '0';

    int max_base = 8 - 1 - dlen;
    if (max_base < 1)
      continue;
    int use_base = (blen < max_base) ? blen : max_base;

    memset(out11, ' ', 11);
    for (int i = 0; i < use_base; i++)
      out11[i] = base[i];
    out11[use_base] = '~';
    for (int i = 0; i < dlen; i++)
      out11[use_base + 1 + i] = (uint8_t)digits[dlen - 1 - i];
    for (int i = 0; i < elen; i++)
      out11[8 + i] = ext[i];

    if (!fat32_short_name_exists(fs, parent_cluster, out11)) {
      *out_nt = 0;
      return 0;
    }
  }
  return -ENOSPC;
}

// ===========================================================================
// Helpers de escritura de FAT
// ===========================================================================

static int fat32_fat_set(fat32_fs_t *fs, uint32_t cluster, uint32_t value) {
  if (!fs->fat_cache)
    return -EIO;
  if (cluster >= fs->fat_entries)
    return -EINVAL;
  fs->fat_cache[cluster] = value & 0x0FFFFFFFu;
  fs->fat_dirty = 1;
  return 0;
}

int fat32_sync(void *fs_priv) {
  if (!fs_priv)
    return -EINVAL;
  fat32_fs_t *fs = (fat32_fs_t *)fs_priv;
  if (!fs->fat_cache || !fs->fat_dirty)
    return 0;

  if (bdev_write(fs->bdev, fs->fat_start_sector, fs->fat_size_sectors,
                 fs->fat_cache) != 0) {
    LOG_ERR("[FAT32] sync FAT#1 falló");
    return -EIO;
  }
  if (fs->use_second_fat) {
    uint64_t fat2_start = fs->fat_start_sector + fs->fat_size_sectors;
    if (bdev_write(fs->bdev, fat2_start, fs->fat_size_sectors, fs->fat_cache) !=
        0) {
      LOG_ERR("[FAT32] sync FAT#2 falló");
      return -EIO;
    }
  }
  if (bdev_flush(fs->bdev) != 0) {
    LOG_ERR("[FAT32] sync flush falló");
    return -EIO;
  }

  fs->fat_dirty = 0;
  LOG_DEBUG("[FAT32] sync OK (%llu KB)",
            (unsigned long long)((uint64_t)fs->fat_size_sectors *
                                 fs->bytes_per_sector / 1024));
  return 0;
}

static uint32_t fat32_alloc_cluster(fat32_fs_t *fs) {
  if (!fs->fat_cache)
    return 0;
  for (uint32_t c = 2; c < fs->fat_entries; c++) {
    if (fs->fat_cache[c] == 0) {
      fs->fat_cache[c] = 0x0FFFFFFFu; // EOC provisional
      fs->fat_dirty = 1;
      return c;
    }
  }
  return 0;
}

static void fat32_free_chain(fat32_fs_t *fs, uint32_t start) {
  if (!fs->fat_cache || !FAT_IS_VALID(start))
    return;

  uint32_t cluster = start;
  int guard = 0;
  while (FAT_IS_VALID(cluster) && guard < 0x1000000) {
    guard++;
    int64_t next = fat32_fat_get(fs, cluster);
    if (next < 0)
      break;
    fs->fat_cache[cluster] = 0;
    fs->fat_dirty = 1;
    if ((uint32_t)next >= FAT_EOC_MIN || (uint32_t)next == FAT_BAD)
      break;
    cluster = (uint32_t)next;
  }
}

// ===========================================================================
// Helpers de dirent
// ===========================================================================

typedef void (*dirent_modifier_t)(uint8_t *buf, void *ctx);

static int fat32_modify_dirent(fat32_fs_t *fs, uint64_t lba, uint32_t off,
                               dirent_modifier_t mod, void *ctx) {
  uint32_t bps = fs->bytes_per_sector;
  if (off + 32 > bps)
    return -EINVAL;

  uint8_t *buf = (uint8_t *)kmalloc(bps);
  if (!buf)
    return -ENOMEM;

  if (bdev_read(fs->bdev, lba, 1, buf) != 0) {
    kfree(buf);
    return -EIO;
  }
  mod(buf + off, ctx);
  int rc = (bdev_write(fs->bdev, lba, 1, buf) == 0) ? 0 : -EIO;
  kfree(buf);
  if (rc == 0)
    (void)bdev_flush(fs->bdev);
  return rc;
}

static void dirent_mark_deleted(uint8_t *e, void *ctx) {
  (void)ctx;
  e[0] = 0xE5;
}

// Modifica el campo file_size (bytes 28-31) de una entrada.
struct update_size_ctx {
  uint32_t new_size;
};

static void dirent_update_size(uint8_t *e, void *ctx) {
  struct update_size_ctx *c = (struct update_size_ctx *)ctx;
  e[28] = (uint8_t)(c->new_size & 0xFF);
  e[29] = (uint8_t)((c->new_size >> 8) & 0xFF);
  e[30] = (uint8_t)((c->new_size >> 16) & 0xFF);
  e[31] = (uint8_t)((c->new_size >> 24) & 0xFF);
}

// Modifica el campo first_cluster (hi en 20-21, lo en 26-27) de una
// entrada.
struct update_cluster_ctx {
  uint32_t new_cluster;
};

static void dirent_update_cluster(uint8_t *e, void *ctx) {
  struct update_cluster_ctx *c = (struct update_cluster_ctx *)ctx;
  e[20] = (uint8_t)((c->new_cluster >> 16) & 0xFF);
  e[21] = (uint8_t)((c->new_cluster >> 24) & 0xFF);
  e[26] = (uint8_t)(c->new_cluster & 0xFF);
  e[27] = (uint8_t)((c->new_cluster >> 8) & 0xFF);
}

// ===========================================================================
// Iteración de directorios
// ===========================================================================
//
// `cb` devuelve:
//   0  continuar
//   1  parar
//   <0 propagar error
//
// `lba`/`off` identifican la ubicación física de la SHORT entry.
// `lfn_chain`/`lfn_chain_len` describen las entries LFN que preceden a
// esa short entry (si hay). El llamante puede usar esa info para borrar
// la entry completa (LFN + short).
//
// [LFN] La cadena LFN se va acumulando a medida que se recorre el
// directorio. Cada vez que aparece una short entry, se entrega la
// cadena acumulada y se resetea.
typedef int (*fat32_dir_cb_t)(const char *name, const struct fat32_dirent *e,
                              uint64_t lba, uint32_t off,
                              const dirent_pos_t *lfn_chain, int lfn_count,
                              void *arg);

// [DIAG] Escritura segura a lfn_buf. Si el índice está fuera de rango,
// log + abort. Devuelve 0 si OK, -1 si OOB (el llamante debe limpiar).
static int lfn_write_checked(uint16_t *lfn_buf, int idx, uint16_t val,
                             const char *site, int seq, int pos) {
  if (idx < 0 || idx >= 256) {
    LOG_ERR("[fat_iter] LFN write OOB: site=%s seq=%d pos=%d idx=%d", site, seq,
            pos, idx);
    return -1;
  }
  lfn_buf[idx] = val;
  return 0;
}

static int fat32_iter_dir(fat32_fs_t *fs, uint32_t start_cluster,
                          fat32_dir_cb_t cb, void *arg) {
  if (!FAT_IS_VALID(start_cluster))
    return 0;

  if (!fs) {
    LOG_ERR("[fat_iter] fs == NULL");
    return -EINVAL;
  }
  if (fs->sectors_per_cluster == 0 || fs->bytes_per_sector == 0 ||
      fs->cluster_size == 0 || fs->cluster_size > 65536) {
    LOG_ERR("[fat_iter] fs corrupto: fs=%p bpS=%u spc=%u cs=%u bdev=%p",
            (void *)fs, fs->bytes_per_sector, fs->sectors_per_cluster,
            fs->cluster_size, (void *)fs->bdev);
    return -EIO;
  }

  uint8_t *cbuf = (uint8_t *)kmalloc(fs->cluster_size);
  if (!cbuf) {
    LOG_ERR("[fat_iter] kmalloc(%u) falló", fs->cluster_size);
    return -ENOMEM;
  }

  // [DIAG] Canarios alrededor de lfn_buf y name.
  uint64_t guard_lo = 0xDEADBEEFCAFEBABEULL;
  uint16_t lfn_buf[256];
  uint64_t guard_mid = 0xCAFEBABEDEADBEEFULL;
  char name[256];
  uint64_t guard_hi = 0xFEEDFACECAFED00DULL;

  int lfn_len = 0;
  int lfn_valid = 0;
  uint8_t lfn_checksum_exp = 0;

  // [LFN] Cadena de posiciones físicas de las entries LFN de la entry
  // corta actual. Se resetea cada vez que empieza una nueva cadena o se
  // entrega al callback.
  dirent_pos_t lfn_chain[FAT32_LFN_MAX_ENTRIES];
  int lfn_chain_count = 0;

  int rc = 0;
  uint32_t cluster = start_cluster;
  int guard = 0;

  while (FAT_IS_VALID(cluster) && guard < 65536) {
    guard++;
    uint64_t cluster_lba = cluster_to_sector(fs, cluster);
    if (bdev_read(fs->bdev, cluster_lba, fs->sectors_per_cluster, cbuf) != 0) {
      rc = -EIO;
      break;
    }

    if (guard_lo != 0xDEADBEEFCAFEBABEULL) {
      LOG_PANIC("[fat_iter] guard_lo roto: 0x%llx (off previo rompió stack)",
                (unsigned long long)guard_lo);
    }
    if (guard_mid != 0xCAFEBABEDEADBEEFULL) {
      LOG_PANIC("[fat_iter] guard_mid roto: 0x%llx (lfn_buf OOB)",
                (unsigned long long)guard_mid);
    }
    if (guard_hi != 0xFEEDFACECAFED00DULL) {
      LOG_PANIC("[fat_iter] guard_hi roto: 0x%llx (name OOB)",
                (unsigned long long)guard_hi);
    }

    int stop = 0;

    for (uint32_t off = 0; off + 32 <= fs->cluster_size; off += 32) {
      const uint8_t *e = cbuf + off;

      if (e[0] == 0x00) {
        // Fin de directorio.
        stop = 1;
        break;
      }
      if (e[0] == 0xE5) {
        // Entry borrada: cualquier cadena LFN pendiente queda huérfana.
        lfn_len = 0;
        lfn_valid = 0;
        lfn_chain_count = 0;
        continue;
      }

      uint8_t attr = e[11];

      if ((attr & FAT_ATTR_LFN) == FAT_ATTR_LFN) {
        uint8_t seq = e[0] & 0x3F;
        if (seq == 0 || seq > FAT32_LFN_MAX_ENTRIES) {
          lfn_len = 0;
          lfn_valid = 0;
          lfn_chain_count = 0;
          continue;
        }
        if (lfn_len == 0) {
          // Nueva cadena LFN.
          lfn_len = (int)seq * 13;
          lfn_checksum_exp = e[13];
          lfn_valid = 1;
          lfn_chain_count = 0;
        }
        int pos = (int)(seq - 1) * 13;
        if (pos + 13 > 256) {
          LOG_WARN("[fat_iter] LFN pos demasiado alto: seq=%d pos=%d", seq,
                   pos);
          lfn_valid = 0;
          lfn_chain_count = 0;
          continue;
        }

        // Registrar posición física de esta entry LFN.
        if (lfn_chain_count < FAT32_LFN_MAX_ENTRIES) {
          lfn_chain[lfn_chain_count].lba =
              cluster_lba + off / fs->bytes_per_sector;
          lfn_chain[lfn_chain_count].off = off % fs->bytes_per_sector;
          lfn_chain_count++;
        }

        // Copiar los chars UTF-16 a lfn_buf[pos..pos+12].
        for (int i = 0; i < 5; i++) {
          uint16_t v = (uint16_t)(e[1 + i * 2] | (e[2 + i * 2] << 8));
          if (lfn_write_checked(lfn_buf, pos + i, v, "chunk1", seq, pos) != 0) {
            lfn_valid = 0;
            lfn_chain_count = 0;
            goto next_entry;
          }
        }
        for (int i = 0; i < 6; i++) {
          uint16_t v = (uint16_t)(e[14 + i * 2] | (e[15 + i * 2] << 8));
          if (lfn_write_checked(lfn_buf, pos + 5 + i, v, "chunk2", seq, pos) !=
              0) {
            lfn_valid = 0;
            lfn_chain_count = 0;
            goto next_entry;
          }
        }
        for (int i = 0; i < 2; i++) {
          uint16_t v = (uint16_t)(e[28 + i * 2] | (e[29 + i * 2] << 8));
          if (lfn_write_checked(lfn_buf, pos + 11 + i, v, "chunk3", seq, pos) !=
              0) {
            lfn_valid = 0;
            lfn_chain_count = 0;
            goto next_entry;
          }
        }
      next_entry:
        continue;
      }

      // Short entry. Si hay LFN pendiente, validar checksum.
      if (lfn_valid) {
        uint8_t sum = fat32_lfn_checksum(e);
        if (sum == lfn_checksum_exp) {
          int n = 0;
          int maxc = lfn_len < 255 ? lfn_len : 255;
          for (int i = 0; i < maxc; i++) {
            if (i >= 256)
              break;
            uint16_t c = lfn_buf[i];
            if (c == 0x0000)
              break;
            if (n >= 255)
              break;
            name[n++] = (c < 0x80) ? (char)c : '?';
          }
          name[n] = '\0';
        } else {
          fat32_format_short(e, e[12], name, sizeof(name));
        }
      } else {
        fat32_format_short(e, e[12], name, sizeof(name));
      }
      lfn_len = 0;
      lfn_valid = 0;

      if (attr & FAT_ATTR_VOLUME_ID) {
        lfn_chain_count = 0;
        continue;
      }

      struct fat32_dirent ent;
      memcpy(&ent, e, 32);
      ent.first_cluster_hi = (uint16_t)(e[20] | (e[21] << 8));
      ent.first_cluster_lo = (uint16_t)(e[26] | (e[27] << 8));
      ent.file_size = (uint32_t)e[28] | ((uint32_t)e[29] << 8) |
                      ((uint32_t)e[30] << 16) | ((uint32_t)e[31] << 24);
      ent.attr = attr;

      uint64_t entry_lba = cluster_lba + off / fs->bytes_per_sector;
      uint32_t entry_off = off % fs->bytes_per_sector;

      int cb_rc =
          cb(name, &ent, entry_lba, entry_off, lfn_chain, lfn_chain_count, arg);
      lfn_chain_count = 0;

      if (cb_rc != 0) {
        rc = cb_rc;
        stop = 1;
        break;
      }
    }

    if (stop)
      break;

    // Avanzar al siguiente cluster del directorio.
    int64_t next = fat32_fat_get(fs, cluster);
    if (next < 0) {
      rc = (int)next;
      break;
    }
    if ((uint32_t)next >= FAT_EOC_MIN || (uint32_t)next == FAT_BAD)
      break;
    if (!FAT_IS_VALID((uint32_t)next)) {
      rc = -EIO;
      break;
    }
    cluster = (uint32_t)next;
  }

  kfree(cbuf);
  return rc;
}

// ===========================================================================
// Lectura de datos de archivo
// ===========================================================================
static int64_t fat32_read_data(fat32_node_priv_t *np, uint64_t offset,
                               size_t size, void *buf) {
  if (!np || !buf)
    return -EINVAL;
  if (np->is_dir)
    return -EISDIR;

  fat32_fs_t *fs = np->fs;
  uint32_t file_size = np->size;
  if (offset >= file_size)
    return 0;
  if (offset + size > file_size)
    size = file_size - offset;
  if (size == 0)
    return 0;

  uint32_t cl_idx = (uint32_t)(offset / fs->cluster_size);
  uint32_t within = (uint32_t)(offset % fs->cluster_size);

  uint32_t cluster = np->start_cluster;
  for (uint32_t i = 0; i < cl_idx; i++) {
    if (!FAT_IS_VALID(cluster))
      return -EIO;
    int64_t next = fat32_fat_get(fs, cluster);
    if (next < 0)
      return next;
    if ((uint32_t)next >= FAT_EOC_MIN)
      return -EIO;
    cluster = (uint32_t)next;
  }

  if (!FAT_IS_VALID(cluster))
    return -EIO;

  uint8_t *cbuf = (uint8_t *)kmalloc(fs->cluster_size);
  if (!cbuf)
    return -ENOMEM;

  uint8_t *out = (uint8_t *)buf;
  size_t remaining = size;

  while (remaining > 0 && FAT_IS_VALID(cluster)) {
    uint64_t sector = cluster_to_sector(fs, cluster);
    if (bdev_read(fs->bdev, sector, fs->sectors_per_cluster, cbuf) != 0) {
      kfree(cbuf);
      return -EIO;
    }
    size_t avail = fs->cluster_size - within;
    size_t to_copy = remaining < avail ? remaining : avail;
    memcpy(out, cbuf + within, to_copy);
    out += to_copy;
    remaining -= to_copy;
    within = 0;

    if (remaining == 0)
      break;

    int64_t next = fat32_fat_get(fs, cluster);
    if (next < 0) {
      kfree(cbuf);
      return next;
    }
    if ((uint32_t)next >= FAT_EOC_MIN)
      break;
    cluster = (uint32_t)next;
  }

  kfree(cbuf);
  return (int64_t)(size - remaining);
}

// ===========================================================================
// [PR 4.3] Escritura de datos.
// ===========================================================================
static int64_t fat32_write_data(fat32_node_priv_t *np, uint64_t offset,
                                size_t size, const void *buf) {
  if (!np || !buf)
    return -EINVAL;
  if (np->is_dir)
    return -EISDIR;

  fat32_fs_t *fs = np->fs;
  if (!fs->fat_cache) {
    LOG_ERR("[FAT32] write requiere caché de FAT");
    return -EIO;
  }
  if (size == 0)
    return 0;

  uint64_t end = offset + size;
  if (end > 0xFFFFFFFFULL)
    return -EINVAL;
  uint32_t new_size = (uint32_t)end;
  int size_changed = 0;
  if (new_size > np->size) {
    size_changed = 1;
  } else {
    new_size = np->size;
  }

  int starting_empty = !FAT_IS_VALID(np->start_cluster);
  uint32_t new_first_cluster = np->start_cluster;

  uint32_t cur;
  if (starting_empty) {
    cur = fat32_alloc_cluster(fs);
    if (cur == 0)
      return -ENOSPC;
    new_first_cluster = cur;
  } else {
    cur = np->start_cluster;
  }

  uint32_t cl_idx = (uint32_t)(offset / fs->cluster_size);
  uint32_t within = (uint32_t)(offset % fs->cluster_size);
  for (uint32_t i = 0; i < cl_idx; i++) {
    int64_t next = fat32_fat_get(fs, cur);
    if (next < 0)
      return next;
    if ((uint32_t)next >= FAT_EOC_MIN) {
      uint32_t new_c = fat32_alloc_cluster(fs);
      if (new_c == 0)
        return -ENOSPC;
      if (fat32_fat_set(fs, cur, new_c) != 0)
        return -EIO;
      cur = new_c;
    } else {
      cur = (uint32_t)next;
    }
  }

  uint8_t *cbuf = (uint8_t *)kmalloc(fs->cluster_size);
  if (!cbuf)
    return -ENOMEM;

  const uint8_t *in = (const uint8_t *)buf;
  size_t remaining = size;
  while (remaining > 0) {
    uint64_t sector = cluster_to_sector(fs, cur);

    if (bdev_read(fs->bdev, sector, fs->sectors_per_cluster, cbuf) != 0) {
      kfree(cbuf);
      return -EIO;
    }
    size_t avail = fs->cluster_size - within;
    size_t to_copy = remaining < avail ? remaining : avail;
    memcpy(cbuf + within, in, to_copy);
    if (bdev_write(fs->bdev, sector, fs->sectors_per_cluster, cbuf) != 0) {
      kfree(cbuf);
      return -EIO;
    }
    in += to_copy;
    remaining -= to_copy;
    within = 0;
    if (remaining == 0)
      break;

    int64_t next = fat32_fat_get(fs, cur);
    if (next < 0) {
      kfree(cbuf);
      return next;
    }
    if ((uint32_t)next >= FAT_EOC_MIN) {
      uint32_t new_c = fat32_alloc_cluster(fs);
      if (new_c == 0) {
        kfree(cbuf);
        return -ENOSPC;
      }
      if (fat32_fat_set(fs, cur, new_c) != 0) {
        fat32_free_chain(fs, new_c);
        kfree(cbuf);
        return -EIO;
      }
      cur = new_c;
    } else {
      cur = (uint32_t)next;
    }
  }
  kfree(cbuf);

  if (fat32_sync(fs) != 0)
    return -EIO;

  if (np->dirent_lba != 0) {
    if (starting_empty) {
      struct update_cluster_ctx cctx = {.new_cluster = new_first_cluster};
      if (fat32_modify_dirent(fs, np->dirent_lba, np->dirent_off,
                              dirent_update_cluster, &cctx) != 0)
        return -EIO;
      np->start_cluster = new_first_cluster;
    }
    if (size_changed) {
      struct update_size_ctx sctx = {.new_size = new_size};
      if (fat32_modify_dirent(fs, np->dirent_lba, np->dirent_off,
                              dirent_update_size, &sctx) != 0)
        return -EIO;
      np->size = new_size;
    }
  } else if (starting_empty) {
    np->start_cluster = new_first_cluster;
  }

  if (size_changed)
    np->size = new_size;

  return (int64_t)size;
}

// ===========================================================================
// [PR 4.3] Truncate.
// ===========================================================================
static int fat32_truncate_impl(fat32_node_priv_t *np, uint32_t new_size) {
  if (!np)
    return -EINVAL;
  if (np->is_dir)
    return -EISDIR;

  fat32_fs_t *fs = np->fs;
  if (!fs->fat_cache)
    return -EIO;

  if (new_size == np->size)
    return 0;
  if (new_size > np->size)
    return -EINVAL;

  if (new_size == 0 || !FAT_IS_VALID(np->start_cluster)) {
    if (FAT_IS_VALID(np->start_cluster)) {
      fat32_free_chain(fs, np->start_cluster);
    }
    np->start_cluster = 0;
    np->size = 0;
    if (np->dirent_lba) {
      struct update_cluster_ctx cctx = {.new_cluster = 0};
      if (fat32_modify_dirent(fs, np->dirent_lba, np->dirent_off,
                              dirent_update_cluster, &cctx) != 0)
        return -EIO;
      struct update_size_ctx sctx = {.new_size = 0};
      if (fat32_modify_dirent(fs, np->dirent_lba, np->dirent_off,
                              dirent_update_size, &sctx) != 0)
        return -EIO;
    }
    return fat32_sync(fs);
  }

  uint32_t keep_clusters = (new_size + fs->cluster_size - 1) / fs->cluster_size;
  uint32_t cur = np->start_cluster;
  for (uint32_t i = 0; i + 1 < keep_clusters; i++) {
    int64_t next = fat32_fat_get(fs, cur);
    if (next < 0)
      return (int)next;
    if ((uint32_t)next >= FAT_EOC_MIN)
      return -EIO;
    cur = (uint32_t)next;
  }

  int64_t next = fat32_fat_get(fs, cur);
  if (next >= 0 && (uint32_t)next < FAT_EOC_MIN) {
    fat32_free_chain(fs, (uint32_t)next);
    if (fat32_fat_set(fs, cur, 0x0FFFFFFFu) != 0)
      return -EIO;
  }
  np->size = new_size;
  if (np->dirent_lba) {
    struct update_size_ctx sctx = {.new_size = new_size};
    if (fat32_modify_dirent(fs, np->dirent_lba, np->dirent_off,
                            dirent_update_size, &sctx) != 0)
      return -EIO;
  }
  return fat32_sync(fs);
}

static int fat32_node_truncate(vfs_node_t *node, uint64_t new_size) {
  if (!node || !node->priv)
    return -EINVAL;
  if (new_size > 0xFFFFFFFFULL)
    return -EINVAL;
  int rc =
      fat32_truncate_impl((fat32_node_priv_t *)node->priv, (uint32_t)new_size);
  if (rc == 0) {
    fat32_node_priv_t *np = (fat32_node_priv_t *)node->priv;
    node->size = np->size;
  }
  return rc;
}

// ===========================================================================
// Callbacks comunes (firma extendida con LFN chain)
// ===========================================================================
static int fat32_lookup_cb(const char *name, const struct fat32_dirent *e,
                           uint64_t lba, uint32_t off,
                           const dirent_pos_t *lfn_chain, int lfn_count,
                           void *arg) {
  (void)lfn_chain;
  (void)lfn_count;
  fat32_lookup_ctx_t *ctx = (fat32_lookup_ctx_t *)arg;
  if (!fat32_name_eq(name, ctx->name))
    return 0;
  ctx->cluster = ((uint32_t)e->first_cluster_hi << 16) | e->first_cluster_lo;
  ctx->size = e->file_size;
  ctx->is_dir = (e->attr & FAT_ATTR_DIRECTORY) ? 1 : 0;
  ctx->lba = lba;
  ctx->off = off;
  ctx->found = 1;
  return 1;
}

// ===========================================================================
// Nombre 8.3
// ===========================================================================
// "myfile.txt" → 11 bytes uppercase space-padded + NT flags.
// Devuelve 0 si OK, -ENAMETOOLONG o -EINVAL.
static int fat32_make_short_name(const char *name, uint8_t *out11,
                                 uint8_t *out_nt_flags) {
  if (!name || !name[0] || name[0] == '.')
    return -EINVAL;

  memset(out11, ' ', 11);
  *out_nt_flags = 0;

  int base_len = 0;
  int ext_len = 0;
  int base_lower = 0;
  int ext_lower = 0;

  const char *p = name;
  for (; *p && *p != '.'; p++) {
    if (base_len >= 8)
      return -ENAMETOOLONG;
    uint8_t c = (uint8_t)*p;
    if (fat32_char_forbidden(c))
      return -EINVAL;
    if (c >= 'a' && c <= 'z') {
      c -= 32;
      base_lower = 1;
    }
    out11[base_len++] = c;
  }

  if (*p == '.') {
    p++;
    for (; *p; p++) {
      if (ext_len >= 3)
        return -ENAMETOOLONG;
      uint8_t c = (uint8_t)*p;
      if (fat32_char_forbidden(c))
        return -EINVAL;
      if (c >= 'a' && c <= 'z') {
        c -= 32;
        ext_lower = 1;
      }
      out11[8 + ext_len++] = c;
    }
  }

  if (base_len == 0)
    return -EINVAL;

  if (base_lower)
    *out_nt_flags |= 0x08;
  if (ext_lower)
    *out_nt_flags |= 0x10;

  return 0;
}

// ===========================================================================
// Búsqueda de slots libres consecutivos en un directorio
// ===========================================================================
// A diferencia de la versión 8.3, ahora necesitamos N slots consecutivos
// para alojar N entries LFN + 1 short. Extiende el directorio si hace
// falta. Devuelve el cluster y offset del PRIMER slot del bloque.
static int fat32_find_free_dirents(fat32_fs_t *fs, uint32_t dir_cluster,
                                   int count, uint32_t *cluster_out,
                                   uint32_t *off_out, int *zeroed_out) {
  if (!FAT_IS_VALID(dir_cluster) || count <= 0)
    return -EINVAL;
  if ((uint32_t)count * 32 > fs->cluster_size)
    return -ENAMETOOLONG;

  uint32_t cluster = dir_cluster;
  int guard = 0;
  while (FAT_IS_VALID(cluster) && guard < 0x100000) {
    guard++;
    uint64_t cluster_lba = cluster_to_sector(fs, cluster);
    uint8_t *cbuf = (uint8_t *)kmalloc(fs->cluster_size);
    if (!cbuf)
      return -ENOMEM;
    if (bdev_read(fs->bdev, cluster_lba, fs->sectors_per_cluster, cbuf) != 0) {
      kfree(cbuf);
      return -EIO;
    }

    int run = 0;
    uint32_t run_start = 0;
    int run_zeroed = 0;
    for (uint32_t off = 0; off + 32 <= fs->cluster_size; off += 32) {
      uint8_t first = cbuf[off];
      if (first == 0x00 || first == 0xE5) {
        if (run == 0) {
          run_start = off;
          run_zeroed = (first == 0x00);
        }
        run++;
        if (run == count) {
          *cluster_out = cluster;
          *off_out = run_start;
          *zeroed_out = run_zeroed;
          kfree(cbuf);
          return 0;
        }
      } else {
        run = 0;
      }
    }
    kfree(cbuf);

    int64_t next = fat32_fat_get(fs, cluster);
    if (next < 0)
      return -EIO;
    if ((uint32_t)next >= FAT_EOC_MIN) {
      // Directorio lleno: allocar un cluster más (todo zeroed = todos
      // los slots libres).
      uint32_t new_c = fat32_alloc_cluster(fs);
      if (new_c == 0)
        return -ENOSPC;
      if (fat32_fat_set(fs, cluster, new_c) != 0) {
        fat32_free_chain(fs, new_c);
        return -EIO;
      }
      uint8_t *zbuf = (uint8_t *)kzalloc(fs->cluster_size);
      if (!zbuf) {
        fat32_free_chain(fs, new_c);
        return -ENOMEM;
      }
      uint64_t zsec = cluster_to_sector(fs, new_c);
      if (bdev_write(fs->bdev, zsec, fs->sectors_per_cluster, zbuf) != 0) {
        kfree(zbuf);
        fat32_free_chain(fs, new_c);
        return -EIO;
      }
      kfree(zbuf);

      if ((uint32_t)count * 32 > fs->cluster_size) {
        fat32_free_chain(fs, new_c);
        return -ENAMETOOLONG;
      }
      *cluster_out = new_c;
      *off_out = 0;
      *zeroed_out = 1;
      return 0;
    }
    cluster = (uint32_t)next;
  }
  return -EIO;
}

// ===========================================================================
// Escritura de un dirent nuevo (short)
// ===========================================================================
struct dirent_write_ctx {
  const uint8_t *name11;
  uint8_t attr;
  uint8_t nt_flags;
  uint32_t first_cluster;
  uint32_t size;
};

static void dirent_write_full(uint8_t *e, void *ctx) {
  struct dirent_write_ctx *c = (struct dirent_write_ctx *)ctx;
  memset(e, 0, 32);
  memcpy(e, c->name11, 11);
  e[11] = c->attr;
  e[12] = c->nt_flags;
  e[13] = 0;
  e[14] = 0;
  e[15] = 0;
  e[16] = 0x21; // 1980-01-01
  e[17] = 0x00;
  e[18] = 0x21;
  e[19] = 0x00;
  e[20] = (uint8_t)((c->first_cluster >> 16) & 0xFF);
  e[21] = (uint8_t)((c->first_cluster >> 24) & 0xFF);
  e[22] = 0;
  e[23] = 0;
  e[24] = 0x21;
  e[25] = 0x00;
  e[26] = (uint8_t)(c->first_cluster & 0xFF);
  e[27] = (uint8_t)((c->first_cluster >> 8) & 0xFF);
  e[28] = (uint8_t)(c->size & 0xFF);
  e[29] = (uint8_t)((c->size >> 8) & 0xFF);
  e[30] = (uint8_t)((c->size >> 16) & 0xFF);
  e[31] = (uint8_t)((c->size >> 24) & 0xFF);
}

// [RENAME] Escribe N entries LFN + 1 short entry en el slot reservado
// por fat32_find_free_dirents. No toca la FAT ni crea clusters.
// El llamante debe haber reservado `lfn_count + 1` slots consecutivos
// a partir de (slot_cluster, slot_off).
static int fat32_write_dirent_chain(fat32_fs_t *fs, uint32_t slot_cluster,
                                    uint32_t slot_off, const char *name,
                                    const uint8_t *name11, uint8_t nt,
                                    uint8_t attr, int lfn_count,
                                    uint32_t first_cluster, uint32_t size) {
  uint8_t *cbuf = (uint8_t *)kmalloc(fs->cluster_size);
  if (!cbuf)
    return -ENOMEM;
  uint64_t cluster_lba = cluster_to_sector(fs, slot_cluster);
  if (bdev_read(fs->bdev, cluster_lba, fs->sectors_per_cluster, cbuf) != 0) {
    kfree(cbuf);
    return -EIO;
  }

  // LFN entries.
  if (lfn_count > 0) {
    uint8_t checksum = fat32_lfn_checksum(name11);
    size_t nlen = strlen(name);
    int total_chars = lfn_count * 13;
    // +1 slot para el NUL/0xFFFF si el nombre acaba justo en el último
    // char del último slot. Buffer en pila: máx 255 chars + 1.
    uint16_t u16[FAT32_LFN_MAX_CHARS + 1];

    for (size_t i = 0; i < nlen && i < FAT32_LFN_MAX_CHARS; i++)
      u16[i] = (uint16_t)(uint8_t)name[i];
    u16[nlen] = 0x0000;
    for (int i = (int)nlen + 1; i < total_chars; i++)
      u16[i] = 0xFFFF;

    for (int i = 0; i < lfn_count; i++) {
      uint8_t *e = cbuf + slot_off + i * 32;
      uint8_t seq = (uint8_t)(lfn_count - i);
      if (i == 0)
        seq |= 0x40;
      int base = ((seq & 0x3F) - 1) * 13;

      memset(e, 0, 32);
      e[0] = seq;
      e[11] = FAT_ATTR_LFN;
      e[12] = 0;
      e[13] = checksum;

      for (int k = 0; k < 5; k++) {
        e[1 + k * 2] = (uint8_t)(u16[base + k] & 0xFF);
        e[2 + k * 2] = (uint8_t)(u16[base + k] >> 8);
      }
      for (int k = 0; k < 6; k++) {
        e[14 + k * 2] = (uint8_t)(u16[base + 5 + k] & 0xFF);
        e[15 + k * 2] = (uint8_t)(u16[base + 5 + k] >> 8);
      }
      for (int k = 0; k < 2; k++) {
        e[28 + k * 2] = (uint8_t)(u16[base + 11 + k] & 0xFF);
        e[29 + k * 2] = (uint8_t)(u16[base + 11 + k] >> 8);
      }
    }
  }

  // Short entry.
  {
    uint8_t *e = cbuf + slot_off + lfn_count * 32;
    struct dirent_write_ctx wctx = {
        .name11 = name11,
        .attr = attr,
        .nt_flags = nt,
        .first_cluster = first_cluster,
        .size = size,
    };
    dirent_write_full(e, &wctx);
  }

  int rc =
      (bdev_write(fs->bdev, cluster_lba, fs->sectors_per_cluster, cbuf) == 0)
          ? 0
          : -EIO;
  kfree(cbuf);
  if (rc == 0)
    (void)bdev_flush(fs->bdev);
  return rc;
}

// ===========================================================================
// [LFN] Crear entry (archivo o directorio).
//
// Decide si el nombre cabe en 8.3 puro o necesita LFN. Si necesita LFN,
// genera un alias 8.3 único, reserva N+1 slots consecutivos y escribe
// N entries LFN (orden inverso) + 1 short entry.
// ===========================================================================
static int fat32_create_entry(fat32_fs_t *fs, uint32_t parent_cluster,
                              const char *name, int is_dir) {
  uint8_t name11[11];
  uint8_t nt = 0;
  int lfn_needed = 0;

  int pure = fat32_is_pure_83(name);
  if (pure < 0)
    return pure;
  if (pure == 1) {
    int rc = fat32_make_short_name(name, name11, &nt);
    if (rc != 0)
      return rc;
  } else {
    int v = fat32_validate_long_name(name);
    if (v != 0)
      return v;
    lfn_needed = 1;
    int rc = fat32_gen_short_alias(fs, parent_cluster, name, name11, &nt);
    if (rc != 0)
      return rc;
  }

  // Comprobar que no existe (por nombre largo o corto, es lo mismo).
  fat32_lookup_ctx_t lctx = {.name = name};
  int rc = fat32_iter_dir(fs, parent_cluster, fat32_lookup_cb, &lctx);
  if (rc < 0)
    return rc;
  if (lctx.found)
    return -EEXIST;

  // Alocar primer cluster si es directorio.
  uint32_t first_cluster = 0;
  if (is_dir) {
    first_cluster = fat32_alloc_cluster(fs);
    if (first_cluster == 0)
      return -ENOSPC;
  }

  // Calcular cuántas entries LFN hacen falta.
  int lfn_count = 0;
  if (lfn_needed) {
    size_t nlen = strlen(name);
    lfn_count = (int)((nlen + 12) / 13);
    if (lfn_count < 1)
      lfn_count = 1;
    if (lfn_count > FAT32_LFN_MAX_ENTRIES) {
      if (is_dir && first_cluster)
        fat32_free_chain(fs, first_cluster);
      return -ENAMETOOLONG;
    }
    if ((uint32_t)(lfn_count + 1) * 32 > fs->cluster_size) {
      if (is_dir && first_cluster)
        fat32_free_chain(fs, first_cluster);
      return -ENAMETOOLONG;
    }
  }
  int total_slots = lfn_count + 1;

  // Buscar `total_slots` slots consecutivos.
  uint32_t slot_cluster;
  uint32_t slot_off;
  int zeroed;
  rc = fat32_find_free_dirents(fs, parent_cluster, total_slots, &slot_cluster,
                               &slot_off, &zeroed);
  if (rc != 0) {
    if (is_dir && first_cluster)
      fat32_free_chain(fs, first_cluster);
    return rc;
  }

  // Sync de la FAT antes de escribir el dirent para evitar huérfanos.
  if (is_dir && first_cluster) {
    if (fat32_sync(fs) != 0) {
      fat32_free_chain(fs, first_cluster);
      return -EIO;
    }
  }

  // Leer el cluster del directorio entero y escribir los slots en un
  // solo bdev_write.
  uint8_t *cbuf = (uint8_t *)kmalloc(fs->cluster_size);
  if (!cbuf) {
    if (is_dir && first_cluster)
      fat32_free_chain(fs, first_cluster);
    return -ENOMEM;
  }
  uint64_t cluster_lba = cluster_to_sector(fs, slot_cluster);
  if (bdev_read(fs->bdev, cluster_lba, fs->sectors_per_cluster, cbuf) != 0) {
    kfree(cbuf);
    if (is_dir && first_cluster)
      fat32_free_chain(fs, first_cluster);
    return -EIO;
  }

  // Escribir LFN entries (orden físico: seq alto → seq bajo).
  if (lfn_needed) {
    uint8_t checksum = fat32_lfn_checksum(name11);

    size_t nlen = strlen(name);
    int total_chars = lfn_count * 13;
    // Buffer temporal de chars UTF-16. +1 para el NUL si no llega al
    // final del último slot.
    static uint16_t u16[(FAT32_LFN_MAX_ENTRIES + 1) * 13];

    for (size_t i = 0; i < nlen; i++)
      u16[i] = (uint16_t)(uint8_t)name[i];
    u16[nlen] = 0x0000;
    for (int i = (int)nlen + 1; i < total_chars; i++)
      u16[i] = 0xFFFF;

    for (int i = 0; i < lfn_count; i++) {
      uint8_t *e = cbuf + slot_off + i * 32;
      uint8_t seq = (uint8_t)(lfn_count - i); // N, N-1, ..., 1
      if (i == 0)
        seq |= 0x40; // bit 0x40 en la primera entry física
      int base = ((seq & 0x3F) - 1) * 13;

      memset(e, 0, 32);
      e[0] = seq;
      e[11] = FAT_ATTR_LFN;
      e[12] = 0;
      e[13] = checksum;

      for (int k = 0; k < 5; k++) {
        e[1 + k * 2] = (uint8_t)(u16[base + k] & 0xFF);
        e[2 + k * 2] = (uint8_t)(u16[base + k] >> 8);
      }
      for (int k = 0; k < 6; k++) {
        e[14 + k * 2] = (uint8_t)(u16[base + 5 + k] & 0xFF);
        e[15 + k * 2] = (uint8_t)(u16[base + 5 + k] >> 8);
      }
      for (int k = 0; k < 2; k++) {
        e[28 + k * 2] = (uint8_t)(u16[base + 11 + k] & 0xFF);
        e[29 + k * 2] = (uint8_t)(u16[base + 11 + k] >> 8);
      }
    }
  }

  // Escribir la short entry justo después de las LFN.
  {
    uint8_t *e = cbuf + slot_off + lfn_count * 32;
    struct dirent_write_ctx wctx = {
        .name11 = name11,
        .attr = (uint8_t)(is_dir ? FAT_ATTR_DIRECTORY : FAT_ATTR_ARCHIVE),
        .nt_flags = nt,
        .first_cluster = first_cluster,
        .size = 0,
    };
    dirent_write_full(e, &wctx);
  }

  if (bdev_write(fs->bdev, cluster_lba, fs->sectors_per_cluster, cbuf) != 0) {
    kfree(cbuf);
    if (is_dir && first_cluster)
      fat32_free_chain(fs, first_cluster);
    return -EIO;
  }
  kfree(cbuf);
  (void)bdev_flush(fs->bdev);

  // Si es dir, inicializar "." y "..".
  if (is_dir) {
    uint64_t dir_lba = cluster_to_sector(fs, first_cluster);
    uint8_t *dbuf = (uint8_t *)kzalloc(fs->cluster_size);
    if (!dbuf)
      return -ENOMEM;

    uint8_t dot11[11];
    memset(dot11, ' ', 11);
    dot11[0] = '.';
    memcpy(dbuf + 0, dot11, 11);
    dbuf[0 + 11] = FAT_ATTR_DIRECTORY;
    dbuf[0 + 20] = (uint8_t)((first_cluster >> 16) & 0xFF);
    dbuf[0 + 21] = (uint8_t)((first_cluster >> 24) & 0xFF);
    dbuf[0 + 26] = (uint8_t)(first_cluster & 0xFF);
    dbuf[0 + 27] = (uint8_t)((first_cluster >> 8) & 0xFF);

    uint32_t parent_for_dd =
        (parent_cluster == fs->root_cluster) ? 0 : parent_cluster;
    uint8_t dotdot11[11];
    memset(dotdot11, ' ', 11);
    dotdot11[0] = '.';
    dotdot11[1] = '.';
    memcpy(dbuf + 32, dotdot11, 11);
    dbuf[32 + 11] = FAT_ATTR_DIRECTORY;
    dbuf[32 + 20] = (uint8_t)((parent_for_dd >> 16) & 0xFF);
    dbuf[32 + 21] = (uint8_t)((parent_for_dd >> 24) & 0xFF);
    dbuf[32 + 26] = (uint8_t)(parent_for_dd & 0xFF);
    dbuf[32 + 27] = (uint8_t)((parent_for_dd >> 8) & 0xFF);

    if (bdev_write(fs->bdev, dir_lba, fs->sectors_per_cluster, dbuf) != 0) {
      kfree(dbuf);
      return -EIO;
    }
    kfree(dbuf);
  }

  return 0;
}

// ===========================================================================
// [LFN] Localizar y borrar una entry (con su cadena LFN).
// ===========================================================================
struct fat32_delete_ctx {
  const char *name;
  int found;
  uint64_t short_lba;
  uint32_t short_off;
  dirent_pos_t lfn[FAT32_LFN_MAX_ENTRIES];
  int lfn_count;
  uint32_t first_cluster;
  uint8_t attr;
};

static int fat32_delete_cb(const char *name, const struct fat32_dirent *e,
                           uint64_t lba, uint32_t off,
                           const dirent_pos_t *chain, int chain_len,
                           void *arg) {
  struct fat32_delete_ctx *ctx = (struct fat32_delete_ctx *)arg;
  if (!fat32_name_eq(name, ctx->name))
    return 0;
  ctx->found = 1;
  ctx->short_lba = lba;
  ctx->short_off = off;
  ctx->lfn_count =
      (chain_len < FAT32_LFN_MAX_ENTRIES) ? chain_len : FAT32_LFN_MAX_ENTRIES;
  for (int i = 0; i < ctx->lfn_count; i++)
    ctx->lfn[i] = chain[i];
  ctx->first_cluster =
      ((uint32_t)e->first_cluster_hi << 16) | e->first_cluster_lo;
  ctx->attr = e->attr;
  return 1;
}

static int fat32_dir_is_empty_cb(const char *name, const struct fat32_dirent *e,
                                 uint64_t lba, uint32_t off,
                                 const dirent_pos_t *lfn_chain, int lfn_count,
                                 void *arg) {
  (void)lba;
  (void)off;
  (void)e;
  (void)lfn_chain;
  (void)lfn_count;
  int *non_empty = (int *)arg;
  if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
    return 0;
  *non_empty = 1;
  return 1;
}

static int fat32_delete_entry(fat32_fs_t *fs, uint32_t parent_cluster,
                              const char *name) {
  struct fat32_delete_ctx ctx = {.name = name};
  int rc = fat32_iter_dir(fs, parent_cluster, fat32_delete_cb, &ctx);
  if (rc < 0)
    return rc;
  if (!ctx.found)
    return -ENOENT;

  int is_dir = (ctx.attr & FAT_ATTR_DIRECTORY) ? 1 : 0;

  if (is_dir) {
    int non_empty = 0;
    rc = fat32_iter_dir(fs, ctx.first_cluster, fat32_dir_is_empty_cb,
                        &non_empty);
    if (rc < 0)
      return rc;
    if (non_empty)
      return -ENOTEMPTY;
  }

  struct {
    int dummy;
  } ignored;

  // Marcar las LFN entries (si las hay).
  for (int i = 0; i < ctx.lfn_count; i++) {
    rc = fat32_modify_dirent(fs, ctx.lfn[i].lba, ctx.lfn[i].off,
                             dirent_mark_deleted, &ignored);
    if (rc != 0)
      return rc;
  }
  // Marcar la short entry.
  rc = fat32_modify_dirent(fs, ctx.short_lba, ctx.short_off,
                           dirent_mark_deleted, &ignored);
  if (rc != 0)
    return rc;

  if (FAT_IS_VALID(ctx.first_cluster)) {
    fat32_free_chain(fs, ctx.first_cluster);
  }

  return fat32_sync(fs);
}

// ===========================================================================
// Node ops
// ===========================================================================
static int64_t fat32_node_read(vfs_node_t *node, uint64_t offset, size_t size,
                               void *buf) {
  if (!node || !node->priv)
    return -EINVAL;
  return fat32_read_data((fat32_node_priv_t *)node->priv, offset, size, buf);
}

static int64_t fat32_node_write(vfs_node_t *node, uint64_t offset, size_t size,
                                const void *buf) {
  if (!node || !node->priv || !buf)
    return -EINVAL;
  int64_t rc =
      fat32_write_data((fat32_node_priv_t *)node->priv, offset, size, buf);
  if (rc >= 0) {
    fat32_node_priv_t *np = (fat32_node_priv_t *)node->priv;
    node->size = np->size;
  }
  return rc;
}

static int fat32_node_open(vfs_node_t *node, int flags) {
  (void)node;
  if ((flags & (O_WRONLY | O_RDWR)) && node && (node->flags & VFS_DIRECTORY))
    return -EISDIR;
  return 0;
}

static int fat32_node_close(vfs_node_t *node) {
  if (!node)
    return 0;
  fat32_node_priv_t *np = (fat32_node_priv_t *)node->priv;
  if (np) {
    kfree(np);
    node->priv = NULL;
  }
  return 0;
}

static int fat32_node_create(vfs_node_t *dir_node, const char *name,
                             int flags) {
  (void)flags;
  if (!dir_node || !dir_node->priv || !name)
    return -EINVAL;
  fat32_node_priv_t *np = (fat32_node_priv_t *)dir_node->priv;
  if (!np->is_dir)
    return -ENOTDIR;
  return fat32_create_entry(np->fs, np->start_cluster, name, 0);
}

static int fat32_node_mkdir(vfs_node_t *dir_node, const char *name) {
  if (!dir_node || !dir_node->priv || !name)
    return -EINVAL;
  fat32_node_priv_t *np = (fat32_node_priv_t *)dir_node->priv;
  if (!np->is_dir)
    return -ENOTDIR;
  return fat32_create_entry(np->fs, np->start_cluster, name, 1);
}

static int fat32_node_unlink(vfs_node_t *dir_node, const char *name) {
  if (!dir_node || !dir_node->priv || !name)
    return -EINVAL;
  fat32_node_priv_t *np = (fat32_node_priv_t *)dir_node->priv;
  if (!np->is_dir)
    return -ENOTDIR;
  return fat32_delete_entry(np->fs, np->start_cluster, name);
}

// ===========================================================================
// [PR 4.4] readdir.
// ===========================================================================
struct fat32_readdir_ctx {
  uint64_t target;
  uint64_t current;
  vfs_dirent_t *out;
  int found;
};

static int fat32_readdir_cb(const char *name, const struct fat32_dirent *e,
                            uint64_t lba, uint32_t off,
                            const dirent_pos_t *lfn_chain, int lfn_count,
                            void *arg) {
  (void)lba;
  (void)off;
  (void)lfn_chain;
  (void)lfn_count;
  struct fat32_readdir_ctx *ctx = (struct fat32_readdir_ctx *)arg;

  if ((name[0] == '.' && name[1] == '\0') ||
      (name[0] == '.' && name[1] == '.' && name[2] == '\0'))
    return 0;

  if (ctx->current == ctx->target) {
    size_t nlen = strlen(name);
    if (nlen >= sizeof(ctx->out->name))
      nlen = sizeof(ctx->out->name) - 1;
    for (size_t i = 0; i < nlen; i++)
      ctx->out->name[i] = name[i];
    ctx->out->name[nlen] = '\0';
    ctx->out->type = (e->attr & FAT_ATTR_DIRECTORY) ? VFS_DIRECTORY : VFS_FILE;
    ctx->out->size = e->file_size;
    ctx->found = 1;
    return 1;
  }
  ctx->current++;
  return 0;
}

static int fat32_node_readdir(vfs_node_t *dir_node, uint64_t index,
                              vfs_dirent_t *out) {
  if (!dir_node || !dir_node->priv || !out)
    return -EINVAL;
  fat32_node_priv_t *np = (fat32_node_priv_t *)dir_node->priv;
  if (!np->is_dir)
    return -ENOTDIR;

  out->name[0] = '\0';
  out->type = 0;
  out->size = 0;

  struct fat32_readdir_ctx ctx = {
      .target = index,
      .current = 0,
      .out = out,
      .found = 0,
  };
  int rc = fat32_iter_dir(np->fs, np->start_cluster, fat32_readdir_cb, &ctx);
  if (rc < 0)
    return rc;
  return 0;
}

static int fat32_node_rename(vfs_node_t *src_dir, const char *src_name,
                             vfs_node_t *dst_dir, const char *dst_name) {
  if (!src_dir || !src_dir->priv || !dst_dir || !dst_dir->priv)
    return -EINVAL;
  fat32_node_priv_t *snp = (fat32_node_priv_t *)src_dir->priv;
  fat32_node_priv_t *dnp = (fat32_node_priv_t *)dst_dir->priv;
  if (!snp->is_dir || !dnp->is_dir)
    return -ENOTDIR;
  if (snp->fs != dnp->fs)
    return -EXDEV;
  return fat32_rename(snp->fs, snp->start_cluster, src_name, dnp->start_cluster,
                      dst_name);
}

static vfs_ops_t fat32_node_ops = {.read = fat32_node_read,
                                   .write = fat32_node_write,
                                   .open = fat32_node_open,
                                   .close = fat32_node_close,
                                   .readable = NULL,
                                   .create = fat32_node_create,
                                   .mkdir = fat32_node_mkdir,
                                   .unlink = fat32_node_unlink,
                                   .rename = fat32_node_rename,
                                   .truncate = fat32_node_truncate,
                                   .readdir = fat32_node_readdir};

// ===========================================================================
// Construcción de nodos
// ===========================================================================
static vfs_node_t *fat32_make_node(fat32_fs_t *fs, uint32_t cluster,
                                   uint32_t size, int is_dir,
                                   const char *full_path, uint64_t dirent_lba,
                                   uint32_t dirent_off) {
  vfs_node_t *n = (vfs_node_t *)kzalloc(sizeof(vfs_node_t));
  if (!n)
    return NULL;
  fat32_node_priv_t *np = (fat32_node_priv_t *)kzalloc(sizeof(*np));
  if (!np) {
    kfree(n);
    return NULL;
  }
  np->fs = fs;
  np->start_cluster = cluster;
  np->size = size;
  np->is_dir = is_dir;
  np->dirent_lba = dirent_lba;
  np->dirent_off = dirent_off;

  size_t plen = strlen(full_path);
  if (plen >= sizeof(n->name))
    plen = sizeof(n->name) - 1;
  for (size_t i = 0; i < plen; i++)
    n->name[i] = full_path[i];
  n->name[plen] = '\0';

  n->flags = is_dir ? VFS_DIRECTORY : VFS_FILE;
  n->size = size;
  n->ops = &fat32_node_ops;
  n->fs = &fat32_fs_ops;
  n->priv = np;
  return n;
}

static int fat32_rename_find_cb(const char *name, const struct fat32_dirent *e,
                                uint64_t lba, uint32_t off,
                                const dirent_pos_t *chain, int chain_len,
                                void *arg) {
  struct rename_src_ctx *ctx = (struct rename_src_ctx *)arg;
  if (!fat32_name_eq(name, ctx->name))
    return 0;
  ctx->found = 1;
  ctx->short_lba = lba;
  ctx->short_off = off;
  ctx->lfn_count =
      (chain_len < FAT32_LFN_MAX_ENTRIES) ? chain_len : FAT32_LFN_MAX_ENTRIES;
  for (int i = 0; i < ctx->lfn_count; i++)
    ctx->lfn[i] = chain[i];
  ctx->first_cluster =
      ((uint32_t)e->first_cluster_hi << 16) | e->first_cluster_lo;
  ctx->size = e->file_size;
  ctx->attr = e->attr;
  return 1;
}

// Actualiza la entrada ".." del directorio en dir_cluster para apuntar
// a new_parent_cluster. Si new_parent es la raíz, escribe 0.
static int fat32_update_dotdot(fat32_fs_t *fs, uint32_t dir_cluster,
                               uint32_t new_parent_cluster) {
  // La entry ".." está en el offset 32 del primer cluster del directorio.
  uint64_t lba = cluster_to_sector(fs, dir_cluster);

  uint8_t buf[512];
  if (fs->bytes_per_sector > sizeof(buf))
    return -EINVAL;
  if (bdev_read(fs->bdev, lba, 1, buf) != 0)
    return -EIO;

  // Verificar que buf[32..43] es "..".
  if (buf[32] != '.' || buf[33] != '.')
    return -EIO;
  if ((buf[32 + 11] & FAT_ATTR_DIRECTORY) == 0)
    return -EIO;

  uint32_t parent =
      (new_parent_cluster == fs->root_cluster) ? 0 : new_parent_cluster;
  buf[32 + 20] = (uint8_t)((parent >> 16) & 0xFF);
  buf[32 + 21] = (uint8_t)((parent >> 24) & 0xFF);
  buf[32 + 26] = (uint8_t)(parent & 0xFF);
  buf[32 + 27] = (uint8_t)((parent >> 8) & 0xFF);

  if (bdev_write(fs->bdev, lba, 1, buf) != 0)
    return -EIO;
  (void)bdev_flush(fs->bdev);
  return 0;
}

// ¿Es `candidate` (o alguno de sus ancestros) el propio src_cluster?
// Se usa para rechazar mv /a /a/b y evitar ciclos.
static int fat32_is_ancestor_of(fat32_fs_t *fs, uint32_t src_cluster,
                                uint32_t candidate) {
  uint32_t cur = candidate;
  int guard = 0;
  while (FAT_IS_VALID(cur) && guard < 0x100000) {
    guard++;
    if (cur == src_cluster)
      return 1;
    if (cur == fs->root_cluster)
      return 0;
    // Leer ".." del directorio actual.
    uint64_t lba = cluster_to_sector(fs, cur);
    uint8_t buf[512];
    if (fs->bytes_per_sector > sizeof(buf))
      return -1;
    if (bdev_read(fs->bdev, lba, 1, buf) != 0)
      return -1;
    if (buf[32] != '.' || buf[33] != '.')
      return -1;
    uint16_t hi = (uint16_t)(buf[32 + 20] | (buf[32 + 21] << 8));
    uint16_t lo = (uint16_t)(buf[32 + 26] | (buf[32 + 27] << 8));
    uint32_t parent = ((uint32_t)hi << 16) | lo;
    if (parent == 0)
      cur = fs->root_cluster;
    else
      cur = parent;
  }
  return 0;
}

static int fat32_rename(fat32_fs_t *fs, uint32_t src_parent_cluster,
                        const char *src_name, uint32_t dst_parent_cluster,
                        const char *dst_name) {
  if (!fs || !src_name || !dst_name)
    return -EINVAL;

  // 1. Localizar src.
  struct rename_src_ctx src = {.name = src_name};
  int rc = fat32_iter_dir(fs, src_parent_cluster, fat32_rename_find_cb, &src);
  if (rc < 0)
    return rc;
  if (!src.found)
    return -ENOENT;

  // 2. Si es el mismo nombre y mismo padre, no hay nada que hacer.
  if (src_parent_cluster == dst_parent_cluster &&
      fat32_name_eq(src_name, dst_name))
    return 0;

  // 3. Verificar que dst no existe.
  fat32_lookup_ctx_t dstl = {.name = dst_name};
  rc = fat32_iter_dir(fs, dst_parent_cluster, fat32_lookup_cb, &dstl);
  if (rc < 0)
    return rc;
  if (dstl.found)
    return -EEXIST;

  // 4. Si es dir, rechazar mover a un descendiente de sí mismo.
  int is_dir = (src.attr & FAT_ATTR_DIRECTORY) ? 1 : 0;
  if (is_dir) {
    int ancestor =
        fat32_is_ancestor_of(fs, src.first_cluster, dst_parent_cluster);
    if (ancestor < 0)
      return -EIO;
    if (ancestor)
      return -EINVAL;
  }

  // 5. Generar name11 + nt para el nuevo nombre.
  uint8_t name11[11];
  uint8_t nt = 0;
  int lfn_needed = 0;
  int pure = fat32_is_pure_83(dst_name);
  if (pure < 0)
    return pure;
  if (pure == 1) {
    rc = fat32_make_short_name(dst_name, name11, &nt);
    if (rc != 0)
      return rc;
  } else {
    rc = fat32_validate_long_name(dst_name);
    if (rc != 0)
      return rc;
    lfn_needed = 1;
    rc = fat32_gen_short_alias(fs, dst_parent_cluster, dst_name, name11, &nt);
    if (rc != 0)
      return rc;
  }

  // 6. Calcular cuántas entries LFN hacen falta.
  int lfn_count = 0;
  if (lfn_needed) {
    size_t nlen = strlen(dst_name);
    lfn_count = (int)((nlen + 12) / 13);
    if (lfn_count < 1)
      lfn_count = 1;
    if (lfn_count > FAT32_LFN_MAX_ENTRIES)
      return -ENAMETOOLONG;
    if ((uint32_t)(lfn_count + 1) * 32 > fs->cluster_size)
      return -ENAMETOOLONG;
  }
  int total_slots = lfn_count + 1;

  // 7. Reservar slots en el padre destino.
  uint32_t slot_cluster, slot_off;
  int zeroed;
  rc = fat32_find_free_dirents(fs, dst_parent_cluster, total_slots,
                               &slot_cluster, &slot_off, &zeroed);
  if (rc != 0)
    return rc;

  // 8. Escribir el nuevo dirent con los datos de src.
  uint8_t attr = src.attr & ~(uint8_t)FAT_ATTR_ARCHIVE;
  if (!is_dir)
    attr |= FAT_ATTR_ARCHIVE;
  rc =
      fat32_write_dirent_chain(fs, slot_cluster, slot_off, dst_name, name11, nt,
                               attr, lfn_count, src.first_cluster, src.size);
  if (rc != 0)
    return rc;

  // 9. Borrar el dirent viejo (LFN + short).
  struct {
    int d;
  } ignored;
  for (int i = 0; i < src.lfn_count; i++) {
    rc = fat32_modify_dirent(fs, src.lfn[i].lba, src.lfn[i].off,
                             dirent_mark_deleted, &ignored);
    if (rc != 0)
      return rc;
  }
  rc = fat32_modify_dirent(fs, src.short_lba, src.short_off,
                           dirent_mark_deleted, &ignored);
  if (rc != 0)
    return rc;

  // 10. Si es dir y cambió de padre, actualizar ".." del directorio movido.
  if (is_dir && src_parent_cluster != dst_parent_cluster) {
    rc = fat32_update_dotdot(fs, src.first_cluster, dst_parent_cluster);
    if (rc != 0)
      return rc;
  }

  (void)bdev_flush(fs->bdev);
  return 0;
}

// ===========================================================================
// fs_ops: lookup
// ===========================================================================
static vfs_node_t *fat32_lookup(void *fs_priv, const char *path) {
  fat32_fs_t *fs = (fat32_fs_t *)fs_priv;
  if (!fs || !path || path[0] != '/')
    return NULL;

  const char *orig = path;
  path++;

  uint32_t cluster = fs->root_cluster;
  uint32_t size = 0;
  int is_dir = 1;
  uint64_t dirent_lba = 0;
  uint32_t dirent_off = 0;

  if (*path == '\0')
    return fat32_make_node(fs, cluster, 0, 1, orig, 0, 0);

  char comp[256];
  while (*path) {
    if (!is_dir)
      return NULL;

    const char *end = path;
    while (*end && *end != '/')
      end++;
    size_t clen = (size_t)(end - path);
    if (clen == 0 || clen >= sizeof(comp))
      return NULL;
    for (size_t i = 0; i < clen; i++)
      comp[i] = path[i];
    comp[clen] = '\0';

    fat32_lookup_ctx_t ctx = {.name = comp};
    int rc = fat32_iter_dir(fs, cluster, fat32_lookup_cb, &ctx);
    if (rc < 0 || !ctx.found)
      return NULL;

    cluster = ctx.cluster;
    size = ctx.size;
    is_dir = ctx.is_dir;
    dirent_lba = ctx.lba;
    dirent_off = ctx.off;

    path = end;
    if (*path == '/')
      path++;
  }

  return fat32_make_node(fs, cluster, size, is_dir, orig, dirent_lba,
                         dirent_off);
}

static vfs_fs_ops_t fat32_fs_ops = {
    .lookup = fat32_lookup,
    .name = "fat32",
};

vfs_fs_ops_t *fat32_get_vfs_ops(void) { return &fat32_fs_ops; }

// ===========================================================================
// Mount / umount
// ===========================================================================
int fat32_mount(block_device_t *bdev, void **fs_priv_out) {
  if (!bdev || !fs_priv_out)
    return -EINVAL;
  *fs_priv_out = NULL;

  if (bdev->sector_size != 512) {
    LOG_ERR("[FAT32] sector_size %u no soportado (solo 512)",
            bdev->sector_size);
    return -EINVAL;
  }

  uint8_t buf[512];
  if (bdev_read(bdev, 0, 1, buf) != 0)
    return -EIO;

  struct fat32_bpb *bpb = (struct fat32_bpb *)buf;

  if (bpb->signature != 0xAA55) {
    LOG_ERR("[FAT32] boot signature 0x%04x != 0xAA55", bpb->signature);
    return -EINVAL;
  }
  if (memcmp(bpb->fs_type, "FAT32   ", 8) != 0) {
    LOG_DEBUG("[FAT32] no es FAT32 (fs_type != 'FAT32   ')");
    return -EINVAL;
  }
  if (bpb->bytes_per_sector < 512 || bpb->bytes_per_sector > 4096 ||
      (bpb->bytes_per_sector & (bpb->bytes_per_sector - 1)) != 0) {
    LOG_ERR("[FAT32] bytes_per_sector inválido: %u", bpb->bytes_per_sector);
    return -EINVAL;
  }
  if (bpb->sectors_per_cluster == 0 ||
      (bpb->sectors_per_cluster & (bpb->sectors_per_cluster - 1)) != 0 ||
      bpb->sectors_per_cluster > 128) {
    LOG_ERR("[FAT32] sectors_per_cluster inválido: %u",
            bpb->sectors_per_cluster);
    return -EINVAL;
  }
  if (bpb->num_fats == 0 || bpb->num_fats > 4) {
    LOG_ERR("[FAT32] num_fats inválido: %u", bpb->num_fats);
    return -EINVAL;
  }
  if (bpb->reserved_sectors == 0) {
    LOG_ERR("[FAT32] reserved_sectors == 0");
    return -EINVAL;
  }
  if (bpb->root_entry_count != 0 || bpb->fat_size_16 != 0) {
    LOG_ERR("[FAT32] no es FAT32 (root_entry_count=%u fat_size_16=%u)",
            bpb->root_entry_count, bpb->fat_size_16);
    return -EINVAL;
  }
  if (bpb->fat_size_32 == 0) {
    LOG_ERR("[FAT32] fat_size_32 == 0");
    return -EINVAL;
  }
  if (bpb->root_cluster < 2) {
    LOG_ERR("[FAT32] root_cluster inválido: %u", bpb->root_cluster);
    return -EINVAL;
  }

  uint32_t total_sectors = bpb->total_sectors_16 != 0 ? bpb->total_sectors_16
                                                      : bpb->total_sectors_32;
  if (total_sectors == 0) {
    LOG_ERR("[FAT32] total_sectors == 0");
    return -EINVAL;
  }
  if ((uint64_t)total_sectors > bdev->num_sectors) {
    LOG_ERR("[FAT32] total_sectors (%u) > bdev (%llu)", total_sectors,
            (unsigned long long)bdev->num_sectors);
    return -EINVAL;
  }

  uint32_t data_start = (uint32_t)bpb->reserved_sectors +
                        (uint32_t)bpb->num_fats * bpb->fat_size_32;
  if (data_start >= total_sectors) {
    LOG_ERR("[FAT32] data_start (%u) >= total_sectors (%u)", data_start,
            total_sectors);
    return -EINVAL;
  }

  fat32_fs_t *fs = (fat32_fs_t *)kzalloc(sizeof(*fs));
  if (!fs)
    return -ENOMEM;

  fs->bdev = bdev;
  fs->bytes_per_sector = bpb->bytes_per_sector;
  fs->sectors_per_cluster = bpb->sectors_per_cluster;
  fs->cluster_size = bpb->bytes_per_sector * bpb->sectors_per_cluster;
  fs->reserved_sectors = bpb->reserved_sectors;
  fs->num_fats = bpb->num_fats;
  fs->fat_size_sectors = bpb->fat_size_32;
  fs->total_sectors = total_sectors;
  fs->root_cluster = bpb->root_cluster;
  fs->fat_start_sector = bpb->reserved_sectors;
  fs->data_start_sector = data_start;
  fs->total_clusters = (total_sectors - data_start) / bpb->sectors_per_cluster;
  fs->use_second_fat = ((bpb->ext_flags & 0x0080) == 0) ? 1 : 0;
  fs->fs_info_sector = bpb->fs_info_sector;
  fs->fat_dirty = 0;

  LOG_INFO("[FAT32] montado %s: %u B/sector, %u sectores/cluster "
           "(%u B/cluster), FAT %u sectores x%u, data en +%u, root=%u, "
           "%u clusters",
           bdev->name, fs->bytes_per_sector, fs->sectors_per_cluster,
           fs->cluster_size, fs->fat_size_sectors, fs->num_fats,
           fs->data_start_sector, fs->root_cluster, fs->total_clusters);

  uint64_t fat_bytes = (uint64_t)fs->fat_size_sectors * fs->bytes_per_sector;
  if (fat_bytes > 0 && fat_bytes <= 16 * 1024 * 1024) {
    fs->fat_cache = (uint32_t *)kmalloc((size_t)fat_bytes);
    if (fs->fat_cache) {
      if (bdev_read(fs->bdev, fs->fat_start_sector, fs->fat_size_sectors,
                    fs->fat_cache) == 0) {
        fs->fat_entries = (uint32_t)(fat_bytes / 4);
        LOG_INFO("[FAT32] FAT cacheada: %u KB, %u entradas",
                 (unsigned)(fat_bytes / 1024), fs->fat_entries);
      } else {
        kfree(fs->fat_cache);
        fs->fat_cache = NULL;
        LOG_WARN("[FAT32] no se pudo leer la FAT, usando path lento");
      }
    } else {
      LOG_WARN("[FAT32] sin memoria para cachear la FAT (%llu KB), "
               "usando path lento",
               (unsigned long long)(fat_bytes / 1024));
    }
  } else if (fat_bytes > 16 * 1024 * 1024) {
    LOG_WARN("[FAT32] FAT demasiado grande (%llu KB), usando path lento",
             (unsigned long long)(fat_bytes / 1024));
  }

  *fs_priv_out = fs;
  return 0;
}

void fat32_umount(void *fs_priv) {
  if (!fs_priv)
    return;
  fat32_fs_t *fs = (fat32_fs_t *)fs_priv;
  if (fs->fat_cache && fs->fat_dirty)
    (void)fat32_sync(fs);
  if (fs->fat_cache)
    kfree(fs->fat_cache);
  kfree(fs);
}

int fat32_mount_bdev(const char *bdev_name, const char *mount_path) {
  if (!bdev_name || !mount_path)
    return -EINVAL;
  block_device_t *bdev = blk_lookup(bdev_name);
  if (!bdev) {
    LOG_ERR("[FAT32] bdev '%s' no existe", bdev_name);
    return -ENOENT;
  }
  void *priv = NULL;
  int rc = fat32_mount(bdev, &priv);
  if (rc != 0)
    return rc;
  rc = vfs_mount(mount_path, &fat32_fs_ops, priv);
  if (rc != 0) {
    fat32_umount(priv);
    return rc;
  }
  return 0;
}

// ===========================================================================
// Hook de test para PR 4.1.
// ===========================================================================
int fat32_test_alloc_free_chain(void *fs_priv) {
  fat32_fs_t *fs = (fat32_fs_t *)fs_priv;
  if (!fs || !fs->fat_cache)
    return -1;

  uint32_t clusters[5] = {0};
  for (int i = 0; i < 5; i++) {
    clusters[i] = fat32_alloc_cluster(fs);
    if (clusters[i] == 0) {
      if (i > 0)
        fat32_free_chain(fs, clusters[0]);
      return -2;
    }
    for (int j = 0; j < i; j++) {
      if (clusters[j] == clusters[i]) {
        fat32_free_chain(fs, clusters[0]);
        return -3;
      }
    }
  }

  for (int i = 0; i < 4; i++) {
    if (fat32_fat_set(fs, clusters[i], clusters[i + 1]) != 0) {
      fat32_free_chain(fs, clusters[0]);
      return -4;
    }
  }

  uint32_t walked[5];
  uint32_t cur = clusters[0];
  for (int i = 0; i < 5; i++) {
    if (!FAT_IS_VALID(cur)) {
      fat32_free_chain(fs, clusters[0]);
      return -5;
    }
    walked[i] = cur;
    int64_t next = fat32_fat_get(fs, cur);
    if (next < 0) {
      fat32_free_chain(fs, clusters[0]);
      return -6;
    }
    cur = (uint32_t)next;
  }
  if (cur < FAT_EOC_MIN) {
    fat32_free_chain(fs, clusters[0]);
    return -7;
  }
  for (int i = 0; i < 5; i++) {
    if (walked[i] != clusters[i]) {
      fat32_free_chain(fs, clusters[0]);
      return -8;
    }
  }

  fat32_free_chain(fs, clusters[0]);

  for (int i = 0; i < 5; i++) {
    if (fs->fat_cache[clusters[i]] != 0)
      return -9;
  }

  return 0;
}