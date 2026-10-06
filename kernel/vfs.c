// kernel/vfs.c
//
// VFS genérico: mount table plana, lookup recursivo, symlinks,
// namespace ops, devfs, fds por proceso. NO conoce implementaciones
// concretas de FS; cada FS provee sus vfs_fs_ops y las registra con
// vfs_mount().
//
// tarfs, fat32, devfs y futuros FS viven en sus propios ficheros.
// vfs_init() solo monta tarfs en / y devfs en /dev, y delega todo el
// resto.

#include "vfs.h"
#include "block.h" // [FIX] devfs: blk_lookup, blk_count, blk_get_by_index
#include "cpu.h"
#include "gfx/winsrv.h"
#include "heap.h"
#include "klog.h"
#include "mutex.h"
#include "process.h"
#include "procfs.h"
#include "pty.h"
#include "rtc.h"
#include "serial.h"
#include "spinlock.h"
#include "string.h"
#include "sysfs.h"
#include "tarfs.h" // solo por tarfs_get_vfs_ops()
#include "tty.h"
#include "uaccess.h"
#include <stddef.h>

// ===========================================================================
// Consola: stdin/stdout/stderr
//
// El nodo consola delega TODO en tty_console(). La lógica de blocking,
// ring buffer, canonical, echo, ioctl y poll vive en tty.c.
// ===========================================================================
static int64_t console_vfs_read(vfs_node_t *node, uint64_t offset, size_t size,
                                void *buf) {
  (void)node;
  return tty_read(tty_console(), offset, size, buf);
}

static int64_t console_vfs_write(vfs_node_t *node, uint64_t offset, size_t size,
                                 const void *buf) {
  (void)node;
  return tty_write(tty_console(), offset, size, buf);
}

static int console_vfs_poll(vfs_node_t *node, short events) {
  (void)node;
  return tty_poll(tty_console(), events);
}

static int64_t console_vfs_ioctl(vfs_node_t *node, unsigned long req,
                                 uint64_t arg) {
  (void)node;
  return tty_ioctl(tty_console(), req, arg);
}

static vfs_ops_t console_ops = {
    .read = console_vfs_read,
    .write = console_vfs_write,
    .open = NULL,
    .close = NULL,
    .readable = NULL,
    .poll = console_vfs_poll,
    .ioctl = console_vfs_ioctl,
};

static vfs_node_t stdin_node;
static vfs_node_t stdout_node;
static vfs_node_t stderr_node;

// ===========================================================================
// Mount table
// ===========================================================================
struct vfs_mount {
  char path[VFS_PATH_MAX];
  vfs_fs_ops_t *ops;
  void *fs_priv;
  int is_bind;
  char bind_source[VFS_PATH_MAX];
  struct vfs_mount *next;
};

static struct vfs_mount *g_mounts = NULL;
static spinlock_t g_mounts_lock;

// ===========================================================================
// Normalización de paths
//
// Colapsa //, elimina ".", resuelve ".." subiendo un nivel (sin pasar
// de "/"). Resultado siempre empieza por '/', no termina por '/' salvo
// que sea exactamente "/".
// ===========================================================================
static int normalize_path(const char *in, char *out, size_t outlen) {
  if (!in || !out || outlen < 2)
    return -EINVAL;

  const char *p = in;
  while (*p == '/')
    p++;

  size_t o = 0;
  out[o++] = '/';

  while (*p) {
    const char *seg = p;
    while (*p && *p != '/')
      p++;
    size_t seglen = (size_t)(p - seg);
    while (*p == '/')
      p++;

    if (seglen == 0)
      continue;
    if (seglen == 1 && seg[0] == '.')
      continue;
    if (seglen == 2 && seg[0] == '.' && seg[1] == '.') {
      while (o > 1 && out[o - 1] != '/')
        o--;
      if (o > 1)
        o--;
      else
        o = 1;
      continue;
    }

    if (o > 1) {
      if (o + 1 >= outlen)
        return -ENAMETOOLONG;
      out[o++] = '/';
    }
    if (o + seglen >= outlen)
      return -ENAMETOOLONG;
    for (size_t i = 0; i < seglen; i++)
      out[o++] = seg[i];
  }
  out[o] = '\0';
  return 0;
}

int vfs_resolve_path(const char *cwd, const char *in, char *out,
                     size_t outlen) {
  if (!in || !in[0] || !out || outlen < 2)
    return -EINVAL;

  if (in[0] == '/')
    return normalize_path(in, out, outlen);

  const char *base = (cwd && cwd[0]) ? cwd : "/";
  size_t blen = strlen(base);
  size_t ilen = strlen(in);
  char tmp[VFS_PATH_MAX * 2];
  if (blen + 1 + ilen >= sizeof(tmp))
    return -ENAMETOOLONG;
  for (size_t i = 0; i < blen; i++)
    tmp[i] = base[i];
  tmp[blen] = '/';
  for (size_t i = 0; i < ilen; i++)
    tmp[blen + 1 + i] = in[i];
  tmp[blen + 1 + ilen] = '\0';
  return normalize_path(tmp, out, outlen);
}

// ===========================================================================
// Mount / umount
// ===========================================================================
int vfs_mount(const char *path, vfs_fs_ops_t *ops, void *fs_priv) {
  if (!path || !ops)
    return -EINVAL;

  char norm[VFS_PATH_MAX];
  int rc = normalize_path(path, norm, sizeof(norm));
  if (rc != 0)
    return rc;

  unsigned long flags = spin_lock_irqsave(&g_mounts_lock);

  for (struct vfs_mount *m = g_mounts; m; m = m->next) {
    if (strcmp(m->path, norm) == 0) {
      spin_unlock_irqrestore(&g_mounts_lock, flags);
      return -EEXIST;
    }
  }

  struct vfs_mount *m = (struct vfs_mount *)kzalloc(sizeof(*m));
  if (!m) {
    spin_unlock_irqrestore(&g_mounts_lock, flags);
    return -ENOMEM;
  }

  size_t plen = strlen(norm);
  for (size_t i = 0; i < plen; i++)
    m->path[i] = norm[i];
  m->path[plen] = '\0';
  m->ops = ops;
  m->fs_priv = fs_priv;
  m->next = g_mounts;
  g_mounts = m;

  spin_unlock_irqrestore(&g_mounts_lock, flags);

  LOG_INFO("[VFS] mount '%s' -> %s", norm, ops->name ? ops->name : "?");
  return 0;
}

int vfs_mount_bind(const char *mount_path, const char *source_path) {
  if (!mount_path || !source_path)
    return -EINVAL;

  char norm_mount[VFS_PATH_MAX];
  char norm_source[VFS_PATH_MAX];
  if (normalize_path(mount_path, norm_mount, sizeof(norm_mount)) != 0)
    return -EINVAL;
  if (normalize_path(source_path, norm_source, sizeof(norm_source)) != 0)
    return -EINVAL;

  vfs_node_t *src = vfs_lookup(norm_source);
  if (!src)
    return -ENOENT;
  if (!(src->flags & VFS_DIRECTORY)) {
    vfs_node_free(src);
    return -ENOTDIR;
  }
  vfs_node_free(src);

  unsigned long flags = spin_lock_irqsave(&g_mounts_lock);
  for (struct vfs_mount *m = g_mounts; m; m = m->next) {
    if (strcmp(m->path, norm_mount) == 0) {
      spin_unlock_irqrestore(&g_mounts_lock, flags);
      return -EEXIST;
    }
  }

  struct vfs_mount *m = (struct vfs_mount *)kzalloc(sizeof(*m));
  if (!m) {
    spin_unlock_irqrestore(&g_mounts_lock, flags);
    return -ENOMEM;
  }
  size_t ml = strlen(norm_mount);
  for (size_t i = 0; i < ml; i++)
    m->path[i] = norm_mount[i];
  m->path[ml] = '\0';
  m->is_bind = 1;
  size_t sl = strlen(norm_source);
  for (size_t i = 0; i < sl; i++)
    m->bind_source[i] = norm_source[i];
  m->bind_source[sl] = '\0';
  m->next = g_mounts;
  g_mounts = m;

  spin_unlock_irqrestore(&g_mounts_lock, flags);

  LOG_INFO("[VFS] bind mount '%s' -> '%s'", norm_mount, norm_source);
  return 0;
}

int vfs_umount(const char *path) {
  if (!path)
    return -EINVAL;

  char norm[VFS_PATH_MAX];
  int rc = normalize_path(path, norm, sizeof(norm));
  if (rc != 0)
    return rc;

  unsigned long flags = spin_lock_irqsave(&g_mounts_lock);

  size_t nlen = strlen(norm);

  for (struct vfs_mount *m = g_mounts; m; m = m->next) {
    if (strcmp(m->path, norm) == 0)
      continue;
    if (norm[0] == '/' && nlen == 1) {
      if (m->path[0] == '/' && m->path[1] != '\0') {
        spin_unlock_irqrestore(&g_mounts_lock, flags);
        return -EBUSY;
      }
    } else if (strncmp(m->path, norm, nlen) == 0 && m->path[nlen] == '/') {
      spin_unlock_irqrestore(&g_mounts_lock, flags);
      return -EBUSY;
    }
  }

  struct vfs_mount **pp = &g_mounts;
  while (*pp) {
    if (strcmp((*pp)->path, norm) == 0) {
      struct vfs_mount *victim = *pp;
      *pp = victim->next;
      spin_unlock_irqrestore(&g_mounts_lock, flags);
      LOG_INFO("[VFS] umount '%s'", norm);
      kfree(victim);
      return 0;
    }
    pp = &(*pp)->next;
  }

  spin_unlock_irqrestore(&g_mounts_lock, flags);
  return -ENOENT;
}

