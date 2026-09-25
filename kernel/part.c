// kernel/part.c
//
// Implementación del partition layer. Ver part.h para el contrato.
//
// Referencias:
//   - MBR: Wikipedia "Master boot record"
//   - GPT: UEFI spec, capítulo 5 "GUID Partition Table (GPT) Disk Layout"
//   - Linux fs/partitions/efi.c para los GUIDs y el byte order

#include "part.h"
#include "heap.h"
#include "klog.h"
#include "string.h"
#include "uaccess.h" // EINVAL, ERANGE, EIO, ENOMEM

// ===========================================================================
// Estructuras on-disk
// ===========================================================================

// --- MBR ---
struct __attribute__((packed)) mbr_entry {
  uint8_t status; // 0x00 inactiva, 0x80 activa
  uint8_t chs_first[3];
  uint8_t type; // 0x00 = vacío, 0xEE = protective MBR
  uint8_t chs_last[3];
  uint32_t lba_first;   // LBA del primer sector (LE)
  uint32_t num_sectors; // número de sectores (LE)
};

struct __attribute__((packed)) mbr_sector {
  uint8_t boot_code[446];
  struct mbr_entry entries[4];
  uint16_t signature; // 0xAA55
};

_Static_assert(sizeof(struct mbr_entry) == 16, "mbr_entry != 16 bytes");
_Static_assert(sizeof(struct mbr_sector) == 512, "mbr_sector != 512 bytes");

// --- GPT ---
struct __attribute__((packed)) gpt_header {
  uint8_t signature[8]; // "EFI PART"
  uint32_t revision;
  uint32_t header_size;
  uint32_t header_crc32;
  uint32_t reserved;
  uint64_t current_lba;
  uint64_t backup_lba;
  uint64_t first_usable_lba;
  uint64_t last_usable_lba;
  uint8_t disk_guid[16];
  uint64_t entries_lba;
  uint32_t num_entries;
  uint32_t entry_size;
  uint32_t entries_crc32;
};

struct __attribute__((packed)) gpt_entry {
  uint8_t type_guid[16];
  uint8_t unique_guid[16];
  uint64_t first_lba;
  uint64_t last_lba; // inclusive
  uint64_t attributes;
  uint8_t name[72]; // UTF-16LE
};

_Static_assert(sizeof(struct gpt_header) == 92, "gpt_header != 92 bytes");
_Static_assert(sizeof(struct gpt_entry) == 128, "gpt_entry != 128 bytes");

// ===========================================================================
// block_ops_t de una partición.
//
// El bio se envía al disco padre con el LBA ajustado por start_lba.
// Para drivers asíncronos el bio queda en vuelo, pero el driver ha
// copiado ya sus campos a estado propio en submit(). Restauramos los
// campos del bio justo tras el retorno, así que end_io() ve el bio
// en su forma original.
// ===========================================================================
static int part_submit(block_device_t *part, bio_t *bio) {
  block_device_t *parent = part->parent;
  if (!parent || !parent->ops || !parent->ops->submit)
    return -EIO;

  // Defensa: el llamante ya validó, pero comprobamos por overflow.
  if (bio->lba > part->num_sectors || bio->count > part->num_sectors - bio->lba)
    return -ERANGE;

  uint64_t orig_lba = bio->lba;
  block_device_t *orig_bdev = bio->bdev;

  bio->lba = orig_lba + part->start_lba;
  bio->bdev = parent;

  int rc = parent->ops->submit(parent, bio);

  bio->lba = orig_lba;
  bio->bdev = orig_bdev;

  return rc;
}

static block_ops_t part_ops = {
    .submit = part_submit,
    .flush = NULL,
    .dump = NULL,
};

// ===========================================================================
// Helpers
// ===========================================================================

// ¿El buffer de 512 bytes parece un boot sector de FS?
//
// Esta heurística evita un falso positivo muy común: un disco con un
// filesystem FAT crudo (USB formateado sin tabla) tiene 0x55AA en
// bytes 510-511 y, sin este chequeo, intentaríamos interpretar bytes
// aleatorios como una tabla MBR.
static int looks_like_fs_boot_sector(const uint8_t *buf) {
  // FAT12/FAT16: "FAT12   " o "FAT16   " en offset 54.
  if (memcmp(buf + 54, "FAT12   ", 8) == 0)
    return 1;
  if (memcmp(buf + 54, "FAT16   ", 8) == 0)
    return 1;
  // FAT32: "FAT32   " en offset 82.
  if (memcmp(buf + 82, "FAT32   ", 8) == 0)
    return 1;
  // NTFS / exFAT: "NTFS    " / "EXFAT   " en offset 3.
  if (memcmp(buf + 3, "NTFS    ", 8) == 0)
    return 1;
  if (memcmp(buf + 3, "EXFAT   ", 8) == 0)
    return 1;
  return 0;
}

