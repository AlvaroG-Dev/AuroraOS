// kernel/ext2.c — ext2 (rev 0 y 1), solo lectura (E1).
//
// Todo lo que viene del disco se trata como hostil: cada número de bloque,
// de inodo, rec_len y longitud se valida antes de usarse. Un disco corrupto
// debe dar -EIO / NULL, nunca un fallo del kernel ni un bucle infinito.

#include "ext2.h"

#include "bcache.h"
#include "heap.h"
#include "klog.h"
#include "mutex.h"
#include "string.h"

#define EXT2_MAGIC 0xEF53
#define EXT2_ROOT_INO 2
#define EXT2_NDIR_BLOCKS 12
#define EXT2_IND_BLOCK 12
#define EXT2_DIND_BLOCK 13
#define EXT2_TIND_BLOCK 14
#define EXT2_N_BLOCKS 15
#define EXT2_NAME_LEN 255
#define EXT2_MAX_BS 4096u
#define EXT2_GOOD_OLD_INODE_SIZE 128
#define EXT2_GOOD_OLD_FIRST_INO 11
#define EXT2_MAX_DIR_ENTRIES_PER_BLOCK 1024 // cota de pasos por bloque

// Features. Solo lectura: las ro_compat desconocidas no impiden leer.
#define EXT2_FEATURE_COMPAT_HAS_JOURNAL 0x0004
#define EXT2_FEATURE_INCOMPAT_FILETYPE 0x0002
#define EXT2_FEATURE_INCOMPAT_RECOVER 0x0004
#define EXT2_INCOMPAT_SUPPORTED (EXT2_FEATURE_INCOMPAT_FILETYPE)
#define EXT2_FEATURE_RO_COMPAT_LARGE_FILE 0x0002

#define EXT2_STATE_VALID 0x0001

typedef struct __attribute__((packed)) {
  uint32_t s_inodes_count, s_blocks_count, s_r_blocks_count;
  uint32_t s_free_blocks_count, s_free_inodes_count;
  uint32_t s_first_data_block, s_log_block_size, s_log_frag_size;
  uint32_t s_blocks_per_group, s_frags_per_group, s_inodes_per_group;
  uint32_t s_mtime, s_wtime;
  uint16_t s_mnt_count;
  int16_t s_max_mnt_count;
  uint16_t s_magic, s_state, s_errors, s_minor_rev;
  uint32_t s_lastcheck, s_checkinterval, s_creator_os, s_rev_level;
  uint16_t s_def_resuid, s_def_resgid;
  // rev 1
  uint32_t s_first_ino;
  uint16_t s_inode_size, s_block_group_nr;
  uint32_t s_feature_compat, s_feature_incompat, s_feature_ro_compat;
  uint8_t s_uuid[16];
  char s_volume_name[16];
} ext2_sb_t;

typedef struct __attribute__((packed)) {
  uint32_t bg_block_bitmap, bg_inode_bitmap, bg_inode_table;
  uint16_t bg_free_blocks_count, bg_free_inodes_count, bg_used_dirs_count;
  uint16_t bg_pad;
  uint32_t bg_reserved[3];
} ext2_gd_t;

typedef struct __attribute__((packed)) {
  uint16_t i_mode, i_uid;
  uint32_t i_size, i_atime, i_ctime, i_mtime, i_dtime;
  uint16_t i_gid, i_links_count;
  uint32_t i_blocks, i_flags, i_osd1;
  uint32_t i_block[EXT2_N_BLOCKS];
  uint32_t i_generation, i_file_acl, i_size_high, i_faddr;
  uint8_t i_frag, i_fsize;
  uint16_t i_pad1;
  uint16_t i_uid_high, i_gid_high;
  uint32_t i_reserved2;
} ext2_inode_t; // los 128 bytes originales

typedef struct __attribute__((packed)) {
  uint32_t inode;
  uint16_t rec_len;
  uint8_t name_len, file_type;
  // char name[]
} ext2_dirent_t;

typedef struct {
  uint32_t inode_table; // 0 = descriptor inválido
} ext2_group_t;