// ---------------------------------------------------------------------------
// [2.2] pivot_root.
//
// Reescribe la tabla de mounts: el mount exacto en new_root pasa a '/',
// los mounts bajo new_root pierden el prefijo, el antiguo '/' se mueve
// a put_old, y el resto de mounts se reubican bajo put_old + path.
//
// No toca inodos, fds, ni cwd del llamante: el árbol se reconstruye
// en cada vfs_lookup().
// ---------------------------------------------------------------------------
int vfs_pivot_root(const char *new_root, const char *put_old) {
  if (!new_root || !put_old)
    return -EINVAL;

  char nr[VFS_PATH_MAX];
  char po[VFS_PATH_MAX];
  int rc = normalize_path(new_root, nr, sizeof(nr));
  if (rc != 0)
    return rc;
  rc = normalize_path(put_old, po, sizeof(po));
  if (rc != 0)
    return rc;

  if (strcmp(nr, "/") == 0)
    return -EINVAL;

  size_t nrlen = strlen(nr);
  if (strncmp(po, nr, nrlen) != 0)
    return -EINVAL;
  if (po[nrlen] != '/')
    return -EINVAL;

  {
    unsigned long lf = spin_lock_irqsave(&g_mounts_lock);
    struct vfs_mount *found = NULL;
    for (struct vfs_mount *m = g_mounts; m; m = m->next) {
      if (strcmp(m->path, nr) == 0) {
        found = m;
        break;
      }
    }
    spin_unlock_irqrestore(&g_mounts_lock, lf);
    if (!found)
      return -EINVAL;
  }

  vfs_node_t *po_node = vfs_lookup(po);
  if (!po_node)
    return -ENOENT;
  if (!(po_node->flags & VFS_DIRECTORY)) {
    vfs_node_free(po_node);
    return -ENOTDIR;
  }
  vfs_node_free(po_node);

  const char *po_rel = po + nrlen;

  unsigned long flags = spin_lock_irqsave(&g_mounts_lock);
  for (struct vfs_mount *m = g_mounts; m; m = m->next) {
    if (strcmp(m->path, nr) == 0) {
      m->path[0] = '/';
      m->path[1] = '\0';
    } else if (strncmp(m->path, nr, nrlen) == 0 && m->path[nrlen] == '/') {
      const char *suffix = m->path + nrlen;
      size_t slen = strlen(suffix);
      for (size_t i = 0; i <= slen; i++)
        m->path[i] = suffix[i];
    } else if (strcmp(m->path, "/") == 0) {
      size_t polen = strlen(po_rel);
      if (polen >= sizeof(m->path)) {
        spin_unlock_irqrestore(&g_mounts_lock, flags);
        return -ENAMETOOLONG;
      }
      for (size_t i = 0; i <= polen; i++)
        m->path[i] = po_rel[i];
    } else {
      char newpath[VFS_PATH_MAX];
      size_t polen = strlen(po_rel);
      size_t mplen = strlen(m->path);
      if (polen + mplen + 1 > sizeof(newpath)) {
        spin_unlock_irqrestore(&g_mounts_lock, flags);
        return -ENAMETOOLONG;
      }
      memcpy(newpath, po_rel, polen);
      memcpy(newpath + polen, m->path, mplen + 1);
      memcpy(m->path, newpath, sizeof(m->path));
    }
  }
  spin_unlock_irqrestore(&g_mounts_lock, flags);

  LOG_INFO("[VFS] pivot_root('%s', '%s') OK", nr, po);
  return 0;
}

struct vfs_fs_ops *vfs_get_mount_ops(const char *path) {
  if (!path)
    return NULL;
  char norm[VFS_PATH_MAX];
  if (normalize_path(path, norm, sizeof(norm)) != 0)
    return NULL;

  unsigned long flags = spin_lock_irqsave(&g_mounts_lock);
  struct vfs_mount *best = NULL;
  size_t best_len = 0;
  size_t norm_len = strlen(norm);
  for (struct vfs_mount *m = g_mounts; m; m = m->next) {
    size_t ml = strlen(m->path);
    if (ml > best_len) {
      int match = 0;
      if (ml == 1 && m->path[0] == '/')
        match = 1;
      else if (ml <= norm_len && strncmp(norm, m->path, ml) == 0) {
        if (norm[ml] == '\0' || norm[ml] == '/')
          match = 1;
      }
      if (match) {
        best = m;
        best_len = ml;
      }
    }
  }
  struct vfs_fs_ops *ret = best ? best->ops : NULL;
  spin_unlock_irqrestore(&g_mounts_lock, flags);
  return ret;
}

void *vfs_get_mount_priv(const char *path) {
  if (!path)
    return NULL;
  char norm[VFS_PATH_MAX];
  if (normalize_path(path, norm, sizeof(norm)) != 0)
    return NULL;

  unsigned long flags = spin_lock_irqsave(&g_mounts_lock);
  void *ret = NULL;
  for (struct vfs_mount *m = g_mounts; m; m = m->next) {
    if (strcmp(m->path, norm) == 0) {
      ret = m->fs_priv;
      break;
    }
  }
  spin_unlock_irqrestore(&g_mounts_lock, flags);
  return ret;
}

// ===========================================================================
// Lookup
// ===========================================================================

// Resuelve el target de un symlink a un path absoluto normalizado.
//   - `symlink_path`: path normalizado del propio symlink.
//   - `target`: contenido del symlink (absoluto o relativo).
static int resolve_symlink_target(const char *symlink_path, const char *target,
                                  char *out, size_t outlen) {
  if (!target || !target[0])
    return -EINVAL;

  if (target[0] == '/')
    return normalize_path(target, out, outlen);

  char dir[VFS_PATH_MAX];
  const char *last_slash = NULL;
  for (const char *p = symlink_path; *p; p++)
    if (*p == '/')
      last_slash = p;
  if (!last_slash) {
    dir[0] = '/';
    dir[1] = '\0';
  } else {
    size_t dlen = (size_t)(last_slash - symlink_path);
    if (dlen == 0)
      dlen = 1;
    if (dlen >= sizeof(dir))
      return -ENAMETOOLONG;
    memcpy(dir, symlink_path, dlen);
    dir[dlen] = '\0';
  }

  char tmp[VFS_PATH_MAX * 2];
  size_t dl = strlen(dir);
  size_t tl = strlen(target);
  if (dl + 1 + tl + 1 > sizeof(tmp))
    return -ENAMETOOLONG;
  memcpy(tmp, dir, dl);
  tmp[dl] = '/';
  memcpy(tmp + dl + 1, target, tl + 1);
  return normalize_path(tmp, out, outlen);
}

// Recorre la mount table buscando el mount más específico, delega el
// path relativo al FS, y sigue symlinks si no estamos en no_follow.
static vfs_node_t *vfs_lookup_rec(const char *path, int depth, int no_follow) {
  if (depth > 8)
    return NULL;
  if (!path)
    return NULL;

  char norm[VFS_PATH_MAX];
  if (normalize_path(path, norm, sizeof(norm)) != 0)
    return NULL;

  unsigned long flags = spin_lock_irqsave(&g_mounts_lock);
  struct vfs_mount *best = NULL;
  size_t best_len = 0;
  size_t norm_len = strlen(norm);

  for (struct vfs_mount *m = g_mounts; m; m = m->next) {
    size_t ml = strlen(m->path);
    if (ml > best_len) {
      int match = 0;
      if (ml == 1 && m->path[0] == '/') {
        match = 1;
      } else if (ml <= norm_len && strncmp(norm, m->path, ml) == 0) {
        if (norm[ml] == '\0' || norm[ml] == '/')
          match = 1;
      }
      if (match) {
        best = m;
        best_len = ml;
      }
    }
  }

  if (!best) {
    spin_unlock_irqrestore(&g_mounts_lock, flags);
    return NULL;
  }

  int is_bind = best->is_bind;
  char bind_source[VFS_PATH_MAX];
  if (is_bind) {
    memcpy(bind_source, best->bind_source, sizeof(bind_source));
    bind_source[sizeof(bind_source) - 1] = '\0';
  }
  vfs_fs_ops_t *ops = best->ops;
  void *fs_priv = best->fs_priv;
  char mount_path[VFS_PATH_MAX];
  memcpy(mount_path, best->path, sizeof(mount_path));
  mount_path[sizeof(mount_path) - 1] = '\0';
  bool mount_is_root = (best_len == 1 && mount_path[0] == '/');
  spin_unlock_irqrestore(&g_mounts_lock, flags);

  if (is_bind) {
    size_t src_len = strlen(bind_source);
    size_t suffix_len = norm_len - best_len;
    if (src_len + suffix_len + 1 > sizeof(bind_source))
      return NULL;
    char new_path[VFS_PATH_MAX];
    memcpy(new_path, bind_source, src_len);
    memcpy(new_path + src_len, norm + best_len, suffix_len);
    new_path[src_len + suffix_len] = '\0';
    return vfs_lookup_rec(new_path, depth + 1, no_follow);
  }

  const char *rel;
  if (mount_is_root) {
    rel = norm;
  } else {
    rel = norm + best_len;
    if (*rel == '\0')
      rel = "/";
  }

  vfs_node_t *node = NULL;
  if (ops->lookup)
    node = ops->lookup(fs_priv, rel);

  // [3.3.c] El nodo debe llevar el path COMPLETO (visto por el usuario)
  // en node->name, no el relativo al mount. k_getdents64 lo necesita
  // para mergear mounts hijas sin volver a hacer lookup.
  if (node) {
    size_t plen = strlen(norm);
    if (plen >= sizeof(node->name))
      plen = sizeof(node->name) - 1;
    memcpy(node->name, norm, plen);
    node->name[plen] = '\0';
  }

  if (!node) {
    if (strcmp(norm, "/") == 0) {
      vfs_node_t *root = (vfs_node_t *)kzalloc(sizeof(vfs_node_t));
      if (!root)
        return NULL;
      root->name[0] = '/';
      root->name[1] = '\0';
      root->flags = VFS_DIRECTORY;
      return root;
    }
    return NULL;
  }

  // [3.1] Symlink: seguir si no estamos en no-follow.
  if (node->is_symlink && !no_follow) {
    char target_path[VFS_PATH_MAX];
    int rc = resolve_symlink_target(norm, node->link_target, target_path,
                                    sizeof(target_path));
    if (rc != 0) {
      vfs_node_free(node);
      return NULL;
    }
    vfs_node_free(node);
    return vfs_lookup_rec(target_path, depth + 1, no_follow);
  }

  return node;
}