// Rangos válidos: start > 0, count > 0, dentro del disco.
static int validate_range(block_device_t *disk, uint64_t start,
                          uint64_t count) {
  if (count == 0)
    return -EINVAL;
  if (start == 0)
    return -EINVAL; // LBA 0 es siempre la tabla
  if (start >= disk->num_sectors)
    return -EINVAL;
  if (count > disk->num_sectors - start)
    return -EINVAL;
  return 0;
}

// ¿Solapa con alguna partición ya registrada del mismo disco?
static int check_overlap(block_device_t *disk, uint64_t start, uint64_t count) {
  uint64_t p_end = start + count;
  int n = blk_count();
  for (int i = 0; i < n; i++) {
    block_device_t *b = blk_get_by_index(i);
    if (!b || !b->is_partition || b->parent != disk)
      continue;
    uint64_t b_start = b->start_lba;
    uint64_t b_end = b_start + b->num_sectors;
    if (start < b_end && p_end > b_start)
      return -EINVAL;
  }
  return 0;
}

// Registra una partición. Devuelve 0 si OK.
static int register_partition(block_device_t *disk, uint64_t start,
                              uint64_t count, int idx) {
  block_device_t *p = (block_device_t *)kzalloc(sizeof(block_device_t));
  if (!p)
    return -ENOMEM;

  // Nombre: "<parent><idx>". disk->name cabe en 15 chars; dejamos margen.
  int n = 0;
  for (int i = 0; disk->name[i] && n < (int)sizeof(p->name) - 4; i++)
    p->name[n++] = disk->name[i];
  // Índice decimal.
  char num[8];
  int nn = 0;
  if (idx == 0)
    num[nn++] = '0';
  while (idx > 0) {
    num[nn++] = (char)('0' + idx % 10);
    idx /= 10;
  }
  while (nn > 0 && n < (int)sizeof(p->name) - 1)
    p->name[n++] = num[--nn];
  p->name[n] = '\0';

  p->num_sectors = count;
  p->sector_size = disk->sector_size;
  p->is_read_only = disk->is_read_only;
  p->is_partition = 1;
  p->parent = disk;
  p->start_lba = start;
  p->ops = &part_ops;
  p->private_data = NULL;

  int rc = blk_register(p);
  if (rc < 0) {
    kfree(p);
    return rc;
  }
  return 0;
}

// Log compacto de una partición registrada.
static void log_partition(block_device_t *disk, int idx, uint64_t start,
                          uint64_t count, const char *type_name) {
  uint64_t mb = count * (uint64_t)disk->sector_size / (1024ULL * 1024ULL);
  LOG_INFO("[PART] %s%d: LBA %llu..%llu (%llu MB, %s)", disk->name, idx,
           (unsigned long long)start, (unsigned long long)(start + count - 1),
           (unsigned long long)mb, type_name);
}

// ===========================================================================
// Nombres de tipo MBR
// ===========================================================================
static const char *mbr_type_name(uint8_t type) {
  switch (type) {
  case 0x01:
    return "FAT12";
  case 0x04:
    return "FAT16 <32MB";
  case 0x05:
    return "Extended";
  case 0x06:
    return "FAT16";
  case 0x07:
    return "NTFS/exFAT";
  case 0x0B:
    return "FAT32";
  case 0x0C:
    return "FAT32 LBA";
  case 0x0E:
    return "FAT16 LBA";
  case 0x0F:
    return "Extended LBA";
  case 0x82:
    return "Linux swap";
  case 0x83:
    return "Linux";
  case 0x85:
    return "Linux extended";
  case 0x8E:
    return "Linux LVM";
  case 0xA5:
    return "FreeBSD";
  case 0xAF:
    return "macOS HFS+";
  case 0xEE:
    return "GPT protective";
  case 0xEF:
    return "EFI System";
  default:
    return "?";
  }
}

// ===========================================================================
// GUIDs GPT (formato on-disk: 3 campos LE + 2 campos BE)
// ===========================================================================
static int guid_is_zero(const uint8_t *g) {
  for (int i = 0; i < 16; i++)
    if (g[i] != 0)
      return 0;
  return 1;
}

