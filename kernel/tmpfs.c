// kernel/tmpfs.c
//
// tmpfs: sistema de ficheros en RAM, escribible, con cota de bytes.
// Pensado para /tmp, /var, /run. VFS puro: no toca block layer, ni
// AHCI, ni FAT32. Vive enteramente en kmalloc.
//
// Modelo de datos:
//   - Cada nodo tiene un buffer `data` con `capacity` bytes reservados
//     y `size` bytes visibles. Crece con krealloc. Cota global por FS.
//   - Los directorios tienen lista enlazada simple de hijos. readdir
//     es O(index) — aceptable para directorios pequenos.
//   - Un solo mutex por instancia serializa todas las operaciones.
//
// Refcounts:
//   nlink:     1 mientras el nodo esta enlazado en un directorio.
//              0 tras unlink. El nodo se libera cuando nlink == 0 Y
//              vfs_refs == 0.
//   vfs_refs:  numero de vfs_node_t vivos que envuelven este nodo.
//              Se incrementa en tmpfs_wrap, se decrementa en close.

#include "tmpfs.h"

#include "heap.h"
#include "klog.h"
#include "mutex.h"
#include "rtc.h"
#include "string.h"
#include "uaccess.h"
#include "vfs.h"

#include <stddef.h>

// ---------------------------------------------------------------------------
// Estructuras
// ---------------------------------------------------------------------------
typedef struct tmpfs_node {
  char name[VFS_PATH_MAX];
  uint32_t mode; // S_IF* | perms
  uint32_t uid;
  uint32_t gid;
  uint64_t size; // solo files/symlinks
  int64_t mtime;
  uint32_t inode;

  int nlink;
  int vfs_refs;

  uint8_t *data;   // files: contenido; symlinks: target sin NUL
  size_t capacity; // bytes reservados en data (>= size)

  struct tmpfs_fs *fs;
  struct tmpfs_node *parent;
  struct tmpfs_node *children_head;
  struct tmpfs_node *sibling_next;
} tmpfs_node_t;

typedef struct tmpfs_fs {
  mutex_t lock;
  tmpfs_node_t *root;
  size_t max_bytes;
  size_t used_bytes;
  uint32_t next_inode;
} tmpfs_fs_t;

// Forward declarations: wrap_locked() (mas arriba) referencia estas
// tablas que se definen al final del fichero.
static vfs_ops_t tmpfs_file_ops;
static vfs_ops_t tmpfs_dir_ops;

// ---------------------------------------------------------------------------
// Helpers (requieren fs->lock cogido)
// ---------------------------------------------------------------------------
static tmpfs_node_t *lookup_child_locked(tmpfs_node_t *dir, const char *name) {
  for (tmpfs_node_t *c = dir->children_head; c; c = c->sibling_next) {
    if (strcmp(c->name, name) == 0)
      return c;
  }
  return NULL;
}

static void link_child_locked(tmpfs_node_t *dir, tmpfs_node_t *child) {
  child->sibling_next = dir->children_head;
  dir->children_head = child;
  child->parent = dir;
}

static void unlink_child_locked(tmpfs_node_t *dir, tmpfs_node_t *child) {
  tmpfs_node_t **pp = &dir->children_head;
  while (*pp) {
    if (*pp == child) {
      *pp = child->sibling_next;
      child->sibling_next = NULL;
      child->parent = NULL;
      return;
    }
    pp = &(*pp)->sibling_next;
  }
}

static void free_node_locked(tmpfs_fs_t *fs, tmpfs_node_t *n) {
  if (!n)
    return;
  if (n->data) {
    if (fs->used_bytes >= n->capacity)
      fs->used_bytes -= n->capacity;
    else
      fs->used_bytes = 0;
    kfree(n->data);
    n->data = NULL;
  }
  kfree(n);
}