vfs_node_t *vfs_lookup(const char *path) { return vfs_lookup_rec(path, 0, 0); }

vfs_node_t *vfs_lookup_nofollow(const char *path) {
  return vfs_lookup_rec(path, 0, 1);
}

// ===========================================================================
// Split de path normalizado en (dirname, basename).
// ===========================================================================
static int split_dirname(const char *path, char *dir_out, size_t dir_outlen,
                         char *base_out, size_t base_outlen) {
  if (!path || path[0] != '/' || !dir_out || !base_out)
    return -EINVAL;

  const char *last = NULL;
  for (const char *p = path; *p; p++) {
    if (*p == '/')
      last = p;
  }
  if (!last)
    return -EINVAL;

  const char *base_start = last + 1;
  if (*base_start == '\0')
    return -EINVAL;

  size_t blen = strlen(base_start);
  if (blen + 1 > base_outlen)
    return -ENAMETOOLONG;
  for (size_t i = 0; i < blen; i++)
    base_out[i] = base_start[i];
  base_out[blen] = '\0';

  size_t dlen = (size_t)(last - path);
  if (dlen == 0)
    dlen = 1;
  if (dlen + 1 > dir_outlen)
    return -ENAMETOOLONG;
  for (size_t i = 0; i < dlen; i++)
    dir_out[i] = path[i];
  dir_out[dlen] = '\0';

  return 0;
}

static vfs_node_t *resolve_parent(const char *path, char *base_out,
                                  size_t base_outlen, int *rc_out) {
  char norm[VFS_PATH_MAX];
  int rc = normalize_path(path, norm, sizeof(norm));
  if (rc != 0) {
    *rc_out = rc;
    return NULL;
  }

  if (strcmp(norm, "/") == 0) {
    *rc_out = -EINVAL;
    return NULL;
  }

  char dir_path[VFS_PATH_MAX];
  rc = split_dirname(norm, dir_path, sizeof(dir_path), base_out, base_outlen);
  if (rc != 0) {
    *rc_out = rc;
    return NULL;
  }

  vfs_node_t *parent = vfs_lookup(dir_path);
  if (!parent) {
    *rc_out = -ENOENT;
    return NULL;
  }
  if (!(parent->flags & VFS_DIRECTORY)) {
    vfs_node_free(parent);
    *rc_out = -ENOTDIR;
    return NULL;
  }
  *rc_out = 0;
  return parent;
}

// ===========================================================================
// Operaciones de namespace
// ===========================================================================
int vfs_create(const char *path, uint32_t mode) {
  if (!path)
    return -EINVAL;

  char base[VFS_PATH_MAX];
  int rc;
  vfs_node_t *parent = resolve_parent(path, base, sizeof(base), &rc);
  if (!parent)
    return rc;

  // [3.4.d] Requerir W+X en el directorio padre.
  int acc = vfs_check_access(parent, VFS_W_OK | VFS_X_OK);
  if (acc != 0) {
    vfs_node_free(parent);
    return acc;
  }

  if (!parent->ops || !parent->ops->create) {
    vfs_node_free(parent);
    return -EROFS;
  }
  rc = parent->ops->create(parent, base, mode & 07777);
  vfs_node_free(parent);
  return rc;
}

int vfs_mkdir(const char *path, uint32_t mode) {
  if (!path)
    return -EINVAL;

  char base[VFS_PATH_MAX];
  int rc;
  vfs_node_t *parent = resolve_parent(path, base, sizeof(base), &rc);
  if (!parent)
    return rc;

  // [3.4.d] Requerir W+X en el directorio padre.
  int acc = vfs_check_access(parent, VFS_W_OK | VFS_X_OK);
  if (acc != 0) {
    vfs_node_free(parent);
    return acc;
  }

  if (!parent->ops || !parent->ops->mkdir) {
    vfs_node_free(parent);
    return -EROFS;
  }
  rc = parent->ops->mkdir(parent, base, mode & 07777);
  vfs_node_free(parent);
  return rc;
}

int vfs_unlink(const char *path) {
  if (!path)
    return -EINVAL;

  char base[VFS_PATH_MAX];
  int rc;
  vfs_node_t *parent = resolve_parent(path, base, sizeof(base), &rc);
  if (!parent)
    return rc;

  // [3.4.d] Requerir W+X en el directorio padre.
  int acc = vfs_check_access(parent, VFS_W_OK | VFS_X_OK);
  if (acc != 0) {
    vfs_node_free(parent);
    return acc;
  }

  if (!parent->ops || !parent->ops->unlink) {
    vfs_node_free(parent);
    return -EROFS;
  }
  rc = parent->ops->unlink(parent, base);
  vfs_node_free(parent);
  return rc;
}

int vfs_truncate(const char *path, uint64_t new_size) {
  if (!path)
    return -EINVAL;

  vfs_node_t *node = vfs_lookup(path);
  if (!node)
    return -ENOENT;
  if (node->flags & VFS_DIRECTORY) {
    vfs_node_free(node);
    return -EISDIR;
  }
  if (!node->ops || !node->ops->truncate) {
    vfs_node_free(node);
    return -EROFS;
  }
  int rc = node->ops->truncate(node, new_size);
  vfs_node_free(node);
  return rc;
}

// [3.3.c] Recolecta los basenames de las mounts cuyo padre es `norm`.
// Devuelve el número escrito en out_names (max MAX_SYNTH_MOUNTS).
#define MAX_SYNTH_MOUNTS 16

static size_t collect_child_mounts(const char *norm,
                                   char out_names[][VFS_PATH_MAX], size_t max) {
  size_t count = 0;
  size_t nlen = strlen(norm);
  unsigned long flags = spin_lock_irqsave(&g_mounts_lock);
  for (struct vfs_mount *m = g_mounts; m && count < max; m = m->next) {
    const char *mp = m->path;
    const char *base = NULL;

    if (nlen == 1 && norm[0] == '/') {
      if (mp[0] == '/' && mp[1] != '\0')
        base = mp + 1;
    } else {
      if (strncmp(mp, norm, nlen) == 0 && mp[nlen] == '/')
        base = mp + nlen + 1;
    }
    if (!base || base[0] == '\0')
      continue;

    // Solo hijos DIRECTOS: sin '/' en `base`.
    int has_slash = 0;
    for (const char *p = base; *p; p++) {
      if (*p == '/') {
        has_slash = 1;
        break;
      }
    }
    if (has_slash)
      continue;

    size_t bl = strlen(base);
    if (bl >= VFS_PATH_MAX)
      bl = VFS_PATH_MAX - 1;
    memcpy(out_names[count], base, bl);
    out_names[count][bl] = '\0';
    count++;
  }
  spin_unlock_irqrestore(&g_mounts_lock, flags);
  return count;
}

// ---------------------------------------------------------------------------
// [4.1] vfs_statfs: busca el mount más específico y delega en fs->statfs.
// ---------------------------------------------------------------------------
int vfs_statfs(const char *path, struct vfs_statfs *out) {
  if (!path || !out)
    return -EINVAL;
  char norm[VFS_PATH_MAX];
  int rc = normalize_path(path, norm, sizeof(norm));
  if (rc != 0)
    return rc;

  unsigned long flags = spin_lock_irqsave(&g_mounts_lock);
  struct vfs_mount *best = NULL;
  size_t best_len = 0;
  size_t norm_len = strlen(norm);
  for (struct vfs_mount *m = g_mounts; m; m = m->next) {
    size_t ml = strlen(m->path);
    if (ml > best_len) {
      int match = 0;
      if (ml == 1 && m->path[0] == '/')
        match = 1;
      else if (ml <= norm_len && strncmp(norm, m->path, ml) == 0) {
        if (norm[ml] == '\0' || norm[ml] == '/')
          match = 1;
      }
      if (match) {
        best = m;
        best_len = ml;
      }
    }
  }
  if (!best) {
    spin_unlock_irqrestore(&g_mounts_lock, flags);
    return -ENOENT;
  }
  vfs_fs_ops_t *ops = best->ops;
  void *fs_priv = best->fs_priv;
  spin_unlock_irqrestore(&g_mounts_lock, flags);

  if (!ops || !ops->statfs)
    return -ENOSYS;
  return ops->statfs(fs_priv, out);
}

