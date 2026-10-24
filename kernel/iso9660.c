// kernel/iso9660.c
//
// Driver ISO9660 (ECMA-119) con soporte opcional de Rock Ridge (SUSP).
//
// Soporta:
//   - Primary Volume Descriptor (PVD)
//   - Directory Records y árbol de directorios
//   - Rock Ridge `NM` (nombres largos, case-preserved) y `SP` (detección)
//   - Lectura de ficheros regulares por offset/size
//
// NO soporta (por ahora):
//   - Joliet (SVD con escape %/E). Si el volumen tiene Joliet + RR,
//     usamos RR. Si solo tiene Joliet, caemos a nombres 8.3.
//   - Rock Ridge `PX`, `SL`, `CL`, `RE` (permisos POSIX, symlinks,
//     relocación de directorios profundos). Los ficheros se exponen
//     read-only con 0444; los directorios con 0555.
//   - Multi-extent files (> 4 GB).
//
// Referencias:
//   - ECMA-119 (ISO9660)
//   - IEEE P1281 (SUSP) y P1282 (Rock Ridge)

#include "iso9660.h"
#include "block.h"
#include "heap.h"
#include "klog.h"
#include "string.h"
#include "uaccess.h"
#include "vfs.h"

#define ISO_SECTOR_SIZE 2048
#define ISO_VD_LBA 16
#define ISO_MAX_VDS 32

#define ISO_VD_PRIMARY 1
#define ISO_VD_SUPPLEMENTARY 2
#define ISO_VD_TERMINATOR 255

#define ISO_FLAG_DIR 0x02

// Offsets del Volume Descriptor (en el sector 2048)
#define VD_TYPE 0
#define VD_ID 1
#define VD_VERSION 6
#define VD_ROOT_DIR_REC 156
#define VD_VOL_SPACE 80 // LE en 80, BE en 84
#define VD_LOG_BLOCK_SIZE 128

typedef struct {
  block_device_t *bdev;
  uint32_t root_lba;
  uint32_t root_size;
  int has_rock_ridge;
} iso9660_fs_t;

typedef struct {
  iso9660_fs_t *fs;
  uint32_t extent_lba;
  uint32_t extent_size;
  int is_dir;
} iso9660_node_t;