typedef struct {
  block_device_t *bdev;
  uint32_t bs, log_bs;
  uint32_t inodes_count, blocks_count, r_blocks_count;
  uint32_t free_blocks, free_inodes;
  uint32_t first_data_block, blocks_per_group, inodes_per_group;
  uint32_t inode_size, first_ino, ngroups;
  uint32_t compat, incompat, ro_compat;
  int filetype;
  ext2_group_t *groups;
  mutex_t lock; // protege el cursor de readdir de los nodos
} ext2_fs_t;

typedef struct {
  ext2_fs_t *fs;
  uint32_t ino;
  ext2_inode_t di; // copia del inodo en disco
  uint64_t size;
  // Cursor de readdir (acelera getdents secuencial): próximo índice y su
  // posición (bloque lógico, desplazamiento) en el directorio.
  uint64_t rd_index;
  uint32_t rd_lblk, rd_off;
} ext2_node_t;

static vfs_ops_t ext2_node_ops;
static vfs_fs_ops_t ext2_fs_ops;

static inline uint16_t u16(const void *p) {
  uint16_t v;
  memcpy(&v, p, 2);
  return v;
}

// ---------------------------------------------------------------------------
// Inodos
// ---------------------------------------------------------------------------
static int ext2_read_inode(ext2_fs_t *fs, uint32_t ino, ext2_inode_t *out) {
  if (ino == 0 || ino > fs->inodes_count)
    return -EINVAL;
  uint32_t grp = (ino - 1) / fs->inodes_per_group;
  uint32_t idx = (ino - 1) % fs->inodes_per_group;
  if (grp >= fs->ngroups || fs->groups[grp].inode_table == 0)
    return -EIO;
  uint64_t byte = (uint64_t)idx * fs->inode_size;
  uint64_t blk = (uint64_t)fs->groups[grp].inode_table + byte / fs->bs;
  uint32_t off = (uint32_t)(byte % fs->bs);
  if (blk >= fs->blocks_count || off + sizeof(*out) > fs->bs)
    return -EIO;
  bc_buf_t *b = bc_get(fs->bdev, blk, fs->bs);
  if (!b)
    return -EIO;
  memcpy(out, (uint8_t *)bc_data(b) + off, sizeof(*out));
  bc_put(b);
  return 0;
}

// Bloque físico del bloque lógico `lblk`. *out = 0 para un hueco.
static int ext2_bmap(ext2_fs_t *fs, const ext2_inode_t *di, uint64_t lblk,
                     uint32_t *out) {
  uint32_t apb = fs->bs / 4;
  uint32_t ptr;
  int depth;
  uint32_t idx[3];

  if (lblk < EXT2_NDIR_BLOCKS) {
    ptr = di->i_block[lblk];
    depth = 0;
  } else {
    lblk -= EXT2_NDIR_BLOCKS;
    if (lblk < apb) {
      ptr = di->i_block[EXT2_IND_BLOCK];
      depth = 1;
      idx[0] = (uint32_t)lblk;
    } else if ((lblk -= apb) < (uint64_t)apb * apb) {
      ptr = di->i_block[EXT2_DIND_BLOCK];
      depth = 2;
      idx[0] = (uint32_t)(lblk / apb);
      idx[1] = (uint32_t)(lblk % apb);
    } else if ((lblk -= (uint64_t)apb * apb) < (uint64_t)apb * apb * apb) {
      ptr = di->i_block[EXT2_TIND_BLOCK];
      depth = 3;
      idx[0] = (uint32_t)(lblk / ((uint64_t)apb * apb));
      idx[1] = (uint32_t)((lblk / apb) % apb);
      idx[2] = (uint32_t)(lblk % apb);
    } else {
      *out = 0;
      return 0; // más allá del máximo direccionable
    }
  }

  for (int d = 0; d < depth; d++) {
    if (ptr == 0) {
      *out = 0;
      return 0;
    }
    if (ptr < fs->first_data_block || ptr >= fs->blocks_count)
      return -EIO;
    bc_buf_t *b = bc_get(fs->bdev, ptr, fs->bs);
    if (!b)
      return -EIO;
    memcpy(&ptr, (uint8_t *)bc_data(b) + (size_t)idx[d] * 4, 4);
    bc_put(b);
  }
  if (ptr != 0 && (ptr < fs->first_data_block || ptr >= fs->blocks_count))
    return -EIO;
  *out = ptr;
  return 0;
}