static vfs_node_t *wrap_locked(tmpfs_node_t *n) {
  vfs_node_t *vn = (vfs_node_t *)kzalloc(sizeof(*vn));
  if (!vn)
    return NULL;

  size_t nl = strlen(n->name);
  if (nl >= sizeof(vn->name))
    nl = sizeof(vn->name) - 1;
  memcpy(vn->name, n->name, nl);
  vn->name[nl] = '\0';

  int is_dir = ((n->mode & S_IFMT) == S_IFDIR);
  vn->flags = is_dir ? VFS_DIRECTORY : VFS_FILE;
  vn->size = n->size;
  vn->inode = n->inode;
  vn->mode = n->mode;
  vn->uid = n->uid;
  vn->gid = n->gid;
  vn->mtime_sec = n->mtime;
  vn->priv = n;
  vn->ref_count = 1;
  vn->ops = is_dir ? &tmpfs_dir_ops : &tmpfs_file_ops;

  if ((n->mode & S_IFMT) == S_IFLNK && n->data && n->size > 0) {
    vn->is_symlink = 1;
    size_t tl = (size_t)n->size;
    if (tl >= sizeof(vn->link_target))
      tl = sizeof(vn->link_target) - 1;
    memcpy(vn->link_target, n->data, tl);
    vn->link_target[tl] = '\0';
  }

  n->vfs_refs++;
  return vn;
}

// ---------------------------------------------------------------------------
// Node ops — ficheros
// ---------------------------------------------------------------------------
static int64_t tmpfs_read_op(vfs_node_t *vn, uint64_t off, size_t sz,
                             void *buf) {
  tmpfs_node_t *n = (tmpfs_node_t *)vn->priv;
  if (!n || !n->fs || !buf)
    return -EINVAL;
  if ((n->mode & S_IFMT) == S_IFDIR)
    return -EISDIR;
  if (sz == 0)
    return 0;

  tmpfs_fs_t *fs = n->fs;
  mutex_lock(&fs->lock);
  if (off >= n->size) {
    mutex_unlock(&fs->lock);
    return 0;
  }
  size_t avail = (size_t)(n->size - off);
  size_t n_copy = (sz < avail) ? sz : avail;
  if (n_copy > 0 && n->data)
    memcpy(buf, n->data + off, n_copy);
  mutex_unlock(&fs->lock);
  return (int64_t)n_copy;
}

static int64_t tmpfs_write_op(vfs_node_t *vn, uint64_t off, size_t sz,
                              const void *buf) {
  tmpfs_node_t *n = (tmpfs_node_t *)vn->priv;
  if (!n || !n->fs || !buf)
    return -EINVAL;
  int type = n->mode & S_IFMT;
  if (type == S_IFDIR)
    return -EISDIR;
  if (type == S_IFLNK)
    return -EPERM;
  if (sz == 0)
    return 0;

  tmpfs_fs_t *fs = n->fs;
  mutex_lock(&fs->lock);

  if (off > SIZE_MAX - sz) {
    mutex_unlock(&fs->lock);
    return -EINVAL;
  }
  size_t end = (size_t)off + sz;

  if (end > n->capacity) {
    size_t new_cap = n->capacity ? n->capacity : 4096;
    while (new_cap < end) {
      if (new_cap > SIZE_MAX / 2) {
        mutex_unlock(&fs->lock);
        return -ENOMEM;
      }
      new_cap *= 2;
    }
    size_t delta = new_cap - n->capacity;
    if (fs->used_bytes + delta > fs->max_bytes) {
      mutex_unlock(&fs->lock);
      return -ENOSPC;
    }
    uint8_t *nd = (uint8_t *)krealloc(n->data, new_cap);
    if (!nd) {
      mutex_unlock(&fs->lock);
      return -ENOMEM;
    }
    memset(nd + n->capacity, 0, delta);
    n->data = nd;
    n->capacity = new_cap;
    fs->used_bytes += delta;
  }

  if (off > n->size)
    memset(n->data + n->size, 0, (size_t)off - n->size);

  memcpy(n->data + off, buf, sz);
  if (end > n->size)
    n->size = end;
  n->mtime = rtc_get_epoch();
  vn->size = n->size;
  vn->mtime_sec = n->mtime;

  mutex_unlock(&fs->lock);
  return (int64_t)sz;
}

static int tmpfs_open_op(vfs_node_t *vn, int flags) {
  (void)vn;
  (void)flags;
  return 0;
}