static const char *gpt_type_name(const uint8_t *guid) {
  // EFI System: C12A7328-F81F-11D2-BA4B-00A0C93EC93B
  static const uint8_t EFI_SYSTEM[16] = {0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8,
                                         0xD2, 0x11, 0xBA, 0x4B, 0x00, 0xA0,
                                         0xC9, 0x3E, 0xC9, 0x3B};
  if (memcmp(guid, EFI_SYSTEM, 16) == 0)
    return "EFI System";

  // Linux filesystem: 0FC63DAF-8483-4772-8E79-3D69D8477DE4
  static const uint8_t LINUX_FS[16] = {0xAF, 0x3D, 0xC6, 0x0F, 0x83, 0x84,
                                       0x72, 0x47, 0x8E, 0x79, 0x3D, 0x69,
                                       0xD8, 0x47, 0x7D, 0xE4};
  if (memcmp(guid, LINUX_FS, 16) == 0)
    return "Linux filesystem";

  // Linux swap: 0657FD6D-A4AB-43C4-84E5-0933C84B4F4F
  static const uint8_t LINUX_SWAP[16] = {0x6D, 0xFD, 0x57, 0x06, 0xAB, 0xA4,
                                         0xC4, 0x43, 0x84, 0xE5, 0x09, 0x33,
                                         0xC8, 0x4B, 0x4F, 0x4F};
  if (memcmp(guid, LINUX_SWAP, 16) == 0)
    return "Linux swap";

  // Microsoft basic data: EBD0A0A2-B9E5-4433-87C0-68B6B72699C7
  static const uint8_t MS_BASIC[16] = {0xA2, 0xA0, 0xD0, 0xEB, 0xE5, 0xB9,
                                       0x33, 0x44, 0x87, 0xC0, 0x68, 0xB6,
                                       0xB7, 0x26, 0x99, 0xC7};
  if (memcmp(guid, MS_BASIC, 16) == 0)
    return "Microsoft basic data";

  return "unknown";
}

// ===========================================================================
// EBR chain (particiones lógicas dentro de una extended MBR)
//
// El extended partition empieza en `ext_start` y tiene `ext_count`
// sectores. Cada EBR vive dentro de ese rango:
//   - entry[0]: partición lógica, LBA relativo a ext_start.
//   - entry[1]: siguiente EBR, LBA relativo a ext_start.
//
// Los índices de partición continúan desde *next_idx (>= 5).
// ===========================================================================
static int part_scan_ebr(block_device_t *disk, uint64_t ext_start,
                         uint64_t ext_count, int *next_idx) {
  if (*next_idx < 5)
    *next_idx = 5;

  uint64_t ext_end = ext_start + ext_count;
  uint64_t cur = ext_start;
  int registered = 0;
  int guard = 0;

  while (cur < ext_end && guard < 64) {
    uint8_t buf[512];
    if (bdev_read(disk, cur, 1, buf) != 0)
      break;

    struct mbr_sector *ebr = (struct mbr_sector *)buf;
    if (ebr->signature != 0xAA55)
      break;

    struct mbr_entry *e0 = &ebr->entries[0];
    struct mbr_entry *e1 = &ebr->entries[1];

    // Partición lógica.
    if (e0->type != 0 && e0->num_sectors > 0) {
      uint64_t lba = ext_start + e0->lba_first;
      uint64_t nsect = e0->num_sectors;
      if (validate_range(disk, lba, nsect) == 0 &&
          check_overlap(disk, lba, nsect) == 0) {
        char type_str[48];
        // Formato manual: "MBR 0x83 (Linux)". snprintf no está.
        const char *tn = mbr_type_name(e0->type);
        // Sin snprintf: string simple
        // (evitamos dependencia de libc de formateo en kernel).
        int k = 0;
        const char *pref = "MBR 0x";
        while (*pref && k < (int)sizeof(type_str) - 1)
          type_str[k++] = *pref++;
        static const char hex[] = "0123456789abcdef";
        type_str[k++] = hex[(e0->type >> 4) & 0xF];
        type_str[k++] = hex[e0->type & 0xF];
        type_str[k++] = ' ';
        type_str[k++] = '(';
        while (*tn && k < (int)sizeof(type_str) - 2)
          type_str[k++] = *tn++;
        type_str[k++] = ')';
        type_str[k] = '\0';

        log_partition(disk, *next_idx, lba, nsect, type_str);
        if (register_partition(disk, lba, nsect, *next_idx) == 0) {
          registered++;
          (*next_idx)++;
        }
      } else {
        LOG_WARN("[PART] %s: EBR en LBA %llu con entrada inválida", disk->name,
                 (unsigned long long)cur);
      }
    }

    // ¿Siguiente EBR?
    if (e1->type == 0 || e1->num_sectors == 0)
      break;
    uint64_t next = ext_start + e1->lba_first;
    if (next <= cur || next >= ext_end)
      break; // evita loops
    cur = next;
    guard++;
  }

  return registered;
}