int vfs_readdir_node(vfs_node_t *node, uint64_t index, vfs_dirent_t *out) {
  if (!node || !out)
    return -EINVAL;
  if (!(node->flags & VFS_DIRECTORY))
    return -ENOTDIR;

  char mount_names[MAX_SYNTH_MOUNTS][VFS_PATH_MAX];
  size_t mount_count =
      collect_child_mounts(node->name, mount_names, MAX_SYNTH_MOUNTS);

  // Primero servimos los nombres de mounts sintéticas.
  if (index < mount_count) {
    size_t bl = strlen(mount_names[index]);
    if (bl >= sizeof(out->name))
      bl = sizeof(out->name) - 1;
    memcpy(out->name, mount_names[index], bl);
    out->name[bl] = '\0';
    out->type = VFS_DIRECTORY;
    out->size = 0;
    return 0;
  }

  // Si el FS no tiene readdir (p. ej. un dir de mount sin backing) y
  // ya pasamos los índices de mounts, EOF.
  if (!node->ops || !node->ops->readdir) {
    out->name[0] = '\0';
    out->type = 0;
    out->size = 0;
    return 0;
  }

  // Delegamos al FS saltando entries cuya basename coincida con una
  // mount (ya la servimos arriba). Búsqueda lineal — los directorios
  // tienen pocas entradas.
  uint64_t inner = index - mount_count;
  uint64_t seen = 0;
  uint64_t probe = 0;

  for (;;) {
    vfs_dirent_t e;
    int rc = node->ops->readdir(node, probe, &e);
    if (rc != 0)
      return rc;
    if (e.name[0] == '\0') {
      out->name[0] = '\0';
      out->type = 0;
      out->size = 0;
      return 0;
    }
    probe++;

    int shadowed = 0;
    for (size_t i = 0; i < mount_count; i++) {
      if (strcmp(e.name, mount_names[i]) == 0) {
        shadowed = 1;
        break;
      }
    }
    if (shadowed)
      continue;

    if (seen == inner) {
      *out = e;
      return 0;
    }
    seen++;
  }
}

int vfs_readdir(const char *path, uint64_t index, vfs_dirent_t *out) {
  if (!path || !out)
    return -EINVAL;
  vfs_node_t *node = vfs_lookup(path);
  if (!node)
    return -ENOENT;
  int rc = vfs_readdir_node(node, index, out);
  vfs_node_free(node);
  return rc;
}

int vfs_rename(const char *oldpath, const char *newpath) {
  if (!oldpath || !newpath)
    return -EINVAL;

  char src_base[VFS_PATH_MAX];
  char dst_base[VFS_PATH_MAX];
  int rc;

  vfs_node_t *src_parent =
      resolve_parent(oldpath, src_base, sizeof(src_base), &rc);
  if (!src_parent)
    return rc;

  vfs_node_t *dst_parent =
      resolve_parent(newpath, dst_base, sizeof(dst_base), &rc);
  if (!dst_parent) {
    vfs_node_free(src_parent);
    return rc;
  }

  if (src_parent->fs != dst_parent->fs) {
    vfs_node_free(src_parent);
    vfs_node_free(dst_parent);
    return -EXDEV;
  }

  if (!src_parent->ops || !src_parent->ops->rename) {
    vfs_node_free(src_parent);
    vfs_node_free(dst_parent);
    return -EROFS;
  }

  rc = src_parent->ops->rename(src_parent, src_base, dst_parent, dst_base);
  vfs_node_free(src_parent);
  vfs_node_free(dst_parent);
  return rc;
}

// ---------------------------------------------------------------------------
// [3.1] readlink / symlink.
// ---------------------------------------------------------------------------
int vfs_readlink(const char *path, char *buf, size_t bufsize) {
  if (!path || !buf || bufsize == 0)
    return -EINVAL;

  vfs_node_t *node = vfs_lookup_nofollow(path);
  if (!node)
    return -ENOENT;
  if (!node->is_symlink) {
    vfs_node_free(node);
    return -EINVAL;
  }

  int rc;
  if (node->ops && node->ops->readlink) {
    rc = node->ops->readlink(node, buf, bufsize);
  } else {
    size_t n = strlen(node->link_target);
    if (n >= bufsize)
      n = bufsize - 1;
    memcpy(buf, node->link_target, n);
    buf[n] = '\0';
    rc = (int)n;
  }
  vfs_node_free(node);
  return rc;
}

// ---------------------------------------------------------------------------
// [4.2] vfs_utimes: aplica mtime al nodo. Si el FS no lo soporta, 0.
// ---------------------------------------------------------------------------
int vfs_utimes(const char *path, int64_t mtime_sec) {
  if (!path)
    return -EINVAL;
  vfs_node_t *node = vfs_lookup_nofollow(path);
  if (!node) {
    return -ENOENT;
  }
  int rc = 0;
  if (node->ops && node->ops->utimes)
    rc = node->ops->utimes(node, mtime_sec);
  vfs_node_free(node);
  return rc;
}

int vfs_symlink(const char *target, const char *linkpath) {
  if (!target || !linkpath)
    return -EINVAL;

  char base[VFS_PATH_MAX];
  int rc;
  vfs_node_t *parent = resolve_parent(linkpath, base, sizeof(base), &rc);
  if (!parent)
    return rc;

  if (!parent->ops || !parent->ops->symlink) {
    vfs_node_free(parent);
    return -EROFS;
  }
  rc = parent->ops->symlink(parent, base, target);
  vfs_node_free(parent);
  return rc;
}

// ---------------------------------------------------------------------------
// [3.2] Hard link. Crea `newpath` apuntando al mismo inodo que `oldpath`.
//
// El FS destino debe implementar `.link`. Ningún FS de Aurora lo hace
// todavía, así que en la práctica siempre devuelve -EPERM/-EROFS. Pero
// la infraestructura queda lista para un ext2 o un tmpfs futuro.
// ---------------------------------------------------------------------------
int vfs_link(const char *oldpath, const char *newpath) {
  if (!oldpath || !newpath)
    return -EINVAL;

  char old_norm[VFS_PATH_MAX];
  char new_norm[VFS_PATH_MAX];
  if (normalize_path(oldpath, old_norm, sizeof(old_norm)) != 0)
    return -EINVAL;
  if (normalize_path(newpath, new_norm, sizeof(new_norm)) != 0)
    return -EINVAL;
  if (strcmp(old_norm, new_norm) == 0)
    return -EEXIST;

  vfs_node_t *target = vfs_lookup(old_norm);
  if (!target)
    return -ENOENT;
  if (target->flags & VFS_DIRECTORY) {
    vfs_node_free(target);
    return -EPERM; // Linux: EPERM para hard link a directorio
  }

  char new_base[VFS_PATH_MAX];
  int rc;
  vfs_node_t *parent =
      resolve_parent(new_norm, new_base, sizeof(new_base), &rc);
  if (!parent) {
    vfs_node_free(target);
    return rc;
  }

  // El FS destino decide. Sin `.link` → EPERM (como FAT32 en Linux).
  // Los FS RO (tarfs) implementan `.link` devolviendo -EROFS.
  if (!parent->ops || !parent->ops->link) {
    vfs_node_free(parent);
    vfs_node_free(target);
    return -EPERM;
  }

  rc = parent->ops->link(parent, new_base, target);
  vfs_node_free(parent);
  vfs_node_free(target);
  return rc;
}

// ---------------------------------------------------------------------------
// [3.4.c] chmod / chown.
//
// Comprobación de permisos contra el proceso actual:
//   - root (euid==0): puede todo.
//   - owner (euid == node->uid): puede cambiar bits 0777.
//   - nadie más.
//
// Los bits S_ISUID/S_ISGID solo los puede poner root (versión
// simplificada de Linux con CAP_FSETID). Un no-root que los tenga en
// un fichero puede quitárselos.
// ---------------------------------------------------------------------------
int vfs_chmod(const char *path, uint32_t mode) {
  process_t *proc = process_current();
  if (!proc)
    return -EPERM;

  vfs_node_t *node = vfs_lookup_nofollow(path);
  if (!node)
    return -ENOENT;

  int is_root = (proc->euid == 0);
  int is_owner = (proc->euid == node->uid);

  if (!is_root && !is_owner) {
    vfs_node_free(node);
    return -EPERM;
  }

  uint32_t filtered = mode & 07777;
  if (!is_root && (filtered & (S_ISUID | S_ISGID))) {
    // Un no-root solo puede mantener esos bits si ya los tenía.
    uint32_t already = node->mode & (S_ISUID | S_ISGID);
    if ((filtered & (S_ISUID | S_ISGID)) != already) {
      vfs_node_free(node);
      return -EPERM;
    }
  }

  if (!node->ops || !node->ops->chmod) {
    vfs_node_free(node);
    return -EROFS;
  }
  int rc = node->ops->chmod(node, filtered);
  if (rc == 0)
    node->mode = (node->mode & S_IFMT) | filtered;
  vfs_node_free(node);
  return rc;
}

int vfs_chown(const char *path, uint32_t uid, uint32_t gid) {
  process_t *proc = process_current();
  if (!proc)
    return -EPERM;

  // Solo root puede cambiar el owner en Aurora (sin CAP_CHOWN por ahora).
  if (proc->euid != 0)
    return -EPERM;

  vfs_node_t *node = vfs_lookup_nofollow(path);
  if (!node)
    return -ENOENT;

  if (!node->ops || !node->ops->chown) {
    vfs_node_free(node);
    return -EROFS;
  }
  int rc = node->ops->chown(node, uid, gid);
  if (rc == 0) {
    if (uid != (uint32_t)-1)
      node->uid = uid;
    if (gid != (uint32_t)-1)
      node->gid = gid;
  }
  vfs_node_free(node);
  return rc;
}

int vfs_fchmod(file_descriptor_t *f, uint32_t mode) {
  if (!f || !f->node)
    return -EBADF;
  process_t *proc = process_current();
  if (!proc)
    return -EPERM;

  int is_root = (proc->euid == 0);
  int is_owner = (proc->euid == f->node->uid);
  if (!is_root && !is_owner)
    return -EPERM;

  uint32_t filtered = mode & 07777;
  if (!is_root && (filtered & (S_ISUID | S_ISGID))) {
    uint32_t already = f->node->mode & (S_ISUID | S_ISGID);
    if ((filtered & (S_ISUID | S_ISGID)) != already)
      return -EPERM;
  }

  if (!f->node->ops || !f->node->ops->chmod)
    return -EROFS;
  int rc = f->node->ops->chmod(f->node, filtered);
  if (rc == 0)
    f->node->mode = (f->node->mode & S_IFMT) | filtered;
  return rc;
}