static int tmpfs_close_op(vfs_node_t *vn) {
  tmpfs_node_t *n = (tmpfs_node_t *)vn->priv;
  if (!n || !n->fs)
    return 0;
  tmpfs_fs_t *fs = n->fs;
  mutex_lock(&fs->lock);
  if (n->vfs_refs > 0)
    n->vfs_refs--;
  if (n->nlink == 0 && n->vfs_refs == 0 && n != fs->root)
    free_node_locked(fs, n);
  mutex_unlock(&fs->lock);
  return 0;
}

static int tmpfs_truncate_op(vfs_node_t *vn, uint64_t new_size) {
  tmpfs_node_t *n = (tmpfs_node_t *)vn->priv;
  if (!n || !n->fs)
    return -EINVAL;
  if ((n->mode & S_IFMT) == S_IFDIR)
    return -EISDIR;
  if (new_size > SIZE_MAX)
    return -EINVAL;

  tmpfs_fs_t *fs = n->fs;
  mutex_lock(&fs->lock);

  if (new_size == 0) {
    if (n->data) {
      fs->used_bytes -= n->capacity;
      kfree(n->data);
      n->data = NULL;
      n->capacity = 0;
    }
  } else if (new_size > n->capacity) {
    size_t new_cap = new_size;
    if (new_cap < 4096)
      new_cap = 4096;
    size_t delta = new_cap - n->capacity;
    if (fs->used_bytes + delta > fs->max_bytes) {
      mutex_unlock(&fs->lock);
      return -ENOSPC;
    }
    uint8_t *nd = (uint8_t *)krealloc(n->data, new_cap);
    if (!nd) {
      mutex_unlock(&fs->lock);
      return -ENOMEM;
    }
    memset(nd + n->capacity, 0, delta);
    n->data = nd;
    n->capacity = new_cap;
    fs->used_bytes += delta;
  } else if (new_size > n->size) {
    memset(n->data + n->size, 0, (size_t)new_size - n->size);
  }

  n->size = new_size;
  n->mtime = rtc_get_epoch();
  vn->size = n->size;
  vn->mtime_sec = n->mtime;
  mutex_unlock(&fs->lock);
  return 0;
}

// ---------------------------------------------------------------------------
// Node ops — namespace
// ---------------------------------------------------------------------------
static int tmpfs_create_op(vfs_node_t *dir_vn, const char *name,
                           uint32_t mode) {
  tmpfs_node_t *dir = (tmpfs_node_t *)dir_vn->priv;
  if (!dir || !dir->fs)
    return -EINVAL;
  if ((dir->mode & S_IFMT) != S_IFDIR)
    return -ENOTDIR;
  if (!name || !name[0] || strlen(name) >= VFS_PATH_MAX)
    return -EINVAL;

  tmpfs_fs_t *fs = dir->fs;
  mutex_lock(&fs->lock);

  if (lookup_child_locked(dir, name)) {
    mutex_unlock(&fs->lock);
    return -EEXIST;
  }

  tmpfs_node_t *n = (tmpfs_node_t *)kzalloc(sizeof(*n));
  if (!n) {
    mutex_unlock(&fs->lock);
    return -ENOMEM;
  }
  size_t nl = strlen(name);
  memcpy(n->name, name, nl);
  n->name[nl] = '\0';
  n->mode = S_IFREG | (mode & 07777);
  n->uid = 0;
  n->gid = 0;
  n->mtime = rtc_get_epoch();
  n->inode = fs->next_inode++;
  n->nlink = 1;
  n->vfs_refs = 0;
  n->fs = fs;
  n->data = NULL;
  n->capacity = 0;
  n->size = 0;

  link_child_locked(dir, n);
  dir->mtime = n->mtime;

  mutex_unlock(&fs->lock);
  return 0;
}