// ===========================================================================
// Escaneo MBR
// ===========================================================================
static int part_scan_mbr(block_device_t *disk, struct mbr_sector *mbr) {
  int registered = 0;
  int next_idx = 1;

  for (int i = 0; i < 4; i++) {
    struct mbr_entry *e = &mbr->entries[i];
    if (e->type == 0x00)
      continue;
    if (e->type == 0xEE)
      continue; // protective MBR (debería haberse filtrado antes)

    uint64_t start = e->lba_first;
    uint64_t nsect = e->num_sectors;

    // Extended partition: no se registra, se recorre el EBR chain.
    if (e->type == 0x05 || e->type == 0x0F || e->type == 0x85) {
      if (validate_range(disk, start, nsect) == 0) {
        registered += part_scan_ebr(disk, start, nsect, &next_idx);
      }
      continue;
    }

    if (validate_range(disk, start, nsect) != 0) {
      LOG_WARN("[PART] %s: entrada MBR %d fuera de rango (LBA %llu n=%llu)",
               disk->name, i, (unsigned long long)start,
               (unsigned long long)nsect);
      continue;
    }
    if (check_overlap(disk, start, nsect) != 0) {
      LOG_WARN("[PART] %s: entrada MBR %d solapa con otra partición",
               disk->name, i);
      continue;
    }

    log_partition(disk, next_idx, start, nsect, mbr_type_name(e->type));
    if (register_partition(disk, start, nsect, next_idx) == 0) {
      registered++;
      next_idx++;
    }
  }

  if (registered > 0)
    LOG_INFO("[PART] %s: MBR, %d particiones registradas", disk->name,
             registered);
  return registered;
}

// ===========================================================================
// Escaneo GPT
// ===========================================================================
static int part_scan_gpt(block_device_t *disk) {
  uint32_t sector_size = disk->sector_size;
  if (sector_size < 512)
    sector_size = 512;

  uint8_t *hdr_buf = (uint8_t *)kmalloc(sector_size);
  if (!hdr_buf)
    return -ENOMEM;

  int rc = bdev_read(disk, 1, 1, hdr_buf);
  if (rc != 0) {
    LOG_WARN("[PART] %s: no se pudo leer GPT header (rc=%d)", disk->name, rc);
    kfree(hdr_buf);
    return rc;
  }

  struct gpt_header *gh = (struct gpt_header *)hdr_buf;
  if (memcmp(gh->signature, "EFI PART", 8) != 0) {
    LOG_WARN("[PART] %s: protective MBR pero GPT header inválido", disk->name);
    kfree(hdr_buf);
    return -EINVAL;
  }

  if (gh->header_size < 92 || gh->header_size > sector_size) {
    LOG_WARN("[PART] %s: GPT header_size inválido (%u)", disk->name,
             gh->header_size);
    kfree(hdr_buf);
    return -EINVAL;
  }

  uint64_t entries_lba = gh->entries_lba;
  uint32_t num_entries = gh->num_entries;
  uint32_t entry_size = gh->entry_size;

  if (entries_lba < 1 || entries_lba >= disk->num_sectors) {
    LOG_WARN("[PART] %s: GPT entries_lba fuera de rango (%llu)", disk->name,
             (unsigned long long)entries_lba);
    kfree(hdr_buf);
    return -EINVAL;
  }
  if (num_entries == 0 || num_entries > 4096) {
    LOG_WARN("[PART] %s: GPT num_entries inválido (%u)", disk->name,
             num_entries);
    kfree(hdr_buf);
    return -EINVAL;
  }
  if (entry_size < 128 || (entry_size & 7) != 0) {
    LOG_WARN("[PART] %s: GPT entry_size inválido (%u)", disk->name, entry_size);
    kfree(hdr_buf);
    return -EINVAL;
  }

  uint64_t total_bytes = (uint64_t)num_entries * entry_size;
  if (total_bytes > 1024 * 1024) {
    LOG_WARN("[PART] %s: GPT entries demasiado grandes (%llu bytes)",
             disk->name, (unsigned long long)total_bytes);
    kfree(hdr_buf);
    return -EINVAL;
  }

  uint32_t bytes_to_read = (uint32_t)total_bytes;
  uint32_t sectors_to_read = (bytes_to_read + sector_size - 1) / sector_size;
  uint32_t buf_bytes = sectors_to_read * sector_size;

  uint8_t *entries_buf = (uint8_t *)kmalloc(buf_bytes);
  if (!entries_buf) {
    kfree(hdr_buf);
    return -ENOMEM;
  }

  rc = bdev_read(disk, entries_lba, sectors_to_read, entries_buf);
  if (rc != 0) {
    LOG_WARN("[PART] %s: no se pudo leer GPT entries (rc=%d)", disk->name, rc);
    kfree(entries_buf);
    kfree(hdr_buf);
    return rc;
  }

  int registered = 0;
  for (uint32_t i = 0; i < num_entries; i++) {
    struct gpt_entry *ge =
        (struct gpt_entry *)(entries_buf + (uint64_t)i * entry_size);

    if (guid_is_zero(ge->type_guid))
      continue;

    uint64_t start = ge->first_lba;
    uint64_t last = ge->last_lba;
    if (last < start) {
      LOG_WARN("[PART] %s: GPT entrada %u con last < start", disk->name, i);
      continue;
    }
    uint64_t nsect = last - start + 1;

    if (validate_range(disk, start, nsect) != 0) {
      LOG_WARN("[PART] %s: GPT entrada %u fuera de rango (LBA %llu n=%llu)",
               disk->name, i, (unsigned long long)start,
               (unsigned long long)nsect);
      continue;
    }
    if (check_overlap(disk, start, nsect) != 0) {
      LOG_WARN("[PART] %s: GPT entrada %u solapa con otra", disk->name, i);
      continue;
    }

    int idx = (int)i + 1;
    log_partition(disk, idx, start, nsect, gpt_type_name(ge->type_guid));
    if (register_partition(disk, start, nsect, idx) == 0)
      registered++;
  }

  kfree(entries_buf);
  kfree(hdr_buf);

  if (registered > 0)
    LOG_INFO("[PART] %s: GPT, %d particiones registradas", disk->name,
             registered);
  return registered;
}

