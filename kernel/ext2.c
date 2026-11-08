// kernel/ext2.c — ext2 (rev 0 y 1). E1: lectura. E2: escritura.
//
// Escritura (E2): sin journal. Al montar rw se borra EXT2_VALID_FS en el
// superbloque y se vuelca ANTES de tocar nada; vuelve a ponerse en sync/umount.
// Un fallo de corriente deja el FS "sucio" y el siguiente montaje es de solo
// lectura (e2fsck del host lo repara). Todo cambio de metadatos pasa por la
// caché de bloques (bcache) y todas las operaciones serializan en fs->lock.
//
// Todo lo que viene del disco se trata como hostil: cada número de bloque,
// de inodo, rec_len y longitud se valida antes de usarse. Un disco corrupto
// debe dar -EIO / NULL, nunca un fallo del kernel ni un bucle infinito.

#include "ext2.h"

#pragma GCC diagnostic ignored "-Waddress-of-packed-member"

#include "bcache.h"
#include "heap.h"
#include "klog.h"
#include "completion.h"
#include "kthread.h"
#include "mutex.h"
#include "paging.h"
#include "pmm.h"
#include "process.h"
#include "rtc.h"
#include "string.h"
#include "uaccess.h"

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
// ext4 (E8): se lee, no se escribe. Journal sin recuperar: se lee tal cual.
#define EXT4_FEATURE_INCOMPAT_EXTENTS 0x0040
#define EXT4_FEATURE_INCOMPAT_64BIT 0x0080
#define EXT4_FEATURE_INCOMPAT_FLEX_BG 0x0200
#define EXT4_FEATURE_INCOMPAT_CSUM_SEED 0x2000
#define EXT4_FEATURE_INCOMPAT_LARGEDIR 0x4000
#define EXT4_EXTENTS_FL 0x00080000u
#define EXT4_EXT_MAGIC 0xF30A
#define EXT2_INCOMPAT_SUPPORTED                                              \
  (EXT2_FEATURE_INCOMPAT_FILETYPE | EXT2_FEATURE_INCOMPAT_RECOVER |          \
   EXT4_FEATURE_INCOMPAT_EXTENTS | EXT4_FEATURE_INCOMPAT_64BIT |             \
   EXT4_FEATURE_INCOMPAT_FLEX_BG | EXT4_FEATURE_INCOMPAT_CSUM_SEED |         \
   EXT4_FEATURE_INCOMPAT_LARGEDIR)
#define EXT2_FEATURE_RO_COMPAT_SPARSE_SUPER 0x0001
#define EXT2_FEATURE_RO_COMPAT_LARGE_FILE 0x0002
#define EXT2_FEATURE_COMPAT_DIR_INDEX 0x0020
// Lo que sabemos escribir sin romper nada (el resto: montaje solo lectura).
#define EXT2_COMPAT_RW_OK 0x003F
// ext4 escribible (E9): extents, 64bit, flex_bg; huge_file/dir_nlink/extra_isize.
// Sigue en solo lectura con metadata_csum, uninit_bg, mmp, largedir o csum_seed.
#define EXT2_INCOMPAT_RW_OK (EXT2_FEATURE_INCOMPAT_FILETYPE | EXT4_FEATURE_INCOMPAT_EXTENTS | \
                             EXT4_FEATURE_INCOMPAT_64BIT | EXT4_FEATURE_INCOMPAT_FLEX_BG)
#define EXT2_RO_COMPAT_RW_OK (EXT2_FEATURE_RO_COMPAT_SPARSE_SUPER | EXT2_FEATURE_RO_COMPAT_LARGE_FILE | 0x0008 | 0x0020 | 0x0040 | EXT4_RO_COMPAT_METADATA_CSUM)
#define EXT2_INDEX_FL 0x1000
// Flags de inodo que chattr puede cambiar (FS_IOC_SETFLAGS) y cuales se imponen.
#define EXT2_SYNC_FL 0x00000008u
#define EXT2_IMMUTABLE_FL 0x00000010u
#define EXT2_APPEND_FL 0x00000020u
#define EXT2_NODUMP_FL 0x00000040u
#define EXT2_NOATIME_FL 0x00000080u
#define EXT2_DIRSYNC_FL 0x00010000u
#define EXT2_USER_FLAGS (EXT2_SYNC_FL | EXT2_IMMUTABLE_FL | EXT2_APPEND_FL | \
                         EXT2_NODUMP_FL | EXT2_NOATIME_FL | EXT2_DIRSYNC_FL)
#define EXT2_PROTECT_FL (EXT2_IMMUTABLE_FL | EXT2_APPEND_FL)
#define FS_IOC_GETFLAGS_ 0x80086601UL
#define FS_IOC_SETFLAGS_ 0x40086602UL
#define FS_IOC_GETVERSION_ 0x80087601UL
#define EXT2_LINK_MAX 32000
#ifndef EXDEV
#define EXDEV 18
#endif
#define EXT2_XATTR_MAGIC 0xEA020000u

#define EXT2_STATE_VALID 0x0001
#define EXT4_RO_COMPAT_METADATA_CSUM 0x0400u
static uint32_t ext4_crc32c(uint32_t crc, const void *buf, size_t len);
struct ext2_fs;
static int ext2_jnl_open(struct ext2_fs *fs);
static void ext2_jnl_close(struct ext2_fs *fs);
static int ext2_jnl_probe_fs(struct ext2_fs *fs);

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
  uint32_t block_bitmap, inode_bitmap; // 0 = grupo inutilizable para escribir
  uint32_t free_blocks, free_inodes, used_dirs;
  uint16_t flags;         // bg_flags (INODE_UNINIT 1, BLOCK_UNINIT 2, ITABLE_ZEROED 4)
  uint32_t itable_unused; // bg_itable_unused (solo con metadata_csum)
} ext2_group_t;

typedef struct ext2_pg {
  struct ext2_pg *next;
  uint64_t pgoff; // página de 4 KiB dentro del fichero
  uint64_t phys;
} ext2_pg_t;

// Inodo en memoria, compartido por todos los nodos VFS abiertos sobre él.
// Toda modificación se escribe en la bcache (write_inode) al terminar la
// operación, así que la bcache es siempre la verdad.
typedef struct ext2_ino {
  struct ext2_ino *next;
  uint32_t ino;
  int refs;
  ext2_inode_t di;
  uint64_t size;
  uint32_t goal; // último bloque asignado (localidad)
  int live;      // tenía enlaces al cargarse: puede evictarse
  int orphan;    // está en la lista de huérfanos del superbloque
  uint64_t ra_next; // fin de la última lectura (detecta lectura secuencial)
  ext2_pg_t *pages; // páginas mapeadas con MAP_SHARED
} ext2_ino_t;

typedef struct ext2_fs {
  block_device_t *bdev;
  uint32_t bs, log_bs;
  uint32_t inodes_count, blocks_count, r_blocks_count;
  uint32_t free_blocks, free_inodes;
  uint32_t first_data_block, blocks_per_group, inodes_per_group;
  uint32_t inode_size, first_ino, ngroups;
  uint32_t compat, incompat, ro_compat;
  uint32_t journal_ino; // s_journal_inum (jbd2 interno)
  uint32_t gdsz;        // tamaño del descriptor de grupo (32, o 64 con 64bit)
  int csum;             // metadata_csum activo
  int jn_ok;            // journal interno que sabemos escribir
  struct ext2_jnl *jn;  // journal activo (solo con el FS montado rw)
  uint32_t csum_seed;   // crc32c(~0, uuid)
  uint32_t gdt_blocks, resgdt;
  int filetype;
  ext2_group_t *groups;
  mutex_t lock; // serializa TODAS las operaciones del FS
  // E2
  int rw;         // montado para escritura
  int rev1;
  int disk_clean; // el superbloque en disco lleva VALID_FS
  int mnt_pending; // al primer sb_store: s_mnt_count++ y s_mtime
  uint32_t counts_fixed; // grupos cuyos contadores corrigió ext2_check()
  uint32_t s_state;
  uint32_t gen;   // sube con cada cambio de directorio (invalida cursores)
  uint32_t itable_blocks, bad_groups;
  ext2_ino_t *icache; // inodos con referencias
  // Liberaciones diferidas hasta que el inodo/dir que dejó de apuntar esté en
  // disco (barrera): evita que un corte deje un bloque libre y reutilizado
  // mientras otro inodo sin volcar aún lo referencia.
  uint32_t *pend_blk, *pend_ino;
  struct ext2_dc *dcache; // caché de nombres de directorios grandes
  uint32_t npend_blk, cap_blk, npend_ino, cap_ino;
} ext2_fs_t;

typedef struct {
  ext2_fs_t *fs;
  uint32_t ino;
  ext2_ino_t *ip; // inodo compartido
  // Cursor de readdir (acelera getdents secuencial): próximo índice y su
  // posición (bloque lógico, desplazamiento) en el directorio.
  uint64_t rd_index;
  uint32_t rd_lblk, rd_off, rd_gen;
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

// ext4: busca `lblk` en el árbol de extents (cabecera en i_block o en un
// bloque). *out = 0 para un hueco o un extent sin inicializar (lee ceros).
static int ext4_extent_map(ext2_fs_t *fs, const ext2_inode_t *di, uint64_t lblk,
                           uint32_t *out) {
  const uint8_t *node = (const uint8_t *)di->i_block;
  size_t nbytes = sizeof(di->i_block);
  bc_buf_t *held = NULL;
  *out = 0;
  for (int level = 0; level < 8; level++) {
    uint16_t magic, entries, maxe, depth;
    memcpy(&magic, node, 2);
    memcpy(&entries, node + 2, 2);
    memcpy(&maxe, node + 4, 2);
    memcpy(&depth, node + 6, 2);
    if (magic != EXT4_EXT_MAGIC || entries > maxe ||
        12u + (size_t)entries * 12u > nbytes || depth > 5) {
      if (held)
        bc_put(held);
      return -EIO;
    }
    if (depth == 0) {
      int rc = 0;
      for (int i = (int)entries - 1; i >= 0; i--) {
        const uint8_t *e = node + 12 + (size_t)i * 12;
        uint32_t eb, lo;
        uint16_t len, hi;
        memcpy(&eb, e, 4);
        memcpy(&len, e + 4, 2);
        memcpy(&hi, e + 6, 2);
        memcpy(&lo, e + 8, 4);
        if (eb > lblk)
          continue;
        uint32_t n = len > 32768 ? (uint32_t)len - 32768 : len;
        if (lblk - eb < n && len <= 32768) {
          uint64_t pb = ((uint64_t)hi << 32 | lo) + (lblk - eb);
          if (pb == 0 || pb >= fs->blocks_count)
            rc = -EIO;
          else
            *out = (uint32_t)pb;
        }
        break;
      }
      if (held)
        bc_put(held);
      return rc;
    }
    uint64_t child = 0;
    int found = 0;
    for (int i = (int)entries - 1; i >= 0; i--) {
      const uint8_t *e = node + 12 + (size_t)i * 12;
      uint32_t eb, lo;
      uint16_t hi;
      memcpy(&eb, e, 4);
      memcpy(&lo, e + 4, 4);
      memcpy(&hi, e + 8, 2);
      if (eb <= lblk) {
        child = (uint64_t)hi << 32 | lo;
        found = 1;
        break;
      }
    }
    if (held)
      bc_put(held);
    held = NULL;
    if (!found)
      return 0; // antes del primer extent: hueco
    if (child == 0 || child >= fs->blocks_count)
      return -EIO;
    held = bc_get(fs->bdev, (uint32_t)child, fs->bs);
    if (!held)
      return -EIO;
    node = bc_data(held);
    nbytes = fs->bs;
  }
  if (held)
    bc_put(held);
  return -EIO;
}

// Bloque físico del bloque lógico `lblk`. *out = 0 para un hueco.
static int ext2_bmap(ext2_fs_t *fs, const ext2_inode_t *di, uint64_t lblk,
                     uint32_t *out) {
  if ((fs->incompat & EXT4_FEATURE_INCOMPAT_EXTENTS) &&
      (di->i_flags & EXT4_EXTENTS_FL))
    return ext4_extent_map(fs, di, lblk, out);
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
// Ante un fallo de caché trae de una sola petición todos los bloques
// físicamente contiguos que pide la lectura más `extra` de lectura anticipada
// (máx. 64): una petición grande al disco en vez de una por bloque de 4 KiB.
static int64_t ext2_read_data_x(ext2_fs_t *fs, const ext2_inode_t *di,
                                uint64_t fsize, uint64_t offset, size_t size,
                                void *buf, uint32_t extra) {
  if (offset >= fsize || size == 0)
    return 0;
  if (size > fsize - offset)
    size = (size_t)(fsize - offset);
  uint8_t *dst = buf;
  size_t done = 0;
  uint64_t flast = (fsize + fs->bs - 1) / fs->bs; // bloques lógicos del fichero
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
      bc_buf_t *b = bc_get_cached(fs->bdev, pblk, fs->bs);
      if (!b) {
        uint64_t want = ((size - done) + boff + fs->bs - 1) / fs->bs + extra;
        if (want > flast - lblk)
          want = flast - lblk;
        if (want > 64)
          want = 64;
        uint32_t run = 1;
        while (run < want) {
          uint32_t nb;
          if (ext2_bmap(fs, di, lblk + run, &nb) != 0 || nb != pblk + run)
            break;
          run++;
        }
        if (run > 1)
          bc_readahead(fs->bdev, pblk, run, fs->bs);
        b = bc_get(fs->bdev, pblk, fs->bs);
      }
      if (!b)
        return done ? (int64_t)done : -EIO;
      memcpy(dst + done, (uint8_t *)bc_data(b) + boff, n);
      bc_put(b);
    }
    done += n;
  }
  return (int64_t)done;
}