static int tmpfs_mkdir_op(vfs_node_t *dir_vn, const char *name, uint32_t mode) {
  tmpfs_node_t *dir = (tmpfs_node_t *)dir_vn->priv;
  if (!dir || !dir->fs)
    return -EINVAL;
  if ((dir->mode & S_IFMT) != S_IFDIR)
    return -ENOTDIR;
  if (!name || !name[0] || strlen(name) >= VFS_PATH_MAX)
    return -EINVAL;

  tmpfs_fs_t *fs = dir->fs;
  mutex_lock(&fs->lock);

  if (lookup_child_locked(dir, name)) {
    mutex_unlock(&fs->lock);
    return -EEXIST;
  }

  tmpfs_node_t *n = (tmpfs_node_t *)kzalloc(sizeof(*n));
  if (!n) {
    mutex_unlock(&fs->lock);
    return -ENOMEM;
  }
  size_t nl = strlen(name);
  memcpy(n->name, name, nl);
  n->name[nl] = '\0';
  n->mode = S_IFDIR | (mode & 07777);
  n->uid = 0;
  n->gid = 0;
  n->mtime = rtc_get_epoch();
  n->inode = fs->next_inode++;
  n->nlink = 1;
  n->vfs_refs = 0;
  n->fs = fs;

  link_child_locked(dir, n);
  dir->mtime = n->mtime;

  mutex_unlock(&fs->lock);
  return 0;
}

static int tmpfs_unlink_op(vfs_node_t *dir_vn, const char *name) {
  tmpfs_node_t *dir = (tmpfs_node_t *)dir_vn->priv;
  if (!dir || !dir->fs)
    return -EINVAL;
  if ((dir->mode & S_IFMT) != S_IFDIR)
    return -ENOTDIR;
  if (!name || !name[0])
    return -EINVAL;

  tmpfs_fs_t *fs = dir->fs;
  mutex_lock(&fs->lock);

  tmpfs_node_t *child = lookup_child_locked(dir, name);
  if (!child) {
    mutex_unlock(&fs->lock);
    return -ENOENT;
  }

  int is_dir = ((child->mode & S_IFMT) == S_IFDIR);
  if (is_dir && child->children_head) {
    mutex_unlock(&fs->lock);
    return -ENOTEMPTY;
  }

  unlink_child_locked(dir, child);
  child->nlink = 0;
  dir->mtime = rtc_get_epoch();

  if (child->vfs_refs == 0)
    free_node_locked(fs, child);

  mutex_unlock(&fs->lock);
  return 0;
}

static int tmpfs_rename_op(vfs_node_t *src_vn, const char *src_name,
                           vfs_node_t *dst_vn, const char *dst_name) {
  tmpfs_node_t *sp = (tmpfs_node_t *)src_vn->priv;
  tmpfs_node_t *dp = (tmpfs_node_t *)dst_vn->priv;
  if (!sp || !dp || !sp->fs || sp->fs != dp->fs)
    return -EXDEV;
  if (!src_name || !dst_name)
    return -EINVAL;

  tmpfs_fs_t *fs = sp->fs;
  mutex_lock(&fs->lock);

  if ((sp->mode & S_IFMT) != S_IFDIR || (dp->mode & S_IFMT) != S_IFDIR) {
    mutex_unlock(&fs->lock);
    return -ENOTDIR;
  }

  tmpfs_node_t *src = lookup_child_locked(sp, src_name);
  if (!src) {
    mutex_unlock(&fs->lock);
    return -ENOENT;
  }

  // Mismo directorio + mismo nombre: nada que hacer.
  if (sp == dp && strcmp(src_name, dst_name) == 0) {
    mutex_unlock(&fs->lock);
    return 0;
  }

  tmpfs_node_t *dst = lookup_child_locked(dp, dst_name);
  if (dst) {
    if (dst == src) {
      mutex_unlock(&fs->lock);
      return 0;
    }
    int src_is_dir = ((src->mode & S_IFMT) == S_IFDIR);
    int dst_is_dir = ((dst->mode & S_IFMT) == S_IFDIR);
    if (src_is_dir && !dst_is_dir) {
      mutex_unlock(&fs->lock);
      return -ENOTDIR;
    }
    if (!src_is_dir && dst_is_dir) {
      mutex_unlock(&fs->lock);
      return -EISDIR;
    }
    if (dst_is_dir && dst->children_head) {
      mutex_unlock(&fs->lock);
      return -ENOTEMPTY;
    }
    unlink_child_locked(dp, dst);
    dst->nlink = 0;
    if (dst->vfs_refs == 0)
      free_node_locked(fs, dst);
  }

  unlink_child_locked(sp, src);
  size_t nl = strlen(dst_name);
  if (nl >= sizeof(src->name))
    nl = sizeof(src->name) - 1;
  memcpy(src->name, dst_name, nl);
  src->name[nl] = '\0';
  link_child_locked(dp, src);

  int64_t now = rtc_get_epoch();
  src->mtime = now;
  sp->mtime = now;
  dp->mtime = now;

  mutex_unlock(&fs->lock);
  return 0;
}