static uint64_t ext2_file_size(const ext2_fs_t *fs, const ext2_inode_t *di) {
  uint64_t sz = di->i_size;
  if ((di->i_mode & S_IFMT) == S_IFREG &&
      (fs->ro_compat & EXT2_FEATURE_RO_COMPAT_LARGE_FILE))
    sz |= (uint64_t)di->i_size_high << 32;
  return sz;
}

// Lee hasta `size` bytes del inodo desde `offset`. Bytes leídos o -errno.
static int64_t ext2_read_data(ext2_fs_t *fs, const ext2_inode_t *di,
                              uint64_t fsize, uint64_t offset, size_t size,
                              void *buf) {
  if (offset >= fsize || size == 0)
    return 0;
  if (size > fsize - offset)
    size = (size_t)(fsize - offset);
  uint8_t *dst = buf;
  size_t done = 0;
  while (done < size) {
    uint64_t pos = offset + done;
    uint64_t lblk = pos / fs->bs;
    uint32_t boff = (uint32_t)(pos % fs->bs);
    size_t n = fs->bs - boff;
    if (n > size - done)
      n = size - done;
    uint32_t pblk;
    int rc = ext2_bmap(fs, di, lblk, &pblk);
    if (rc != 0)
      return done ? (int64_t)done : rc;
    if (pblk == 0) {
      memset(dst + done, 0, n);
    } else {
      bc_buf_t *b = bc_get(fs->bdev, pblk, fs->bs);
      if (!b)
        return done ? (int64_t)done : -EIO;
      memcpy(dst + done, (uint8_t *)bc_data(b) + boff, n);
      bc_put(b);
    }
    done += n;
  }
  return (int64_t)done;
}

// ---------------------------------------------------------------------------
// Directorios
// ---------------------------------------------------------------------------
// Valida la entrada en `p` (dentro de un bloque de `bs` bytes, en off).
static int ext2_dirent_ok(const ext2_fs_t *fs, const uint8_t *blk, uint32_t off,
                          uint16_t *rec_len, uint32_t *ino, uint8_t *nlen) {
  if (off + 8 > fs->bs)
    return 0;
  ext2_dirent_t d;
  memcpy(&d, blk + off, sizeof(d));
  if (d.rec_len < 8 || (d.rec_len & 3) || off + d.rec_len > fs->bs)
    return 0;
  if ((uint32_t)8 + d.name_len > d.rec_len)
    return 0;
  *rec_len = d.rec_len;
  *ino = d.inode;
  *nlen = d.name_len;
  return 1;
}

// Busca `name` en el directorio. 0 y *ino_out, -ENOENT, o -EIO.
static int ext2_dir_find(ext2_fs_t *fs, const ext2_inode_t *di, uint64_t dsize,
                         const char *name, size_t nlen, uint32_t *ino_out) {
  uint64_t nblocks = (dsize + fs->bs - 1) / fs->bs;
  for (uint64_t l = 0; l < nblocks; l++) {
    uint32_t pblk;
    int rc = ext2_bmap(fs, di, l, &pblk);
    if (rc != 0)
      return rc;
    if (pblk == 0)
      continue;
    bc_buf_t *b = bc_get(fs->bdev, pblk, fs->bs);
    if (!b)
      return -EIO;
    const uint8_t *blk = bc_data(b);
    uint32_t off = 0;
    int bad = 0;
    for (int steps = 0; off < fs->bs && steps < EXT2_MAX_DIR_ENTRIES_PER_BLOCK;
         steps++) {
      uint16_t rl;
      uint32_t ino;
      uint8_t nl;
      if (!ext2_dirent_ok(fs, blk, off, &rl, &ino, &nl)) {
        bad = 1;
        break;
      }
      if (ino != 0 && nl == nlen && memcmp(blk + off + 8, name, nlen) == 0) {
        bc_put(b);
        if (ino > fs->inodes_count)
          return -EIO;
        *ino_out = ino;
        return 0;
      }
      off += rl;
    }
    bc_put(b);
    if (bad) {
      LOG_WARN("[EXT2] %s: directorio corrupto (bloque lógico %llu)",
               fs->bdev->name, (unsigned long long)l);
      return -EIO;
    }
  }
  return -ENOENT;
}