// ===========================================================================
// API pública
// ===========================================================================
int part_scan(block_device_t *disk) {
  if (!disk || disk->is_partition || disk->is_read_only)
    return 0;

  // Idempotencia: si ya hay particiones registradas para este disco, no
  // volver a escanear.
  int n = blk_count();
  for (int i = 0; i < n; i++) {
    block_device_t *b = blk_get_by_index(i);
    if (b && b->is_partition && b->parent == disk) {
      LOG_DEBUG("[PART] %s: ya escaneado, saltando", disk->name);
      return 0;
    }
  }

  uint32_t sz = disk->sector_size ? disk->sector_size : 512;
  if (sz < 512)
    sz = 512;

  uint8_t *buf = (uint8_t *)kmalloc(sz);
  if (!buf)
    return -ENOMEM;

  int rc = bdev_read(disk, 0, 1, buf);
  if (rc != 0) {
    LOG_WARN("[PART] %s: no se pudo leer LBA 0 (rc=%d)", disk->name, rc);
    kfree(buf);
    return rc;
  }

  // Firma MBR (independiente de sz, siempre en 510-511 del primer sector).
  struct mbr_sector *mbr = (struct mbr_sector *)buf;
  if (mbr->signature != 0xAA55) {
    LOG_INFO("[PART] %s: sin firma MBR/GPT", disk->name);
    kfree(buf);
    return 0;
  }

  // Heurística: ¿es un FS crudo? Evita el falso positivo FAT→MBR.
  if (looks_like_fs_boot_sector(buf)) {
    LOG_INFO("[PART] %s: filesystem crudo (sin tabla de particiones)",
             disk->name);
    kfree(buf);
    return 0;
  }

  // Protective MBR → GPT.
  if (mbr->entries[0].type == 0xEE) {
    LOG_DEBUG("[PART] %s: protective MBR, leyendo GPT", disk->name);
    int rc_gpt = part_scan_gpt(disk);
    kfree(buf);
    if (rc_gpt >= 0)
      return rc_gpt;
    // Si GPT falló, caer a MBR. En un híbrido real, MBR tiene
    // particiones útiles en las entradas 1-3.
    LOG_WARN("[PART] %s: GPT inválido, intentando MBR", disk->name);
    rc = bdev_read(disk, 0, 1, buf);
    if (rc != 0) {
      kfree(buf);
      return rc;
    }
  }

  rc = part_scan_mbr(disk, mbr);
  kfree(buf);
  return rc;
}

void part_scan_all(void) {
  int disks = 0, total = 0;
  int n = blk_count(); // snapshot: los registros añaden al final
  for (int i = 0; i < n; i++) {
    block_device_t *b = blk_get_by_index(i);
    if (!b || b->is_partition || b->is_read_only)
      continue;
    disks++;
    total += part_scan(b);
  }
  if (disks > 0)
    LOG_INFO("[PART] %d discos escaneados, %d particiones registradas", disks,
             total);
}