static int64_t ext2_read_data(ext2_fs_t *fs, const ext2_inode_t *di,
                              uint64_t fsize, uint64_t offset, size_t size,
                              void *buf) {
  return ext2_read_data_x(fs, di, fsize, offset, size, buf, 0);
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


// Caché directa (dir, nombre) -> inodo para directorios de >= 2 bloques: evita
// el escaneo lineal (ls -l sobre miles de ficheros era cuadrático). Solo
// entradas positivas; se invalida al borrar/renombrar y al liberar el dir.
#define EXT2_DC_N 32768
#define EXT2_DC_NAME 23
struct ext2_dc {
  uint32_t dir, ino;
  uint8_t nl;
  char name[EXT2_DC_NAME];
};
static uint32_t ext2_dc_slot(uint32_t dir, const char *n, size_t nl) {
  uint32_t h = 2166136261u ^ dir;
  for (size_t i = 0; i < nl; i++)
    h = (h ^ (uint8_t)n[i]) * 16777619u;
  h ^= h >> 15;
  return h & (EXT2_DC_N - 1);
}
static void ext2_dc_inval(ext2_fs_t *fs, uint32_t dir, const char *n,
                          size_t nl) {
  if (!fs->dcache || nl > EXT2_DC_NAME)
    return;
  struct ext2_dc *e = &fs->dcache[ext2_dc_slot(dir, n, nl)];
  if (e->dir == dir && e->nl == nl && memcmp(e->name, n, nl) == 0)
    e->dir = 0;
}
static void ext2_dc_drop_dir(ext2_fs_t *fs, uint32_t dir) {
  if (!fs->dcache)
    return;
  for (uint32_t i = 0; i < EXT2_DC_N; i++)
    if (fs->dcache[i].dir == dir)
      fs->dcache[i].dir = 0;
}
static void ext2_dc_put(ext2_fs_t *fs, uint32_t dir, const char *n, size_t nl,
                        uint32_t ino) {
  if (nl > EXT2_DC_NAME || !dir)
    return;
  if (!fs->dcache)
    fs->dcache = kzalloc(sizeof(struct ext2_dc) * EXT2_DC_N);
  if (!fs->dcache)
    return;
  struct ext2_dc *e = &fs->dcache[ext2_dc_slot(dir, n, nl)];
  e->dir = dir;
  e->ino = ino;
  e->nl = (uint8_t)nl;
  memcpy(e->name, n, nl);
}
static int ext2_dir_find(ext2_fs_t *fs, const ext2_inode_t *di, uint64_t dsize,
                         const char *name, size_t nlen, uint32_t *ino_out);
static int ext2_dir_lookup(ext2_fs_t *fs, uint32_t dino, const ext2_inode_t *di,
                           uint64_t dsize, const char *name, size_t nlen,
                           uint32_t *ino_out) {
  if (dsize < 2ull * fs->bs || nlen > EXT2_DC_NAME || dino == 0)
    return ext2_dir_find(fs, di, dsize, name, nlen, ino_out);
  if (fs->dcache) {
    struct ext2_dc *e = &fs->dcache[ext2_dc_slot(dino, name, nlen)];
    if (e->dir == dino && e->nl == nlen && memcmp(e->name, name, nlen) == 0) {
      *ino_out = e->ino;
      return 0;
    }
  }
  int rc = ext2_dir_find(fs, di, dsize, name, nlen, ino_out);
  if (rc == 0)
    ext2_dc_put(fs, dino, name, nlen, *ino_out);
  return rc;
}

// ---------------------------------------------------------------------------
// E2: base de escritura (superbloque, bitmaps, inodos en memoria)
// ---------------------------------------------------------------------------
static inline uint32_t ext2_now(void) { return (uint32_t)rtc_get_epoch(); }
static inline int bit_test(const uint8_t *m, uint32_t i) {
  return (m[i >> 3] >> (i & 7)) & 1;
}
static inline void bit_set(uint8_t *m, uint32_t i) { m[i >> 3] |= 1u << (i & 7); }
static inline void bit_clr(uint8_t *m, uint32_t i) {
  m[i >> 3] &= (uint8_t)~(1u << (i & 7));
}
// Primer bit a 0 en [from, nbits) o UINT32_MAX.
static uint32_t bit_find_zero(const uint8_t *m, uint32_t from, uint32_t nbits) {
  uint32_t i = from;
  while (i < nbits) {
    if ((i & 7) == 0 && i + 8 <= nbits && m[i >> 3] == 0xFF) {
      i += 8;
      continue;
    }
    if (!bit_test(m, i))
      return i;
    i++;
  }
  return UINT32_MAX;
}

static uint32_t ext2_group_first(const ext2_fs_t *fs, uint32_t g) {
  return fs->first_data_block + g * fs->blocks_per_group;
}
static uint32_t ext2_group_nblocks(const ext2_fs_t *fs, uint32_t g) {
  uint32_t first = ext2_group_first(fs, g);
  uint32_t left = fs->blocks_count - first;
  return left < fs->blocks_per_group ? left : fs->blocks_per_group;
}

// Escribe el superbloque (contadores, estado) en la bcache. `state` es el
// valor de s_state a dejar.
static int ext2_sb_store(ext2_fs_t *fs, uint16_t state) {
  uint32_t blk = 1024 / fs->bs, off = 1024 % fs->bs;
  bc_buf_t *b = bc_get(fs->bdev, blk, fs->bs);
  if (!b)
    return -EIO;
  ext2_sb_t sb;
  uint8_t *d = bc_data(b);
  memcpy(&sb, d + off, sizeof(sb));
  sb.s_free_blocks_count = fs->free_blocks;
  sb.s_free_inodes_count = fs->free_inodes;
  sb.s_state = state;
  if (fs->jn) { // con el journal activo, sucio = needs_recovery
    if (state & EXT2_STATE_VALID)
      sb.s_feature_incompat &= ~EXT2_FEATURE_INCOMPAT_RECOVER;
    else
      sb.s_feature_incompat |= EXT2_FEATURE_INCOMPAT_RECOVER;
  }
  sb.s_wtime = ext2_now();
  if (fs->mnt_pending) {
    fs->mnt_pending = 0;
    sb.s_mnt_count++;
    sb.s_mtime = sb.s_wtime;
  }
  if (sb.s_rev_level == 1)
    sb.s_feature_ro_compat = fs->ro_compat;
  memcpy(d + off, &sb, sizeof(sb));
  if (fs->csum) {
    uint32_t c = ext4_crc32c(~0u, d + off, 1020);
    memcpy(d + off + 1020, &c, 4);
  }
  bc_mark_dirty(b);
  bc_put(b);
  return 0;
}

// Superbloque a disco ya (con la barrera): se usa para pasar a "sucio" antes
// de la primera escritura y a "limpio" al sincronizar.
static int ext2_sb_commit(ext2_fs_t *fs, int clean) {
  uint16_t st = (uint16_t)(fs->s_state & ~EXT2_STATE_VALID);
  if (clean)
    st |= EXT2_STATE_VALID;
  int rc = ext2_sb_store(fs, st);
  if (rc == 0)
    rc = bc_sync_dev(fs->bdev);
  if (rc == 0)
    fs->disk_clean = clean;
  return rc;
}

// Antes de cualquier cambio: el FS debe ser escribible y el disco marcado
// como sucio (con barrera) por si cae la corriente a mitad.
static int ext2_begin_write(ext2_fs_t *fs) {
  if (!fs->rw)
    return -EROFS;
  if (fs->jn) { // que una transacción nunca desborde el journal
    struct bc_stats st;
    bc_get_stats(&st);
    if (st.dirty > 768)
      bc_sync_dev(fs->bdev);
  }
  if (fs->disk_clean) {
    int rc = ext2_sb_commit(fs, 0);
    if (rc != 0) {
      LOG_ERR("[EXT2] %s: no se pudo marcar el FS como sucio (%d)",
              fs->bdev->name, rc);
      fs->rw = 0;
      return -EIO;
    }
  }
  return 0;
}

static int ext2_gd_write(ext2_fs_t *fs, uint32_t g) {
  uint32_t per = fs->bs / fs->gdsz;
  bc_buf_t *b = bc_get(fs->bdev, fs->first_data_block + 1 + g / per, fs->bs);
  if (!b)
    return -EIO;
  uint8_t *p = (uint8_t *)bc_data(b) + (g % per) * fs->gdsz;
  ext2_gd_t gd;
  memcpy(&gd, p, sizeof(gd));
  gd.bg_free_blocks_count = (uint16_t)fs->groups[g].free_blocks;
  gd.bg_free_inodes_count = (uint16_t)fs->groups[g].free_inodes;
  gd.bg_used_dirs_count = (uint16_t)fs->groups[g].used_dirs;
  memcpy(p, &gd, sizeof(gd));
  if (fs->gdsz >= 64) { // mitades altas de los contadores
    uint16_t hb = (uint16_t)(fs->groups[g].free_blocks >> 16);
    uint16_t hi = (uint16_t)(fs->groups[g].free_inodes >> 16);
    uint16_t hd = (uint16_t)(fs->groups[g].used_dirs >> 16);
    memcpy(p + 0x2C, &hb, 2);
    memcpy(p + 0x2E, &hi, 2);
    memcpy(p + 0x30, &hd, 2);
  }
  if (fs->csum) {
    ext2_group_t *gr = &fs->groups[g];
    uint16_t v16 = gr->flags;
    memcpy(p + 0x12, &v16, 2);
    v16 = (uint16_t)gr->itable_unused;
    memcpy(p + 0x1C, &v16, 2);
    if (fs->gdsz >= 64) {
      v16 = (uint16_t)(gr->itable_unused >> 16);
      memcpy(p + 0x32, &v16, 2);
    }
    for (int k = 0; k < 2; k++) {
      uint32_t bm = k ? gr->inode_bitmap : gr->block_bitmap;
      if (!bm)
        continue;
      bc_buf_t *bb = bc_get(fs->bdev, bm, fs->bs);
      if (!bb)
        continue;
      uint32_t c = ext4_crc32c(fs->csum_seed, bc_data(bb),
                               (k ? fs->inodes_per_group : fs->blocks_per_group) / 8);
      bc_put(bb);
      uint16_t lo = (uint16_t)c, hi = (uint16_t)(c >> 16);
      memcpy(p + (k ? 0x1A : 0x18), &lo, 2);
      if (fs->gdsz >= 64)
        memcpy(p + (k ? 0x3A : 0x38), &hi, 2);
    }
    memset(p + 0x1E, 0, 2);
    uint32_t c = ext4_crc32c(fs->csum_seed, &g, 4);
    c = ext4_crc32c(c, p, fs->gdsz);
    uint16_t c16 = (uint16_t)c;
    memcpy(p + 0x1E, &c16, 2);
  }
  bc_mark_dirty(b);
  bc_put(b);
  return 0;
}

static int ext2_inode_slot(const ext2_fs_t *fs, uint32_t ino, uint64_t *blk,
                           uint32_t *off) {
  if (ino == 0 || ino > fs->inodes_count)
    return -EINVAL;
  uint32_t grp = (ino - 1) / fs->inodes_per_group;
  uint32_t idx = (ino - 1) % fs->inodes_per_group;
  if (grp >= fs->ngroups || fs->groups[grp].inode_table == 0)
    return -EIO;
  uint64_t byte = (uint64_t)idx * fs->inode_size;
  *blk = (uint64_t)fs->groups[grp].inode_table + byte / fs->bs;
  *off = (uint32_t)(byte % fs->bs);
  if (*blk >= fs->blocks_count || *off + fs->inode_size > fs->bs)
    return -EIO;
  return 0;
}

// Escribe los 128 bytes base del inodo (la parte extendida no se toca).
// zero_rest: pone a cero toda la ranura antes (inodo recién asignado).
static int ext2_write_inode(ext2_fs_t *fs, uint32_t ino, const ext2_inode_t *di,
                            int zero_rest) {
  uint64_t blk;
  uint32_t off;
  int rc = ext2_inode_slot(fs, ino, &blk, &off);
  if (rc != 0)
    return rc;
  bc_buf_t *b = bc_get(fs->bdev, blk, fs->bs);
  if (!b)
    return -EIO;
  uint8_t *d = (uint8_t *)bc_data(b) + off;
  if (zero_rest)
    memset(d, 0, fs->inode_size);
  memcpy(d, di, sizeof(*di));
  if (fs->csum && fs->inode_size >= 160) {
    uint16_t extra;
    memcpy(&extra, d + 0x80, 2);
    int has_hi = extra >= 4;
    memset(d + 0x7C, 0, 2);
    if (has_hi)
      memset(d + 0x82, 0, 2);
    uint32_t gen;
    memcpy(&gen, d + 0x64, 4);
    uint32_t c = ext4_crc32c(fs->csum_seed, &ino, 4);
    c = ext4_crc32c(c, &gen, 4);
    c = ext4_crc32c(c, d, fs->inode_size);
    uint16_t lo = (uint16_t)c, hi = (uint16_t)(c >> 16);
    memcpy(d + 0x7C, &lo, 2);
    if (has_hi)
      memcpy(d + 0x82, &hi, 2);
  }
  bc_mark_dirty(b);
  bc_put(b);
  return 0;
}

static void ext2_set_size(ext2_fs_t *fs, ext2_ino_t *ip, uint64_t sz) {
  ip->size = sz;
  ip->di.i_size = (uint32_t)sz;
  if ((ip->di.i_mode & S_IFMT) == S_IFREG &&
      (fs->ro_compat & EXT2_FEATURE_RO_COMPAT_LARGE_FILE))
    ip->di.i_size_high = (uint32_t)(sz >> 32);
}

static inline int ext2_ip_flush(ext2_fs_t *fs, ext2_ino_t *ip) {
  return ext2_write_inode(fs, ip->ino, &ip->di, 0);
}

static void ext2_blocks_add(ext2_fs_t *fs, ext2_ino_t *ip, int nblk) {
  uint32_t u = fs->bs >> 9;
  if (nblk > 0)
    ip->di.i_blocks += u;
  else
    ip->di.i_blocks = ip->di.i_blocks > u ? ip->di.i_blocks - u : 0;
}

// ---------------------------------------------------------------------------
// Bloques
// ---------------------------------------------------------------------------
static int ext2_blk_is_meta(const ext2_fs_t *fs, uint32_t g, uint32_t blk) {
  const ext2_group_t *gr = &fs->groups[g];
  return blk == gr->block_bitmap || blk == gr->inode_bitmap ||
         (blk >= gr->inode_table && blk < gr->inode_table + fs->itable_blocks);
}

static void ext2_commit_frees(ext2_fs_t *fs);

static int ext2_alloc_block_try(ext2_fs_t *fs, uint32_t goal, uint32_t *out) {
  process_t *p = process_current();
  if (fs->free_blocks == 0 ||
      (fs->free_blocks <= fs->r_blocks_count && p && p->euid != 0))
    return -ENOSPC;
  uint32_t g0 = 0, gbit = 0;
  if (goal >= fs->first_data_block && goal < fs->blocks_count) {
    g0 = (goal - fs->first_data_block) / fs->blocks_per_group;
    gbit = (goal - fs->first_data_block) % fs->blocks_per_group;
  }
  for (uint32_t n = 0; n < fs->ngroups; n++) {
    uint32_t g = (g0 + n) % fs->ngroups;
    ext2_group_t *gr = &fs->groups[g];
    if (gr->free_blocks == 0 || gr->block_bitmap == 0)
      continue;
    uint32_t first = ext2_group_first(fs, g), nbits = ext2_group_nblocks(fs, g);
    bc_buf_t *b = bc_get(fs->bdev, gr->block_bitmap, fs->bs);
    if (!b)
      return -EIO;
    uint8_t *m = bc_data(b);
    uint32_t from = (n == 0) ? gbit : 0;
    uint32_t bit = bit_find_zero(m, from, nbits);
    if (bit == UINT32_MAX && from > 0)
      bit = bit_find_zero(m, 0, from);
    if (bit != UINT32_MAX && ext2_blk_is_meta(fs, g, first + bit)) {
      LOG_WARN("[EXT2] %s: bitmap de bloques incoherente (grupo %u): bloque de "
               "metadatos libre", fs->bdev->name, g);
      bit = UINT32_MAX;
    }
    if (bit == UINT32_MAX) {
      bc_put(b);
      LOG_WARN("[EXT2] %s: grupo %u dice tener bloques libres pero su bitmap "
               "está lleno", fs->bdev->name, g);
      gr->free_blocks = 0;
      continue;
    }
    bit_set(m, bit);
    bc_mark_dirty(b);
    bc_put(b);
    gr->free_blocks--;
    fs->free_blocks--;
    ext2_gd_write(fs, g);
    *out = first + bit;
    return 0;
  }
  return -ENOSPC;
}

static void ext2_free_block_now(ext2_fs_t *fs, uint32_t blk) {
  if (blk < fs->first_data_block || blk >= fs->blocks_count) {
    LOG_WARN("[EXT2] %s: liberar bloque fuera de rango (%u)", fs->bdev->name,
             blk);
    return;
  }
  uint32_t g = (blk - fs->first_data_block) / fs->blocks_per_group;
  uint32_t bit = (blk - fs->first_data_block) % fs->blocks_per_group;
  ext2_group_t *gr = &fs->groups[g];
  if (gr->block_bitmap == 0 || ext2_blk_is_meta(fs, g, blk))
    return;
  bc_buf_t *b = bc_get(fs->bdev, gr->block_bitmap, fs->bs);
  if (!b)
    return;
  uint8_t *m = bc_data(b);
  if (!bit_test(m, bit)) {
    bc_put(b);
    LOG_WARN("[EXT2] %s: doble liberación del bloque %u", fs->bdev->name, blk);
    return;
  }
  bit_clr(m, bit);
  bc_mark_dirty(b);
  bc_put(b);
  gr->free_blocks++;
  fs->free_blocks++;
  ext2_gd_write(fs, g);
}

static int ext2_pend_push(uint32_t **arr, uint32_t *n, uint32_t *cap,
                          uint32_t v) {
  if (*n == *cap) {
    uint32_t nc = *cap ? *cap * 2 : 256;
    uint32_t *na = kmalloc((size_t)nc * 4);
    if (!na)
      return -ENOMEM;
    if (*arr) {
      memcpy(na, *arr, (size_t)*n * 4);
      kfree(*arr);
    }
    *arr = na;
    *cap = nc;
  }
  (*arr)[(*n)++] = v;
  return 0;
}

// Liberar (diferido): el bit sigue ocupado hasta la próxima barrera.
static void ext2_free_block(ext2_fs_t *fs, uint32_t blk) {
  if (ext2_pend_push(&fs->pend_blk, &fs->npend_blk, &fs->cap_blk, blk) != 0)
    ext2_free_block_now(fs, blk); // sin memoria: se libera ya
}

// Asignar; si no hay sitio pero hay liberaciones pendientes, se fuerza la
// barrera para recuperarlas y se reintenta.
static int ext2_alloc_block(ext2_fs_t *fs, uint32_t goal, uint32_t *out) {
  int rc = ext2_alloc_block_try(fs, goal, out);
  if (rc == -ENOSPC && fs->npend_blk) {
    ext2_commit_frees(fs);
    rc = ext2_alloc_block_try(fs, goal, out);
  }
  return rc;
}

// Bloque nuevo a ceros (datos o indirecto) ya sucio en la bcache.
static int ext2_new_zero_block(ext2_fs_t *fs, uint32_t goal, uint32_t *out) {
  uint32_t blk;
  int rc = ext2_alloc_block(fs, goal, &blk);
  if (rc != 0)
    return rc;
  bc_buf_t *b = bc_get_new(fs->bdev, blk, fs->bs);
  if (!b) {
    ext2_free_block_now(fs, blk);
    return -EIO;
  }
  memset(bc_data(b), 0, fs->bs);
  bc_mark_dirty(b);
  bc_put(b);
  *out = blk;
  return 0;
}

// ---------------------------------------------------------------------------
// Inodos: asignar / liberar / caché
// ---------------------------------------------------------------------------
static int ext2_alloc_inode_try(ext2_fs_t *fs, uint32_t parent_ino, int is_dir,
                                uint32_t *out) {
  if (fs->free_inodes == 0)
    return -ENOSPC;
  uint32_t g0 = 0;
  if (parent_ino >= 1 && parent_ino <= fs->inodes_count)
    g0 = (parent_ino - 1) / fs->inodes_per_group;
  uint32_t avg_i = fs->free_inodes / fs->ngroups;
  uint32_t avg_b = fs->free_blocks / fs->ngroups;
  for (int pass = 0; pass < 2; pass++) {
    for (uint32_t n = 0; n < fs->ngroups; n++) {
      uint32_t g = (g0 + n) % fs->ngroups;
      ext2_group_t *gr = &fs->groups[g];
      if (gr->free_inodes == 0 || gr->inode_bitmap == 0 || gr->inode_table == 0)
        continue;
      // Directorios: preferir grupos con espacio de sobra (los reparte).
      if (is_dir && pass == 0 &&
          (gr->free_inodes < avg_i || gr->free_blocks < avg_b))
        continue;
      uint32_t nbits = fs->inodes_per_group;
      if ((uint64_t)(g + 1) * fs->inodes_per_group > fs->inodes_count)
        nbits = fs->inodes_count - g * fs->inodes_per_group;
      bc_buf_t *b = bc_get(fs->bdev, gr->inode_bitmap, fs->bs);
      if (!b)
        return -EIO;
      uint8_t *m = bc_data(b);
      uint32_t from = 0, bit;
      for (;;) {
        bit = bit_find_zero(m, from, nbits);
        if (bit == UINT32_MAX)
          break;
        if (g * fs->inodes_per_group + bit + 1 < fs->first_ino) {
          from = bit + 1; // inodos reservados
          continue;
        }
        break;
      }
      if (bit == UINT32_MAX) {
        bc_put(b);
        gr->free_inodes = 0;
        continue;
      }
      bit_set(m, bit);
      bc_mark_dirty(b);
      bc_put(b);
      if (fs->csum) {
        uint32_t top = bit + 1;
        if (top > fs->inodes_per_group - gr->itable_unused)
          gr->itable_unused = fs->inodes_per_group - top;
        gr->flags &= (uint16_t)~1u;
      }
      gr->free_inodes--;
      fs->free_inodes--;
      if (is_dir)
        gr->used_dirs++;
      ext2_gd_write(fs, g);
      *out = g * fs->inodes_per_group + bit + 1;
      return 0;
    }
  }
  return -ENOSPC;
}

static void ext2_free_inode_now(ext2_fs_t *fs, uint32_t ino, int is_dir) {
  if (ino < fs->first_ino || ino > fs->inodes_count)
    return;
  uint32_t g = (ino - 1) / fs->inodes_per_group;
  uint32_t bit = (ino - 1) % fs->inodes_per_group;
  ext2_group_t *gr = &fs->groups[g];
  if (gr->inode_bitmap == 0)
    return;
  bc_buf_t *b = bc_get(fs->bdev, gr->inode_bitmap, fs->bs);
  if (!b)
    return;
  uint8_t *m = bc_data(b);
  if (!bit_test(m, bit)) {
    bc_put(b);
    LOG_WARN("[EXT2] %s: doble liberación del inodo %u", fs->bdev->name, ino);
    return;
  }
  bit_clr(m, bit);
  bc_mark_dirty(b);
  bc_put(b);
  gr->free_inodes++;
  fs->free_inodes++;
  if (is_dir && gr->used_dirs > 0)
    gr->used_dirs--;
  ext2_gd_write(fs, g);
}

static void ext2_free_inode(ext2_fs_t *fs, uint32_t ino, int is_dir) {
  if (ext2_pend_push(&fs->pend_ino, &fs->npend_ino, &fs->cap_ino,
                     ino | (is_dir ? 0x80000000u : 0)) != 0)
    ext2_free_inode_now(fs, ino, is_dir);
}

static int ext2_alloc_inode(ext2_fs_t *fs, uint32_t parent_ino, int is_dir,
                            uint32_t *out) {
  int rc = ext2_alloc_inode_try(fs, parent_ino, is_dir, out);
  if (rc == -ENOSPC && fs->npend_ino) {
    ext2_commit_frees(fs);
    rc = ext2_alloc_inode_try(fs, parent_ino, is_dir, out);
  }
  return rc;
}

// Barrera + liberación real de lo diferido. Con fs->lock tomado.
static void ext2_commit_frees(ext2_fs_t *fs) {
  if (!fs->npend_blk && !fs->npend_ino)
    return;
  if (bc_sync_dev(fs->bdev) != 0) // inodos/directorios ya en disco
    return;                       // si falla, se reintenta más tarde
  for (uint32_t i = 0; i < fs->npend_blk; i++)
    ext2_free_block_now(fs, fs->pend_blk[i]);
  for (uint32_t i = 0; i < fs->npend_ino; i++)
    ext2_free_inode_now(fs, fs->pend_ino[i] & 0x7fffffffu,
                        (fs->pend_ino[i] & 0x80000000u) != 0);
  fs->npend_blk = fs->npend_ino = 0;
}

// Un inodo tiene punteros a bloques en i_block[]? (no: dispositivos, FIFOs,
// sockets ni symlinks rápidos).
static int ext2_has_blocks(const ext2_fs_t *fs, const ext2_inode_t *di) {
  uint16_t t = di->i_mode & S_IFMT;
  if (t == S_IFREG || t == S_IFDIR)
    return 1;
  if (t == S_IFLNK) {
    int ea = di->i_file_acl ? (int)(fs->bs >> 9) : 0;
    return (int)di->i_blocks - ea != 0;
  }
  return 0;
}

// Maneja el camino de indirección de `lblk`: devuelve la profundidad 0..3 y
// rellena idx[0] (ranura de i_block) e idx[1..d] (índices en cada bloque), o -1
// si está más allá del máximo direccionable.
static int ext2_path(const ext2_fs_t *fs, uint64_t lblk, uint32_t idx[4]) {
  uint64_t apb = fs->bs / 4;
  if (lblk < EXT2_NDIR_BLOCKS) {
    idx[0] = (uint32_t)lblk;
    return 0;
  }
  lblk -= EXT2_NDIR_BLOCKS;
  if (lblk < apb) {
    idx[0] = EXT2_IND_BLOCK;
    idx[1] = (uint32_t)lblk;
    return 1;
  }
  lblk -= apb;
  if (lblk < apb * apb) {
    idx[0] = EXT2_DIND_BLOCK;
    idx[1] = (uint32_t)(lblk / apb);
    idx[2] = (uint32_t)(lblk % apb);
    return 2;
  }
  lblk -= apb * apb;
  if (lblk < apb * apb * apb) {
    idx[0] = EXT2_TIND_BLOCK;
    idx[1] = (uint32_t)(lblk / (apb * apb));
    idx[2] = (uint32_t)((lblk / apb) % apb);
    idx[3] = (uint32_t)(lblk % apb);
    return 3;
  }
  return -1;
}

static uint32_t ext2_ip_goal(const ext2_fs_t *fs, const ext2_ino_t *ip) {
  if (ip->goal)
    return ip->goal + 1;
  uint32_t g = (ip->ino - 1) / fs->inodes_per_group;
  return ext2_group_first(fs, g);
}

// Bloque físico del lógico `lblk`; si `create`, lo asigna (y los indirectos
// que falten). *out = 0 si es un hueco y !create. Modifica ip->di: el
// llamante debe volcar el inodo (ext2_ip_flush).
static int ext4_bmap_create(ext2_fs_t *fs, ext2_ino_t *ip, uint64_t lblk, int create,
                            uint32_t *out);
static int ext2_bmap_create(ext2_fs_t *fs, ext2_ino_t *ip, uint64_t lblk,
                            int create, uint32_t *out) {
  if (ip->di.i_flags & EXT4_EXTENTS_FL)
    return ext4_bmap_create(fs, ip, lblk, create, out);
  uint32_t idx[4];
  int d = ext2_path(fs, lblk, idx);
  if (d < 0)
    return -EFBIG;
  uint32_t *slot = (uint32_t *)(void *)&ip->di.i_block[idx[0]];
  uint32_t ptr = *slot;
  bc_buf_t *parent = NULL;
  int rc = 0;
  for (int level = 0;; level++) {
    if (ptr == 0) {
      if (!create) {
        *out = 0;
        goto done;
      }
      uint32_t nb;
      rc = ext2_new_zero_block(fs, ext2_ip_goal(fs, ip), &nb);
      if (rc != 0)
        goto done;
      ext2_blocks_add(fs, ip, 1);
      ip->goal = nb;
      *slot = nb;
      if (parent)
        bc_mark_dirty(parent);
      ptr = nb;
    }
    if (level == d)
      break;
    if (ptr < fs->first_data_block || ptr >= fs->blocks_count) {
      rc = -EIO;
      goto done;
    }
    bc_buf_t *b = bc_get(fs->bdev, ptr, fs->bs);
    if (!b) {
      rc = -EIO;
      goto done;
    }
    if (parent)
      bc_put(parent);
    parent = b;
    slot = (uint32_t *)bc_data(b) + idx[level + 1];
    ptr = *slot;
  }
  if (ptr != 0 && (ptr < fs->first_data_block || ptr >= fs->blocks_count)) {
    rc = -EIO;
    goto done;
  }
  *out = ptr;
done:
  if (parent)
    bc_put(parent);
  return rc;
}


// ---------------------------------------------------------------------------
// E9.2: escritura en ficheros con extents (árbol de profundidad 0 o 1)
// ---------------------------------------------------------------------------
#define E4_HDR 12u
static inline uint16_t e4_u16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static inline uint32_t e4_u32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static inline void e4_set16(uint8_t *p, uint16_t v) { memcpy(p, &v, 2); }
static inline void e4_set32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
#define E4_ENT(n, i) ((n) + E4_HDR + (size_t)(i) * 12)
#define E4_ENTRIES(n) e4_u16((n) + 2)
#define E4_MAXE(n) e4_u16((n) + 4)
#define E4_DEPTH(n) e4_u16((n) + 6)
// extent: eb(4) len(2) hi(2) lo(4). índice: eb(4) leaf_lo(4) leaf_hi(2) pad(2)
#define E4_LEN(e) ((uint32_t)e4_u16((e) + 4))
static inline uint32_t e4_nblk(const uint8_t *e) {
  uint32_t l = E4_LEN(e);
  return l > 32768 ? l - 32768 : l;
}
static inline int e4_unwritten(const uint8_t *e) { return E4_LEN(e) > 32768; }

static int e4_node_ok(const ext2_fs_t *fs, const uint8_t *n, size_t nbytes) {
  (void)fs;
  return e4_u16(n) == EXT4_EXT_MAGIC && E4_ENTRIES(n) <= E4_MAXE(n) &&
         E4_HDR + (size_t)E4_MAXE(n) * 12 <= nbytes && E4_DEPTH(n) <= 5;
}

// Marca sucio un bloque de extents recalculando su checksum (tail tras los maxe).
static void e4_leaf_dirty(ext2_fs_t *fs, ext2_ino_t *ip, bc_buf_t *b) {
  uint8_t *n = bc_data(b);
  if (fs->csum) {
    size_t off = E4_HDR + (size_t)E4_MAXE(n) * 12;
    if (off + 4 <= fs->bs) {
      uint32_t c = ext4_crc32c(fs->csum_seed, &ip->ino, 4);
      c = ext4_crc32c(c, &ip->di.i_generation, 4);
      c = ext4_crc32c(c, n, off);
      memcpy(n + off, &c, 4);
    }
  }
  bc_mark_dirty(b);
}

// Libera `n` bloques desde `pb`.
static void e4_free_run(ext2_fs_t *fs, ext2_ino_t *ip, uint32_t pb, uint32_t n) {
  for (uint32_t i = 0; i < n; i++) {
    if (pb + i >= fs->first_data_block && pb + i < fs->blocks_count) {
      ext2_free_block(fs, pb + i);
      ext2_blocks_add(fs, ip, -1);
    }
  }
}

// Recorta las hojas de `n` dejando solo los bloques lógicos < keep. Devuelve
// cuántas entradas quedan.
static uint16_t e4_trim_leaf(ext2_fs_t *fs, ext2_ino_t *ip, uint8_t *n,
                             uint64_t keep, int *changed) {
  uint16_t ne = E4_ENTRIES(n);
  while (ne > 0) {
    uint8_t *e = E4_ENT(n, ne - 1);
    uint32_t eb = e4_u32(e), cnt = e4_nblk(e), lo = e4_u32(e + 8);
    if (e4_u16(e + 6) != 0)
      break; // bloque físico > 2^32: no lo tocamos
    if (eb >= keep) {
      e4_free_run(fs, ip, lo, cnt);
      ne--;
      *changed = 1;
      continue;
    }
    if ((uint64_t)eb + cnt > keep) {
      uint32_t nl = (uint32_t)(keep - eb);
      e4_free_run(fs, ip, lo + nl, cnt - nl);
      e4_set16(e + 4, (uint16_t)(e4_unwritten(e) ? 32768 + nl : nl));
      *changed = 1;
    }
    break;
  }
  e4_set16(n + 2, ne);
  return ne;
}

static void e4_write_hdr(uint8_t *n, uint16_t entries, uint16_t maxe, uint16_t depth) {
  e4_set16(n, EXT4_EXT_MAGIC);
  e4_set16(n + 2, entries);
  e4_set16(n + 4, maxe);
  e4_set16(n + 6, depth);
  e4_set32(n + 8, 0);
}

// Un único extent (lblk 0, 1 bloque) en la raíz vacía del inodo.
static void e4_first_extent(ext2_inode_t *di, uint32_t pb) {
  uint8_t *root = (uint8_t *)di->i_block;
  e4_set16(root + 2, 1);
  uint8_t *e = E4_ENT(root, 0);
  e4_set32(e, 0);
  e4_set16(e + 4, 1);
  e4_set16(e + 6, 0);
  e4_set32(e + 8, pb);
}

// Recorta recursivamente el nodo `node` (de profundidad `depth`) dejando solo
// los bloques lógicos < keep. Libera los nodos hijos que quedan vacíos.
// Devuelve las entradas que quedan; *changed = 1 si tocó algo.
static uint16_t e4_trim_node(ext2_fs_t *fs, ext2_ino_t *ip, uint8_t *node, int depth,
                             uint64_t keep, int *changed) {
  if (depth == 0)
    return e4_trim_leaf(fs, ip, node, keep, changed);
  uint16_t ne = E4_ENTRIES(node);
  while (ne > 0) {
    uint8_t *ie = E4_ENT(node, ne - 1);
    uint32_t child = e4_u32(ie + 4);
    if (e4_u16(ie + 8) != 0 || child < fs->first_data_block || child >= fs->blocks_count)
      break;
    bc_buf_t *b = bc_get(fs->bdev, child, fs->bs);
    if (!b)
      break;
    uint8_t *cn = bc_data(b);
    if (!e4_node_ok(fs, cn, fs->bs) || E4_DEPTH(cn) != depth - 1) {
      bc_put(b);
      break;
    }
    int cch = 0;
    uint16_t left = e4_trim_node(fs, ip, cn, depth - 1, keep, &cch);
    if (cch)
      e4_leaf_dirty(fs, ip, b);
    bc_put(b);
    if (left > 0)
      break; // quedan extents < keep aquí: los hermanos anteriores no cambian
    ext2_free_block(fs, child);
    ext2_blocks_add(fs, ip, -1);
    ne--;
    *changed = 1;
  }
  e4_set16(node + 2, ne);
  return ne;
}

static void ext4_trunc_blocks(ext2_fs_t *fs, ext2_ino_t *ip, uint64_t keep) {
  uint8_t *root = (uint8_t *)ip->di.i_block;
  if (!e4_node_ok(fs, root, sizeof(ip->di.i_block))) {
    LOG_WARN("[EXT2] %s: árbol de extents inválido en ino %u", fs->bdev->name, ip->ino);
    return;
  }
  int ch = 0;
  uint16_t left = e4_trim_node(fs, ip, root, E4_DEPTH(root), keep, &ch);
  if (left == 0)
    e4_set16(root + 6, 0); // árbol vacío: vuelve a profundidad 0
  ip->goal = 0;
}

// --- camino raíz -> hoja -------------------------------------------------
typedef struct {
  uint8_t *node[6];
  bc_buf_t *buf[6]; // NULL en el nivel 0 (raíz, dentro del inodo)
  int idx[6];       // entrada elegida en cada nivel (-1: ninguna <= lblk, solo en la hoja)
  int depth;
} e4path_t;

static void e4_path_put(e4path_t *p) {
  for (int i = 0; i <= p->depth && i < 6; i++)
    if (p->buf[i]) {
      bc_put(p->buf[i]);
      p->buf[i] = NULL;
    }
}

static int e4_find(ext2_fs_t *fs, ext2_ino_t *ip, uint64_t lblk, e4path_t *p) {
  memset(p, 0, sizeof(*p));
  uint8_t *node = (uint8_t *)ip->di.i_block;
  if (!e4_node_ok(fs, node, sizeof(ip->di.i_block)))
    return -EIO;
  p->depth = E4_DEPTH(node);
  for (int lv = 0; lv <= p->depth; lv++) {
    p->node[lv] = node;
    uint16_t ne = E4_ENTRIES(node);
    int idx = -1;
    for (int i = 0; i < ne; i++)
      if (e4_u32(E4_ENT(node, i)) <= lblk)
        idx = i;
      else
        break;
    if (lv == p->depth) {
      p->idx[lv] = idx;
      return 0;
    }
    if (ne == 0) {
      e4_path_put(p);
      return -EIO;
    }
    if (idx < 0)
      idx = 0;
    p->idx[lv] = idx;
    uint8_t *ie = E4_ENT(node, idx);
    uint32_t child = e4_u32(ie + 4);
    if (e4_u16(ie + 8) != 0 || child < fs->first_data_block || child >= fs->blocks_count) {
      p->depth = lv;
      e4_path_put(p);
      return -EIO;
    }
    bc_buf_t *b = bc_get(fs->bdev, child, fs->bs);
    if (!b) {
      p->depth = lv;
      e4_path_put(p);
      return -EIO;
    }
    node = bc_data(b);
    if (!e4_node_ok(fs, node, fs->bs) || E4_DEPTH(node) != p->depth - lv - 1) {
      bc_put(b);
      p->depth = lv;
      e4_path_put(p);
      return -EIO;
    }
    p->buf[lv + 1] = b;
  }
  return -EIO;
}

static void e4_dirty_level(ext2_fs_t *fs, ext2_ino_t *ip, e4path_t *p, int lv) {
  if (lv > 0)
    e4_leaf_dirty(fs, ip, p->buf[lv]);
}

// Parte el nodo del nivel `lv` (o, si es la raíz, la hace crecer). El padre debe
// tener hueco. Después hay que volver a buscar el camino.
static int e4_split(ext2_fs_t *fs, ext2_ino_t *ip, e4path_t *p, int lv) {
  uint8_t *node = p->node[lv];
  uint32_t nb;
  int rc = ext2_new_zero_block(fs, ext2_ip_goal(fs, ip), &nb);
  if (rc != 0)
    return rc;
  bc_buf_t *nbuf = bc_get_new(fs->bdev, nb, fs->bs);
  if (!nbuf) {
    ext2_free_block(fs, nb);
    return -EIO;
  }
  ext2_blocks_add(fs, ip, 1);
  uint8_t *nn = bc_data(nbuf);
  uint16_t cap = (uint16_t)((fs->bs - E4_HDR) / 12);
  uint16_t ne = E4_ENTRIES(node), depth = E4_DEPTH(node);
  memset(nn, 0, fs->bs);
  if (lv == 0) { // la raíz crece: todo su contenido pasa al bloque nuevo
    if (depth >= 4) {
      bc_put(nbuf);
      ext2_free_block(fs, nb);
      ext2_blocks_add(fs, ip, -1);
      return -EFBIG;
    }
    e4_write_hdr(nn, ne, cap, depth);
    memcpy(E4_ENT(nn, 0), E4_ENT(node, 0), (size_t)ne * 12);
    uint32_t first = ne ? e4_u32(E4_ENT(node, 0)) : 0;
    e4_write_hdr(node, 1, E4_MAXE(node), (uint16_t)(depth + 1));
    uint8_t *ie = E4_ENT(node, 0);
    memset(ie, 0, 12);
    e4_set32(ie, first);
    e4_set32(ie + 4, nb);
  } else { // nodo normal: la mitad alta va al bloque nuevo
    uint8_t *par = p->node[lv - 1];
    uint16_t pn = E4_ENTRIES(par);
    uint16_t half = ne / 2, cnt = ne - half;
    e4_write_hdr(nn, cnt, cap, depth);
    memcpy(E4_ENT(nn, 0), E4_ENT(node, half), (size_t)cnt * 12);
    e4_set16(node + 2, half);
    int at = p->idx[lv - 1] + 1;
    memmove(E4_ENT(par, at + 1), E4_ENT(par, at), (size_t)(pn - at) * 12);
    uint8_t *ie = E4_ENT(par, at);
    memset(ie, 0, 12);
    e4_set32(ie, e4_u32(E4_ENT(nn, 0)));
    e4_set32(ie + 4, nb);
    e4_set16(par + 2, pn + 1);
    e4_dirty_level(fs, ip, p, lv - 1);
    e4_dirty_level(fs, ip, p, lv);
  }
  e4_leaf_dirty(fs, ip, nbuf);
  bc_put(nbuf);
  return 0;
}

// Garantiza `need` entradas libres en la hoja que cubre `lblk`.
static int e4_ensure_room(ext2_fs_t *fs, ext2_ino_t *ip, uint64_t lblk, int need) {
  for (int round = 0; round < 12; round++) {
    e4path_t p;
    int rc = e4_find(fs, ip, lblk, &p);
    if (rc != 0)
      return rc;
    uint8_t *leaf = p.node[p.depth];
    if ((int)E4_MAXE(leaf) - (int)E4_ENTRIES(leaf) >= need) {
      e4_path_put(&p);
      return 0;
    }
    int lv = p.depth;
    while (lv > 0 && E4_ENTRIES(p.node[lv - 1]) >= E4_MAXE(p.node[lv - 1]))
      lv--;
    rc = e4_split(fs, ip, &p, lv);
    e4_path_put(&p);
    if (rc != 0)
      return rc;
  }
  return -ENOSPC;
}

// Si lblk cae dentro de un extent sin inicializar, lo parte y deja inicializado
// solo ese bloque (a ceros). *out = bloque físico. 0 si no estaba en uno.
static int e4_convert_unwritten(ext2_fs_t *fs, ext2_ino_t *ip, uint64_t lblk,
                                uint32_t *out, int *converted) {
  *converted = 0;
  e4path_t p;
  int rc = e4_find(fs, ip, lblk, &p);
  if (rc != 0)
    return rc;
  uint8_t *leaf = p.node[p.depth];
  int idx = p.idx[p.depth];
  if (idx < 0) {
    e4_path_put(&p);
    return 0;
  }
  uint8_t *e = E4_ENT(leaf, idx);
  uint32_t eb = e4_u32(e), n = e4_nblk(e);
  if (!e4_unwritten(e) || lblk - eb >= n) {
    e4_path_put(&p);
    return 0;
  }
  if (e4_u16(e + 6) != 0) {
    e4_path_put(&p);
    return -EIO;
  }
  uint32_t pb0 = e4_u32(e + 8);
  uint32_t n1 = (uint32_t)(lblk - eb), n3 = n - n1 - 1;
  int np = 1 + (n1 ? 1 : 0) + (n3 ? 1 : 0);
  if (np > 1) {
    e4_path_put(&p);
    rc = e4_ensure_room(fs, ip, lblk, np - 1);
    if (rc != 0)
      return rc;
    rc = e4_find(fs, ip, lblk, &p);
    if (rc != 0)
      return rc;
    leaf = p.node[p.depth];
    idx = p.idx[p.depth];
    e = E4_ENT(leaf, idx);
    uint16_t ne = E4_ENTRIES(leaf);
    memmove(E4_ENT(leaf, idx + np), E4_ENT(leaf, idx + 1), (size_t)(ne - idx - 1) * 12);
    e4_set16(leaf + 2, ne + np - 1);
  }
  struct { uint32_t eb, len, pb; } pc[3];
  int k = 0;
  if (n1) { pc[k].eb = eb; pc[k].len = 32768 + n1; pc[k++].pb = pb0; }
  pc[k].eb = (uint32_t)lblk; pc[k].len = 1; pc[k++].pb = pb0 + n1;
  if (n3) { pc[k].eb = (uint32_t)lblk + 1; pc[k].len = 32768 + n3; pc[k++].pb = pb0 + n1 + 1; }
  for (int i = 0; i < k; i++) {
    uint8_t *d = E4_ENT(leaf, idx + i);
    e4_set32(d, pc[i].eb);
    e4_set16(d + 4, (uint16_t)pc[i].len);
    e4_set16(d + 6, 0);
    e4_set32(d + 8, pc[i].pb);
  }
  e4_dirty_level(fs, ip, &p, p.depth);
  e4_path_put(&p);
  // el bloque pasa a leerse como datos: debe estar a ceros
  bc_buf_t *zb = bc_get_new(fs->bdev, pb0 + n1, fs->bs);
  if (!zb)
    return -EIO;
  memset(bc_data(zb), 0, fs->bs);
  bc_mark_dirty_data(zb);
  bc_put(zb);
  *out = pb0 + n1;
  *converted = 1;
  return 0;
}

// Inserta (lblk -> pb) de un bloque.
static int ext4_insert(ext2_fs_t *fs, ext2_ino_t *ip, uint64_t lblk, uint32_t pb) {
  for (int attempt = 0; attempt < 2; attempt++) {
    e4path_t p;
    int rc = e4_find(fs, ip, lblk, &p);
    if (rc != 0)
      return rc;
    int lv = p.depth;
    uint8_t *leaf = p.node[lv];
    int idx = p.idx[lv];
    uint16_t ne = E4_ENTRIES(leaf);
    if (idx >= 0) { // extender el extent anterior si es contiguo
      uint8_t *pe = E4_ENT(leaf, idx);
      uint32_t peb = e4_u32(pe), pl = E4_LEN(pe), plo = e4_u32(pe + 8);
      if (pl < 32768 && e4_u16(pe + 6) == 0 && (uint64_t)peb + pl == lblk &&
          plo + pl == pb) {
        e4_set16(pe + 4, (uint16_t)(pl + 1));
        e4_dirty_level(fs, ip, &p, lv);
        e4_path_put(&p);
        return 0;
      }
    }
    if (ne < E4_MAXE(leaf)) {
      int pos = idx + 1;
      memmove(E4_ENT(leaf, pos + 1), E4_ENT(leaf, pos), (size_t)(ne - pos) * 12);
      uint8_t *e = E4_ENT(leaf, pos);
      e4_set32(e, (uint32_t)lblk);
      e4_set16(e + 4, 1);
      e4_set16(e + 6, 0);
      e4_set32(e + 8, pb);
      e4_set16(leaf + 2, ne + 1);
      if (pos == 0) // nuevo primer extent: los índices de arriba deben cubrirlo
        for (int l = lv - 1; l >= 0; l--) {
          uint8_t *ie = E4_ENT(p.node[l], p.idx[l]);
          if (lblk < e4_u32(ie))
            e4_set32(ie, (uint32_t)lblk);
          e4_dirty_level(fs, ip, &p, l);
          if (p.idx[l] != 0)
            break;
        }
      e4_dirty_level(fs, ip, &p, lv);
      e4_path_put(&p);
      return 0;
    }
    e4_path_put(&p);
    rc = e4_ensure_room(fs, ip, lblk, 1);
    if (rc != 0)
      return rc;
  }
  return -ENOSPC;
}

static int ext4_bmap_create(ext2_fs_t *fs, ext2_ino_t *ip, uint64_t lblk, int create,
                            uint32_t *out) {
  int rc = ext4_extent_map(fs, &ip->di, lblk, out);
  if (rc != 0 || *out != 0 || !create)
    return rc;
  if (lblk > 0xFFFFFFFEULL)
    return -EFBIG;
  int conv = 0;
  rc = e4_convert_unwritten(fs, ip, lblk, out, &conv);
  if (rc != 0 || conv)
    return rc;
  uint32_t nb;
  rc = ext2_new_zero_block(fs, ext2_ip_goal(fs, ip), &nb);
  if (rc != 0)
    return rc;
  rc = ext4_insert(fs, ip, lblk, nb);
  if (rc != 0) {
    ext2_free_block(fs, nb);
    return rc;
  }
  ext2_blocks_add(fs, ip, 1);
  ip->goal = nb;
  *out = nb;
  return 0;
}

// Libera el subárbol colgado de *pp (profundidad `depth`), cuyo primer bloque
// lógico es `base`, conservando los bloques lógicos < keep. Devuelve 1 si el
// bloque apuntado por *pp se liberó (y *pp queda a 0).
static int ext2_free_tree(ext2_fs_t *fs, ext2_ino_t *ip, uint32_t *pp,
                          int depth, uint64_t base, uint64_t keep) {
  uint32_t ptr = *pp;
  if (!ptr)
    return 0;
  if (ptr < fs->first_data_block || ptr >= fs->blocks_count) {
    LOG_WARN("[EXT2] %s: inodo %u con puntero inválido %u (descartado)",
             fs->bdev->name, ip->ino, ptr);
    *pp = 0;
    return 0;
  }
  if (depth == 0) {
    if (base < keep)
      return 0;
    ext2_free_block(fs, ptr);
    ext2_blocks_add(fs, ip, -1);
    *pp = 0;
    return 1;
  }
  uint64_t apb = fs->bs / 4, child_span = 1;
  for (int i = 1; i < depth; i++)
    child_span *= apb;
  bc_buf_t *b = bc_get(fs->bdev, ptr, fs->bs);
  if (!b)
    return 0;
  uint32_t *e = bc_data(b);
  int any_left = 0, changed = 0;
  for (uint64_t i = 0; i < apb; i++) {
    if (!e[i])
      continue;
    uint64_t cbase = base + i * child_span;
    if (cbase + child_span <= keep) {
      any_left = 1;
      continue;
    }
    uint32_t v = e[i];
    ext2_free_tree(fs, ip, &v, depth - 1, cbase, keep);
    if (v != e[i]) {
      e[i] = v;
      changed = 1;
    }
    if (v)
      any_left = 1;
  }
  if (changed)
    bc_mark_dirty(b);
  bc_put(b);
  if (any_left)
    return 0;
  ext2_free_block(fs, ptr);
  ext2_blocks_add(fs, ip, -1);
  *pp = 0;
  return 1;
}

// Libera todos los bloques con índice lógico >= keep.
static void ext2_trunc_blocks(ext2_fs_t *fs, ext2_ino_t *ip, uint64_t keep) {
  if (!ext2_has_blocks(fs, &ip->di))
    return;
  if (ip->di.i_flags & EXT4_EXTENTS_FL) {
    ext4_trunc_blocks(fs, ip, keep);
    return;
  }
  for (uint32_t i = 0; i < EXT2_NDIR_BLOCKS; i++)
    ext2_free_tree(fs, ip, (uint32_t *)(void *)&ip->di.i_block[i], 0, i, keep);
  uint64_t apb = fs->bs / 4;
  ext2_free_tree(fs, ip, (uint32_t *)(void *)&ip->di.i_block[EXT2_IND_BLOCK], 1, EXT2_NDIR_BLOCKS,
                 keep);
  ext2_free_tree(fs, ip, (uint32_t *)(void *)&ip->di.i_block[EXT2_DIND_BLOCK], 2,
                 EXT2_NDIR_BLOCKS + apb, keep);
  ext2_free_tree(fs, ip, (uint32_t *)(void *)&ip->di.i_block[EXT2_TIND_BLOCK], 3,
                 EXT2_NDIR_BLOCKS + apb + apb * apb, keep);
  ip->goal = 0;
}

// Libera el bloque de atributos extendidos (compartido: cuenta de referencias).
static void ext2_free_xattr(ext2_fs_t *fs, ext2_ino_t *ip) {
  uint32_t blk = ip->di.i_file_acl;
  if (!blk)
    return;
  if (blk >= fs->first_data_block && blk < fs->blocks_count) {
    bc_buf_t *b = bc_get(fs->bdev, blk, fs->bs);
    if (b) {
      uint32_t *h = bc_data(b); // h_magic, h_refcount
      if (h[0] == EXT2_XATTR_MAGIC && h[1] > 1) {
        h[1]--;
        if (fs->csum) { // h_checksum en +16: crc32c(seed, nº de bloque u64, bloque)
          uint64_t bn = blk;
          memset((uint8_t *)h + 16, 0, 4);
          uint32_t c = ext4_crc32c(fs->csum_seed, &bn, 8);
          c = ext4_crc32c(c, h, fs->bs);
          memcpy((uint8_t *)h + 16, &c, 4);
        }
        bc_mark_dirty(b);
        bc_put(b);
        goto out;
      }
      bc_put(b);
    }
    ext2_free_block(fs, blk);
    ext2_blocks_add(fs, ip, -1);
  }
out:
  ip->di.i_file_acl = 0;
}

// ---------------------------------------------------------------------------
// Lista de huérfanos en disco (s_last_orphan, rev 1): inodos ya sin enlaces
// pero aún abiertos. Si cae la luz, e2fsck los libera; en el kernel el FS
// sucio se monta solo lectura, así que la lista solo la usa el host.
// i_dtime del inodo guarda el siguiente de la cadena.
// ---------------------------------------------------------------------------
#define EXT2_SB_LAST_ORPHAN 232

static ext2_ino_t *ext2_iget(ext2_fs_t *fs, uint32_t ino);
static void ext2_iput(ext2_fs_t *fs, ext2_ino_t *ip);

static int ext2_sb_rd32(ext2_fs_t *fs, uint32_t off, uint32_t *v) {
  uint32_t pos = 1024 + off;
  bc_buf_t *b = bc_get(fs->bdev, pos / fs->bs, fs->bs);
  if (!b)
    return -EIO;
  memcpy(v, (uint8_t *)bc_data(b) + pos % fs->bs, 4);
  bc_put(b);
  return 0;
}
static int ext2_sb_wr32(ext2_fs_t *fs, uint32_t off, uint32_t v) {
  uint32_t pos = 1024 + off;
  bc_buf_t *b = bc_get(fs->bdev, pos / fs->bs, fs->bs);
  if (!b)
    return -EIO;
  memcpy((uint8_t *)bc_data(b) + pos % fs->bs, &v, 4);
  bc_mark_dirty(b);
  bc_put(b);
  return 0;
}

static void ext2_orphan_add(ext2_fs_t *fs, ext2_ino_t *ip) {
  uint32_t head;
  if (!fs->rev1 || ip->orphan || ext2_sb_rd32(fs, EXT2_SB_LAST_ORPHAN, &head) != 0)
    return;
  ip->di.i_dtime = head; // siguiente
  ext2_ip_flush(fs, ip); // primero el inodo, luego la cabeza
  if (ext2_sb_wr32(fs, EXT2_SB_LAST_ORPHAN, ip->ino) == 0)
    ip->orphan = 1;
}

static void ext2_orphan_del(ext2_fs_t *fs, ext2_ino_t *ip) {
  uint32_t head;
  if (!ip->orphan || ext2_sb_rd32(fs, EXT2_SB_LAST_ORPHAN, &head) != 0)
    return;
  if (head == ip->ino) {
    ext2_sb_wr32(fs, EXT2_SB_LAST_ORPHAN, ip->di.i_dtime);
  } else {
    for (uint32_t n = head, steps = 0; n && steps < fs->inodes_count; steps++) {
      ext2_ino_t *p = ext2_iget(fs, n);
      if (!p)
        break;
      if (p->di.i_dtime == ip->ino) {
        p->di.i_dtime = ip->di.i_dtime;
        ext2_ip_flush(fs, p);
        ext2_iput(fs, p);
        break;
      }
      n = p->di.i_dtime;
      ext2_iput(fs, p);
    }
  }
  ip->orphan = 0;
}

// El último nodo soltó un inodo sin enlaces: liberar bloques e inodo.
static void ext2_evict(ext2_fs_t *fs, ext2_ino_t *ip) {
  if (ext2_begin_write(fs) != 0)
    return;
  int is_dir = (ip->di.i_mode & S_IFMT) == S_IFDIR;
  ext2_orphan_del(fs, ip);
  if (is_dir)
    ext2_dc_drop_dir(fs, ip->ino);
  ext2_trunc_blocks(fs, ip, 0);
  ext2_free_xattr(fs, ip);
  ip->di.i_dtime = ext2_now();
  ext2_set_size(fs, ip, 0);
  ip->di.i_size_high = 0;
  ip->di.i_blocks = 0;
  ext2_ip_flush(fs, ip);
  ext2_free_inode(fs, ip->ino, is_dir);
}

static ext2_ino_t *ext2_iget(ext2_fs_t *fs, uint32_t ino) {
  for (ext2_ino_t *p = fs->icache; p; p = p->next)
    if (p->ino == ino) {
      p->refs++;
      return p;
    }
  ext2_ino_t *ip = kzalloc(sizeof(*ip));
  if (!ip)
    return NULL;
  if (ext2_read_inode(fs, ino, &ip->di) != 0) {
    kfree(ip);
    return NULL;
  }
  ip->ino = ino;
  ip->refs = 1;
  ip->live = ip->di.i_links_count > 0;
  ip->size = ext2_file_size(fs, &ip->di);
  ip->next = fs->icache;
  fs->icache = ip;
  return ip;
}

// ---------------------------------------------------------------------------
// mmap(MAP_SHARED): páginas de 4 KiB compartidas con los procesos
// ---------------------------------------------------------------------------
// Solo existen páginas de los trozos que alguien mapeó. La página es la copia
// que ven los procesos; la bcache sigue siendo la verdad para read()/write().
// Coherencia: write() actualiza la página; read(), fsync, sync, umount, el
// cierre del último nodo y el hilo de volcado hacen "página -> bcache" cuando
// difieren (no hay forma barata de saber si un proceso escribió, así que se
// compara). Un mapeo no puede alargar el fichero (más allá del final = SIGBUS).
#define EXT2_PG 4096u

static ext2_pg_t *ext2_pg_find(ext2_ino_t *ip, uint64_t pgoff) {
  for (ext2_pg_t *p = ip->pages; p; p = p->next)
    if (p->pgoff == pgoff)
      return p;
  return NULL;
}

// Copia a la bcache lo que los procesos hayan cambiado en la página.
static int ext2_pg_push(ext2_fs_t *fs, ext2_ino_t *ip, ext2_pg_t *pg) {
  uint8_t *mem = phys_to_virt(pg->phys);
  uint64_t base = pg->pgoff * EXT2_PG;
  if (base >= ip->size)
    return 0;
  int changed = 0, rc = 0;
  for (uint32_t off = 0; off < EXT2_PG; off += fs->bs) {
    uint64_t pos = base + off;
    if (pos >= ip->size)
      break;
    uint32_t n = fs->bs;
    if (pos + n > ip->size)
      n = (uint32_t)(ip->size - pos);
    uint32_t pblk;
    rc = ext2_bmap(fs, &ip->di, pos / fs->bs, &pblk);
    if (rc != 0)
      break;
    if (pblk == 0) { // hueco: solo si el proceso escribió algo distinto de ceros
      int zero = 1;
      for (uint32_t i = 0; i < n && zero; i++)
        if (mem[off + i])
          zero = 0;
      if (zero)
        continue;
      rc = ext2_begin_write(fs);
      if (rc == 0)
        rc = ext2_bmap_create(fs, ip, pos / fs->bs, 1, &pblk);
      if (rc != 0) {
        LOG_WARN("[EXT2] %s: sin espacio para volcar una página mmap (ino %u)",
                 fs->bdev->name, ip->ino);
        break;
      }
      changed = 1;
    }
    bc_buf_t *b = bc_get(fs->bdev, pblk, fs->bs);
    if (!b) {
      rc = -EIO;
      break;
    }
    uint8_t *d = bc_data(b);
    if (memcmp(d, mem + off, n) != 0) {
      rc = ext2_begin_write(fs); // solo si hay algo que escribir
      if (rc != 0) {
        bc_put(b);
        break;
      }
      memcpy(d, mem + off, n);
      bc_mark_dirty_data(b);
      changed = 1;
    }
    bc_put(b);
  }
  if (changed) {
    ip->di.i_mtime = ip->di.i_ctime = ext2_now();
    int frc = ext2_ip_flush(fs, ip);
    if (rc == 0)
      rc = frc;
  }
  return rc;
}

// Vuelca todas las páginas mapeadas de un inodo. Con fs->lock tomado.
static int ext2_pg_sync_ino(ext2_fs_t *fs, ext2_ino_t *ip) {
  if (!ip->pages || !fs->rw || ip->di.i_links_count == 0)
    return 0;
  int rc = 0;
  for (ext2_pg_t *p = ip->pages; p; p = p->next) {
    int r = ext2_pg_push(fs, ip, p);
    if (r != 0)
      rc = r;
  }
  return rc;
}

static void ext2_pg_sync_all(ext2_fs_t *fs) {
  for (ext2_ino_t *ip = fs->icache; ip; ip = ip->next)
    if (ip->pages)
      ext2_pg_sync_ino(fs, ip);
}

// write()/truncate sobre un rango: las páginas mapeadas ven lo nuevo.
static void ext2_pg_pull(ext2_fs_t *fs, ext2_ino_t *ip, uint64_t off,
                         uint64_t len) {
  for (ext2_pg_t *p = ip->pages; p; p = p->next) {
    uint64_t base = p->pgoff * EXT2_PG;
    if (base >= off + len || base + EXT2_PG <= off)
      continue;
    uint64_t lo = off > base ? off : base;
    uint64_t hi = off + len < base + EXT2_PG ? off + len : base + EXT2_PG;
    uint8_t *dst = (uint8_t *)phys_to_virt(p->phys) + (lo - base);
    uint64_t valid = ip->size > lo ? (ip->size < hi ? ip->size : hi) - lo : 0;
    if (valid)
      ext2_read_data(fs, &ip->di, ip->size, lo, (size_t)valid, dst);
    if (valid < hi - lo) // más allá del final: ceros
      memset(dst + valid, 0, (size_t)(hi - lo - valid));
  }
}

static void ext2_pg_free_all(ext2_ino_t *ip) {
  while (ip->pages) {
    ext2_pg_t *p = ip->pages;
    ip->pages = p->next;
    pmm_free_page(p->phys);
    kfree(p);
  }
}

static void ext2_iput(ext2_fs_t *fs, ext2_ino_t *ip) {
  if (!ip || --ip->refs > 0)
    return;
  if (ip->pages) { // último nodo: nadie mapea ya estas páginas
    ext2_pg_sync_ino(fs, ip);
    ext2_pg_free_all(ip);
  }
  if (ip->live && ip->di.i_links_count == 0 && ip->ino != EXT2_ROOT_INO &&
      fs->rw)
    ext2_evict(fs, ip);
  for (ext2_ino_t **pp = &fs->icache; *pp; pp = &(*pp)->next)
    if (*pp == ip) {
      *pp = ip->next;
      break;
    }
  kfree(ip);
}

// ---------------------------------------------------------------------------
// Directorios: insertar / quitar entradas
// ---------------------------------------------------------------------------
static inline uint32_t ext2_dirent_need(size_t nlen) {
  return (uint32_t)((8 + nlen + 3) & ~3u);
}

static void ext2_dirent_put(uint8_t *p, uint32_t ino, uint16_t rec_len,
                            const char *name, size_t nlen, uint8_t ft) {
  ext2_dirent_t d = {ino, rec_len, (uint8_t)nlen, ft};
  memcpy(p, &d, sizeof(d));
  if (nlen)
    memcpy(p + 8, name, nlen);
}

// metadata_csum: cada bloque de directorio termina en un "dirent" falso de 12 B
// (ino 0, tipo 0xDE) con el crc32c del bloque.
static int ext2_dir_has_tail(const ext2_fs_t *fs, const uint8_t *blk) {
  const uint8_t *t = blk + fs->bs - 12;
  uint32_t i0;
  uint16_t rl;
  memcpy(&i0, t, 4);
  memcpy(&rl, t + 4, 2);
  return i0 == 0 && rl == 12 && t[6] == 0 && t[7] == 0xDE;
}
static void ext2_dir_init_tail(const ext2_fs_t *fs, uint8_t *blk) {
  uint8_t *t = blk + fs->bs - 12;
  memset(t, 0, 12);
  uint16_t rl = 12;
  memcpy(t + 4, &rl, 2);
  t[7] = 0xDE;
}
static void ext2_dir_csum_set(const ext2_fs_t *fs, uint32_t ino, uint32_t gen,
                              uint8_t *blk) {
  if (!fs->csum || !ext2_dir_has_tail(fs, blk))
    return;
  uint32_t c = ext4_crc32c(fs->csum_seed, &ino, 4);
  c = ext4_crc32c(c, &gen, 4);
  c = ext4_crc32c(c, blk, fs->bs - 12);
  memcpy(blk + fs->bs - 4, &c, 4);
}

// Con checksums, un directorio htree se desindexa antes de tocarlo: los nodos
// del índice pasan a ser bloques vacíos con su cola y la raíz conserva solo
// "." y "..". Se hace una vez (se limpia EXT2_INDEX_FL).
static int ext2_dir_unindex(ext2_fs_t *fs, ext2_ino_t *dp) {
  if (!fs->csum || !(dp->di.i_flags & EXT2_INDEX_FL))
    return 0;
  uint64_t nblocks = (dp->size + fs->bs - 1) / fs->bs;
  for (uint64_t l = 0; l < nblocks; l++) {
    uint32_t pblk;
    int rc = ext2_bmap(fs, &dp->di, l, &pblk);
    if (rc != 0)
      return rc;
    if (pblk == 0)
      continue;
    bc_buf_t *b = bc_get(fs->bdev, pblk, fs->bs);
    if (!b)
      return -EIO;
    uint8_t *blk = bc_data(b);
    if (!ext2_dir_has_tail(fs, blk)) {
      uint32_t i0;
      uint16_t rl0, rl1;
      memcpy(&i0, blk, 4);
      memcpy(&rl0, blk + 4, 2);
      memcpy(&rl1, blk + 12 + 4, 2);
      if (l == 0 && rl0 == 12 && rl1 == fs->bs - 12) {
        uint16_t nrl = (uint16_t)(fs->bs - 24);
        memcpy(blk + 16, &nrl, 2);
        memset(blk + 24, 0, fs->bs - 24);
      } else if (l != 0 && i0 == 0 && rl0 == fs->bs) {
        memset(blk, 0, fs->bs);
        uint16_t nrl = (uint16_t)(fs->bs - 12);
        memcpy(blk + 4, &nrl, 2);
      } else {
        bc_put(b);
        return -EIO;
      }
      ext2_dir_init_tail(fs, blk);
      ext2_dir_csum_set(fs, dp->ino, dp->di.i_generation, blk);
      bc_mark_dirty(b);
    }
    bc_put(b);
  }
  dp->di.i_flags &= ~EXT2_INDEX_FL;
  return ext2_ip_flush(fs, dp);
}

static void ext2_dir_touch(ext2_fs_t *fs, ext2_ino_t *dp) {
  dp->di.i_mtime = dp->di.i_ctime = ext2_now();
  dp->di.i_flags &= ~EXT2_INDEX_FL; // escribimos lineal: sin árbol htree
  fs->gen++;
}

static int ext2_dir_add(ext2_fs_t *fs, ext2_ino_t *dp, const char *name,
                        size_t nlen, uint32_t ino, uint8_t ftype) {
  uint32_t need = ext2_dirent_need(nlen);
  uint64_t nblocks = (dp->size + fs->bs - 1) / fs->bs;
  uint8_t ft = fs->filetype ? ftype : 0;
  int urc = ext2_dir_unindex(fs, dp);
  if (urc != 0)
    return urc;
  for (uint64_t l = 0; l < nblocks; l++) {
    uint32_t pblk;
    int rc = ext2_bmap(fs, &dp->di, l, &pblk);
    if (rc != 0)
      return rc;
    if (pblk == 0)
      continue;
    bc_buf_t *b = bc_get(fs->bdev, pblk, fs->bs);
    if (!b)
      return -EIO;
    uint8_t *blk = bc_data(b);
    uint32_t off = 0;
    uint32_t lim = (fs->csum && ext2_dir_has_tail(fs, blk)) ? fs->bs - 12 : fs->bs;
    for (int steps = 0; off < lim && steps < EXT2_MAX_DIR_ENTRIES_PER_BLOCK;
         steps++) {
      uint16_t rl;
      uint32_t eino;
      uint8_t nl;
      if (!ext2_dirent_ok(fs, blk, off, &rl, &eino, &nl)) {
        bc_put(b);
        return -EIO;
      }
      uint32_t used = eino ? ext2_dirent_need(nl) : 0;
      if (rl >= used + need) {
        if (eino) {
          uint16_t nrl = (uint16_t)used;
          memcpy(blk + off + 4, &nrl, 2);
          ext2_dirent_put(blk + off + used, ino, (uint16_t)(rl - used), name,
                          nlen, ft);
        } else {
          ext2_dirent_put(blk + off, ino, rl, name, nlen, ft);
        }
        ext2_dir_csum_set(fs, dp->ino, dp->di.i_generation, blk);
        bc_mark_dirty(b);
        bc_put(b);
        ext2_dir_touch(fs, dp);
        return ext2_ip_flush(fs, dp);
      }
      off += rl;
    }
    bc_put(b);
  }
  // Sin hueco: bloque nuevo al final.
  uint32_t pblk;
  int rc = ext2_bmap_create(fs, dp, nblocks, 1, &pblk);
  if (rc != 0) {
    ext2_ip_flush(fs, dp);
    return rc;
  }
  bc_buf_t *b = bc_get(fs->bdev, pblk, fs->bs);
  if (!b)
    return -EIO;
  ext2_dirent_put(bc_data(b), ino, (uint16_t)(fs->csum ? fs->bs - 12 : fs->bs), name, nlen, ft);
  if (fs->csum) {
    ext2_dir_init_tail(fs, bc_data(b));
    ext2_dir_csum_set(fs, dp->ino, dp->di.i_generation, bc_data(b));
  }
  bc_mark_dirty(b);
  bc_put(b);
  ext2_set_size(fs, dp, (nblocks + 1) * fs->bs);
  ext2_dir_touch(fs, dp);
  return ext2_ip_flush(fs, dp);
}

// Quita la entrada `name`. Devuelve su inodo en *ino_out.
static int ext2_dir_remove(ext2_fs_t *fs, ext2_ino_t *dp, const char *name,
                           size_t nlen, uint32_t *ino_out) {
  uint64_t nblocks = (dp->size + fs->bs - 1) / fs->bs;
  int urc = ext2_dir_unindex(fs, dp);
  if (urc != 0)
    return urc;
  for (uint64_t l = 0; l < nblocks; l++) {
    uint32_t pblk;
    int rc = ext2_bmap(fs, &dp->di, l, &pblk);
    if (rc != 0)
      return rc;
    if (pblk == 0)
      continue;
    bc_buf_t *b = bc_get(fs->bdev, pblk, fs->bs);
    if (!b)
      return -EIO;
    uint8_t *blk = bc_data(b);
    uint32_t off = 0, prev = UINT32_MAX;
    for (int steps = 0; off < fs->bs && steps < EXT2_MAX_DIR_ENTRIES_PER_BLOCK;
         steps++) {
      uint16_t rl;
      uint32_t eino;
      uint8_t nl;
      if (!ext2_dirent_ok(fs, blk, off, &rl, &eino, &nl)) {
        bc_put(b);
        return -EIO;
      }
      if (eino && nl == nlen && memcmp(blk + off + 8, name, nlen) == 0) {
        if (prev != UINT32_MAX) {
          uint16_t prl;
          memcpy(&prl, blk + prev + 4, 2);
          prl = (uint16_t)(prl + rl);
          memcpy(blk + prev + 4, &prl, 2);
        } else {
          uint32_t z = 0;
          memcpy(blk + off, &z, 4);
        }
        ext2_dir_csum_set(fs, dp->ino, dp->di.i_generation, blk);
        bc_mark_dirty(b);
        bc_put(b);
        *ino_out = eino;
        ext2_dc_inval(fs, dp->ino, name, nlen);
        ext2_dir_touch(fs, dp);
        return ext2_ip_flush(fs, dp);
      }
      prev = off;
      off += rl;
    }
    bc_put(b);
  }
  return -ENOENT;
}

// 1 si el directorio solo contiene "." y "..".
static int ext2_dir_is_empty(ext2_fs_t *fs, const ext2_ino_t *dp) {
  uint64_t nblocks = (dp->size + fs->bs - 1) / fs->bs;
  for (uint64_t l = 0; l < nblocks; l++) {
    uint32_t pblk;
    if (ext2_bmap(fs, &dp->di, l, &pblk) != 0)
      return 0;
    if (pblk == 0)
      continue;
    bc_buf_t *b = bc_get(fs->bdev, pblk, fs->bs);
    if (!b)
      return 0;
    const uint8_t *blk = bc_data(b);
    uint32_t off = 0;
    for (int steps = 0; off < fs->bs && steps < EXT2_MAX_DIR_ENTRIES_PER_BLOCK;
         steps++) {
      uint16_t rl;
      uint32_t eino;
      uint8_t nl;
      if (!ext2_dirent_ok(fs, blk, off, &rl, &eino, &nl)) {
        bc_put(b);
        return 0;
      }
      if (eino) {
        const uint8_t *nm = blk + off + 8;
        int dot = nl == 1 && nm[0] == '.';
        int dotdot = nl == 2 && nm[0] == '.' && nm[1] == '.';
        if (!dot && !dotdot) {
          bc_put(b);
          return 0;
        }
      }
      off += rl;
    }
    bc_put(b);
  }
  return 1;
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

static void ext2_refresh_vnode(vfs_node_t *n, const ext2_ino_t *ip) {
  const ext2_inode_t *di = &ip->di;
  n->size = (size_t)ip->size;
  n->mode = di->i_mode;
  n->uid = (uint32_t)di->i_uid | ((uint32_t)di->i_uid_high << 16);
  n->gid = (uint32_t)di->i_gid | ((uint32_t)di->i_gid_high << 16);
  n->mtime_sec = di->i_mtime;
  n->atime_sec = di->i_atime;
  n->ctime_sec = di->i_ctime;
  n->nlink = di->i_links_count;
  n->blocks = di->i_blocks;
}

static vfs_node_t *ext2_make_node(ext2_fs_t *fs, uint32_t ino) {
  ext2_node_t *en = kzalloc(sizeof(*en));
  if (!en)
    return NULL;
  ext2_ino_t *ip = ext2_iget(fs, ino);
  if (!ip) {
    kfree(en);
    return NULL;
  }
  en->fs = fs;
  en->ino = ino;
  en->ip = ip;
  const ext2_inode_t *di = &ip->di;
  uint32_t mode = di->i_mode;
  if (di->i_links_count == 0 && ino != EXT2_ROOT_INO) {
    ext2_iput(fs, ip); // inodo borrado: no debería ser alcanzable
    kfree(en);
    return NULL;
  }

  vfs_node_t *n = kzalloc(sizeof(*n));
  if (!n) {
    ext2_iput(fs, ip);
    kfree(en);
    return NULL;
  }
  n->flags = ((mode & S_IFMT) == S_IFDIR) ? VFS_DIRECTORY : VFS_FILE;
  n->inode = ino;
  n->ops = &ext2_node_ops;
  n->fs = &ext2_fs_ops;
  n->priv = en;
  ext2_refresh_vnode(n, ip);
  if ((mode & S_IFMT) == S_IFCHR || (mode & S_IFMT) == S_IFBLK)
    n->rdev = ext2_decode_rdev(di);
  if ((mode & S_IFMT) == S_IFLNK) {
    char tmp[VFS_PATH_MAX];
    int len = ext2_read_link(fs, di, ip->size, tmp, sizeof(tmp) - 1);
    if (len > 0) {
      memcpy(n->link_target, tmp, (size_t)len);
      n->link_target[len] = '\0';
      n->is_symlink = 1;
    } else {
      LOG_WARN("[EXT2] %s: symlink ino %u ilegible o demasiado largo",
               fs->bdev->name, ino);
      ext2_iput(fs, ip);
      kfree(en);
      kfree(n);
      return NULL;
    }
  }
  return n;
}

static vfs_node_t *ext2_lookup_locked(ext2_fs_t *fs, const char *path) {
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
    if (ext2_dir_lookup(fs, ino, &di, ext2_file_size(fs, &di), seg, nl, &next) != 0)
      return NULL;
    ino = next;
  }
  return ext2_make_node(fs, ino);
}

static vfs_node_t *ext2_lookup(void *fs_priv, const char *path) {
  ext2_fs_t *fs = fs_priv;
  if (!fs)
    return NULL;
  mutex_lock(&fs->lock);
  vfs_node_t *n = ext2_lookup_locked(fs, path);
  mutex_unlock(&fs->lock);
  return n;
}

static int64_t ext2_node_read(vfs_node_t *node, uint64_t offset, size_t size,
                              void *buf) {
  ext2_node_t *en = node ? node->priv : NULL;
  if (!en || !buf)
    return -EINVAL;
  ext2_fs_t *fs = en->fs;
  mutex_lock(&fs->lock);
  int64_t rc;
  if ((en->ip->di.i_mode & S_IFMT) == S_IFDIR)
    rc = -EISDIR;
  else {
    if (en->ip->pages && fs->rw) // lo escrito por mmap debe verse en read()
      ext2_pg_sync_ino(fs, en->ip);
    // Lectura secuencial (o el comienzo del fichero): pedir más por adelantado.
    uint32_t extra = (offset == en->ip->ra_next || offset == 0) ? 31 : 0;
    rc = ext2_read_data_x(fs, &en->ip->di, en->ip->size, offset, size, buf, extra);
    if (rc > 0)
      en->ip->ra_next = offset + (uint64_t)rc;
  }
  mutex_unlock(&fs->lock);
  return rc;
}

static int ext2_node_open(vfs_node_t *node, int flags) {
  ext2_node_t *en = node ? node->priv : NULL;
  if (!en)
    return -EINVAL;
  if ((flags & (O_WRONLY | O_RDWR | 0x200 /*O_TRUNC*/)) && !en->fs->rw)
    return -EROFS;
  return 0;
}

static int ext2_node_close(vfs_node_t *node) {
  ext2_node_t *en = node ? node->priv : NULL;
  if (en) {
    mutex_lock(&en->fs->lock);
    ext2_iput(en->fs, en->ip);
    mutex_unlock(&en->fs->lock);
    kfree(en);
    node->priv = NULL;
  }
  return 0;
}

static int ext2_node_readlink(vfs_node_t *node, char *buf, size_t bufsize) {
  ext2_node_t *en = node ? node->priv : NULL;
  if (!en || (en->ip->di.i_mode & S_IFMT) != S_IFLNK || bufsize < 2)
    return -EINVAL;
  mutex_lock(&en->fs->lock);
  int n = ext2_read_link(en->fs, &en->ip->di, en->ip->size, buf, bufsize - 1);
  mutex_unlock(&en->fs->lock);
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
  if ((en->ip->di.i_mode & S_IFMT) != S_IFDIR)
    return -ENOTDIR;
  ext2_fs_t *fs = en->fs;
  out->name[0] = '\0';
  out->type = 0;
  out->size = 0;

  mutex_lock(&fs->lock);
  uint64_t cur_idx = en->rd_index;
  uint32_t lblk = en->rd_lblk, off = en->rd_off;
  if (en->rd_gen != fs->gen) { // el directorio cambió: cursor inválido
    cur_idx = 0;
    lblk = 0;
    off = 0;
    en->rd_gen = fs->gen;
  }
  if (index < cur_idx) {
    cur_idx = 0;
    lblk = 0;
    off = 0;
  }
  uint64_t nblocks = (en->ip->size + fs->bs - 1) / fs->bs;
  int rc = 0;

  while (lblk < nblocks) {
    uint32_t pblk;
    rc = ext2_bmap(fs, &en->ip->di, lblk, &pblk);
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
          if (en->ip->size >= 2ull * fs->bs && n == nl)
            ext2_dc_put(fs, en->ip->ino, out->name, n, ino);
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

// ---------------------------------------------------------------------------
// E2: operaciones de escritura sobre nodos VFS
// ---------------------------------------------------------------------------
static int ext2_enable_large_file(ext2_fs_t *fs) {
  if (fs->ro_compat & EXT2_FEATURE_RO_COMPAT_LARGE_FILE)
    return 0;
  fs->ro_compat |= EXT2_FEATURE_RO_COMPAT_LARGE_FILE;
  return ext2_sb_commit(fs, 0); // a disco ya: el FS tiene ficheros >2 GiB
}

static int64_t ext2_node_write(vfs_node_t *node, uint64_t offset, size_t size,
                               const void *buf) {
  ext2_node_t *en = node ? node->priv : NULL;
  if (!en || (!buf && size))
    return -EINVAL;
  ext2_fs_t *fs = en->fs;
  ext2_ino_t *ip = en->ip;
  if (size == 0)
    return 0;

  mutex_lock(&fs->lock);
  if (offset == (uint64_t)-1) // O_APPEND: al final real del inodo compartido
    offset = ip->size;
  int64_t done = 0;
  if (offset + size < offset) {
    mutex_unlock(&fs->lock);
    return -EFBIG;
  }
  int rc = ext2_begin_write(fs);
  if (rc != 0)
    goto out;
  if ((ip->di.i_mode & S_IFMT) != S_IFREG) {
    rc = ((ip->di.i_mode & S_IFMT) == S_IFDIR) ? -EISDIR : -EINVAL;
    goto out;
  }
  if ((ip->di.i_flags & EXT2_IMMUTABLE_FL) ||
      ((ip->di.i_flags & EXT2_APPEND_FL) && offset != ip->size)) {
    rc = -EPERM;
    goto out;
  }
  uint64_t end = offset + size;
  if (end > 0x7fffffffULL && !fs->rev1) {
    rc = -EFBIG;
    goto out;
  }
  if (end > 0x7fffffffULL && (rc = ext2_enable_large_file(fs)) != 0)
    goto out;

  const uint8_t *src = buf;
  while ((size_t)done < size) {
    uint64_t pos = offset + (uint64_t)done;
    uint64_t lblk = pos / fs->bs;
    uint32_t boff = (uint32_t)(pos % fs->bs);
    size_t n = fs->bs - boff;
    if (n > size - (size_t)done)
      n = size - (size_t)done;
    uint32_t pblk;
    rc = ext2_bmap_create(fs, ip, lblk, 1, &pblk);
    if (rc != 0)
      break;
    bc_buf_t *b = (n == fs->bs) ? bc_get_new(fs->bdev, pblk, fs->bs)
                                : bc_get(fs->bdev, pblk, fs->bs);
    if (!b) {
      rc = -EIO;
      break;
    }
    memcpy((uint8_t *)bc_data(b) + boff, src + done, n);
    bc_mark_dirty_data(b);
    bc_put(b);
    done += (int64_t)n;
  }
  if (done > 0) {
    if (offset + (uint64_t)done > ip->size)
      ext2_set_size(fs, ip, offset + (uint64_t)done);
    ip->di.i_mtime = ip->di.i_ctime = ext2_now();
    int frc = ext2_ip_flush(fs, ip);
    if (frc != 0 && rc == 0)
      rc = frc;
    if (ip->pages)
      ext2_pg_pull(fs, ip, offset, (uint64_t)done);
    ext2_refresh_vnode(node, ip);
  }
  // Si los bloques cambiaron pero no se escribió nada (ENOSPC a medias):
  if (done == 0 && rc != 0)
    ext2_ip_flush(fs, ip);
out:
  mutex_unlock(&fs->lock);
  return done > 0 ? done : rc;
}

static int ext2_node_truncate(vfs_node_t *node, uint64_t new_size) {
  ext2_node_t *en = node ? node->priv : NULL;
  if (!en)
    return -EINVAL;
  ext2_fs_t *fs = en->fs;
  ext2_ino_t *ip = en->ip;
  mutex_lock(&fs->lock);
  int rc = ext2_begin_write(fs);
  if (rc != 0)
    goto out;
  if ((ip->di.i_mode & S_IFMT) != S_IFREG) {
    rc = ((ip->di.i_mode & S_IFMT) == S_IFDIR) ? -EISDIR : -EINVAL;
    goto out;
  }
  if (ip->di.i_flags & EXT2_PROTECT_FL) {
    rc = -EPERM;
    goto out;
  }
  if (new_size == ip->size)
    goto out;
  if (new_size > 0x7fffffffULL && (rc = ext2_enable_large_file(fs)) != 0)
    goto out;
  uint32_t idx[4];
  if (!(ip->di.i_flags & EXT4_EXTENTS_FL) &&
      ext2_path(fs, (new_size + fs->bs - 1) / fs->bs, idx) < 0 &&
      new_size > ip->size) {
    rc = -EFBIG;
    goto out;
  }
  if (ip->pages)
    ext2_pg_sync_ino(fs, ip); // antes de cambiar el tamaño
  if (new_size < ip->size) {
    ext2_trunc_blocks(fs, ip, (new_size + fs->bs - 1) / fs->bs);
    uint32_t tail = (uint32_t)(new_size % fs->bs);
    if (tail) { // el resto del último bloque debe quedar a ceros
      uint32_t pblk;
      if (ext2_bmap(fs, &ip->di, new_size / fs->bs, &pblk) == 0 && pblk) {
        bc_buf_t *b = bc_get(fs->bdev, pblk, fs->bs);
        if (b) {
          memset((uint8_t *)bc_data(b) + tail, 0, fs->bs - tail);
          bc_mark_dirty(b);
          bc_put(b);
        }
      }
    }
  }
  ext2_set_size(fs, ip, new_size);
  ip->di.i_mtime = ip->di.i_ctime = ext2_now();
  rc = ext2_ip_flush(fs, ip);
  if (ip->pages)
    ext2_pg_pull(fs, ip, new_size, 1ULL << 40); // todo lo que queda más allá: ceros
  ext2_refresh_vnode(node, ip);
out:
  mutex_unlock(&fs->lock);
  return rc;
}

static int ext2_name_check(const char *name, size_t nl) {
  if (nl == 0)
    return -EINVAL;
  if (nl > EXT2_NAME_LEN)
    return -ENAMETOOLONG;
  for (size_t i = 0; i < nl; i++)
    if (name[i] == '/')
      return -EINVAL;
  return 0;
}

static uint8_t ext2_type_to_ft(uint16_t type) {
  switch (type) {
  case S_IFREG: return 1;
  case S_IFDIR: return 2;
  case S_IFCHR: return 3;
  case S_IFBLK: return 4;
  case S_IFIFO: return 5;
  case S_IFSOCK: return 6;
  case S_IFLNK: return 7;
  default: return 0;
  }
}

static void ext2_encode_rdev(ext2_inode_t *di, uint32_t rdev) {
  uint32_t maj = (rdev >> 8) & 0xFFF, min = rdev & 0xFF;
  memset(di->i_block, 0, sizeof(di->i_block));
  if (maj < 256 && min < 256)
    di->i_block[0] = (maj << 8) | min;
  else
    di->i_block[1] = (min & 0xFF) | (maj << 8) | ((min & ~0xFFu) << 12);
}

// Crea un inodo (fichero, directorio, symlink, dispositivo, FIFO, socket) con
// nombre `name` en `dvn`.
static int ext2_new_inode_op(vfs_node_t *dvn, const char *name, uint32_t mode,
                             uint32_t rdev, const char *target) {
  ext2_node_t *den = dvn ? dvn->priv : NULL;
  if (!den || !name)
    return -EINVAL;
  ext2_fs_t *fs = den->fs;
  ext2_ino_t *dp = den->ip;
  size_t nl = strlen(name);
  int rc = ext2_name_check(name, nl);
  if (rc != 0)
    return rc;
  uint16_t type = (uint16_t)(mode & S_IFMT);
  if (type == 0)
    type = S_IFREG;
  size_t tlen = target ? strlen(target) : 0;
  if (type == S_IFLNK && (tlen == 0 || tlen >= VFS_PATH_MAX))
    return tlen ? -ENAMETOOLONG : -EINVAL;

  mutex_lock(&fs->lock);
  rc = ext2_begin_write(fs);
  if (rc != 0)
    goto out;
  if ((dp->di.i_mode & S_IFMT) != S_IFDIR) {
    rc = -ENOTDIR;
    goto out;
  }
  if (dp->di.i_links_count == 0) {
    rc = -ENOENT;
    goto out;
  }
  if ((nl == 1 && name[0] == '.') || (nl == 2 && name[0] == '.' && name[1] == '.')) {
    rc = -EEXIST;
    goto out;
  }
  uint32_t tmp;
  rc = ext2_dir_lookup(fs, dp->ino, &dp->di, dp->size, name, nl, &tmp);
  if (rc == 0) {
    rc = -EEXIST;
    goto out;
  }
  if (rc != -ENOENT)
    goto out;
  if (type == S_IFDIR && dp->di.i_links_count >= EXT2_LINK_MAX) {
    rc = -EMLINK;
    goto out;
  }
  if (type != S_IFREG && type != S_IFDIR && type != S_IFLNK &&
      type != S_IFCHR && type != S_IFBLK && type != S_IFIFO &&
      type != S_IFSOCK) {
    rc = -EINVAL;
    goto out;
  }

  uint32_t ino;
  rc = ext2_alloc_inode(fs, dp->ino, type == S_IFDIR, &ino);
  if (rc != 0)
    goto out;

  ext2_ino_t nip;
  memset(&nip, 0, sizeof(nip));
  nip.ino = ino;
  ext2_inode_t *di = &nip.di;
  process_t *pr = process_current();
  uint32_t uid = pr ? pr->euid : 0, gid = pr ? pr->egid : 0;
  if (dp->di.i_mode & S_ISGID) { // directorio setgid: hereda el grupo
    gid = (uint32_t)dp->di.i_gid | ((uint32_t)dp->di.i_gid_high << 16);
    if (type == S_IFDIR)
      mode |= S_ISGID;
  }
  di->i_mode = (uint16_t)(type | (mode & 07777));
  di->i_uid = (uint16_t)uid;
  di->i_uid_high = (uint16_t)(uid >> 16);
  di->i_gid = (uint16_t)gid;
  di->i_gid_high = (uint16_t)(gid >> 16);
  di->i_atime = di->i_ctime = di->i_mtime = ext2_now();
  di->i_links_count = (type == S_IFDIR) ? 2 : 1;
  di->i_generation = ext2_now() ^ (ino * 2654435761u);

  uint32_t data_blk = 0;
  // En un FS con extents los ficheros, directorios y symlinks lentos nacen con
  // árbol de extents (raíz vacía en i_block).
  int use_ext = (fs->incompat & EXT4_FEATURE_INCOMPAT_EXTENTS) &&
                (type == S_IFREG || type == S_IFDIR ||
                 (type == S_IFLNK && tlen >= sizeof(di->i_block)));
  if (use_ext) {
    di->i_flags |= EXT4_EXTENTS_FL;
    memset(di->i_block, 0, sizeof(di->i_block));
    e4_write_hdr((uint8_t *)di->i_block, 0, 4, 0);
  }
  if (type == S_IFDIR) {
    rc = ext2_new_zero_block(fs, ext2_group_first(fs, (ino - 1) / fs->inodes_per_group),
                             &data_blk);
    if (rc != 0)
      goto undo_inode;
    bc_buf_t *b = bc_get(fs->bdev, data_blk, fs->bs);
    if (!b) {
      rc = -EIO;
      goto undo_block;
    }
    uint8_t *d = bc_data(b);
    uint8_t ft = fs->filetype ? 2 : 0;
    ext2_dirent_put(d, ino, 12, ".", 1, ft);
    ext2_dirent_put(d + 12, dp->ino, (uint16_t)(fs->csum ? fs->bs - 24 : fs->bs - 12), "..", 2, ft);
    if (fs->csum) {
      ext2_dir_init_tail(fs, d);
      ext2_dir_csum_set(fs, ino, di->i_generation, d);
    }
    bc_mark_dirty(b);
    bc_put(b);
    if (use_ext)
      e4_first_extent(di, data_blk);
    else
      di->i_block[0] = data_blk;
    di->i_blocks = fs->bs >> 9;
    ext2_set_size(fs, &nip, fs->bs);
  } else if (type == S_IFLNK) {
    if (tlen < sizeof(di->i_block)) { // symlink rápido
      memcpy(di->i_block, target, tlen);
    } else {
      rc = ext2_new_zero_block(fs, ext2_group_first(fs, (ino - 1) / fs->inodes_per_group),
                               &data_blk);
      if (rc != 0)
        goto undo_inode;
      bc_buf_t *b = bc_get(fs->bdev, data_blk, fs->bs);
      if (!b) {
        rc = -EIO;
        goto undo_block;
      }
      memcpy(bc_data(b), target, tlen);
      bc_mark_dirty(b);
      bc_put(b);
      if (use_ext)
        e4_first_extent(di, data_blk);
      else
        di->i_block[0] = data_blk;
      di->i_blocks = fs->bs >> 9;
    }
    ext2_set_size(fs, &nip, tlen);
  } else if (type == S_IFCHR || type == S_IFBLK) {
    ext2_encode_rdev(di, rdev);
  }

  rc = ext2_write_inode(fs, ino, di, 1);
  if (rc != 0)
    goto undo_block;
  rc = ext2_dir_add(fs, dp, name, nl, ino, ext2_type_to_ft(type));
  if (rc != 0) {
    di->i_links_count = 0;
    di->i_dtime = ext2_now();
    ext2_write_inode(fs, ino, di, 0);
    goto undo_block;
  }
  if (type == S_IFDIR) { // ".." del hijo apunta al padre
    dp->di.i_links_count++;
    ext2_ip_flush(fs, dp);
  }
  goto out;

undo_block:
  if (data_blk)
    ext2_free_block_now(fs, data_blk);
undo_inode:
  ext2_free_inode_now(fs, ino, type == S_IFDIR);
out:
  mutex_unlock(&fs->lock);
  return rc;
}

static int ext2_create_op(vfs_node_t *dir, const char *name, uint32_t mode) {
  return ext2_new_inode_op(dir, name, S_IFREG | (mode & 07777), 0, NULL);
}
static int ext2_mkdir_op(vfs_node_t *dir, const char *name, uint32_t mode) {
  return ext2_new_inode_op(dir, name, S_IFDIR | (mode & 07777), 0, NULL);
}
static int ext2_mknod_op(vfs_node_t *dir, const char *name, uint32_t mode,
                         uint32_t rdev) {
  uint32_t t = mode & S_IFMT;
  if (t == 0) { // vfs_mknod() quita el tipo: con rdev es un char device
    t = rdev ? S_IFCHR : S_IFREG;
    mode |= t;
  }
  if (t != S_IFCHR && t != S_IFBLK && t != S_IFIFO && t != S_IFSOCK && t != S_IFREG)
    return -EPERM;
  return ext2_new_inode_op(dir, name, mode, rdev, NULL);
}
static int ext2_symlink_op(vfs_node_t *dir, const char *name,
                           const char *target) {
  if (!target)
    return -EINVAL;
  return ext2_new_inode_op(dir, name, S_IFLNK | 0777, 0, target);
}

// unlink(2) y rmdir(2): borra la entrada; el inodo se libera al cerrarse el
// último nodo abierto sobre él.
static int ext2_unlink_op(vfs_node_t *dvn, const char *name) {
  ext2_node_t *den = dvn ? dvn->priv : NULL;
  if (!den || !name)
    return -EINVAL;
  ext2_fs_t *fs = den->fs;
  ext2_ino_t *dp = den->ip;
  size_t nl = strlen(name);
  int rc = ext2_name_check(name, nl);
  if (rc != 0)
    return rc;
  if ((nl == 1 && name[0] == '.') || (nl == 2 && name[0] == '.' && name[1] == '.'))
    return -EINVAL;

  mutex_lock(&fs->lock);
  rc = ext2_begin_write(fs);
  if (rc != 0)
    goto out;
  if ((dp->di.i_mode & S_IFMT) != S_IFDIR) {
    rc = -ENOTDIR;
    goto out;
  }
  uint32_t ino;
  rc = ext2_dir_lookup(fs, dp->ino, &dp->di, dp->size, name, nl, &ino);
  if (rc != 0)
    goto out;
  ext2_ino_t *cip = ext2_iget(fs, ino);
  if (!cip) {
    rc = -EIO;
    goto out;
  }
  int isdir = (cip->di.i_mode & S_IFMT) == S_IFDIR;
  if (cip->di.i_flags & EXT2_PROTECT_FL) {
    rc = -EPERM;
    ext2_iput(fs, cip);
    goto out;
  }
  if (isdir && !ext2_dir_is_empty(fs, cip)) {
    rc = -ENOTEMPTY;
    ext2_iput(fs, cip);
    goto out;
  }
  uint32_t rino;
  rc = ext2_dir_remove(fs, dp, name, nl, &rino);
  if (rc != 0) {
    ext2_iput(fs, cip);
    goto out;
  }
  if (isdir) {
    cip->di.i_links_count = 0;
    if (dp->di.i_links_count > 2)
      dp->di.i_links_count--;
    ext2_ip_flush(fs, dp);
  } else if (cip->di.i_links_count > 0) {
    cip->di.i_links_count--;
  }
  cip->di.i_ctime = ext2_now();
  if (cip->di.i_links_count == 0)
    cip->di.i_dtime = ext2_now();
  ext2_ip_flush(fs, cip);
  if (cip->di.i_links_count == 0 && cip->refs > 1)
    ext2_orphan_add(fs, cip); // sigue abierto: a la lista de huérfanos
  ext2_iput(fs, cip); // si era el último, se libera aquí
out:
  mutex_unlock(&fs->lock);
  return rc;
}

static int ext2_chmod_op(vfs_node_t *node, uint32_t mode) {
  ext2_node_t *en = node ? node->priv : NULL;
  if (!en)
    return -EINVAL;
  ext2_fs_t *fs = en->fs;
  mutex_lock(&fs->lock);
  int rc = ext2_begin_write(fs);
  if (rc == 0 && (en->ip->di.i_flags & EXT2_IMMUTABLE_FL))
    rc = -EPERM;
  if (rc == 0) {
    en->ip->di.i_mode =
        (uint16_t)((en->ip->di.i_mode & S_IFMT) | (mode & 07777));
    en->ip->di.i_ctime = ext2_now();
    rc = ext2_ip_flush(fs, en->ip);
    ext2_refresh_vnode(node, en->ip);
  }
  mutex_unlock(&fs->lock);
  return rc;
}

static int ext2_chown_op(vfs_node_t *node, uint32_t uid, uint32_t gid) {
  ext2_node_t *en = node ? node->priv : NULL;
  if (!en)
    return -EINVAL;
  ext2_fs_t *fs = en->fs;
  mutex_lock(&fs->lock);
  int rc = ext2_begin_write(fs);
  if (rc == 0 && (en->ip->di.i_flags & EXT2_IMMUTABLE_FL))
    rc = -EPERM;
  if (rc == 0) {
    ext2_inode_t *di = &en->ip->di;
    if (uid != (uint32_t)-1) {
      di->i_uid = (uint16_t)uid;
      di->i_uid_high = (uint16_t)(uid >> 16);
    }
    if (gid != (uint32_t)-1) {
      di->i_gid = (uint16_t)gid;
      di->i_gid_high = (uint16_t)(gid >> 16);
    }
    di->i_ctime = ext2_now();
    rc = ext2_ip_flush(fs, en->ip);
    ext2_refresh_vnode(node, en->ip);
  }
  mutex_unlock(&fs->lock);
  return rc;
}

static int ext2_utimes_op(vfs_node_t *node, int64_t mtime_sec) {
  ext2_node_t *en = node ? node->priv : NULL;
  if (!en)
    return -EINVAL;
  ext2_fs_t *fs = en->fs;
  mutex_lock(&fs->lock);
  int rc = ext2_begin_write(fs);
  if (rc == 0 && (en->ip->di.i_flags & EXT2_IMMUTABLE_FL))
    rc = -EPERM;
  if (rc == 0) {
    en->ip->di.i_mtime = (uint32_t)mtime_sec;
    en->ip->di.i_ctime = ext2_now();
    rc = ext2_ip_flush(fs, en->ip);
    ext2_refresh_vnode(node, en->ip);
  }
  mutex_unlock(&fs->lock);
  return rc;
}

// FS_IOC_GETFLAGS / SETFLAGS / GETVERSION (lsattr, chattr).
static int64_t ext2_node_ioctl(vfs_node_t *node, unsigned long req,
                               uint64_t arg) {
  ext2_node_t *en = node ? node->priv : NULL;
  if (!en)
    return -ENOTTY;
  ext2_fs_t *fs = en->fs;
  ext2_ino_t *ip = en->ip;
  process_t *p = process_current();
  if (req == FS_IOC_GETFLAGS_ || req == FS_IOC_GETVERSION_) {
    mutex_lock(&fs->lock);
    long v = (req == FS_IOC_GETFLAGS_) ? (long)ip->di.i_flags : (long)ip->di.i_generation;
    mutex_unlock(&fs->lock);
    return copy_to_user((void *)arg, &v, sizeof(v)) < 0 ? -EFAULT : 0;
  }
  if (req != FS_IOC_SETFLAGS_)
    return -ENOTTY;
  long v = 0;
  if (copy_from_user(&v, (void *)arg, sizeof(v)) < 0)
    return -EFAULT;
  uint32_t want = (uint32_t)v & EXT2_USER_FLAGS;
  mutex_lock(&fs->lock);
  int rc = ext2_begin_write(fs);
  if (rc == 0) {
    uint32_t old = ip->di.i_flags;
    uint32_t uid = ip->di.i_uid | ((uint32_t)ip->di.i_uid_high << 16);
    if (!p || (p->euid != 0 && p->euid != uid))
      rc = -EPERM;
    else if (((old ^ want) & EXT2_PROTECT_FL) && p->euid != 0)
      rc = -EPERM; // immutable/append-only: solo root (CAP_LINUX_IMMUTABLE)
    else {
      ip->di.i_flags = (old & ~EXT2_USER_FLAGS) | want;
      ip->di.i_ctime = ext2_now();
      rc = ext2_ip_flush(fs, ip);
    }
  }
  mutex_unlock(&fs->lock);
  return rc;
}

static int ext2_fsync_op(vfs_node_t *node) {
  ext2_node_t *en = node ? node->priv : NULL;
  if (!en)
    return -EINVAL;
  mutex_lock(&en->fs->lock);
  ext2_pg_sync_ino(en->fs, en->ip); // cambios hechos a través de mmap
  mutex_unlock(&en->fs->lock);
  return bc_sync_dev(en->fs->bdev) == 0 ? 0 : -EIO;
}

// mmap(MAP_SHARED): página `pgoff` (4 KiB) del fichero. Más allá del final
// -> -EFAULT (el proceso recibe SIGBUS). La página vive mientras el inodo.
static int ext2_get_page_op(vfs_node_t *node, uint64_t pgoff, uint64_t *phys_out) {
  ext2_node_t *en = node ? node->priv : NULL;
  if (!en || !phys_out)
    return -EINVAL;
  ext2_fs_t *fs = en->fs;
  ext2_ino_t *ip = en->ip;
  if (EXT2_PG % fs->bs)
    return -ENODEV;
  mutex_lock(&fs->lock);
  int rc = 0;
  if ((ip->di.i_mode & S_IFMT) != S_IFREG) {
    rc = -ENODEV;
  } else if (pgoff * EXT2_PG >= ip->size) {
    rc = -EFAULT;
  } else {
    ext2_pg_t *pg = ext2_pg_find(ip, pgoff);
    if (!pg) {
      pg = kzalloc(sizeof(*pg));
      uint64_t ph = pg ? pmm_alloc_page() : 0;
      if (!pg || !ph) {
        kfree(pg);
        rc = -ENOMEM;
      } else {
        uint8_t *mem = phys_to_virt(ph);
        memset(mem, 0, EXT2_PG);
        ext2_read_data_x(fs, &ip->di, ip->size, pgoff * EXT2_PG, EXT2_PG, mem, 15);
        pg->pgoff = pgoff;
        pg->phys = ph;
        pg->next = ip->pages;
        ip->pages = pg;
      }
    }
    if (rc == 0)
      *phys_out = pg->phys;
  }
  mutex_unlock(&fs->lock);
  return rc;
}

// ---------------------------------------------------------------------------
// E3: rename y link
// ---------------------------------------------------------------------------
static int ext2_set_dotdot(ext2_fs_t *fs, ext2_ino_t *dir, uint32_t parent) {
  uint32_t pblk;
  int rc = ext2_dir_unindex(fs, dir);
  if (rc != 0)
    return rc;
  rc = ext2_bmap(fs, &dir->di, 0, &pblk);
  if (rc != 0)
    return rc;
  if (pblk == 0)
    return -EIO;
  bc_buf_t *b = bc_get(fs->bdev, pblk, fs->bs);
  if (!b)
    return -EIO;
  uint8_t *blk = bc_data(b);
  // "." ocupa rec_len bytes; ".." empieza justo después.
  uint16_t dot_rl;
  memcpy(&dot_rl, blk + 4, 2);
  if (dot_rl < 12u || (uint32_t)dot_rl + 12 > fs->bs || blk[dot_rl + 6] != 2 ||
      blk[dot_rl + 8] != '.' || blk[dot_rl + 9] != '.') {
    bc_put(b);
    return -EIO;
  }
  memcpy(blk + dot_rl, &parent, 4);
  ext2_dc_inval(fs, dir->ino, "..", 2);
  ext2_dir_csum_set(fs, dir->ino, dir->di.i_generation, blk);
  bc_mark_dirty(b);
  bc_put(b);
  return 0;
}

// ¿`anc` es `dir` o un ancestro suyo? (sube por "..")
static int ext2_is_ancestor(ext2_fs_t *fs, uint32_t anc, uint32_t dir) {
  uint32_t cur = dir;
  for (int i = 0; i < 4096; i++) {
    if (cur == anc)
      return 1;
    if (cur == EXT2_ROOT_INO)
      return 0;
    ext2_inode_t di;
    if (ext2_read_inode(fs, cur, &di) != 0)
      return 1; // en duda, denegar
    uint32_t up;
    if (ext2_dir_find(fs, &di, ext2_file_size(fs, &di), "..", 2, &up) != 0)
      return 1;
    if (up == cur)
      return 0;
    cur = up;
  }
  return 1;
}

static int ext2_rename_op(vfs_node_t *sdvn, const char *sname,
                          vfs_node_t *ddvn, const char *dname) {
  ext2_node_t *sden = sdvn ? sdvn->priv : NULL, *dden = ddvn ? ddvn->priv : NULL;
  if (!sden || !dden || !sname || !dname)
    return -EINVAL;
  ext2_fs_t *fs = sden->fs;
  if (dden->fs != fs)
    return -EXDEV;
  size_t sl = strlen(sname), dl = strlen(dname);
  int rc = ext2_name_check(sname, sl);
  if (rc == 0)
    rc = ext2_name_check(dname, dl);
  if (rc != 0)
    return rc;
  if ((sl == 1 && sname[0] == '.') || (sl == 2 && sname[0] == '.' && sname[1] == '.') ||
      (dl == 1 && dname[0] == '.') || (dl == 2 && dname[0] == '.' && dname[1] == '.'))
    return -EBUSY; // como Linux
  ext2_ino_t *sp = sden->ip, *dp = dden->ip;

  mutex_lock(&fs->lock);
  ext2_ino_t *sip = NULL, *dip = NULL;
  rc = ext2_begin_write(fs);
  if (rc != 0)
    goto out;
  if ((sp->di.i_mode & S_IFMT) != S_IFDIR || (dp->di.i_mode & S_IFMT) != S_IFDIR) {
    rc = -ENOTDIR;
    goto out;
  }
  if (dp->di.i_links_count == 0) {
    rc = -ENOENT;
    goto out;
  }
  uint32_t sino, dino = 0;
  rc = ext2_dir_lookup(fs, sp->ino, &sp->di, sp->size, sname, sl, &sino);
  if (rc != 0)
    goto out;
  sip = ext2_iget(fs, sino);
  if (!sip) {
    rc = -EIO;
    goto out;
  }
  int sdir = (sip->di.i_mode & S_IFMT) == S_IFDIR;
  if (sip->di.i_flags & EXT2_PROTECT_FL) {
    rc = -EPERM;
    goto out;
  }
  int drc = ext2_dir_lookup(fs, dp->ino, &dp->di, dp->size, dname, dl, &dino);
  if (drc != 0 && drc != -ENOENT) {
    rc = drc;
    goto out;
  }
  int exists = (drc == 0);
  if (exists && dino == sino) // mismo fichero (p. ej. dos hard links): nada
    goto out;
  if (exists) {
    dip = ext2_iget(fs, dino);
    if (!dip) {
      rc = -EIO;
      goto out;
    }
    int ddir = (dip->di.i_mode & S_IFMT) == S_IFDIR;
    if (dip->di.i_flags & EXT2_PROTECT_FL) {
      rc = -EPERM;
      goto out;
    }
    if (sdir && !ddir) {
      rc = -ENOTDIR;
      goto out;
    }
    if (!sdir && ddir) {
      rc = -EISDIR;
      goto out;
    }
    if (ddir && !ext2_dir_is_empty(fs, dip)) {
      rc = -ENOTEMPTY;
      goto out;
    }
  }
  int moving_dir = sdir && sp != dp;
  if (moving_dir) {
    if (ext2_is_ancestor(fs, sino, dp->ino)) { // mover un dir dentro de sí mismo
      rc = -EINVAL;
      goto out;
    }
    if (dp->di.i_links_count >= EXT2_LINK_MAX && !(exists && dip)) {
      rc = -EMLINK;
      goto out;
    }
  }

  uint8_t ft = ext2_type_to_ft(sip->di.i_mode & S_IFMT);
  if (exists) {
    uint32_t rino;
    rc = ext2_dir_remove(fs, dp, dname, dl, &rino);
    if (rc != 0)
      goto out;
    if ((dip->di.i_mode & S_IFMT) == S_IFDIR) {
      dip->di.i_links_count = 0;
      if (dp->di.i_links_count > 2)
        dp->di.i_links_count--;
    } else if (dip->di.i_links_count > 0) {
      dip->di.i_links_count--;
    }
    dip->di.i_ctime = ext2_now();
    if (dip->di.i_links_count == 0)
      dip->di.i_dtime = ext2_now();
    ext2_ip_flush(fs, dip);
    if (dip->di.i_links_count == 0 && dip->refs > 1)
      ext2_orphan_add(fs, dip);
  }
  rc = ext2_dir_add(fs, dp, dname, dl, sino, ft);
  if (rc != 0)
    goto out; // (si dst existía, ya se quitó: fsck lo ve como inodo huérfano)
  uint32_t rino2;
  rc = ext2_dir_remove(fs, sp, sname, sl, &rino2);
  if (rc != 0)
    goto out;
  if (moving_dir) {
    ext2_set_dotdot(fs, sip, dp->ino);
    if (sp->di.i_links_count > 2)
      sp->di.i_links_count--;
    dp->di.i_links_count++;
    ext2_ip_flush(fs, sp);
    ext2_ip_flush(fs, dp);
  }
  sip->di.i_ctime = ext2_now();
  ext2_ip_flush(fs, sip);
out:
  if (dip)
    ext2_iput(fs, dip);
  if (sip)
    ext2_iput(fs, sip);
  mutex_unlock(&fs->lock);
  return rc;
}

static int ext2_link_op(vfs_node_t *dvn, const char *name, vfs_node_t *target) {
  ext2_node_t *den = dvn ? dvn->priv : NULL, *ten = target ? target->priv : NULL;
  if (!den || !ten || !name)
    return -EINVAL;
  ext2_fs_t *fs = den->fs;
  if (ten->fs != fs)
    return -EXDEV;
  size_t nl = strlen(name);
  int rc = ext2_name_check(name, nl);
  if (rc != 0)
    return rc;
  ext2_ino_t *dp = den->ip, *tip = ten->ip;
  mutex_lock(&fs->lock);
  rc = ext2_begin_write(fs);
  if (rc != 0)
    goto out;
  if ((dp->di.i_mode & S_IFMT) != S_IFDIR) {
    rc = -ENOTDIR;
    goto out;
  }
  if ((tip->di.i_mode & S_IFMT) == S_IFDIR) {
    rc = -EPERM;
    goto out;
  }
  if (tip->di.i_links_count == 0 || dp->di.i_links_count == 0) {
    rc = -ENOENT;
    goto out;
  }
  if (tip->di.i_flags & EXT2_PROTECT_FL) {
    rc = -EPERM;
    goto out;
  }
  if (tip->di.i_links_count >= EXT2_LINK_MAX) {
    rc = -EMLINK;
    goto out;
  }
  uint32_t tmp;
  rc = ext2_dir_lookup(fs, dp->ino, &dp->di, dp->size, name, nl, &tmp);
  if (rc == 0) {
    rc = -EEXIST;
    goto out;
  }
  if (rc != -ENOENT)
    goto out;
  rc = ext2_dir_add(fs, dp, name, nl, tip->ino,
                    ext2_type_to_ft(tip->di.i_mode & S_IFMT));
  if (rc != 0)
    goto out;
  tip->di.i_links_count++;
  tip->di.i_ctime = ext2_now();
  rc = ext2_ip_flush(fs, tip);
  ext2_refresh_vnode(target, tip);
out:
  mutex_unlock(&fs->lock);
  return rc;
}

static vfs_ops_t ext2_node_ops = {
    .read = ext2_node_read,
    .write = ext2_node_write,
    .open = ext2_node_open,
    .close = ext2_node_close,
    .create = ext2_create_op,
    .mkdir = ext2_mkdir_op,
    .unlink = ext2_unlink_op,
    .mknod = ext2_mknod_op,
    .truncate = ext2_node_truncate,
    .readdir = ext2_node_readdir,
    .readlink = ext2_node_readlink,
    .symlink = ext2_symlink_op,
    .rename = ext2_rename_op,
    .link = ext2_link_op,
    .utimes = ext2_utimes_op,
    .chmod = ext2_chmod_op,
    .chown = ext2_chown_op,
    .ioctl = ext2_node_ioctl,
    .fsync = ext2_fsync_op,
    .get_page = ext2_get_page_op,
};

// ---------------------------------------------------------------------------
// Montaje
// ---------------------------------------------------------------------------
// Comprobación ligera al montar: recuenta los bits libres de cada bitmap y los
// compara con los descriptores de grupo. Si difieren (típico tras un corte),
// los contadores en RAM se corrigen (statfs y la asignación dejan de
// fiarse de cifras viejas) y se marcan para escribirse al pasar a rw.
// Devuelve cuántos grupos discrepaban.
static uint32_t ext2_count_zero(const uint8_t *m, uint32_t nbits) {
  uint32_t z = 0, i = 0;
  for (; i + 8 <= nbits; i += 8) {
    uint32_t v = m[i / 8];
    v = v - ((v >> 1) & 0x55);
    v = (v & 0x33) + ((v >> 2) & 0x33);
    z += 8 - ((v + (v >> 4)) & 0x0F);
  }
  for (; i < nbits; i++)
    z += !bit_test(m, i);
  return z;
}

static uint32_t ext2_check(ext2_fs_t *fs) {
  uint32_t bad = 0, tb = 0, ti = 0;
  for (uint32_t g = 0; g < fs->ngroups; g++) {
    ext2_group_t *gr = &fs->groups[g];
    if (!gr->block_bitmap || !gr->inode_bitmap) {
      tb += gr->free_blocks;
      ti += gr->free_inodes;
      continue;
    }
    int diff = 0;
    bc_buf_t *b = bc_get(fs->bdev, gr->block_bitmap, fs->bs);
    if (b) {
      uint32_t n = ext2_group_nblocks(fs, g);
      if (n > fs->bs * 8)
        n = fs->bs * 8;
      uint32_t z = ext2_count_zero(bc_data(b), n);
      bc_put(b);
      if (z != gr->free_blocks) {
        LOG_WARN("[EXT2] %s: grupo %u: bloques libres %u, bitmap dice %u",
                 fs->bdev->name, g, gr->free_blocks, z);
        gr->free_blocks = z;
        diff = 1;
      }
    }
    b = bc_get(fs->bdev, gr->inode_bitmap, fs->bs);
    if (b) {
      uint32_t n = fs->inodes_per_group;
      if (n > fs->bs * 8)
        n = fs->bs * 8;
      uint32_t z = ext2_count_zero(bc_data(b), n);
      bc_put(b);
      if (z != gr->free_inodes) {
        LOG_WARN("[EXT2] %s: grupo %u: inodos libres %u, bitmap dice %u",
                 fs->bdev->name, g, gr->free_inodes, z);
        gr->free_inodes = z;
        diff = 1;
      }
    }
    bad += diff;
    tb += gr->free_blocks;
    ti += gr->free_inodes;
  }
  if (tb != fs->free_blocks || ti != fs->free_inodes) {
    if (bad || (fs->s_state & EXT2_STATE_VALID))
      LOG_WARN("[EXT2] %s: totales del superbloque (%u bloques, %u inodos "
               "libres) corregidos a %u/%u", fs->bdev->name, fs->free_blocks,
               fs->free_inodes, tb, ti);
    fs->free_blocks = tb;
    fs->free_inodes = ti;
  }
  return bad;
}

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
  uint8_t sbraw[1024];
  memcpy(sbraw, raw + (1024 - (size_t)first * ss), sizeof(sbraw));
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
  if (incompat & EXT2_FEATURE_INCOMPAT_RECOVER)
    LOG_WARN("[EXT2] %s: el journal necesita recuperación: se lee sin "
             "reproducirlo (solo lectura; ejecuta e2fsck)", dn);
  if (incompat & ~EXT2_INCOMPAT_SUPPORTED) {
    LOG_ERR("[EXT2] %s: features incompatibles no soportadas 0x%x "
            "(meta_bg/mmp/inline_data/encrypt...)",
            dn, incompat & ~EXT2_INCOMPAT_SUPPORTED);
    return -EINVAL;
  }
  uint16_t desc_size = 0;
  uint32_t blocks_hi = 0;
  memcpy(&desc_size, sbraw + 254, 2);
  memcpy(&blocks_hi, sbraw + 336, 4);
  uint32_t gdsz = sizeof(ext2_gd_t);
  if (incompat & EXT4_FEATURE_INCOMPAT_64BIT) {
    if (desc_size < 64 || desc_size > 1024 || (desc_size & (desc_size - 1))) {
      LOG_ERR("[EXT2] %s: tamaño de descriptor de grupo inválido (%u)", dn,
              desc_size);
      return -EINVAL;
    }
    gdsz = desc_size;
    if (blocks_hi) {
      LOG_ERR("[EXT2] %s: FS de más de 2^32 bloques no soportado", dn);
      return -EINVAL;
    }
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
  uint32_t gd_per_blk = bs / gdsz;
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
  memcpy(&fs->journal_ino, sbraw + 224, 4);
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
    const uint8_t *gp = (uint8_t *)bc_data(b) + (size_t)(g % gd_per_blk) * gdsz;
    memcpy(&gd, gp, sizeof(gd));
    uint8_t raw[64] = {0};
    memcpy(raw, gp, gdsz < 64 ? gdsz : 64);
    uint32_t hi_ptrs = 0;
    if (gdsz >= 64) { // 64bit: mitades altas (solo se admiten FS < 2^32 bloques)
      uint32_t h0, h1, h2;
      memcpy(&h0, gp + 0x20, 4);
      memcpy(&h1, gp + 0x24, 4);
      memcpy(&h2, gp + 0x28, 4);
      hi_ptrs = h0 | h1 | h2;
    }
    bc_put(b);
    if (hi_ptrs) {
      bad_groups++;
      continue;
    }
    ext2_group_t *gr = &fs->groups[g];
    if (gd.bg_inode_table >= fdb && gd.bg_inode_table < sb.s_blocks_count &&
        (uint64_t)gd.bg_inode_table + itable_blocks <= sb.s_blocks_count)
      gr->inode_table = gd.bg_inode_table;
    else
      bad_groups++;
    if (gd.bg_block_bitmap >= fdb && gd.bg_block_bitmap < sb.s_blocks_count &&
        gd.bg_inode_bitmap >= fdb && gd.bg_inode_bitmap < sb.s_blocks_count) {
      gr->block_bitmap = gd.bg_block_bitmap;
      gr->inode_bitmap = gd.bg_inode_bitmap;
    } else if (gr->inode_table) {
      bad_groups++;
    }
    gr->free_blocks = gd.bg_free_blocks_count;
    gr->free_inodes = gd.bg_free_inodes_count;
    gr->used_dirs = gd.bg_used_dirs_count;
    gr->flags = gd.bg_pad;
    {
      uint16_t iu;
      memcpy(&iu, raw + 0x1C, 2);
      gr->itable_unused = iu;
    }
    if (gdsz >= 64) {
      uint16_t hb, hi, hd, hu;
      memcpy(&hb, raw + 0x2C, 2);
      memcpy(&hi, raw + 0x2E, 2);
      memcpy(&hd, raw + 0x30, 2);
      memcpy(&hu, raw + 0x32, 2);
      gr->itable_unused |= (uint32_t)hu << 16;
      gr->free_blocks |= (uint32_t)hb << 16;
      gr->free_inodes |= (uint32_t)hi << 16;
      gr->used_dirs |= (uint32_t)hd << 16;
    }
  }
  fs->itable_blocks = itable_blocks;
  fs->gdsz = gdsz;
  fs->gdt_blocks = gdt_blocks;
  {
    uint16_t rg;
    memcpy(&rg, sbraw + 0xCE, 2);
    fs->resgdt = rg;
  }
  fs->csum = (ro_compat & EXT4_RO_COMPAT_METADATA_CSUM) != 0;
  if (fs->csum)
    fs->csum_seed = ext4_crc32c(~0u, sbraw + 0x68, 16);
  fs->bad_groups = bad_groups;
  fs->s_state = sb.s_state;
  fs->rev1 = sb.s_rev_level == 1;

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

  fs->jn_ok = (compat & EXT2_FEATURE_COMPAT_HAS_JOURNAL) ? ext2_jnl_probe_fs(fs) : 0;
  LOG_INFO("[EXT2] montado %s: rev %u, bloque %u B, %u grupos, "
           "%u inodos de %u B, features c/i/ro=0x%x/0x%x/0x%x%s%s",
           dn, sb.s_rev_level, bs, ngroups, fs->inodes_count, isz, compat,
           incompat, ro_compat,
           (sb.s_state & EXT2_STATE_VALID) ? "" : " [NO DESMONTADO LIMPIO]",
           (compat & EXT2_FEATURE_COMPAT_HAS_JOURNAL) ? " [ext3 con journal]"
                                                      : "");
  if (bad_groups)
    LOG_WARN("[EXT2] %s: %u grupos con tabla de inodos inválida (ignorados)",
             dn, bad_groups);
  // ext4/uninit_bg: los bitmaps pueden no estar inicializados en disco, asi que
  // no se recuentan (el FS se monta solo para lectura de todos modos).
  if (!(incompat & ~EXT2_FEATURE_INCOMPAT_FILETYPE) &&
      !(ro_compat & ~EXT2_RO_COMPAT_RW_OK))
    fs->counts_fixed = ext2_check(fs);
  else
    LOG_INFO("[EXT2] %s: FS tipo ext4 (incompat=0x%x ro=0x%x): solo lectura",
             dn, incompat, ro_compat);
  *fs_priv_out = fs;
  return 0;
}

// ---------------------------------------------------------------------------
// Escritura: habilitar, sincronizar, desmontar
// ---------------------------------------------------------------------------
static int g_flushd_started;
#define EXT2_MAX_RW 8
static ext2_fs_t *g_rw_fs[EXT2_MAX_RW];
static mutex_t g_rw_lock; // orden: g_rw_lock -> fs->lock
static int g_rw_lock_init;

static void ext2_rw_register(ext2_fs_t *fs, int add) {
  if (!g_rw_lock_init) {
    mutex_init(&g_rw_lock);
    g_rw_lock_init = 1;
  }
  mutex_lock(&g_rw_lock);
  for (int i = 0; add && i < EXT2_MAX_RW; i++)
    if (g_rw_fs[i] == fs) {
      mutex_unlock(&g_rw_lock);
      return; // ya registrado
    }
  for (int i = 0; i < EXT2_MAX_RW; i++) {
    if (add && !g_rw_fs[i]) {
      g_rw_fs[i] = fs;
      break;
    }
    if (!add && g_rw_fs[i] == fs)
      g_rw_fs[i] = NULL;
  }
  mutex_unlock(&g_rw_lock);
}

// Vuelca la caché cada pocos segundos para acotar lo que se pierde si se va la
// luz y libera lo diferido. El estado "limpio" NO se marca aquí: solo en
// sync/umount.
static int ext2_flushd(void *arg) {
  (void)arg;
  completion_t never;
  completion_init(&never);
  while (!kthread_should_stop()) {
    wait_for_completion_timeout(&never, 5000);
    mutex_lock(&g_rw_lock);
    for (int i = 0; i < EXT2_MAX_RW; i++) {
      ext2_fs_t *fs = g_rw_fs[i];
      if (!fs)
        continue;
      mutex_lock(&fs->lock);
      if (fs->rw) {
        ext2_pg_sync_all(fs);
        ext2_commit_frees(fs);
      }
      mutex_unlock(&fs->lock);
    }
    mutex_unlock(&g_rw_lock);
    bc_sync_all();
  }
  return 0;
}

// Como el ext2 de Linux: avisa si toca revisar el FS (no impide montar).
static void ext2_mnt_warn(ext2_fs_t *fs) {
  uint32_t blk = 1024 / fs->bs, off = 1024 % fs->bs;
  bc_buf_t *b = bc_get(fs->bdev, blk, fs->bs);
  if (!b)
    return;
  ext2_sb_t sb;
  memcpy(&sb, (uint8_t *)bc_data(b) + off, sizeof(sb));
  bc_put(b);
  if (sb.s_max_mnt_count > 0 && (int)sb.s_mnt_count >= sb.s_max_mnt_count)
    LOG_WARN("[EXT2] %s: %u montajes desde la última comprobación (máx. %d): "
             "conviene ejecutar e2fsck en el host", fs->bdev->name,
             sb.s_mnt_count, sb.s_max_mnt_count);
  uint32_t now = ext2_now();
  if (sb.s_checkinterval && sb.s_lastcheck &&
      now > sb.s_lastcheck + sb.s_checkinterval)
    LOG_WARN("[EXT2] %s: pasó el intervalo de comprobación: conviene ejecutar "
             "e2fsck en el host", fs->bdev->name);
}

// ¿El grupo `g` lleva copia del superbloque y de la GDT? (sparse_super)
static int ext2_group_has_super(const ext2_fs_t *fs, uint32_t g) {
  if (!(fs->ro_compat & EXT2_FEATURE_RO_COMPAT_SPARSE_SUPER) || g <= 1)
    return 1;
  for (uint32_t b = 3; b <= 7; b += 2) {
    uint64_t x = b;
    while (x < g)
      x *= b;
    if (x == g)
      return 1;
  }
  return 0;
}

// metadata_csum: los grupos marcados INODE_UNINIT/BLOCK_UNINIT no tienen el
// bitmap en disco. Los materializamos antes de escribir para poder asignar.
static void ext2_init_uninit_groups(ext2_fs_t *fs) {
  if (!fs->csum)
    return;
  for (uint32_t g = 0; g < fs->ngroups; g++) {
    ext2_group_t *gr = &fs->groups[g];
    if (!(gr->flags & 3))
      continue;
    if ((gr->flags & 1) && gr->inode_bitmap) {
      bc_buf_t *b = bc_get_new(fs->bdev, gr->inode_bitmap, fs->bs);
      if (b) {
        uint8_t *im = bc_data(b);
        memset(im, 0, fs->bs);
        for (uint32_t i = fs->inodes_per_group; i < fs->bs * 8; i++)
          bit_set(im, i);
        bc_mark_dirty(b);
        bc_put(b);
      }
      gr->itable_unused = fs->inodes_per_group;
    }
    if ((gr->flags & 2) && gr->block_bitmap) {
      bc_buf_t *b = bc_get_new(fs->bdev, gr->block_bitmap, fs->bs);
      if (b) {
        uint8_t *m = bc_data(b);
        memset(m, 0, fs->bs);
        uint32_t first = ext2_group_first(fs, g), nb = ext2_group_nblocks(fs, g);
        uint32_t meta = ext2_group_has_super(fs, g) ? 1 + fs->gdt_blocks + fs->resgdt : 0;
        for (uint32_t i = 0; i < meta && i < nb; i++)
          bit_set(m, i);
        uint32_t rngs[3][2] = {{gr->block_bitmap, 1}, {gr->inode_bitmap, 1},
                               {gr->inode_table, fs->itable_blocks}};
        for (int r = 0; r < 3; r++)
          for (uint32_t i = 0; i < rngs[r][1]; i++) {
            uint32_t x = rngs[r][0] + i;
            if (x >= first && x < first + nb)
              bit_set(m, x - first);
          }
        for (uint32_t i = nb; i < fs->bs * 8; i++)
          bit_set(m, i);
        bc_mark_dirty(b);
        bc_put(b);
      }
    }
    gr->flags &= (uint16_t)~3u;
    ext2_gd_write(fs, g);
  }
}

// Pasa el FS a escribible si es seguro. Con el mutex tomado.
static int ext2_set_rw_locked(ext2_fs_t *fs) {
  const char *dn = fs->bdev->name;
  if (fs->rw)
    return 0;
  if (fs->bdev->is_read_only) {
    LOG_WARN("[EXT2] %s: dispositivo de solo lectura", dn);
    return -EROFS;
  }
  if (!(fs->s_state & EXT2_STATE_VALID) && !fs->jn_ok) {
    LOG_WARN("[EXT2] %s: no se desmontó limpio: solo lectura (ejecuta e2fsck "
             "en el host)", dn);
    return -EROFS;
  }
  if (fs->s_state & 2 /*ERROR_FS*/) {
    LOG_WARN("[EXT2] %s: el FS tiene errores registrados: solo lectura", dn);
    return -EROFS;
  }
  if (fs->incompat & ~EXT2_INCOMPAT_RW_OK) {
    LOG_WARN("[EXT2] %s: FS ext4/con journal pendiente (incompat=0x%x): solo "
             "lectura", dn, fs->incompat & ~EXT2_INCOMPAT_RW_OK);
    return -EROFS;
  }
  if ((fs->compat & ~EXT2_COMPAT_RW_OK) || (fs->ro_compat & ~EXT2_RO_COMPAT_RW_OK)) {
    LOG_WARN("[EXT2] %s: features no soportadas para escribir (c=0x%x ro=0x%x): "
             "solo lectura", dn, fs->compat & ~EXT2_COMPAT_RW_OK,
             fs->ro_compat & ~EXT2_RO_COMPAT_RW_OK);
    return -EROFS;
  }
  if (fs->bad_groups) {
    LOG_WARN("[EXT2] %s: %u grupos con metadatos inválidos: solo lectura", dn,
             fs->bad_groups);
    return -EROFS;
  }
  if (fs->jn_ok && ext2_jnl_open(fs) != 0) {
    LOG_WARN("[EXT2] %s: no se pudo abrir el journal: se escribe sin él", dn);
    if (!(fs->s_state & EXT2_STATE_VALID))
      return -EROFS;
  }
  ext2_init_uninit_groups(fs);
  ext2_mnt_warn(fs);
  fs->rw = 1;
  fs->disk_clean = 1;
  fs->mnt_pending = 1;
  int rc = ext2_sb_commit(fs, 0); // sucio a disco antes de tocar nada
  if (rc != 0) {
    fs->rw = 0;
    ext2_jnl_close(fs);
    return -EIO;
  }
  if (!g_flushd_started) {
    g_flushd_started = 1;
    if (!kthread_run(ext2_flushd, NULL, "ext2-flushd")) {
      g_flushd_started = 0;
      LOG_WARN("[EXT2] sin hilo de volcado periódico (solo en sync/umount)");
    }
  }
  if (fs->counts_fixed) { // persistir lo que corrigió ext2_check()
    for (uint32_t g = 0; g < fs->ngroups; g++)
      ext2_gd_write(fs, g);
    fs->counts_fixed = 0;
  }
  LOG_INFO("[EXT2] %s: montado para lectura/escritura", dn);
  return 0;
}

// Todo a disco y, si estaba escribible, marca el FS como limpio.
static int ext2_flush_clean_locked(ext2_fs_t *fs) {
  if (!fs->rw)
    return bc_sync_dev(fs->bdev) == 0 ? 0 : -EIO;
  ext2_pg_sync_all(fs); // páginas mmap
  ext2_commit_frees(fs); // liberaciones diferidas (con su barrera)
  int rc = bc_sync_dev(fs->bdev); // datos y metadatos primero
  if (rc != 0)
    return -EIO;
  rc = ext2_sb_commit(fs, 1); // contadores del superbloque + VALID
  return rc == 0 ? 0 : -EIO;
}

static int ext2_sync_op(void *fs_priv) {
  ext2_fs_t *fs = fs_priv;
  if (!fs)
    return -EINVAL;
  mutex_lock(&fs->lock);
  int rc = ext2_flush_clean_locked(fs);
  mutex_unlock(&fs->lock);
  return rc;
}

// Garantiza /lost+found (e2fsck lo necesita para reconectar inodos). Sin el
// mutex tomado: crea el directorio con la misma ruta que mkdir.
static void ext2_ensure_lostfound(ext2_fs_t *fs) {
  static const char name[] = "lost+found";
  mutex_lock(&fs->lock);
  ext2_ino_t *root = fs->rw ? ext2_iget(fs, EXT2_ROOT_INO) : NULL;
  if (!root) {
    mutex_unlock(&fs->lock);
    return;
  }
  uint32_t tmp;
  int rc = ext2_dir_lookup(fs, root->ino, &root->di, root->size, name, sizeof(name) - 1, &tmp);
  mutex_unlock(&fs->lock);
  if (rc == -ENOENT) {
    vfs_node_t *dn = kzalloc(sizeof(*dn));
    ext2_node_t *en = kzalloc(sizeof(*en));
    if (dn && en) {
      en->fs = fs;
      en->ino = EXT2_ROOT_INO;
      en->ip = root;
      dn->priv = en;
      rc = ext2_new_inode_op(dn, name, S_IFDIR | 0700, 0, NULL);
      LOG_INFO("[EXT2] %s: lost+found %s", fs->bdev->name,
               rc == 0 ? "creado" : "no se pudo crear");
    }
    kfree(dn);
    kfree(en);
  }
  mutex_lock(&fs->lock);
  ext2_iput(fs, root);
  mutex_unlock(&fs->lock);
}

int ext2_remount(void *fs_priv, int rw) {
  ext2_fs_t *fs = fs_priv;
  if (!fs)
    return -EINVAL;
  mutex_lock(&fs->lock);
  int rc = 0;
  if (rw) {
    rc = ext2_set_rw_locked(fs);
  } else if (fs->rw) {
    rc = ext2_flush_clean_locked(fs);
    if (rc == 0) {
      fs->rw = 0;
      ext2_jnl_close(fs);
    }
  }
  int now_rw = fs->rw;
  mutex_unlock(&fs->lock);
  if (rc == 0) {
    ext2_rw_register(fs, now_rw);
    if (now_rw)
      ext2_ensure_lostfound(fs);
  }
  return rc;
}

void ext2_umount(void *fs_priv) {
  ext2_fs_t *fs = fs_priv;
  if (!fs)
    return;
  ext2_rw_register(fs, 0); // el hilo de volcado ya no lo toca
  mutex_lock(&fs->lock);
  if (ext2_flush_clean_locked(fs) != 0)
    LOG_ERR("[EXT2] %s: error al volcar en el desmontaje (el FS queda sucio)",
            fs->bdev->name);
  ext2_jnl_close(fs);
  if (fs->icache)
    LOG_WARN("[EXT2] %s: inodos con referencias al desmontar", fs->bdev->name);
  mutex_unlock(&fs->lock);
  bc_invalidate_dev(fs->bdev);
  while (fs->icache) {
    ext2_ino_t *n = fs->icache->next;
    kfree(fs->icache);
    fs->icache = n;
  }
  kfree(fs->pend_blk);
  kfree(fs->pend_ino);
  kfree(fs->dcache);
  kfree(fs->groups);
  kfree(fs);
}

static vfs_fs_ops_t ext2_fs_ops = {
    .lookup = ext2_lookup,
    .statfs = ext2_statfs,
    .name = "ext2",
    .umount = ext2_umount,
    .sync = ext2_sync_op,
};

vfs_fs_ops_t *ext2_get_vfs_ops(void) { return &ext2_fs_ops; }

int ext2_read_label(block_device_t *bdev, char out[17]) {
  if (!bdev || !out || bdev->sector_size == 0 || bdev->sector_size > EXT2_MAX_BS)
    return -EINVAL;
  uint32_t ss = bdev->sector_size, first = 1024 / ss;
  uint32_t nsec = (1024 + ss - 1) / ss;
  uint8_t *raw = kmalloc((size_t)(nsec ? nsec : 1) * ss);
  if (!raw)
    return -ENOMEM;
  int rc = bdev_read(bdev, first, nsec ? nsec : 1, raw) == 0 ? 0 : -EIO;
  if (rc == 0) {
    ext2_sb_t sb;
    memcpy(&sb, raw + (1024 - (size_t)first * ss), sizeof(sb));
    if (sb.s_magic != EXT2_MAGIC)
      rc = -EINVAL;
    else {
      memcpy(out, sb.s_volume_name, 16);
      out[16] = '\0';
    }
  }
  kfree(raw);
  return rc;
}


// ---------------------------------------------------------------------------
// E9.1: reproducción del journal (jbd2) al montar para escritura
// ---------------------------------------------------------------------------
#define JBD2_MAGIC 0xC03B3998u
#define JBD2_DESC 1
#define JBD2_COMMIT 2
#define JBD2_SB_V1 3
#define JBD2_SB_V2 4
#define JBD2_REVOKE 5
#define JBD2_INC_REVOKE 0x1u
#define JBD2_INC_64BIT 0x2u
#define JBD2_INC_CSUM_V2 0x8u
#define JBD2_INC_CSUM_V3 0x10u
#define JBD2_INC_FAST_COMMIT 0x20u
#define JBD2_TAG_ESCAPE 1u
#define JBD2_TAG_SAME_UUID 2u
#define JBD2_TAG_DELETED 4u
#define JBD2_TAG_LAST 8u
#define EXT4_FEATURE_RO_COMPAT_METADATA_CSUM 0x0400u

static uint32_t be32(const uint8_t *p) {
  return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static void put_be32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
  p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

// CRC-32C (Castagnoli) en bruto, como crc32c_le() de Linux: sin inversiones.
static uint32_t g_crc32c_tab[256];
static int g_crc32c_init;
static uint32_t ext4_crc32c(uint32_t crc, const void *buf, size_t len) {
  if (!g_crc32c_init) {
    for (uint32_t i = 0; i < 256; i++) {
      uint32_t c = i;
      for (int k = 0; k < 8; k++)
        c = (c & 1) ? (c >> 1) ^ 0x82F63B78u : c >> 1;
      g_crc32c_tab[i] = c;
    }
    g_crc32c_init = 1;
  }
  const uint8_t *p = buf;
  while (len--)
    crc = g_crc32c_tab[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
  return crc;
}

typedef struct {
  ext2_fs_t *fs;
  ext2_inode_t ji;
  uint32_t maxlen, first, seq, start, feat_incompat;
  uint32_t tag_bytes;
  uint32_t *rev_blk, *rev_seq; // bloques revocados y la transacción que los revoca
  uint32_t nrev, caprev;
  uint8_t *tmp;
} jrn_t;

static int jrn_pblk(jrn_t *j, uint32_t idx, uint32_t *out) {
  if (idx >= j->maxlen)
    return -EIO;
  int rc = ext2_bmap(j->fs, &j->ji, idx, out);
  if (rc == 0 && *out == 0)
    rc = -EIO;
  return rc;
}
static uint32_t jrn_next(const jrn_t *j, uint32_t idx) {
  idx++;
  return idx >= j->maxlen ? j->first + (idx - j->maxlen) : idx;
}
static const uint8_t *jrn_get(jrn_t *j, uint32_t idx, bc_buf_t **h) {
  uint32_t pb;
  if (jrn_pblk(j, idx, &pb) != 0)
    return NULL;
  *h = bc_get(j->fs->bdev, pb, j->fs->bs);
  return *h ? bc_data(*h) : NULL;
}

// Cuántas etiquetas (bloques de datos) lleva un bloque descriptor.
static uint32_t jrn_count_tags(const jrn_t *j, const uint8_t *d) {
  uint32_t bs = j->fs->bs, p = 12, n = 0;
  uint32_t tail = (j->feat_incompat & (JBD2_INC_CSUM_V2 | JBD2_INC_CSUM_V3)) ? 4 : 0;
  while (p + j->tag_bytes <= bs - tail) {
    uint32_t flags = (j->feat_incompat & JBD2_INC_CSUM_V3) ? be32(d + p + 4)
                                                          : be16(d + p + 6);
    n++;
    p += j->tag_bytes;
    if (!(flags & JBD2_TAG_SAME_UUID))
      p += 16;
    if (flags & JBD2_TAG_LAST)
      break;
  }
  return n;
}

static int jrn_revoke_add(jrn_t *j, uint32_t blk, uint32_t seq) {
  for (uint32_t i = 0; i < j->nrev; i++)
    if (j->rev_blk[i] == blk) {
      if ((int32_t)(seq - j->rev_seq[i]) > 0)
        j->rev_seq[i] = seq;
      return 0;
    }
  if (j->nrev == j->caprev) {
    uint32_t nc = j->caprev ? j->caprev * 2 : 256;
    uint32_t *nb = kmalloc((size_t)nc * 4), *ns = kmalloc((size_t)nc * 4);
    if (!nb || !ns) {
      kfree(nb);
      kfree(ns);
      return -ENOMEM;
    }
    if (j->nrev) {
      memcpy(nb, j->rev_blk, (size_t)j->nrev * 4);
      memcpy(ns, j->rev_seq, (size_t)j->nrev * 4);
    }
    kfree(j->rev_blk);
    kfree(j->rev_seq);
    j->rev_blk = nb;
    j->rev_seq = ns;
    j->caprev = nc;
  }
  j->rev_blk[j->nrev] = blk;
  j->rev_seq[j->nrev++] = seq;
  return 0;
}

// Recorre el journal. pass 0: busca el final; 1: recoge revocaciones; 2: aplica.
static int jrn_pass(jrn_t *j, int pass, uint32_t *end_seq) {
  ext2_fs_t *fs = j->fs;
  uint32_t idx = j->start, seq = j->seq;
  int is64 = (fs->incompat & EXT4_FEATURE_INCOMPAT_64BIT) != 0;
  uint32_t steps = 0;
  for (;;) {
    if (++steps > j->maxlen * 2)
      return -EIO;
    if (pass != 0 && (int32_t)(seq - *end_seq) >= 0)
      break; // más allá de la última transacción confirmada
    bc_buf_t *h = NULL;
    const uint8_t *d = jrn_get(j, idx, &h);
    if (!d)
      return -EIO;
    if (be32(d) != JBD2_MAGIC || be32(d + 8) != seq) {
      bc_put(h);
      break;
    }
    uint32_t type = be32(d + 4);
    if (type == JBD2_COMMIT) {
      bc_put(h);
      seq++;
      idx = jrn_next(j, idx);
    } else if (type == JBD2_REVOKE) {
      if (pass == 1) {
        uint32_t used = be32(d + 12), esz = (j->feat_incompat & JBD2_INC_64BIT) ? 8 : 4;
        if (used > fs->bs)
          used = fs->bs;
        for (uint32_t o = 16; o + esz <= used; o += esz) {
          uint32_t b = (esz == 8) ? be32(d + o + 4) : be32(d + o);
          if (jrn_revoke_add(j, b, seq) != 0) {
            bc_put(h);
            return -ENOMEM;
          }
        }
      }
      bc_put(h);
      idx = jrn_next(j, idx);
    } else if (type == JBD2_DESC) {
      uint32_t tail = (j->feat_incompat & (JBD2_INC_CSUM_V2 | JBD2_INC_CSUM_V3)) ? 4 : 0;
      uint32_t p = 12;
      uint32_t didx = jrn_next(j, idx);
      int rc = 0;
      while (p + j->tag_bytes <= fs->bs - tail) {
        uint32_t blk = be32(d + p), flags;
        if (j->feat_incompat & JBD2_INC_CSUM_V3) {
          flags = be32(d + p + 4);
          if (is64) blk = blk; // la mitad alta se ignora (FS < 2^32 bloques)
        } else {
          flags = be16(d + p + 6);
        }
        if (pass == 2) {
          int revoked = 0;
          for (uint32_t r = 0; r < j->nrev; r++)
            if (j->rev_blk[r] == blk && (int32_t)(j->rev_seq[r] - seq) >= 0)
              revoked = 1;
          if (!revoked && !(flags & JBD2_TAG_DELETED)) {
            bc_buf_t *sh = NULL;
            const uint8_t *src = jrn_get(j, didx, &sh);
            if (!src) {
              rc = -EIO;
              break;
            }
            if (blk >= fs->blocks_count) {
              bc_put(sh);
              rc = -EIO;
              break;
            }
            memcpy(j->tmp, src, fs->bs);
            bc_put(sh);
            if (flags & JBD2_TAG_ESCAPE)
              put_be32(j->tmp, JBD2_MAGIC);
            bc_buf_t *dh = bc_get_new(fs->bdev, blk, fs->bs);
            if (!dh) {
              rc = -EIO;
              break;
            }
            memcpy(bc_data(dh), j->tmp, fs->bs);
            bc_mark_dirty(dh);
            bc_put(dh);
          }
        }
        didx = jrn_next(j, didx);
        p += j->tag_bytes;
        if (!(flags & JBD2_TAG_SAME_UUID))
          p += 16;
        if (flags & JBD2_TAG_LAST)
          break;
      }
      bc_put(h);
      if (rc != 0)
        return rc;
      idx = didx;
    } else {
      bc_put(h);
      break;
    }
  }
  if (pass == 0)
    *end_seq = seq;
  return 0;
}

// Reproduce el journal de `fs` (montado en solo lectura) y lo deja limpio:
// escribe los bloques de las transacciones confirmadas, pone s_start=0 en el
// journal y quita needs_recovery del superbloque. 0 o -errno.
static int ext2_journal_replay(ext2_fs_t *fs) {
  if (!(fs->compat & EXT2_FEATURE_COMPAT_HAS_JOURNAL) || fs->journal_ino == 0)
    return -ENODEV; // journal externo: no soportado
  jrn_t j;
  memset(&j, 0, sizeof(j));
  j.fs = fs;
  if (ext2_read_inode(fs, fs->journal_ino, &j.ji) != 0)
    return -EIO;
  bc_buf_t *sbh = NULL;
  j.maxlen = UINT32_MAX; // para que jrn_pblk(0) funcione antes de conocerlo
  const uint8_t *jsb = jrn_get(&j, 0, &sbh);
  if (!jsb)
    return -EIO;
  uint32_t type = be32(jsb + 4), bsz = be32(jsb + 12);
  if (be32(jsb) != JBD2_MAGIC || (type != JBD2_SB_V1 && type != JBD2_SB_V2) ||
      bsz != fs->bs) {
    bc_put(sbh);
    LOG_ERR("[EXT2] %s: superbloque del journal inválido", fs->bdev->name);
    return -EINVAL;
  }
  j.maxlen = be32(jsb + 16);
  j.first = be32(jsb + 20);
  j.seq = be32(jsb + 24);
  j.start = be32(jsb + 28);
  uint32_t feat_in = (type == JBD2_SB_V2) ? be32(jsb + 40) : 0;
  bc_put(sbh);
  if (feat_in & JBD2_INC_FAST_COMMIT) {
    LOG_ERR("[EXT2] %s: journal con fast commit no soportado", fs->bdev->name);
    return -EOPNOTSUPP;
  }
  if (feat_in & ~(JBD2_INC_REVOKE | JBD2_INC_64BIT | JBD2_INC_CSUM_V2 | JBD2_INC_CSUM_V3 | 4u)) {
    LOG_ERR("[EXT2] %s: features del journal no soportadas 0x%x", fs->bdev->name, feat_in);
    return -EOPNOTSUPP;
  }
  j.feat_incompat = feat_in;
  if (j.maxlen < 8 || j.first == 0 || j.first >= j.maxlen)
    return -EINVAL;
  uint32_t tb = (feat_in & JBD2_INC_CSUM_V3) ? 16u : 12u;
  if (!(feat_in & JBD2_INC_CSUM_V3)) {
    if (feat_in & JBD2_INC_CSUM_V2) tb += 2;
    if (!(feat_in & JBD2_INC_64BIT)) tb -= 4;
  }
  j.tag_bytes = tb;
  j.tmp = kmalloc(fs->bs);
  if (!j.tmp)
    return -ENOMEM;
  int rc = 0;
  uint32_t end_seq = j.seq;
  if (j.start != 0) {
    rc = jrn_pass(&j, 0, &end_seq);
    if (rc == 0) rc = jrn_pass(&j, 1, &end_seq);
    if (rc == 0) rc = jrn_pass(&j, 2, &end_seq);
    LOG_INFO("[EXT2] %s: journal reproducido (secuencias %u..%u, %u revocaciones) rc=%d",
             fs->bdev->name, j.seq, end_seq, j.nrev, rc);
  }
  kfree(j.rev_blk);
  kfree(j.rev_seq);
  kfree(j.tmp);
  if (rc != 0)
    return rc;
  // Journal limpio: start=0 y secuencia siguiente.
  uint32_t pb;
  if (jrn_pblk(&j, 0, &pb) != 0)
    return -EIO;
  bc_buf_t *jb = bc_get(fs->bdev, pb, fs->bs);
  if (!jb)
    return -EIO;
  uint8_t *jd = bc_data(jb);
  put_be32(jd + 24, j.start ? end_seq + 1 : j.seq);
  put_be32(jd + 28, 0);
  if (feat_in & (JBD2_INC_CSUM_V2 | JBD2_INC_CSUM_V3)) {
    put_be32(jd + 0xFC, 0);
    put_be32(jd + 0xFC, ext4_crc32c(~0u, jd, 1024));
  }
  bc_mark_dirty(jb);
  bc_put(jb);
  // Superbloque del FS: quitar needs_recovery (y recalcular su checksum).
  uint32_t sblk = 1024 / fs->bs, soff = 1024 % fs->bs;
  bc_buf_t *sb = bc_get(fs->bdev, sblk, fs->bs);
  if (!sb)
    return -EIO;
  uint8_t *sd = (uint8_t *)bc_data(sb) + soff;
  uint32_t inc;
  memcpy(&inc, sd + 96, 4);
  inc &= ~EXT2_FEATURE_INCOMPAT_RECOVER;
  memcpy(sd + 96, &inc, 4);
  if (fs->ro_compat & EXT4_FEATURE_RO_COMPAT_METADATA_CSUM) {
    uint32_t c = ext4_crc32c(~0u, sd, 1020);
    memcpy(sd + 1020, &c, 4);
  }
  bc_mark_dirty(sb);
  bc_put(sb);
  return bc_sync_dev(fs->bdev) == 0 ? 0 : -EIO;
}

// ---------------------------------------------------------------------------
// E9 (L4): escritura por el journal. Cada volcado de metadatos es una
// transacción jbd2 completa (descriptores + bloques + commit) que se
// "checkpointea" enseguida y deja el journal vacío (start = 0). Si se va la luz
// a medias, el siguiente montaje reproduce la transacción (o la ignora si no
// llegó a confirmarse).
// ---------------------------------------------------------------------------
typedef struct ext2_jnl {
  ext2_fs_t *fs;
  uint32_t *map;
  uint32_t maxlen, first, seq, tag_bytes, cap, seed;
  int v3;
  uint8_t uuid[16];
  uint8_t *jsb, *desc, *tmp;
} ext2_jnl_t;

static void put_be16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)(v >> 8);
  p[1] = (uint8_t)v;
}

// Lee y valida el superbloque del journal. 0 si lo sabemos escribir.
static int jnl_read_sb(ext2_fs_t *fs, jrn_t *j, uint32_t *feat, uint8_t *uuid) {
  if (!(fs->compat & EXT2_FEATURE_COMPAT_HAS_JOURNAL) || fs->journal_ino == 0)
    return -ENODEV;
  memset(j, 0, sizeof(*j));
  j->fs = fs;
  if (ext2_read_inode(fs, fs->journal_ino, &j->ji) != 0)
    return -EIO;
  j->maxlen = UINT32_MAX;
  bc_buf_t *h = NULL;
  const uint8_t *sb = jrn_get(j, 0, &h);
  if (!sb)
    return -EIO;
  uint32_t type = be32(sb + 4);
  int ok = be32(sb) == JBD2_MAGIC && type == JBD2_SB_V2 && be32(sb + 12) == fs->bs;
  if (ok) {
    j->maxlen = be32(sb + 16);
    j->first = be32(sb + 20);
    j->seq = be32(sb + 24);
    j->start = be32(sb + 28);
    *feat = be32(sb + 40);
    if (uuid)
      memcpy(uuid, sb + 0x30, 16);
  }
  bc_put(h);
  if (!ok || j->maxlen < 64 || j->first == 0 || j->first >= j->maxlen)
    return -EINVAL;
  // Soportado: revoke, 64bit y csum v3 (el v2, async commit y fast commit no).
  if (*feat & ~(JBD2_INC_REVOKE | JBD2_INC_64BIT | JBD2_INC_CSUM_V3))
    return -EOPNOTSUPP;
  return 0;
}

static int ext2_jnl_probe_fs(ext2_fs_t *fs) {
  jrn_t j;
  uint32_t feat = 0;
  return jnl_read_sb(fs, &j, &feat, NULL) == 0;
}

static int jnl_wr(ext2_jnl_t *j, uint32_t idx, const void *buf) {
  ext2_fs_t *fs = j->fs;
  uint32_t ss = fs->bdev->sector_size;
  return bdev_write(fs->bdev, (uint64_t)j->map[idx] * (fs->bs / ss), fs->bs / ss, buf);
}
static uint32_t jnl_next(const ext2_jnl_t *j, uint32_t idx) {
  idx++;
  return idx >= j->maxlen ? j->first : idx;
}

static int jnl_write_sb(ext2_jnl_t *j, uint32_t start, uint32_t seq) {
  put_be32(j->jsb + 24, seq);
  put_be32(j->jsb + 28, start);
  if (j->v3) {
    put_be32(j->jsb + 0xFC, 0);
    put_be32(j->jsb + 0xFC, ext4_crc32c(~0u, j->jsb, 1024));
  }
  return jnl_wr(j, 0, j->jsb);
}

// Escribe `n` bloques de metadatos como una transacción y la confirma.
static int jnl_commit(void *priv, bc_buf_t **bufs, uint32_t n) {
  ext2_jnl_t *j = priv;
  ext2_fs_t *fs = j->fs;
  uint32_t bs = fs->bs, tb = j->tag_bytes, tail = j->v3 ? 4 : 0;
  uint32_t seq = j->seq, idx = j->first, i = 0;
  uint8_t seqb[4];
  put_be32(seqb, seq);
  while (i < n) {
    uint32_t didx = idx;
    uint8_t *d = j->desc;
    memset(d, 0, bs);
    put_be32(d, JBD2_MAGIC);
    put_be32(d + 4, JBD2_DESC);
    put_be32(d + 8, seq);
    idx = jnl_next(j, idx);
    uint32_t p = 12, cnt = 0, last_p = 0;
    while (i < n) {
      uint32_t need = tb + (cnt == 0 ? 16 : 0);
      if (p + need > bs - tail)
        break;
      const uint8_t *src = bc_data(bufs[i]);
      uint32_t flags = cnt ? JBD2_TAG_SAME_UUID : 0;
      memcpy(j->tmp, src, bs);
      if (be32(j->tmp) == JBD2_MAGIC) {
        memset(j->tmp, 0, 4);
        flags |= JBD2_TAG_ESCAPE;
      }
      uint32_t blk = (uint32_t)bc_block_no(bufs[i]);
      uint32_t c = 0;
      if (j->v3) {
        c = ext4_crc32c(j->seed, seqb, 4);
        c = ext4_crc32c(c, j->tmp, bs);
        put_be32(d + p, blk);
        put_be32(d + p + 4, flags);
        put_be32(d + p + 8, 0);
        put_be32(d + p + 12, c);
      } else {
        put_be32(d + p, blk);
        put_be16(d + p + 4, 0);
        put_be16(d + p + 6, (uint16_t)flags);
        if (tb >= 12)
          put_be32(d + p + 8, 0);
      }
      if (jnl_wr(j, idx, j->tmp) != 0)
        return -EIO;
      idx = jnl_next(j, idx);
      last_p = p;
      p += tb;
      if (cnt == 0) {
        memcpy(d + p, j->uuid, 16);
        p += 16;
      }
      cnt++;
      i++;
    }
    if (j->v3) {
      uint32_t f = be32(d + last_p + 4) | JBD2_TAG_LAST;
      put_be32(d + last_p + 4, f);
      put_be32(d + bs - 4, 0);
      put_be32(d + bs - 4, ext4_crc32c(j->seed, d, bs));
    } else {
      uint16_t f = be16(d + last_p + 6) | JBD2_TAG_LAST;
      put_be16(d + last_p + 6, f);
    }
    if (jnl_wr(j, didx, d) != 0)
      return -EIO;
  }
  if (bdev_flush(fs->bdev) != 0) // descriptores y datos antes que el commit
    return -EIO;
  uint8_t *cb = j->desc;
  memset(cb, 0, bs);
  put_be32(cb, JBD2_MAGIC);
  put_be32(cb + 4, JBD2_COMMIT);
  put_be32(cb + 8, seq);
  uint64_t now = rtc_get_epoch();
  put_be32(cb + 48, (uint32_t)(now >> 32));
  put_be32(cb + 52, (uint32_t)now);
  if (j->v3)
    put_be32(cb + 16, ext4_crc32c(j->seed, cb, bs));
  if (jnl_wr(j, idx, cb) != 0 || bdev_flush(fs->bdev) != 0)
    return -EIO;
  if (jnl_write_sb(j, j->first, seq) != 0 || bdev_flush(fs->bdev) != 0)
    return -EIO;
  return 0;
}

// Checkpoint hecho: el journal vuelve a estar vacío.
static void jnl_done(void *priv) {
  ext2_jnl_t *j = priv;
  j->seq++;
  if (jnl_write_sb(j, 0, j->seq) == 0)
    bdev_flush(j->fs->bdev);
}

static int ext2_jnl_open(ext2_fs_t *fs) {
  if (fs->jn)
    return 0;
  jrn_t jr;
  uint32_t feat = 0;
  ext2_jnl_t *j = kmalloc(sizeof(*j));
  if (!j)
    return -ENOMEM;
  memset(j, 0, sizeof(*j));
  int rc = jnl_read_sb(fs, &jr, &feat, j->uuid);
  if (rc != 0 || jr.start != 0) {
    kfree(j);
    return rc ? rc : -EBUSY; // con start != 0 hay que reproducirlo antes
  }
  j->fs = fs;
  j->maxlen = jr.maxlen;
  j->first = jr.first;
  j->seq = jr.seq;
  j->v3 = (feat & JBD2_INC_CSUM_V3) != 0;
  j->tag_bytes = j->v3 ? 16u : ((feat & JBD2_INC_64BIT) ? 12u : 8u);
  j->seed = ext4_crc32c(~0u, j->uuid, 16);
  j->map = kmalloc((size_t)j->maxlen * 4);
  j->jsb = kmalloc(fs->bs);
  j->desc = kmalloc(fs->bs);
  j->tmp = kmalloc(fs->bs);
  if (!j->map || !j->jsb || !j->desc || !j->tmp) {
    rc = -ENOMEM;
    goto fail;
  }
  for (uint32_t k = 0; k < j->maxlen; k++) {
    uint32_t pb;
    if (jrn_pblk(&jr, k, &pb) != 0) {
      rc = -EIO;
      goto fail;
    }
    j->map[k] = pb;
  }
  {
    bc_buf_t *h = bc_get(fs->bdev, j->map[0], fs->bs);
    if (!h) {
      rc = -EIO;
      goto fail;
    }
    memcpy(j->jsb, bc_data(h), fs->bs);
    bc_put(h);
  }
  // Una transacción debe caber entera (descriptores + bloques + commit).
  uint32_t room = j->maxlen - j->first - 2;
  uint32_t per_desc = (fs->bs - 12 - (j->v3 ? 4 : 0) - 16) / j->tag_bytes;
  if (per_desc == 0) {
    rc = -EINVAL;
    goto fail;
  }
  j->cap = (uint32_t)((uint64_t)room * per_desc / (per_desc + 1));
  if (j->cap > 1024)
    j->cap = 1024;
  if (j->cap < 8) {
    rc = -EINVAL;
    goto fail;
  }
  fs->jn = j;
  bc_set_journal(fs->bdev, jnl_commit, jnl_done, j, j->cap);
  LOG_INFO("[EXT2] %s: escrituras por el journal (%u bloques, transacciones de hasta %u)",
           fs->bdev->name, j->maxlen, j->cap);
  return 0;
fail:
  kfree(j->map);
  kfree(j->jsb);
  kfree(j->desc);
  kfree(j->tmp);
  kfree(j);
  return rc;
}

static void ext2_jnl_close(ext2_fs_t *fs) {
  ext2_jnl_t *j = fs->jn;
  if (!j)
    return;
  bc_set_journal(fs->bdev, NULL, NULL, NULL, 0);
  fs->jn = NULL;
  kfree(j->map);
  kfree(j->jsb);
  kfree(j->desc);
  kfree(j->tmp);
  kfree(j);
}

int ext2_mount_bdev_rw(const char *bdev_name, const char *mount_path, int rw) {
  if (!bdev_name || !mount_path)
    return -EINVAL;
  block_device_t *bdev = blk_lookup(bdev_name);
  if (!bdev)
    return -ENOENT;
  void *priv = NULL;
  int rc = ext2_mount(bdev, &priv);
  if (rc != 0)
    return rc;
  if (rw && (((ext2_fs_t *)priv)->incompat & EXT2_FEATURE_INCOMPAT_RECOVER)) {
    // E9.1: reproducir el journal y volver a montar con el estado ya al día.
    int jrc = ext2_journal_replay(priv);
    ext2_umount(priv);
    priv = NULL;
    if (jrc != 0)
      return jrc;
    rc = ext2_mount(bdev, &priv);
    if (rc != 0)
      return rc;
  }
  if (rw) {
    rc = ext2_remount(priv, 1);
    if (rc != 0) {
      ext2_umount(priv);
      return rc;
    }
  }
  rc = vfs_mount(mount_path, &ext2_fs_ops, priv);
  if (rc != 0) {
    ext2_umount(priv);
    return rc;
  }
  if (!rw)
    vfs_set_mount_flags(mount_path, VFS_MS_RDONLY);
  return 0;
}

int ext2_mount_bdev(const char *bdev_name, const char *mount_path) {
  return ext2_mount_bdev_rw(bdev_name, mount_path, 0);
}