static int tmpfs_symlink_op(vfs_node_t *dir_vn, const char *name,
                            const char *target) {
  tmpfs_node_t *dir = (tmpfs_node_t *)dir_vn->priv;
  if (!dir || !dir->fs || !name || !name[0] || !target)
    return -EINVAL;
  if ((dir->mode & S_IFMT) != S_IFDIR)
    return -ENOTDIR;
  size_t tlen = strlen(target);
  if (tlen == 0 || tlen >= VFS_PATH_MAX)
    return -EINVAL;

  tmpfs_fs_t *fs = dir->fs;
  mutex_lock(&fs->lock);

  if (lookup_child_locked(dir, name)) {
    mutex_unlock(&fs->lock);
    return -EEXIST;
  }
  if (fs->used_bytes + tlen > fs->max_bytes) {
    mutex_unlock(&fs->lock);
    return -ENOSPC;
  }

  tmpfs_node_t *n = (tmpfs_node_t *)kzalloc(sizeof(*n));
  if (!n) {
    mutex_unlock(&fs->lock);
    return -ENOMEM;
  }
  size_t nl = strlen(name);
  memcpy(n->name, name, nl);
  n->name[nl] = '\0';
  n->mode = S_IFLNK | 0777;
  n->uid = 0;
  n->gid = 0;
  n->mtime = rtc_get_epoch();
  n->inode = fs->next_inode++;
  n->nlink = 1;
  n->fs = fs;

  n->data = (uint8_t *)kmalloc(tlen);
  if (!n->data) {
    kfree(n);
    mutex_unlock(&fs->lock);
    return -ENOMEM;
  }
  memcpy(n->data, target, tlen);
  n->size = tlen;
  n->capacity = tlen;
  fs->used_bytes += tlen;

  link_child_locked(dir, n);
  dir->mtime = n->mtime;

  mutex_unlock(&fs->lock);
  return 0;
}

static int tmpfs_readdir_op(vfs_node_t *dir_vn, uint64_t index,
                            vfs_dirent_t *out) {
  tmpfs_node_t *dir = (tmpfs_node_t *)dir_vn->priv;
  if (!dir || !dir->fs || !out)
    return -EINVAL;
  if ((dir->mode & S_IFMT) != S_IFDIR)
    return -ENOTDIR;

  tmpfs_fs_t *fs = dir->fs;
  mutex_lock(&fs->lock);

  tmpfs_node_t *c = dir->children_head;
  for (uint64_t i = 0; c && i < index; i++)
    c = c->sibling_next;

  if (!c) {
    mutex_unlock(&fs->lock);
    out->name[0] = '\0';
    out->type = 0;
    out->size = 0;
    return 0;
  }

  size_t nl = strlen(c->name);
  if (nl >= sizeof(out->name))
    nl = sizeof(out->name) - 1;
  memcpy(out->name, c->name, nl);
  out->name[nl] = '\0';
  out->type = ((c->mode & S_IFMT) == S_IFDIR) ? VFS_DIRECTORY : VFS_FILE;
  out->size = (out->type == VFS_FILE) ? c->size : 0;

  mutex_unlock(&fs->lock);
  return 0;
}

static int tmpfs_utimes_op(vfs_node_t *vn, int64_t mtime_sec) {
  tmpfs_node_t *n = (tmpfs_node_t *)vn->priv;
  if (!n || !n->fs)
    return -EINVAL;
  tmpfs_fs_t *fs = n->fs;
  mutex_lock(&fs->lock);
  n->mtime = mtime_sec;
  vn->mtime_sec = mtime_sec;
  mutex_unlock(&fs->lock);
  return 0;
}