// ---------------------------------------------------------------------------
// Nodos VFS
// ---------------------------------------------------------------------------
static uint32_t ext2_decode_rdev(const ext2_inode_t *di) {
  uint32_t maj, min;
  if (di->i_block[0]) { // codificación antigua: 8+8 bits
    maj = (di->i_block[0] >> 8) & 0xFF;
    min = di->i_block[0] & 0xFF;
  } else { // nueva: i_block[1]
    uint32_t v = di->i_block[1];
    maj = (v & 0xFFF00) >> 8;
    min = (v & 0xFF) | ((v >> 12) & 0xFFF00);
  }
  return (maj << 8) | (min & 0xFF); // formato Aurora (major<<8|minor)
}

// Lee el destino de un symlink en buf (sin NUL). Longitud o -errno.
static int ext2_read_link(ext2_fs_t *fs, const ext2_inode_t *di, uint64_t sz,
                          char *buf, size_t bufsize) {
  if (sz == 0 || sz > 4095)
    return -EIO;
  size_t n = sz < bufsize ? (size_t)sz : bufsize;
  int ea = di->i_file_acl ? (int)(fs->bs >> 9) : 0;
  if ((int)di->i_blocks - ea == 0) { // symlink rápido: vive en i_block[]
    if (sz >= sizeof(di->i_block))
      return -EIO;
    memcpy(buf, di->i_block, n);
    return (int)n;
  }
  int64_t rc = ext2_read_data(fs, di, sz, 0, n, buf);
  return rc < 0 ? (int)rc : (int)rc;
}

static vfs_node_t *ext2_make_node(ext2_fs_t *fs, uint32_t ino) {
  ext2_node_t *en = kzalloc(sizeof(*en));
  if (!en)
    return NULL;
  if (ext2_read_inode(fs, ino, &en->di) != 0) {
    kfree(en);
    return NULL;
  }
  en->fs = fs;
  en->ino = ino;
  const ext2_inode_t *di = &en->di;
  uint32_t mode = di->i_mode;
  if (di->i_links_count == 0 && ino != EXT2_ROOT_INO) {
    kfree(en); // inodo borrado: no debería ser alcanzable
    return NULL;
  }
  en->size = ext2_file_size(fs, di);

  vfs_node_t *n = kzalloc(sizeof(*n));
  if (!n) {
    kfree(en);
    return NULL;
  }
  n->flags = ((mode & S_IFMT) == S_IFDIR) ? VFS_DIRECTORY : VFS_FILE;
  n->size = (size_t)en->size;
  n->inode = ino;
  n->ops = &ext2_node_ops;
  n->fs = &ext2_fs_ops;
  n->priv = en;
  n->mode = mode;
  n->uid = (uint32_t)di->i_uid | ((uint32_t)di->i_uid_high << 16);
  n->gid = (uint32_t)di->i_gid | ((uint32_t)di->i_gid_high << 16);
  n->mtime_sec = di->i_mtime;
  n->atime_sec = di->i_atime;
  n->ctime_sec = di->i_ctime;
  n->nlink = di->i_links_count;
  n->blocks = di->i_blocks;
  if ((mode & S_IFMT) == S_IFCHR || (mode & S_IFMT) == S_IFBLK)
    n->rdev = ext2_decode_rdev(di);
  if ((mode & S_IFMT) == S_IFLNK) {
    char tmp[VFS_PATH_MAX];
    int len = ext2_read_link(fs, di, en->size, tmp, sizeof(tmp) - 1);
    if (len > 0) {
      memcpy(n->link_target, tmp, (size_t)len);
      n->link_target[len] = '\0';
      n->is_symlink = 1;
    } else {
      LOG_WARN("[EXT2] %s: symlink ino %u ilegible o demasiado largo",
               fs->bdev->name, ino);
      kfree(en);
      kfree(n);
      return NULL;
    }
  }
  return n;
}