int vfs_fchown(file_descriptor_t *f, uint32_t uid, uint32_t gid) {
  if (!f || !f->node)
    return -EBADF;
  process_t *proc = process_current();
  if (!proc || proc->euid != 0)
    return -EPERM;

  if (!f->node->ops || !f->node->ops->chown)
    return -EROFS;
  int rc = f->node->ops->chown(f->node, uid, gid);
  if (rc == 0) {
    if (uid != (uint32_t)-1)
      f->node->uid = uid;
    if (gid != (uint32_t)-1)
      f->node->gid = gid;
  }
  return rc;
}

// ---------------------------------------------------------------------------
// [3.4.d] Comprobación de permisos POSIX.
//
// Reglas (sin capabilities, como Linux sin CAP_DAC_*):
//   - root (euid == 0): bypass total. El bit X de execve se comprueba
//     aparte en process_execve_prepare.
//   - owner (euid == node->uid): bits (mode >> 6) & 7.
//   - group (egid == node->gid, o node->gid en groups[]): (mode >> 3) & 7.
//   - other: mode & 7.
//
// No sigue symlinks: se llama con el nodo ya resuelto por vfs_lookup.
// ---------------------------------------------------------------------------
int vfs_check_access(vfs_node_t *node, int mask) {
  if (!node)
    return -EINVAL;
  if (mask == VFS_F_OK)
    return 0;

  process_t *proc = process_current();
  // Contexto kernel (tests, kmain_task): permitir.
  if (!proc)
    return 0;

  // Root bypasea todo.
  if (proc->euid == 0)
    return 0;

  uint32_t bits;
  if (proc->euid == node->uid) {
    bits = (node->mode >> 6) & 7;
  } else if (proc->egid == node->gid) {
    bits = (node->mode >> 3) & 7;
  } else {
    int in_group = 0;
    for (int i = 0; i < proc->ngroups; i++) {
      if (proc->groups[i] == node->gid) {
        in_group = 1;
        break;
      }
    }
    bits = in_group ? ((node->mode >> 3) & 7) : (node->mode & 7);
  }

  uint32_t need = 0;
  if (mask & VFS_R_OK)
    need |= 4;
  if (mask & VFS_W_OK)
    need |= 2;
  if (mask & VFS_X_OK)
    need |= 1;

  if ((bits & need) == need)
    return 0;
  return -EACCES;
}

// ===========================================================================
// Free de nodo
// ===========================================================================
void vfs_node_free(vfs_node_t *node) {
  if (!node)
    return;
  if (node == &stdin_node || node == &stdout_node || node == &stderr_node)
    return;
  if (node->ops && node->ops->close)
    node->ops->close(node);
  kfree(node);
}

// ===========================================================================
// read_all
// ===========================================================================
int vfs_read_all(const char *path, void **out_buf, size_t *out_size) {
  if (!path || !out_buf || !out_size)
    return -EINVAL;
  *out_buf = NULL;
  *out_size = 0;

  vfs_node_t *node = vfs_lookup(path);
  if (!node)
    return -ENOENT;
  if (node->flags & VFS_DIRECTORY) {
    vfs_node_free(node);
    return -EISDIR;
  }
  if (!node->ops || !node->ops->read) {
    vfs_node_free(node);
    return -EIO;
  }

  size_t size = node->size;
  if (size == 0) {
    vfs_node_free(node);
    return -EINVAL;
  }

  void *buf = kmalloc(size);
  if (!buf) {
    vfs_node_free(node);
    return -ENOMEM;
  }

  int64_t n = node->ops->read(node, 0, size, buf);
  vfs_node_free(node);
  if (n < 0 || (size_t)n != size) {
    kfree(buf);
    return -EIO;
  }

  *out_buf = buf;
  *out_size = size;
  return 0;
}

// ===========================================================================
// devfs: sistema de ficheros virtual para /dev.
//
// Registra nodos estáticos en una tabla. readdir del directorio raíz
// devuelve los nombres de la tabla. Los nodos se construyen on-demand
// en cada lookup.
// ===========================================================================

static int64_t dev_null_read(vfs_node_t *node, uint64_t offset, size_t size,
                             void *buf) {
  (void)node;
  (void)offset;
  (void)size;
  (void)buf;
  return 0;
}

static int64_t dev_null_write(vfs_node_t *node, uint64_t offset, size_t size,
                              const void *buf) {
  (void)node;
  (void)offset;
  (void)buf;
  return (int64_t)size;
}

static vfs_ops_t dev_null_ops = {
    .read = dev_null_read,
    .write = dev_null_write,
};

static int64_t dev_zero_read(vfs_node_t *node, uint64_t offset, size_t size,
                             void *buf) {
  (void)node;
  (void)offset;
  if (!buf)
    return -EINVAL;
  memset(buf, 0, size);
  return (int64_t)size;
}

static int64_t dev_zero_write(vfs_node_t *node, uint64_t offset, size_t size,
                              const void *buf) {
  (void)node;
  (void)offset;
  (void)buf;
  return (int64_t)size;
}

static vfs_ops_t dev_zero_ops = {
    .read = dev_zero_read,
    .write = dev_zero_write,
};

static int64_t dev_kmsg_read(vfs_node_t *node, uint64_t offset, size_t size,
                             void *buf) {
  (void)node;
  (void)offset;
  if (!buf)
    return -EINVAL;
  return (int64_t)klog_read((char *)buf, size);
}

static int64_t dev_kmsg_write(vfs_node_t *node, uint64_t offset, size_t size,
                              const void *buf) {
  (void)node;
  (void)offset;
  if (!buf || size == 0)
    return 0;
  klog_write_raw((const char *)buf, size);
  return (int64_t)size;
}

static vfs_ops_t dev_kmsg_ops = {
    .read = dev_kmsg_read,
    .write = dev_kmsg_write,
};

typedef struct {
  const char *name;
  vfs_ops_t *ops;
  uint32_t type;
  uint32_t rdev; // [4.4] (major << 8) | minor, 0 si no aplica
} devfs_entry_t;

// [PTY] readdir de /dev/pts: solo los slots en uso.
static int devfs_pts_readdir(vfs_node_t *dir, uint64_t index,
                             vfs_dirent_t *out) {
  (void)dir;
  if (!out)
    return -EINVAL;

  uint64_t seen = 0;
  for (int i = 0; i < PTY_MAX; i++) {
    if (!pty_is_in_use(i))
      continue;
    if (seen == index) {
      char tmp[8];
      int t = 0;
      int v = i;
      if (v == 0)
        tmp[t++] = '0';
      while (v > 0) {
        tmp[t++] = (char)('0' + v % 10);
        v /= 10;
      }
      int k = 0;
      while (t > 0 && k < (int)sizeof(out->name) - 1)
        out->name[k++] = tmp[--t];
      out->name[k] = '\0';
      out->type = VFS_CHARDEVICE;
      out->size = 0;
      return 0;
    }
    seen++;
  }
  out->name[0] = '\0';
  out->type = 0;
  out->size = 0;
  return 0;
}

static vfs_ops_t devfs_pts_dir_ops = {
    .readdir = devfs_pts_readdir,
};

// ---------------------------------------------------------------------------
// [FIX C] /dev/rtc — acceso al RTC CMOS vía ioctl.
// hwclock de BusyBox lo usa con RTC_RD_TIME (leer) y RTC_SET_TIME (escribir).
// ---------------------------------------------------------------------------
#define RTC_RD_TIME 0x80247009
#define RTC_SET_TIME 0x4024700a

struct rtc_time_user {
  int32_t tm_sec;
  int32_t tm_min;
  int32_t tm_hour;
  int32_t tm_mday;
  int32_t tm_mon;  // 0-11
  int32_t tm_year; // años desde 1900
  int32_t tm_wday;
  int32_t tm_yday;
  int32_t tm_isdst;
};

extern int rtc_read_datetime(rtc_datetime_t *out);
extern int rtc_set_datetime(const rtc_datetime_t *dt);

static int64_t dev_rtc_ioctl(vfs_node_t *node, unsigned long req,
                             uint64_t arg) {
  (void)node;
  if (req == RTC_RD_TIME) {
    if (!access_ok((void *)arg, sizeof(struct rtc_time_user)))
      return -EFAULT;
    rtc_datetime_t dt;
    if (!rtc_read_datetime(&dt))
      return -EIO;
    struct rtc_time_user rt = {
        .tm_sec = dt.second,
        .tm_min = dt.minute,
        .tm_hour = dt.hour,
        .tm_mday = dt.day,
        .tm_mon = dt.month - 1,
        .tm_year = dt.year - 1900,
        .tm_wday = 0,
        .tm_yday = 0,
        .tm_isdst = 0,
    };
    if (copy_to_user((void *)arg, &rt, sizeof(rt)) < 0)
      return -EFAULT;
    return 0;
  }
  if (req == RTC_SET_TIME) {
    if (!access_ok((void *)arg, sizeof(struct rtc_time_user)))
      return -EFAULT;
    struct rtc_time_user rt;
    if (copy_from_user(&rt, (void *)arg, sizeof(rt)) < 0)
      return -EFAULT;
    rtc_datetime_t dt = {
        .second = (uint8_t)rt.tm_sec,
        .minute = (uint8_t)rt.tm_min,
        .hour = (uint8_t)rt.tm_hour,
        .day = (uint8_t)rt.tm_mday,
        .month = (uint8_t)(rt.tm_mon + 1),
        .year = (uint16_t)(rt.tm_year + 1900),
        .century = (uint8_t)((rt.tm_year + 1900) / 100),
    };
    if (!rtc_set_datetime(&dt))
      return -EIO;
    return 0;
  }
  return -ENOTTY;
}