static int tmpfs_chmod_op(vfs_node_t *vn, uint32_t mode) {
  tmpfs_node_t *n = (tmpfs_node_t *)vn->priv;
  if (!n || !n->fs)
    return -EINVAL;
  tmpfs_fs_t *fs = n->fs;
  mutex_lock(&fs->lock);
  n->mode = (n->mode & S_IFMT) | (mode & 07777);
  vn->mode = n->mode;
  mutex_unlock(&fs->lock);
  return 0;
}

static int tmpfs_chown_op(vfs_node_t *vn, uint32_t uid, uint32_t gid) {
  tmpfs_node_t *n = (tmpfs_node_t *)vn->priv;
  if (!n || !n->fs)
    return -EINVAL;
  tmpfs_fs_t *fs = n->fs;
  mutex_lock(&fs->lock);
  if (uid != (uint32_t)-1)
    n->uid = uid;
  if (gid != (uint32_t)-1)
    n->gid = gid;
  vn->uid = n->uid;
  vn->gid = n->gid;
  mutex_unlock(&fs->lock);
  return 0;
}

// ---------------------------------------------------------------------------
// Tablas de ops
// ---------------------------------------------------------------------------
static vfs_ops_t tmpfs_file_ops = {
    .read = tmpfs_read_op,
    .write = tmpfs_write_op,
    .open = tmpfs_open_op,
    .close = tmpfs_close_op,
    .truncate = tmpfs_truncate_op,
    .utimes = tmpfs_utimes_op,
    .chmod = tmpfs_chmod_op,
    .chown = tmpfs_chown_op,
};

static vfs_ops_t tmpfs_dir_ops = {
    .read = tmpfs_read_op,
    .open = tmpfs_open_op,
    .close = tmpfs_close_op,
    .create = tmpfs_create_op,
    .mkdir = tmpfs_mkdir_op,
    .unlink = tmpfs_unlink_op,
    .rename = tmpfs_rename_op,
    .symlink = tmpfs_symlink_op,
    .readdir = tmpfs_readdir_op,
    .utimes = tmpfs_utimes_op,
    .chmod = tmpfs_chmod_op,
    .chown = tmpfs_chown_op,
};

// ---------------------------------------------------------------------------
// fs lookup
// ---------------------------------------------------------------------------
static vfs_node_t *tmpfs_lookup_impl(void *fs_priv, const char *path) {
  tmpfs_fs_t *fs = (tmpfs_fs_t *)fs_priv;
  if (!fs || !path)
    return NULL;

  mutex_lock(&fs->lock);

  tmpfs_node_t *cur = fs->root;
  const char *p = path;
  while (*p == '/')
    p++;

  while (*p) {
    const char *seg = p;
    while (*p && *p != '/')
      p++;
    size_t seg_len = (size_t)(p - seg);
    while (*p == '/')
      p++;

    if (seg_len == 0)
      continue;
    if (seg_len >= VFS_PATH_MAX) {
      mutex_unlock(&fs->lock);
      return NULL;
    }
    if ((cur->mode & S_IFMT) != S_IFDIR) {
      mutex_unlock(&fs->lock);
      return NULL;
    }
    char name[VFS_PATH_MAX];
    memcpy(name, seg, seg_len);
    name[seg_len] = '\0';

    tmpfs_node_t *child = lookup_child_locked(cur, name);
    if (!child) {
      mutex_unlock(&fs->lock);
      return NULL;
    }
    cur = child;
  }

  vfs_node_t *vn = wrap_locked(cur);
  mutex_unlock(&fs->lock);
  return vn;
}

static int tmpfs_statfs_impl(void *fs_priv, struct vfs_statfs *out) {
  tmpfs_fs_t *fs = (tmpfs_fs_t *)fs_priv;
  if (!fs || !out)
    return -EINVAL;
  memset(out, 0, sizeof(*out));
  mutex_lock(&fs->lock);
  out->f_type = 0x01021994; // TMPFS_MAGIC
  out->f_bsize = 4096;
  out->f_frsize = 4096;
  out->f_blocks = fs->max_bytes / 4096;
  out->f_bfree = (fs->max_bytes - fs->used_bytes) / 4096;
  out->f_bavail = out->f_bfree;
  out->f_files = 0;
  out->f_ffree = 0;
  out->f_namelen = VFS_PATH_MAX - 1;
  mutex_unlock(&fs->lock);
  return 0;
}