static vfs_node_t *ext2_lookup(void *fs_priv, const char *path) {
  ext2_fs_t *fs = fs_priv;
  if (!fs || !path)
    return NULL;
  uint32_t ino = EXT2_ROOT_INO;
  const char *p = path;
  while (*p) {
    while (*p == '/')
      p++;
    if (!*p)
      break;
    const char *seg = p;
    while (*p && *p != '/')
      p++;
    size_t nl = (size_t)(p - seg);
    if (nl > EXT2_NAME_LEN)
      return NULL;
    ext2_inode_t di;
    if (ext2_read_inode(fs, ino, &di) != 0)
      return NULL;
    if ((di.i_mode & S_IFMT) != S_IFDIR)
      return NULL; // symlinks intermedios: los expande el VFS
    uint32_t next;
    if (ext2_dir_find(fs, &di, ext2_file_size(fs, &di), seg, nl, &next) != 0)
      return NULL;
    ino = next;
  }
  return ext2_make_node(fs, ino);
}

static int64_t ext2_node_read(vfs_node_t *node, uint64_t offset, size_t size,
                              void *buf) {
  ext2_node_t *en = node ? node->priv : NULL;
  if (!en || !buf)
    return -EINVAL;
  if ((en->di.i_mode & S_IFMT) == S_IFDIR)
    return -EISDIR;
  return ext2_read_data(en->fs, &en->di, en->size, offset, size, buf);
}

static int ext2_node_open(vfs_node_t *node, int flags) {
  (void)node;
  if (flags & (O_WRONLY | O_RDWR | 0x200 /*O_TRUNC*/))
    return -EROFS;
  return 0;
}

static int ext2_node_close(vfs_node_t *node) {
  if (node && node->priv) {
    kfree(node->priv);
    node->priv = NULL;
  }
  return 0;
}

static int ext2_node_readlink(vfs_node_t *node, char *buf, size_t bufsize) {
  ext2_node_t *en = node ? node->priv : NULL;
  if (!en || (en->di.i_mode & S_IFMT) != S_IFLNK || bufsize < 2)
    return -EINVAL;
  int n = ext2_read_link(en->fs, &en->di, en->size, buf, bufsize - 1);
  if (n < 0)
    return n;
  buf[n] = '\0';
  return n;
}

// Tipo de entrada para getdents a partir de file_type o de i_mode.
static uint32_t ext2_ft_to_vfs(uint8_t ft) {
  switch (ft) {
  case 2: return VFS_DIRECTORY;
  case 3: return VFS_CHARDEVICE;
  case 4: return VFS_BLK_ENT;
  case 5: return VFS_FIFO_ENT;
  case 6: return VFS_SOCK_ENT;
  case 7: return VFS_SYMLINK_ENT;
  default: return VFS_FILE;
  }
}
static uint32_t ext2_mode_to_vfs(uint16_t mode) {
  switch (mode & S_IFMT) {
  case S_IFDIR: return VFS_DIRECTORY;
  case S_IFCHR: return VFS_CHARDEVICE;
  case S_IFBLK: return VFS_BLK_ENT;
  case S_IFIFO: return VFS_FIFO_ENT;
  case S_IFSOCK: return VFS_SOCK_ENT;
  case S_IFLNK: return VFS_SYMLINK_ENT;
  default: return VFS_FILE;
  }
}