static vfs_ops_t dev_rtc_ops = {
    .ioctl = dev_rtc_ioctl,
};

// /dev/tty: nodo mágico, resuelve al ctty del proceso en cada operación.
static struct tty *dev_tty_resolve(void) {
  process_t *p = process_current();
  return p ? p->ctty : NULL;
}

static int64_t dev_tty_read(vfs_node_t *n, uint64_t off, size_t sz, void *buf) {
  (void)n;
  struct tty *t = dev_tty_resolve();
  if (!t)
    return -ENXIO;
  return tty_read(t, off, sz, buf);
}

static int64_t dev_tty_write(vfs_node_t *n, uint64_t off, size_t sz,
                             const void *buf) {
  (void)n;
  struct tty *t = dev_tty_resolve();
  if (!t)
    return -ENXIO;
  return tty_write(t, off, sz, buf);
}

static int dev_tty_poll(vfs_node_t *n, short events) {
  (void)n;
  struct tty *t = dev_tty_resolve();
  if (!t)
    return 0;
  return tty_poll(t, events);
}

static int64_t dev_tty_ioctl(vfs_node_t *n, unsigned long req, uint64_t arg) {
  (void)n;
  struct tty *t = dev_tty_resolve();
  if (!t)
    return -ENOTTY;
  return tty_ioctl(t, req, arg);
}

static vfs_ops_t dev_tty_ops = {
    .read = dev_tty_read,
    .write = dev_tty_write,
    .poll = dev_tty_poll,
    .ioctl = dev_tty_ioctl,
};

static const devfs_entry_t devfs_entries[] = {
    {"null", &dev_null_ops, VFS_CHARDEVICE, (1u << 8) | 3},
    {"zero", &dev_zero_ops, VFS_CHARDEVICE, (1u << 8) | 5},
    {"kmsg", &dev_kmsg_ops, VFS_CHARDEVICE, (1u << 8) | 11},
    {"tty", &dev_tty_ops, VFS_CHARDEVICE, (5u << 8) | 0},
    {"rtc", &dev_rtc_ops, VFS_CHARDEVICE, (10u << 8) | 135}, // [FIX C]
    {"pts", &devfs_pts_dir_ops, VFS_DIRECTORY, 0},
};

// ===========================================================================
// [FIX] Ops de un block device del block layer. Se aplican a cualquier
// nodo devfs cuyo `priv` apunte a un `block_device_t`.
//
// Permite que fdisk, blkid, dd, mkdosfs abran /dev/<name> y lean/escriban
// sectores directamente. El VFS solo pasa la operación; el block layer
// hace el trabajo real (PIO, DMA, AHCI, ...).
//
// ioctls implementados: BLKGETSIZE, BLKGETSIZE64, BLKSSZGET. Los tres
// que usa busybox fdisk/blkid/partprobe. Sin ellos, fdisk cree que el
// disco tiene 0 sectores y rechaza operar.
// ===========================================================================

#define BLKGETSIZE 0x1260
#define BLKSSZGET 0x1268
#define BLKGETSIZE64 0x80081272

// ===========================================================================
// [FIX] Block devices: reads/writes con offset/size arbitrarios.
//
// El block layer solo entiende de sectores completos y alineados. Los
// consumidores (blkid, fdisk, dd con bs distinto, hexdump) piden rangos
// de bytes arbitrarios. Sin este bounce buffer, pedir 4 bytes en el
// offset 82 (para leer la firma "FAT32   " de un BPB) fallaba con EINVAL,
// que es por lo que `blkid` no detectaba nada.
//
// La lógica: alinear el rango a [sector_ini, sector_fin], leer/escribir
// los sectores completos al bounce buffer, y copiar solo la parte que
// pide el usuario. Sin allocs en el camino caliente: un solo bounce de
// 4 KB en pila por llamada.
// ===========================================================================

static int64_t devfs_block_read(vfs_node_t *node, uint64_t offset, size_t size,
                                void *buf) {
  block_device_t *bdev = (block_device_t *)node->priv;
  if (!bdev || !buf || size == 0)
    return 0;

  uint64_t total_bytes = bdev->num_sectors * (uint64_t)bdev->sector_size;
  if (offset >= total_bytes)
    return 0;
  if (offset + size > total_bytes)
    size = total_bytes - offset;
  if (size == 0)
    return 0;

  const uint32_t ssz = bdev->sector_size;
  const size_t MAX_SECTORS = 8; /* 4 KB con ssz=512 */
  uint8_t bounce[MAX_SECTORS * 512];

  size_t done = 0;
  uint8_t *out = (uint8_t *)buf;
  while (done < size) {
    uint64_t cur = offset + done;
    uint64_t first_lba = cur / ssz;
    uint32_t first_off = (uint32_t)(cur % ssz);

    size_t want = size - done;
    uint32_t nsec = (uint32_t)((first_off + want + ssz - 1) / ssz);
    if (nsec > MAX_SECTORS)
      nsec = MAX_SECTORS;

    int rc = bdev_read(bdev, first_lba, nsec, bounce);
    if (rc != 0)
      return done > 0 ? (int64_t)done : rc;

    size_t avail = (size_t)nsec * ssz - first_off;
    size_t to_copy = (avail < want) ? avail : want;
    if (first_off + to_copy > (size_t)nsec * ssz)
      to_copy = (size_t)nsec * ssz - first_off;
    if (to_copy == 0)
      break;

    memcpy(out + done, bounce + first_off, to_copy);
    done += to_copy;
  }
  return (int64_t)done;
}

static int64_t devfs_block_write(vfs_node_t *node, uint64_t offset, size_t size,
                                 const void *buf) {
  block_device_t *bdev = (block_device_t *)node->priv;
  if (!bdev || !buf)
    return -EINVAL;
  if (bdev->is_read_only)
    return -EROFS;
  if (size == 0)
    return 0;

  uint64_t total_bytes = bdev->num_sectors * (uint64_t)bdev->sector_size;
  if (offset + size > total_bytes)
    return -ERANGE;

  const uint32_t ssz = bdev->sector_size;
  const size_t MAX_SECTORS = 8;
  uint8_t bounce[MAX_SECTORS * 512];

  size_t done = 0;
  const uint8_t *in = (const uint8_t *)buf;
  while (done < size) {
    uint64_t cur = offset + done;
    uint64_t first_lba = cur / ssz;
    uint32_t first_off = (uint32_t)(cur % ssz);

    size_t want = size - done;
    uint32_t nsec = (uint32_t)((first_off + want + ssz - 1) / ssz);
    if (nsec > MAX_SECTORS)
      nsec = MAX_SECTORS;

    size_t span = (size_t)nsec * ssz;

    // Si el rango cubre el sector completo Y está alineado, escribimos
    // directo (sin leer primero). Si no, hay que hacer read-modify-write.
    if (first_off == 0 && want >= span) {
      int rc = bdev_write(bdev, first_lba, nsec, in + done);
      if (rc != 0)
        return done > 0 ? (int64_t)done : rc;
      done += span;
      continue;
    }

    int rc = bdev_read(bdev, first_lba, nsec, bounce);
    if (rc != 0)
      return done > 0 ? (int64_t)done : rc;

    size_t avail = span - first_off;
    size_t to_copy = (avail < want) ? avail : want;
    memcpy(bounce + first_off, in + done, to_copy);

    rc = bdev_write(bdev, first_lba, nsec, bounce);
    if (rc != 0)
      return done > 0 ? (int64_t)done : rc;

    done += to_copy;
  }
  return (int64_t)done;
}

static int devfs_block_poll(vfs_node_t *node, short events) {
  (void)node;
  int rev = 0;
  if (events & 1) /*POLLIN*/
    rev |= 1;
  if (events & 4) /*POLLOUT*/
    rev |= 4;
  return rev;
}

static int64_t devfs_block_ioctl(vfs_node_t *node, unsigned long req,
                                 uint64_t arg) {
  block_device_t *bdev = (block_device_t *)node->priv;
  if (!bdev)
    return -ENOTTY;

  if (req == BLKGETSIZE) {
    uint32_t sz = (bdev->num_sectors > 0xFFFFFFFFULL)
                      ? 0xFFFFFFFFu
                      : (uint32_t)bdev->num_sectors;
    if (!access_ok((void *)arg, sizeof(sz)))
      return -EFAULT;
    if (copy_to_user((void *)arg, &sz, sizeof(sz)) < 0)
      return -EFAULT;
    return 0;
  }
  if (req == BLKGETSIZE64) {
    uint64_t sz = bdev->num_sectors * (uint64_t)bdev->sector_size;
    if (!access_ok((void *)arg, sizeof(sz)))
      return -EFAULT;
    if (copy_to_user((void *)arg, &sz, sizeof(sz)) < 0)
      return -EFAULT;
    return 0;
  }
  if (req == BLKSSZGET) {
    int32_t ssz = (int32_t)bdev->sector_size;
    if (!access_ok((void *)arg, sizeof(ssz)))
      return -EFAULT;
    if (copy_to_user((void *)arg, &ssz, sizeof(ssz)) < 0)
      return -EFAULT;
    return 0;
  }
  return -ENOTTY;
}

static vfs_ops_t devfs_block_ops = {
    .read = devfs_block_read,
    .write = devfs_block_write,
    .poll = devfs_block_poll,
    .ioctl = devfs_block_ioctl,
};