static vfs_fs_ops_t tmpfs_fs_ops = {
    .lookup = tmpfs_lookup_impl,
    .statfs = tmpfs_statfs_impl,
    .name = "tmpfs",
};

struct vfs_fs_ops *tmpfs_get_vfs_ops(void) {
  return (struct vfs_fs_ops *)&tmpfs_fs_ops;
}

// ---------------------------------------------------------------------------
// API publica
// ---------------------------------------------------------------------------
void *tmpfs_new(size_t max_bytes) {
  tmpfs_fs_t *fs = (tmpfs_fs_t *)kzalloc(sizeof(*fs));
  if (!fs)
    return NULL;
  mutex_init(&fs->lock);
  fs->max_bytes = max_bytes;
  fs->used_bytes = 0;
  fs->next_inode = 2;

  tmpfs_node_t *root = (tmpfs_node_t *)kzalloc(sizeof(*root));
  if (!root) {
    kfree(fs);
    return NULL;
  }
  root->name[0] = '\0';
  root->mode = S_IFDIR | 0755;
  root->uid = 0;
  root->gid = 0;
  root->mtime = rtc_get_epoch();
  root->inode = 1;
  root->nlink = 1;
  root->fs = fs;
  fs->root = root;

  return fs;
}

static void free_tree_recursive(tmpfs_fs_t *fs, tmpfs_node_t *n) {
  if (!n)
    return;
  if ((n->mode & S_IFMT) == S_IFDIR) {
    tmpfs_node_t *c = n->children_head;
    while (c) {
      tmpfs_node_t *next = c->sibling_next;
      free_tree_recursive(fs, c);
      c = next;
    }
  }
  if (n->data)
    kfree(n->data);
  kfree(n);
}

void tmpfs_destroy(void *priv) {
  tmpfs_fs_t *fs = (tmpfs_fs_t *)priv;
  if (!fs)
    return;
  free_tree_recursive(fs, fs->root);
  kfree(fs);
}

size_t tmpfs_bytes_used(void *priv) {
  tmpfs_fs_t *fs = (tmpfs_fs_t *)priv;
  if (!fs)
    return 0;
  mutex_lock(&fs->lock);
  size_t u = fs->used_bytes;
  mutex_unlock(&fs->lock);
  return u;
}

// ---------------------------------------------------------------------------
// Helpers de pre-poblacion (solo usados por tmpfs_init)
// ---------------------------------------------------------------------------
static char *tmpfs_read_kernel_file(const char *path, size_t *out_len) {
  vfs_node_t *n = vfs_lookup(path);
  if (!n)
    return NULL;
  if (!n->ops || !n->ops->read) {
    vfs_node_free(n);
    return NULL;
  }
  size_t sz = n->size;
  if (sz == 0 || sz > 16 * 1024 * 1024) {
    vfs_node_free(n);
    return NULL;
  }
  char *buf = (char *)kmalloc(sz + 1);
  if (!buf) {
    vfs_node_free(n);
    return NULL;
  }
  int64_t r = n->ops->read(n, 0, sz, buf);
  vfs_node_free(n);
  if (r != (int64_t)sz) {
    kfree(buf);
    return NULL;
  }
  buf[sz] = '\0';
  *out_len = sz;
  return buf;
}

static int tmpfs_populate_file(const char *path, const void *data, size_t len) {
  int rc = vfs_create(path, 0644);
  if (rc != 0 && rc != -EEXIST)
    return rc;
  vfs_node_t *n = vfs_lookup(path);
  if (!n)
    return -ENOENT;
  if (!n->ops || !n->ops->write) {
    vfs_node_free(n);
    return -EINVAL;
  }
  int64_t w = n->ops->write(n, 0, len, data);
  vfs_node_free(n);
  return (w < 0) ? (int)w : 0;
}

#define TMPFS_PUT(path, str) tmpfs_populate_file(path, str, strlen(str))