// readdir por índice con cursor cacheado en el nodo.
static int ext2_node_readdir(vfs_node_t *dir, uint64_t index,
                             vfs_dirent_t *out) {
  ext2_node_t *en = dir ? dir->priv : NULL;
  if (!en || !out)
    return -EINVAL;
  if ((en->di.i_mode & S_IFMT) != S_IFDIR)
    return -ENOTDIR;
  ext2_fs_t *fs = en->fs;
  out->name[0] = '\0';
  out->type = 0;
  out->size = 0;

  mutex_lock(&fs->lock);
  uint64_t cur_idx = en->rd_index;
  uint32_t lblk = en->rd_lblk, off = en->rd_off;
  if (index < cur_idx) {
    cur_idx = 0;
    lblk = 0;
    off = 0;
  }
  uint64_t nblocks = (en->size + fs->bs - 1) / fs->bs;
  int rc = 0;

  while (lblk < nblocks) {
    uint32_t pblk;
    rc = ext2_bmap(fs, &en->di, lblk, &pblk);
    if (rc != 0)
      goto out;
    if (pblk == 0) {
      lblk++;
      off = 0;
      continue;
    }
    bc_buf_t *b = bc_get(fs->bdev, pblk, fs->bs);
    if (!b) {
      rc = -EIO;
      goto out;
    }
    const uint8_t *blk = bc_data(b);
    int steps = 0;
    while (off < fs->bs) {
      uint16_t rl;
      uint32_t ino;
      uint8_t nl;
      if (++steps > EXT2_MAX_DIR_ENTRIES_PER_BLOCK ||
          !ext2_dirent_ok(fs, blk, off, &rl, &ino, &nl)) {
        bc_put(b);
        rc = -EIO;
        goto out;
      }
      if (ino != 0 && ino <= fs->inodes_count) {
        if (cur_idx == index) {
          size_t n = nl < sizeof(out->name) - 1 ? nl : sizeof(out->name) - 1;
          memcpy(out->name, blk + off + 8, n);
          out->name[n] = '\0';
          uint8_t ft = fs->filetype ? ((const uint8_t *)blk)[off + 7] : 0;
          out->type = ext2_ft_to_vfs(ft);
          if (ft == 0) { // sin file_type: hay que mirar el inodo
            ext2_inode_t ci;
            if (ext2_read_inode(fs, ino, &ci) == 0) {
              out->type = ext2_mode_to_vfs(ci.i_mode);
              out->size = ci.i_size;
            }
          } else if (ft == 1) {
            ext2_inode_t ci;
            if (ext2_read_inode(fs, ino, &ci) == 0)
              out->size = ext2_file_size(fs, &ci);
          }
          // Siguiente posición para el siguiente índice.
          en->rd_index = index + 1;
          en->rd_lblk = lblk;
          en->rd_off = off + rl;
          bc_put(b);
          rc = 0;
          goto out;
        }
        cur_idx++;
      }
      off += rl;
    }
    bc_put(b);
    lblk++;
    off = 0;
  }
  // Fin del directorio: nombre vacío. Se deja el cursor al final.
  en->rd_index = cur_idx;
  en->rd_lblk = lblk;
  en->rd_off = 0;
out:
  mutex_unlock(&fs->lock);
  return rc;
}

static int ext2_statfs(void *fs_priv, struct vfs_statfs *out) {
  ext2_fs_t *fs = fs_priv;
  if (!fs || !out)
    return -EINVAL;
  memset(out, 0, sizeof(*out));
  out->f_type = EXT2_MAGIC;
  out->f_bsize = fs->bs;
  out->f_frsize = fs->bs;
  out->f_blocks = fs->blocks_count;
  out->f_bfree = fs->free_blocks;
  out->f_bavail = fs->free_blocks > fs->r_blocks_count
                      ? fs->free_blocks - fs->r_blocks_count
                      : 0;
  out->f_files = fs->inodes_count;
  out->f_ffree = fs->free_inodes;
  out->f_namelen = EXT2_NAME_LEN;
  return 0;
}

static vfs_ops_t ext2_node_ops = {
    .read = ext2_node_read,
    .open = ext2_node_open,
    .close = ext2_node_close,
    .readdir = ext2_node_readdir,
    .readlink = ext2_node_readlink,
};