static int devfs_readdir(vfs_node_t *dir, uint64_t index, vfs_dirent_t *out) {
  if (!dir || !out)
    return -EINVAL;
  if (!(dir->flags & VFS_DIRECTORY))
    return -ENOTDIR;

  size_t n_entries = sizeof(devfs_entries) / sizeof(devfs_entries[0]);
  if (index < n_entries) {
    const char *name = devfs_entries[index].name;
    size_t len = strlen(name);
    if (len >= sizeof(out->name))
      len = sizeof(out->name) - 1;
    for (size_t i = 0; i < len; i++)
      out->name[i] = name[i];
    out->name[len] = '\0';
    out->type = devfs_entries[index].type;
    out->size = 0;
    return 0;
  }

  // [FIX] Tras los nodos estáticos, listar los block devices del block
  // layer. Como las particiones también se registran como devices
  // independientes (hda1, hda2, ...), aparecen aquí solas.
  int blk_idx = (int)(index - n_entries);
  int nblocks = blk_count();
  if (blk_idx < nblocks) {
    block_device_t *bdev = blk_get_by_index(blk_idx);
    if (bdev) {
      size_t len = strlen(bdev->name);
      if (len >= sizeof(out->name))
        len = sizeof(out->name) - 1;
      memcpy(out->name, bdev->name, len);
      out->name[len] = '\0';
      out->type = VFS_FILE;
      out->size = bdev->num_sectors * (uint64_t)bdev->sector_size;
      return 0;
    }
  }

  out->name[0] = '\0';
  out->type = 0;
  out->size = 0;
  return 0;
}

static vfs_ops_t devfs_dir_ops = {
    .readdir = devfs_readdir,
};

static vfs_node_t *devfs_lookup(void *fs_priv, const char *path) {
  (void)fs_priv;
  if (!path || path[0] != '/')
    return NULL;

  if (path[1] == '\0') {
    vfs_node_t *n = kzalloc(sizeof(vfs_node_t));
    if (!n)
      return NULL;
    n->name[0] = '/';
    n->name[1] = '\0';
    n->flags = VFS_DIRECTORY;
    n->ops = &devfs_dir_ops;
    n->mode = S_IFDIR | 0755;
    n->uid = 0;
    n->gid = 0;
    return n;
  }

  const char *name = path + 1;

  // /pts/N
  if (strncmp(name, "pts/", 4) == 0) {
    const char *num = name + 4;
    if (*num == '\0')
      return NULL;
    int idx = 0;
    while (*num >= '0' && *num <= '9') {
      idx = idx * 10 + (*num - '0');
      if (idx >= PTY_MAX)
        return NULL;
      num++;
    }
    if (*num != '\0')
      return NULL;
    if (!pty_is_in_use(idx))
      return NULL;

    vfs_node_t *n = kzalloc(sizeof(vfs_node_t));
    if (!n)
      return NULL;
    size_t nl = strlen(name);
    if (nl >= sizeof(n->name))
      nl = sizeof(n->name) - 1;
    memcpy(n->name, name, nl);
    n->name[nl] = '\0';
    n->flags = VFS_CHARDEVICE;
    n->ops = NULL;
    n->mode = S_IFCHR | 0620;
    n->uid = 0;
    n->gid = 5;
    n->rdev = (136u << 8) | (uint32_t)idx;
    return n;
  }

  // Rechazar subpaths no soportados.
  for (const char *p = name; *p; p++) {
    if (*p == '/')
      return NULL;
  }

  // [FIX] ¿Es un block device del block layer?
  block_device_t *bdev = blk_lookup(name);
  if (bdev) {
    vfs_node_t *n = kzalloc(sizeof(vfs_node_t));
    if (!n)
      return NULL;
    size_t nlen = strlen(name);
    if (nlen >= sizeof(n->name))
      nlen = sizeof(n->name) - 1;
    memcpy(n->name, name, nlen);
    n->name[nlen] = '\0';
    n->flags = VFS_FILE; // VFS no tiene bit block device; el modo sí
    n->ops = &devfs_block_ops;
    n->priv = bdev;
    n->size = bdev->num_sectors * (uint64_t)bdev->sector_size;
    n->mode = S_IFBLK | (bdev->is_read_only ? 0444 : 0660);
    n->uid = 0;
    n->gid = 6; // "disk"
    // [FIX] rdev único por disco: major=8, minor=índice.
    int idx = -1;
    int nb = blk_count();
    for (int i = 0; i < nb; i++) {
      if (blk_get_by_index(i) == bdev) {
        idx = i;
        break;
      }
    }
    n->rdev = (8u << 8) | (uint32_t)(idx < 0 ? 0 : idx);
    return n;
  }

  // Nodos estáticos.
  size_t n_entries = sizeof(devfs_entries) / sizeof(devfs_entries[0]);
  for (size_t i = 0; i < n_entries; i++) {
    if (strcmp(name, devfs_entries[i].name) == 0) {
      vfs_node_t *n = kzalloc(sizeof(vfs_node_t));
      if (!n)
        return NULL;
      size_t nlen = strlen(name);
      if (nlen >= sizeof(n->name))
        nlen = sizeof(n->name) - 1;
      memcpy(n->name, name, nlen);
      n->name[nlen] = '\0';
      n->flags = devfs_entries[i].type;
      n->ops = devfs_entries[i].ops;
      if (devfs_entries[i].type & VFS_DIRECTORY) {
        n->mode = S_IFDIR | 0755;
      } else {
        n->mode = S_IFCHR | 0666;
      }
      n->uid = 0;
      n->gid = 0;
      n->rdev = devfs_entries[i].rdev;
      return n;
    }
  }
  return NULL;
}

static int devfs_statfs(void *fs_priv, struct vfs_statfs *out) {
  (void)fs_priv;
  memset(out, 0, sizeof(*out));
  out->f_type = 0x01021994;
  out->f_bsize = 4096;
  out->f_frsize = 4096;
  out->f_namelen = 255;
  return 0;
}

static vfs_fs_ops_t devfs_fs_ops = {
    .lookup = devfs_lookup,
    .statfs = devfs_statfs,
    .name = "devfs",
};

// [3.3.c] Itera la mount table. El callback recibe cada mount.
void vfs_for_each_mount(vfs_mount_iter_cb_t cb, void *arg) {
  if (!cb)
    return;
  unsigned long flags = spin_lock_irqsave(&g_mounts_lock);
  for (struct vfs_mount *m = g_mounts; m; m = m->next) {
    const char *name = (m->ops && m->ops->name) ? m->ops->name : "unknown";
    if (cb(m->path, name, m->is_bind, m->bind_source, arg) != 0)
      break;
  }
  spin_unlock_irqrestore(&g_mounts_lock, flags);
}

// ===========================================================================
// Init
// ===========================================================================
void vfs_init(void) {
  memset(&stdin_node, 0, sizeof(stdin_node));
  strcpy(stdin_node.name, "/dev/console");
  stdin_node.flags = VFS_CHARDEVICE;
  stdin_node.ops = &console_ops;
  stdin_node.mode = S_IFCHR | 0666;
  stdin_node.uid = 0;
  stdin_node.gid = 0;

  memset(&stdout_node, 0, sizeof(stdout_node));
  strcpy(stdout_node.name, "/dev/console");
  stdout_node.flags = VFS_CHARDEVICE;
  stdout_node.ops = &console_ops;
  stdout_node.mode = S_IFCHR | 0666;
  stdout_node.uid = 0;
  stdout_node.gid = 0;

  memset(&stderr_node, 0, sizeof(stderr_node));
  strcpy(stderr_node.name, "/dev/console");
  stderr_node.flags = VFS_CHARDEVICE;
  stderr_node.ops = &console_ops;
  stderr_node.mode = S_IFCHR | 0666;
  stderr_node.uid = 0;
  stderr_node.gid = 0;

  spin_init(&g_mounts_lock);
  g_mounts = NULL;

  int rc = vfs_mount("/", tarfs_get_vfs_ops(), NULL);
  if (rc != 0) {
    LOG_ERR("[VFS] fallo al montar tarfs en /: %d", rc);
  } else {
    LOG_INFO("[VFS] VFS inicializado (tarfs en /)");
  }
  int rc2 = vfs_mount("/dev", &devfs_fs_ops, NULL);
  if (rc2 != 0) {
    LOG_ERR("[VFS] fallo al montar devfs en /dev: %d", rc2);
  } else {
    LOG_INFO("[VFS] devfs montado en /dev");
  }
  int rc3 = vfs_mount("/proc", procfs_get_vfs_ops(), NULL);
  if (rc3 != 0) {
    LOG_ERR("[VFS] fallo al montar procfs en /proc: %d", rc3);
  } else {
    LOG_INFO("[VFS] procfs montado en /proc");
  }
  extern struct vfs_fs_ops *sysfs_get_vfs_ops(void);
  extern void sysfs_init(void);
  sysfs_init();
  int rc4 = vfs_mount("/sys", sysfs_get_vfs_ops(), NULL);
  if (rc4 != 0) {
    LOG_ERR("[VFS] fallo al montar sysfs en /sys: %d", rc4);
  } else {
    LOG_INFO("[VFS] sysfs montado en /sys");
  }
}

// ===========================================================================
// Helpers internos de FDs
// ===========================================================================
static void fd_init_wqs(file_descriptor_t *fd) {
  mutex_init(&fd->lock);
  wait_queue_init(&fd->read_wq);
  wait_queue_init(&fd->write_wq);
  fd->flock_mode = 0; // [flock]
}

file_descriptor_t *vfs_create_stdio_fd(int stdio_type) {
  file_descriptor_t *fd =
      (file_descriptor_t *)kmalloc(sizeof(file_descriptor_t));
  if (!fd)
    return NULL;
  fd->offset = 0;
  fd->ref_count = 1;
  fd_init_wqs(fd);

  if (stdio_type == 0) {
    fd->node = &stdin_node;
    fd->flags = O_RDONLY;
  } else if (stdio_type == 1) {
    fd->node = &stdout_node;
    fd->flags = O_WRONLY;
  } else {
    fd->node = &stderr_node;
    fd->flags = O_WRONLY;
  }
  return fd;
}