// ===========================================================================
// Helpers
// ===========================================================================
static inline uint32_t le32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}
static inline uint16_t le16(const uint8_t *p) {
  return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

// Comprueba si el root directory record (offset 156 del PVD) tiene
// una entrada SUSP `SP` en su System Use field → Rock Ridge presente.
static int has_rock_ridge(const uint8_t *root_rec) {
  uint8_t len = root_rec[0];
  if (len < 34)
    return 0;
  uint8_t name_len = root_rec[32];
  uint32_t su_off = 33 + name_len;
  if (su_off & 1)
    su_off++;
  if (su_off + 7 > len)
    return 0;
  const uint8_t *su = root_rec + su_off;
  if (su[0] != 'S' || su[1] != 'P')
    return 0;
  if (su[4] != 0xBE || su[5] != 0xEF)
    return 0;
  return 1;
}

// Extrae el nombre vía Rock Ridge `NM`. Devuelve 1 si lo encontró,
// 0 si no (para que el llamante caiga al nombre ISO 8.3).
static int rr_extract_name(const uint8_t *rec, char *out, size_t outlen) {
  uint8_t len = rec[0];
  if (len < 34)
    return 0;
  uint8_t name_len = rec[32];
  uint32_t su_off = 33 + name_len;
  if (su_off & 1)
    su_off++;
  if (su_off + 4 > len)
    return 0;

  const uint8_t *su = rec + su_off;
  const uint8_t *su_end = rec + len;
  size_t written = 0;

  while (su + 4 <= su_end) {
    uint8_t entry_len = su[2];
    if (entry_len < 4 || su + entry_len > su_end)
      break;

    if (su[0] == 'N' && su[1] == 'M') {
      uint8_t flags = su[4];
      uint8_t frag_len = entry_len - 5;
      for (uint8_t i = 0; i < frag_len && written + 1 < outlen; i++)
        out[written++] = (char)su[5 + i];
      if (!(flags & 0x01)) { // Continue flag no puesto → fin
        out[written] = '\0';
        return written > 0 ? 1 : 0;
      }
    } else if (su[0] == 'S' && su[1] == 'T') {
      break; // SUSP Terminator
    }
    su += entry_len;
  }
  return 0;
}

// Nombre fallback base ISO9660 (uppercase, quita ";1", trims trailing '.').
static void iso_extract_name(const uint8_t *rec, char *out, size_t outlen) {
  uint8_t name_len = rec[32];
  const char *name = (const char *)(rec + 33);
  size_t j = 0;
  for (uint8_t i = 0; i < name_len && j + 1 < outlen; i++) {
    if (name[i] == ';')
      break;
    out[j++] = name[i];
  }
  while (j > 0 && out[j - 1] == '.')
    j--;
  out[j] = '\0';
}

// ===========================================================================
// VFS node
// ===========================================================================
static int iso9660_close(vfs_node_t *node);

static vfs_node_t *iso9660_make_node(iso9660_fs_t *fs, uint32_t lba,
                                     uint32_t size, int is_dir,
                                     const char *name) {
  vfs_node_t *node = kzalloc(sizeof(*node));
  if (!node)
    return NULL;
  iso9660_node_t *priv = kzalloc(sizeof(*priv));
  if (!priv) {
    kfree(node);
    return NULL;
  }
  priv->fs = fs;
  priv->extent_lba = lba;
  priv->extent_size = size;
  priv->is_dir = is_dir;

  node->flags = is_dir ? VFS_DIRECTORY : VFS_FILE;
  node->size = size;
  node->ops = is_dir ? NULL : NULL; // rellena el llamante según tipo
  node->priv = priv;
  node->mode = is_dir ? (S_IFDIR | 0555) : (S_IFREG | 0444);
  if (name && name[0]) {
    size_t nl = strlen(name);
    if (nl >= sizeof(node->name))
      nl = sizeof(node->name) - 1;
    memcpy(node->name, name, nl);
    node->name[nl] = '\0';
  }
  return node;
}

// ===========================================================================
// read (files)
// ===========================================================================
static int64_t iso9660_read(vfs_node_t *node, uint64_t offset, size_t size,
                            void *buf) {
  iso9660_node_t *priv = (iso9660_node_t *)node->priv;
  if (!priv || priv->is_dir)
    return -EISDIR;
  if (offset >= priv->extent_size)
    return 0;
  if (size > priv->extent_size - offset)
    size = priv->extent_size - offset;

  uint8_t *out = (uint8_t *)buf;
  uint8_t *sector = (uint8_t *)kmalloc(ISO_SECTOR_SIZE);
  if (!sector)
    return -ENOMEM;

  size_t done = 0;
  while (done < size) {
    uint64_t cur = offset + done;
    uint32_t lba = priv->extent_lba + (uint32_t)(cur / ISO_SECTOR_SIZE);
    uint32_t in_sector = (uint32_t)(cur % ISO_SECTOR_SIZE);
    size_t chunk = ISO_SECTOR_SIZE - in_sector;
    if (chunk > size - done)
      chunk = size - done;

    if (bdev_read(priv->fs->bdev, lba, 1, sector) != 0) {
      kfree(sector);
      return done > 0 ? (int64_t)done : -EIO;
    }
    memcpy(out + done, sector + in_sector, chunk);
    done += chunk;
  }
  kfree(sector);
  return (int64_t)done;
}

// ===========================================================================
// readdir (directories)
// ===========================================================================
//
// Los Directory Records NO cruzan límites de sector. Cada sector de
// directorio contiene N registros; cuando el siguiente no cabe, se
// rellena el resto del sector con bytes cero (len=0).
//
// Los dos primeros registros de cada directorio son "." y "..".
// Los saltamos.
// ===========================================================================
static int iso9660_readdir(vfs_node_t *dir, uint64_t index, vfs_dirent_t *out) {
  iso9660_node_t *priv = (iso9660_node_t *)dir->priv;
  if (!priv || !priv->is_dir)
    return -ENOTDIR;

  uint32_t sectors =
      (priv->extent_size + ISO_SECTOR_SIZE - 1) / ISO_SECTOR_SIZE;
  uint8_t *sector = (uint8_t *)kmalloc(ISO_SECTOR_SIZE);
  if (!sector)
    return -ENOMEM;

  uint64_t seen = 0;
  for (uint32_t s = 0; s < sectors; s++) {
    if (bdev_read(priv->fs->bdev, priv->extent_lba + s, 1, sector) != 0) {
      kfree(sector);
      return -EIO;
    }
    uint32_t off = 0;
    while (off + 34 <= ISO_SECTOR_SIZE) {
      uint8_t rec_len = sector[off];
      if (rec_len == 0)
        break; // padding de fin de sector
      if (off + rec_len > ISO_SECTOR_SIZE)
        break;

      uint8_t name_len = sector[off + 32];
      const char *name = (const char *)(sector + off + 33);
      // "." y "..": name_len==1 y primer byte 0 o 1.
      if (name_len == 1 && (name[0] == 0 || name[0] == 1)) {
        off += rec_len;
        continue;
      }

      if (seen == index) {
        char real[VFS_PATH_MAX];
        if (!rr_extract_name(sector + off, real, sizeof(real))) {
          iso_extract_name(sector + off, real, sizeof(real));
        }
        size_t nl = strlen(real);
        if (nl >= sizeof(out->name))
          nl = sizeof(out->name) - 1;
        memcpy(out->name, real, nl);
        out->name[nl] = '\0';
        uint8_t flags = sector[off + 25];
        out->type = (flags & ISO_FLAG_DIR) ? VFS_DIRECTORY : VFS_FILE;
        out->size = le32(sector + off + 10);
        kfree(sector);
        return 0;
      }
      seen++;
      off += rec_len;
    }
  }
  out->name[0] = '\0';
  out->type = 0;
  out->size = 0;
  kfree(sector);
  return 0;
}

// ===========================================================================
// lookup
// ===========================================================================
static int iso9660_find_in_dir(iso9660_fs_t *fs, uint32_t dir_lba,
                               uint32_t dir_size, const char *want,
                               uint32_t *out_lba, uint32_t *out_size,
                               uint8_t *out_flags) {
  uint32_t sectors = (dir_size + ISO_SECTOR_SIZE - 1) / ISO_SECTOR_SIZE;
  uint8_t *sector = (uint8_t *)kmalloc(ISO_SECTOR_SIZE);
  if (!sector)
    return -ENOMEM;

  for (uint32_t s = 0; s < sectors; s++) {
    if (bdev_read(fs->bdev, dir_lba + s, 1, sector) != 0) {
      kfree(sector);
      return -EIO;
    }
    uint32_t off = 0;
    while (off + 34 <= ISO_SECTOR_SIZE) {
      uint8_t rec_len = sector[off];
      if (rec_len == 0)
        break;
      if (off + rec_len > ISO_SECTOR_SIZE)
        break;

      uint8_t name_len = sector[off + 32];
      const char *name = (const char *)(sector + off + 33);
      if (name_len == 1 && (name[0] == 0 || name[0] == 1)) {
        off += rec_len;
        continue;
      }

      // Rock Ridge primero (siempre), fallback a 8.3 si el entry no
      // tiene NM. NO gatear por `has_rock_ridge`: xorriso no siempre
      // pone SP en el root del PVD, pero los entries sí llevan NM.
      char real[VFS_PATH_MAX];
      if (!rr_extract_name(sector + off, real, sizeof(real))) {
        iso_extract_name(sector + off, real, sizeof(real));
      }

      if (strcmp(real, want) == 0) {
        *out_lba = le32(sector + off + 2);
        *out_size = le32(sector + off + 10);
        *out_flags = sector[off + 25];
        kfree(sector);
        return 0;
      }
      off += rec_len;
    }
  }
  kfree(sector);
  return -ENOENT;
}

static int iso9660_close(vfs_node_t *node) {
  if (node && node->priv) {
    kfree(node->priv);
    node->priv = NULL;
  }
  return 0;
}

static vfs_ops_t iso_file_ops = {
    .read = iso9660_read,
    .close = iso9660_close,
};
static vfs_ops_t iso_dir_ops = {
    .readdir = iso9660_readdir,
    .close = iso9660_close,
};

// Sobrescribe el `ops` del nodo antes de devolverlo (VFS lo necesita
// para dispatchar read/readdir por tipo).
static vfs_node_t *iso9660_make_node_vfs(iso9660_fs_t *fs, uint32_t lba,
                                         uint32_t size, int is_dir,
                                         const char *name) {
  vfs_node_t *n = iso9660_make_node(fs, lba, size, is_dir, name);
  if (!n)
    return NULL;
  n->ops = is_dir ? &iso_dir_ops : &iso_file_ops;
  return n;
}

// Reemplaza el lookup para usar make_node_vfs.
static vfs_node_t *iso9660_lookup_final(void *fs_priv, const char *path) {
  iso9660_fs_t *fs = (iso9660_fs_t *)fs_priv;
  if (!fs)
    return NULL;

  while (*path == '/')
    path++;
  if (*path == '\0')
    return iso9660_make_node_vfs(fs, fs->root_lba, fs->root_size, 1, "/");

  uint32_t cur_lba = fs->root_lba;
  uint32_t cur_size = fs->root_size;
  char comp[VFS_PATH_MAX];

  while (*path) {
    size_t clen = 0;
    while (*path && *path != '/' && clen + 1 < sizeof(comp))
      comp[clen++] = *path++;
    comp[clen] = '\0';
    while (*path == '/')
      path++;

    uint32_t next_lba = 0, next_size = 0;
    uint8_t flags = 0;
    if (iso9660_find_in_dir(fs, cur_lba, cur_size, comp, &next_lba, &next_size,
                            &flags) != 0)
      return NULL;
    cur_lba = next_lba;
    cur_size = next_size;

    if (*path == '\0')
      return iso9660_make_node_vfs(fs, cur_lba, cur_size,
                                   !!(flags & ISO_FLAG_DIR), comp);
    if (!(flags & ISO_FLAG_DIR))
      return NULL;
  }
  return NULL;
}

static int iso9660_statfs(void *fs_priv, struct vfs_statfs *out) {
  iso9660_fs_t *fs = (iso9660_fs_t *)fs_priv;
  memset(out, 0, sizeof(*out));
  out->f_type = 0x9660;
  out->f_bsize = ISO_SECTOR_SIZE;
  out->f_frsize = ISO_SECTOR_SIZE;
  if (fs && fs->bdev)
    out->f_blocks = fs->bdev->num_sectors;
  out->f_namelen = 255;
  return 0;
}

static vfs_fs_ops_t iso_fs_ops = {
    .lookup = iso9660_lookup_final,
    .statfs = iso9660_statfs,
    .name = "iso9660",
};

struct vfs_fs_ops *iso9660_get_vfs_ops(void) { return &iso_fs_ops; }

// ===========================================================================
// Mount
// ===========================================================================
int iso9660_mount_bdev(const char *bdev_name, const char *mountpoint) {
  if (!bdev_name || !mountpoint)
    return -EINVAL;

  block_device_t *bdev = blk_lookup(bdev_name);
  if (!bdev) {
    LOG_ERR("[ISO9660] %s no existe en el block layer", bdev_name);
    return -ENODEV;
  }
  if (bdev->sector_size != ISO_SECTOR_SIZE) {
    LOG_ERR("[ISO9660] %s: sector_size %u (esperado 2048)", bdev_name,
            bdev->sector_size);
    return -EINVAL;
  }

  iso9660_fs_t *fs = kzalloc(sizeof(*fs));
  if (!fs)
    return -ENOMEM;
  fs->bdev = bdev;

  uint8_t *vd = (uint8_t *)kmalloc(ISO_SECTOR_SIZE);
  if (!vd) {
    kfree(fs);
    return -ENOMEM;
  }

  int found_primary = 0;
  for (int i = 0; i < ISO_MAX_VDS; i++) {
    if (bdev_read(bdev, ISO_VD_LBA + i, 1, vd) != 0) {
      kfree(vd);
      kfree(fs);
      return -EIO;
    }
    if (memcmp(vd + VD_ID, "CD001", 5) != 0) {
      LOG_ERR("[ISO9660] %s: missing CD001 magic", bdev_name);
      kfree(vd);
      kfree(fs);
      return -EINVAL;
    }
    uint8_t type = vd[VD_TYPE];
    if (type == ISO_VD_TERMINATOR)
      break;
    if (type == ISO_VD_PRIMARY) {
      const uint8_t *root = vd + VD_ROOT_DIR_REC;
      fs->root_lba = le32(root + 2);
      fs->root_size = le32(root + 10);
      fs->has_rock_ridge = has_rock_ridge(root);
      found_primary = 1;
      break;
    }
  }
  kfree(vd);

  if (!found_primary) {
    LOG_ERR("[ISO9660] %s: no PVD", bdev_name);
    kfree(fs);
    return -EINVAL;
  }

  // Etiqueta del volumen (para el log)
  // (la dejamos fuera por simplicidad; el PVD la tiene en bytes 40..71)

  LOG_INFO("[ISO9660] %s: root LBA=%u size=%u RR=%d", bdev_name, fs->root_lba,
           fs->root_size, fs->has_rock_ridge);

  int rc = vfs_mount(mountpoint, &iso_fs_ops, fs);
  if (rc != 0) {
    LOG_ERR("[ISO9660] vfs_mount('%s') falló: %d", mountpoint, rc);
    kfree(fs);
    return rc;
  }
  return 0;
}