// ---------------------------------------------------------------------------
// Montaje
// ---------------------------------------------------------------------------
int ext2_mount(block_device_t *bdev, void **fs_priv_out) {
  if (!bdev || !fs_priv_out)
    return -EINVAL;
  const char *dn = bdev->name;
  uint32_t ss = bdev->sector_size;
  if (ss == 0 || ss > EXT2_MAX_BS)
    return -EINVAL;

  // Superbloque: 1024 bytes en el byte 1024.
  uint32_t first = 1024 / ss;
  uint32_t nsec = (1024 + ss - 1) / ss;
  if (nsec == 0)
    nsec = 1;
  uint8_t *raw = kmalloc((size_t)nsec * ss);
  if (!raw)
    return -ENOMEM;
  if (bdev_read(bdev, first, nsec, raw) != 0) {
    kfree(raw);
    return -EIO;
  }
  ext2_sb_t sb;
  memcpy(&sb, raw + (1024 - (size_t)first * ss), sizeof(sb));
  kfree(raw);

  if (sb.s_magic != EXT2_MAGIC)
    return -EINVAL;
  if (sb.s_rev_level > 1) {
    LOG_ERR("[EXT2] %s: revisión %u no soportada", dn, sb.s_rev_level);
    return -EINVAL;
  }
  uint32_t compat = 0, incompat = 0, ro_compat = 0;
  uint32_t isz = EXT2_GOOD_OLD_INODE_SIZE, first_ino = EXT2_GOOD_OLD_FIRST_INO;
  if (sb.s_rev_level == 1) {
    compat = sb.s_feature_compat;
    incompat = sb.s_feature_incompat;
    ro_compat = sb.s_feature_ro_compat;
    isz = sb.s_inode_size;
    first_ino = sb.s_first_ino;
  }
  if (incompat & EXT2_FEATURE_INCOMPAT_RECOVER) {
    LOG_ERR("[EXT2] %s: el journal necesita recuperación (ejecuta e2fsck)", dn);
    return -EINVAL;
  }
  if (incompat & ~EXT2_INCOMPAT_SUPPORTED) {
    LOG_ERR("[EXT2] %s: features incompatibles no soportadas 0x%x "
            "(extents/64bit/flex_bg/meta_bg... son ext4)",
            dn, incompat & ~EXT2_INCOMPAT_SUPPORTED);
    return -EINVAL;
  }
  if (sb.s_log_block_size > 2) {
    LOG_ERR("[EXT2] %s: bloque de %u bytes no soportado (máx 4096)", dn,
            1024u << (sb.s_log_block_size > 20 ? 20 : sb.s_log_block_size));
    return -EINVAL;
  }
  uint32_t bs = 1024u << sb.s_log_block_size;
  if (bs < ss || (bs % ss)) {
    LOG_ERR("[EXT2] %s: bloque %u incompatible con sectores de %u", dn, bs, ss);
    return -EINVAL;
  }
  if (isz < EXT2_GOOD_OLD_INODE_SIZE || isz > bs || (isz & (isz - 1)) ||
      first_ino < 2) {
    LOG_ERR("[EXT2] %s: tamaño de inodo/first_ino inválidos (%u/%u)", dn, isz,
            first_ino);
    return -EINVAL;
  }
  uint32_t fdb = (bs == 1024) ? 1 : 0;
  if (sb.s_first_data_block != fdb || sb.s_blocks_per_group == 0 ||
      sb.s_blocks_per_group > 8 * bs || sb.s_inodes_per_group == 0 ||
      sb.s_inodes_per_group > 8 * bs || sb.s_blocks_count <= fdb ||
      sb.s_inodes_count == 0) {
    LOG_ERR("[EXT2] %s: geometría inválida", dn);
    return -EINVAL;
  }
  uint64_t dev_blocks = bdev_size_bytes(bdev) / bs;
  if (sb.s_blocks_count > dev_blocks) {
    LOG_ERR("[EXT2] %s: el FS (%u bloques) es mayor que el dispositivo (%llu)",
            dn, sb.s_blocks_count, (unsigned long long)dev_blocks);
    return -EINVAL;
  }
  uint32_t ngroups =
      (sb.s_blocks_count - fdb + sb.s_blocks_per_group - 1) / sb.s_blocks_per_group;
  if (ngroups == 0 || (uint64_t)ngroups * sb.s_inodes_per_group < sb.s_inodes_count ||
      (uint64_t)(ngroups - 1) * sb.s_inodes_per_group >= sb.s_inodes_count) {
    LOG_ERR("[EXT2] %s: nº de inodos incoherente con los grupos", dn);
    return -EINVAL;
  }
  uint32_t gd_per_blk = bs / sizeof(ext2_gd_t);
  uint32_t gdt_blocks = (ngroups + gd_per_blk - 1) / gd_per_blk;
  if (fdb + 1 + gdt_blocks > sb.s_blocks_count)
    return -EINVAL;

  ext2_fs_t *fs = kzalloc(sizeof(*fs));
  if (!fs)
    return -ENOMEM;
  fs->groups = kzalloc((size_t)ngroups * sizeof(ext2_group_t));
  if (!fs->groups) {
    kfree(fs);
    return -ENOMEM;
  }
  mutex_init(&fs->lock);
  fs->bdev = bdev;
  fs->bs = bs;
  fs->log_bs = sb.s_log_block_size;
  fs->inodes_count = sb.s_inodes_count;
  fs->blocks_count = sb.s_blocks_count;
  fs->r_blocks_count = sb.s_r_blocks_count;
  fs->free_blocks = sb.s_free_blocks_count;
  fs->free_inodes = sb.s_free_inodes_count;
  fs->first_data_block = fdb;
  fs->blocks_per_group = sb.s_blocks_per_group;
  fs->inodes_per_group = sb.s_inodes_per_group;
  fs->inode_size = isz;
  fs->first_ino = first_ino;
  fs->ngroups = ngroups;
  fs->compat = compat;
  fs->incompat = incompat;
  fs->ro_compat = ro_compat;
  fs->filetype = (incompat & EXT2_FEATURE_INCOMPAT_FILETYPE) != 0;

  uint32_t itable_blocks =
      (uint32_t)(((uint64_t)sb.s_inodes_per_group * isz + bs - 1) / bs);
  uint32_t bad_groups = 0;
  for (uint32_t g = 0; g < ngroups; g++) {
    uint32_t gb = fdb + 1 + g / gd_per_blk;
    bc_buf_t *b = bc_get(bdev, gb, bs);
    if (!b) {
      kfree(fs->groups);
      kfree(fs);
      return -EIO;
    }
    ext2_gd_t gd;
    memcpy(&gd, (uint8_t *)bc_data(b) + (g % gd_per_blk) * sizeof(gd), sizeof(gd));
    bc_put(b);
    if (gd.bg_inode_table >= fdb && gd.bg_inode_table < sb.s_blocks_count &&
        (uint64_t)gd.bg_inode_table + itable_blocks <= sb.s_blocks_count)
      fs->groups[g].inode_table = gd.bg_inode_table;
    else
      bad_groups++;
  }

  // La raíz debe ser un directorio legible.
  ext2_inode_t root;
  if (ext2_read_inode(fs, EXT2_ROOT_INO, &root) != 0 ||
      (root.i_mode & S_IFMT) != S_IFDIR) {
    LOG_ERR("[EXT2] %s: inodo raíz ilegible o no es un directorio", dn);
    bc_invalidate_dev(bdev);
    kfree(fs->groups);
    kfree(fs);
    return -EINVAL;
  }

  LOG_INFO("[EXT2] montado %s (solo lectura): rev %u, bloque %u B, %u grupos, "
           "%u inodos de %u B, features c/i/ro=0x%x/0x%x/0x%x%s%s",
           dn, sb.s_rev_level, bs, ngroups, fs->inodes_count, isz, compat,
           incompat, ro_compat,
           (sb.s_state & EXT2_STATE_VALID) ? "" : " [NO DESMONTADO LIMPIO]",
           (compat & EXT2_FEATURE_COMPAT_HAS_JOURNAL) ? " [ext3 con journal]"
                                                      : "");
  if (bad_groups)
    LOG_WARN("[EXT2] %s: %u grupos con tabla de inodos inválida (ignorados)",
             dn, bad_groups);
  *fs_priv_out = fs;
  return 0;
}

void ext2_umount(void *fs_priv) {
  ext2_fs_t *fs = fs_priv;
  if (!fs)
    return;
  bc_invalidate_dev(fs->bdev);
  kfree(fs->groups);
  kfree(fs);
}

static vfs_fs_ops_t ext2_fs_ops = {
    .lookup = ext2_lookup,
    .statfs = ext2_statfs,
    .name = "ext2",
    .umount = ext2_umount,
};

vfs_fs_ops_t *ext2_get_vfs_ops(void) { return &ext2_fs_ops; }

int ext2_mount_bdev(const char *bdev_name, const char *mount_path) {
  if (!bdev_name || !mount_path)
    return -EINVAL;
  block_device_t *bdev = blk_lookup(bdev_name);
  if (!bdev)
    return -ENOENT;
  void *priv = NULL;
  int rc = ext2_mount(bdev, &priv);
  if (rc != 0)
    return rc;
  rc = vfs_mount(mount_path, &ext2_fs_ops, priv);
  if (rc != 0) {
    ext2_umount(priv);
    return rc;
  }
  vfs_set_mount_flags(mount_path, VFS_MS_RDONLY);
  return 0;
}