// ===========================================================================
// FDs por proceso
// ===========================================================================
int vfs_open_for_proc(void *proc_ptr, const char *path, int flags) {
  process_t *proc = (process_t *)proc_ptr;
  if (!proc || !path)
    return -EINVAL;

  int free_fd = -1;
  for (int i = 0; i < MAX_PROCESS_FDS; i++) {
    if (!proc->fds[i]) {
      free_fd = i;
      break;
    }
  }
  if (free_fd == -1)
    return -EMFILE;

  // /dev/ptmx y /dev/pts/N crean recursos frescos en cada open. La
  // firma de ops->open no permite pasar contexto por-open, así que
  // interceptamos aquí.
  if (strcmp(path, "/dev/ptmx") == 0) {
    int rc = pty_open_master_fd(proc, free_fd);
    return rc == 0 ? free_fd : rc;
  }
  if (strncmp(path, "/dev/pts/", 9) == 0) {
    const char *p = path + 9;
    if (*p < '0' || *p > '9')
      return -ENOENT;
    int idx = 0;
    while (*p >= '0' && *p <= '9') {
      idx = idx * 10 + (*p - '0');
      if (idx > 1000)
        return -ENOENT;
      p++;
    }
    if (*p != '\0')
      return -ENOENT;
    int rc = pty_open_slave_fd(proc, free_fd, idx);
    return rc == 0 ? free_fd : rc;
  }

  vfs_node_t *node = vfs_lookup(path);
  if (!node) {
    LOG_INFO("[VFS-OPEN-FAIL] lookup('%s')=NULL", path);
    return -ENOENT;
  }

  // [3.4.d] Comprobar permiso según los flags de apertura.
  int acc_mask = (flags & (O_WRONLY | O_RDWR)) ? VFS_W_OK : VFS_R_OK;
  int acc_rc = vfs_check_access(node, acc_mask);
  if (acc_rc != 0) {
    vfs_node_free(node);
    return acc_rc;
  }

  if (node->ops && node->ops->open) {
    int orc = node->ops->open(node, flags);
    if (orc != 0) {
      vfs_node_free(node);
      return orc;
    }
  }

  file_descriptor_t *fd_entry =
      (file_descriptor_t *)kmalloc(sizeof(file_descriptor_t));
  if (!fd_entry) {
    vfs_node_free(node);
    return -ENOMEM;
  }

  fd_entry->node = node;
  fd_entry->offset = 0;
  fd_entry->flags = flags;
  fd_entry->ref_count = 1;
  fd_init_wqs(fd_entry);

  proc->fds[free_fd] = fd_entry;
  return free_fd;
}

int vfs_close_for_proc(void *proc_ptr, int fd) {
  process_t *proc = (process_t *)proc_ptr;
  if (!proc || fd < 0 || fd >= MAX_PROCESS_FDS || !proc->fds[fd])
    return -1;

  file_descriptor_t *f = proc->fds[fd];
  proc->fds[fd] = NULL;

  int new_rc = __atomic_sub_fetch(&f->ref_count, 1, __ATOMIC_ACQ_REL);

  if (new_rc == 0) {
    vfs_node_free(f->node);
    kfree(f);
  }
  return 0;
}

int64_t vfs_read_for_proc(void *proc_ptr, int fd, void *buf, size_t count) {
  process_t *proc = (process_t *)proc_ptr;
  if (!proc || fd < 0 || fd >= MAX_PROCESS_FDS || !proc->fds[fd] || !buf)
    return -1;

  file_descriptor_t *f = proc->fds[fd];
  if (!f->node || !f->node->ops || !f->node->ops->read)
    return -1;
  if (count == 0)
    return 0;

  const size_t chunk_size = 4096;
  uint8_t *kbuf = (uint8_t *)kmalloc(chunk_size);
  if (!kbuf)
    return -ENOMEM;

  size_t total = 0;
  while (total < count) {
    size_t chunk = count - total;
    if (chunk > chunk_size)
      chunk = chunk_size;

    int64_t bytes = f->node->ops->read(f->node, f->offset, chunk, kbuf);
    if (bytes < 0) {
      if (total == 0) {
        kfree(kbuf);
        return bytes;
      }
      break;
    }
    if (bytes == 0)
      break;
    if ((uint64_t)bytes > chunk) {
      kfree(kbuf);
      return total > 0 ? (int64_t)total : -EIO;
    }

    if (copy_to_user((uint8_t *)buf + total, kbuf, (size_t)bytes) < 0) {
      if (total == 0) {
        kfree(kbuf);
        return -EFAULT;
      }
      break;
    }

    f->offset += (uint64_t)bytes;
    total += (size_t)bytes;
    if ((size_t)bytes < chunk)
      break;
  }

  kfree(kbuf);
  return (int64_t)total;
}

int64_t vfs_write_for_proc(void *proc_ptr, int fd, const void *buf,
                           size_t count) {
  process_t *proc = (process_t *)proc_ptr;
  if (!proc || fd < 0 || fd >= MAX_PROCESS_FDS || !proc->fds[fd] || !buf)
    return -1;

  file_descriptor_t *f = proc->fds[fd];
  if (!f->node || !f->node->ops || !f->node->ops->write)
    return -1;
  if (count == 0)
    return 0;

  const size_t chunk_size = 4096;
  uint8_t *kbuf = (uint8_t *)kmalloc(chunk_size);
  if (!kbuf)
    return -ENOMEM;

  size_t total = 0;
  while (total < count) {
    size_t chunk = count - total;
    if (chunk > chunk_size)
      chunk = chunk_size;

    if (copy_from_user(kbuf, (const uint8_t *)buf + total, chunk) < 0) {
      if (total == 0) {
        kfree(kbuf);
        return -EFAULT;
      }
      break;
    }

    int64_t bytes = f->node->ops->write(f->node, f->offset, chunk, kbuf);
    if (bytes < 0) {
      if (total == 0) {
        kfree(kbuf);
        return bytes;
      }
      break;
    }
    if ((uint64_t)bytes > chunk) {
      kfree(kbuf);
      return total > 0 ? (int64_t)total : -EIO;
    }

    f->offset += (uint64_t)bytes;
    total += (size_t)bytes;
    if ((size_t)bytes < chunk)
      break;
  }

  kfree(kbuf);
  return (int64_t)total;
}

int64_t vfs_seek_for_proc(void *proc_ptr, int fd, int64_t offset, int whence) {
  process_t *proc = (process_t *)proc_ptr;
  if (!proc || fd < 0 || fd >= MAX_PROCESS_FDS || !proc->fds[fd])
    return -1;

  file_descriptor_t *f = proc->fds[fd];
  if (!f->node)
    return -1;

  uint64_t new_off = f->offset;
  if (whence == SEEK_SET) {
    if (offset < 0)
      return -1;
    new_off = (uint64_t)offset;
  } else if (whence == SEEK_CUR) {
    if (offset < 0 && (uint64_t)(-offset) > f->offset)
      return -1;
    new_off = f->offset + offset;
  } else if (whence == SEEK_END) {
    if (offset < 0 && (uint64_t)(-offset) > f->node->size)
      return -1;
    new_off = f->node->size + offset;
  } else {
    return -1;
  }
  f->offset = new_off;
  return (int64_t)new_off;
}

int vfs_fstat_for_proc(void *proc_ptr, int fd, vfs_stat_t *st) {
  process_t *proc = (process_t *)proc_ptr;
  if (!proc || fd < 0 || fd >= MAX_PROCESS_FDS || !proc->fds[fd] || !st)
    return -1;

  file_descriptor_t *f = proc->fds[fd];
  if (!f->node)
    return -1;

  stac();
  st->flags = f->node->flags;
  st->size = f->node->size;
  st->inode = f->node->inode;
  st->mtime_sec = f->node->mtime_sec;
  st->mode = f->node->mode;
  st->uid = f->node->uid;
  st->gid = f->node->gid;
  st->rdev = f->node->rdev;
  clac();
  return 0;
}

// ===========================================================================
// Wait queues por fd
// ===========================================================================
static bool fd_node_readable(void *arg) {
  file_descriptor_t *f = (file_descriptor_t *)arg;
  if (!f || !f->node)
    return true;
  if (!f->node->ops || !f->node->ops->readable)
    return true;
  return f->node->ops->readable(f->node);
}

int vfs_wait_readable(void *proc_ptr, int fd) {
  process_t *proc = (process_t *)proc_ptr;
  if (!proc || fd < 0 || fd >= MAX_PROCESS_FDS || !proc->fds[fd])
    return -1;

  file_descriptor_t *f = proc->fds[fd];
  if (!f->node || !f->node->ops || !f->node->ops->readable)
    return 0;

  return wait_event_interruptible(&f->read_wq, fd_node_readable, f);
}

void vfs_notify_readable(file_descriptor_t *fd) {
  if (!fd)
    return;
  wake_up_all(&fd->read_wq);
}

void vfs_notify_writable(file_descriptor_t *fd) {
  if (!fd)
    return;
  wake_up_all(&fd->write_wq);
}

int vfs_node_poll(vfs_node_t *node, short events) {
  if (!node)
    return 0;
  if (node->ops && node->ops->poll)
    return node->ops->poll(node, events);
  int r = 0;
  if (events & 1)
    r |= 1;
  if (events & 4)
    r |= 4;
  return r;
}

int64_t vfs_node_ioctl(vfs_node_t *node, unsigned long req, uint64_t arg) {
  if (!node || !node->ops || !node->ops->ioctl)
    return -ENOTTY;
  return node->ops->ioctl(node, req, arg);
}