// ---------------------------------------------------------------------------
// tmpfs_init: monta /etc, /tmp, /var, /run y pre-popula /etc.
// ---------------------------------------------------------------------------
void tmpfs_init(void) {
  // ---------------------------------------------------------------------
  // [1] Leer de tarfs lo inmutable que debe sobrevivir al mount tmpfs.
  //
  // Hoy solo /etc/ld.so.cache (generado en build time por ldconfig).
  // Sin esto, glibc no encuentra el cache y hace probing por cada lib
  // (visible en el serial como OPEN-FAIL en cascada).
  // ---------------------------------------------------------------------
  size_t cache_len = 0;
  char *cache_buf = tmpfs_read_kernel_file("/etc/ld.so.cache", &cache_len);

  // Alternativa por si el usuario deja un /etc/hosts custom en tarfs.
  size_t hosts_len = 0;
  char *hosts_buf = tmpfs_read_kernel_file("/etc/hosts", &hosts_len);

  // ---------------------------------------------------------------------
  // [2] /etc
  // ---------------------------------------------------------------------
  void *etc = tmpfs_new(4 * 1024 * 1024);
  if (!etc) {
    LOG_ERR("[TMPFS] sin memoria para /etc");
  } else {
    int rc = vfs_mount("/etc", tmpfs_get_vfs_ops(), etc);
    if (rc != 0) {
      LOG_ERR("[TMPFS] mount /etc: %d", rc);
      tmpfs_destroy(etc);
    } else {
      TMPFS_PUT("/etc/passwd", "root:x:0:0:root:/root:/bin/sh\n");
      TMPFS_PUT("/etc/group", "root:x:0:\n");
      if (hosts_buf && hosts_len > 0)
        tmpfs_populate_file("/etc/hosts", hosts_buf, hosts_len);
      else
        TMPFS_PUT("/etc/hosts", "127.0.0.1\tlocalhost\n");
      TMPFS_PUT("/etc/resolv.conf", "");
      TMPFS_PUT("/etc/profile", "# /etc/profile\n");
      if (cache_buf && cache_len > 0)
        tmpfs_populate_file("/etc/ld.so.cache", cache_buf, cache_len);
      LOG_INFO("[TMPFS] /etc montado (writable, ld.so.cache %lu B)",
               (unsigned long)cache_len);
    }
  }
  if (cache_buf)
    kfree(cache_buf);
  if (hosts_buf)
    kfree(hosts_buf);

  // ---------------------------------------------------------------------
  // [3] /tmp
  // ---------------------------------------------------------------------
  void *tmp = tmpfs_new(64 * 1024 * 1024);
  if (!tmp) {
    LOG_ERR("[TMPFS] sin memoria para /tmp");
  } else {
    int rc = vfs_mount("/tmp", tmpfs_get_vfs_ops(), tmp);
    if (rc != 0) {
      LOG_ERR("[TMPFS] mount /tmp: %d", rc);
      tmpfs_destroy(tmp);
    } else {
      LOG_INFO("[TMPFS] /tmp montado (64 MB)");
    }
  }

  // ---------------------------------------------------------------------
  // [4] /var + subdirs estandar
  // ---------------------------------------------------------------------
  void *var = tmpfs_new(16 * 1024 * 1024);
  if (!var) {
    LOG_ERR("[TMPFS] sin memoria para /var");
  } else {
    int rc = vfs_mount("/var", tmpfs_get_vfs_ops(), var);
    if (rc != 0) {
      LOG_ERR("[TMPFS] mount /var: %d", rc);
      tmpfs_destroy(var);
    } else {
      vfs_mkdir("/var/tmp", 01777);
      vfs_mkdir("/var/log", 0755);
      vfs_mkdir("/var/run", 0755);
      LOG_INFO("[TMPFS] /var montado (16 MB)");
    }
  }

  // ---------------------------------------------------------------------
  // [5] /run
  // ---------------------------------------------------------------------
  void *run = tmpfs_new(4 * 1024 * 1024);
  if (!run) {
    LOG_ERR("[TMPFS] sin memoria para /run");
  } else {
    int rc = vfs_mount("/run", tmpfs_get_vfs_ops(), run);
    if (rc != 0) {
      LOG_ERR("[TMPFS] mount /run: %d", rc);
      tmpfs_destroy(run);
    } else {
      LOG_INFO("[TMPFS] /run montado (4 MB)");
    }
  }
}