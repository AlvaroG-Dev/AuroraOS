// kernel/vfs.c
#include "vfs.h"
#include "cpu.h"
#include "gfx/winsrv.h"
#include "heap.h"
#include "klog.h"
#include "process.h"
#include "pty.h"
#include "serial.h"
#include "spinlock.h"
#include "string.h"
#include "tarfs.h"
#include "tty.h"
#include "uaccess.h" // EINVAL, EEXIST, ENOENT, ENOMEM, EISDIR, EIO, EBUSY,
                     // ENAMETOOLONG
#include <stddef.h>

// ===========================================================================
// Node-level ops de TarFS.
//
// tarfs es un FS plano (los ficheros del tar son "apps/shell",
// "system/config.txt"...). Cuando se monta, el VFS le pasa paths
// relativos al mount; los normaliza y delega en estas ops.
// ===========================================================================

static int64_t tar_vfs_read(vfs_node_t *node, uint64_t offset, size_t size,
                            void *buf) {
  if (!node || !node->priv || !buf)
    return -1;
  tar_node_t *tn = (tar_node_t *)node->priv;
  if (tn->is_dir)
    return -EISDIR;
  if (offset >= tn->size)
    return 0;

  size_t to_read = size;
  if (offset + to_read > tn->size)
    to_read = tn->size - offset;
  memcpy(buf, tn->data + offset, to_read);
  return (int64_t)to_read;
}

static int64_t tar_vfs_write(vfs_node_t *node, uint64_t offset, size_t size,
                             const void *buf) {
  (void)node;
  (void)offset;
  (void)size;
  (void)buf;
  return -EROFS;
}

static int tar_vfs_open(vfs_node_t *node, int flags) {
  (void)node;
  // tarfs es read-only.
  if ((flags & O_WRONLY) || (flags & O_RDWR))
    return -EROFS;
  return 0;
}

static int tar_vfs_close(vfs_node_t *node) {
  (void)node;
  return 0;
}

// ---------------------------------------------------------------------------
// [PIVOT] readdir de tarfs.
//
// Itera los nodos del tar y devuelve los hijos DIRECTOS del directorio
// `dir` (dir->priv es un tar_node_t* con is_dir=1). Un nodo hijo es
// directo si:
//   - su nombre empieza por "<dir_name>/" (o por nada, si es la raíz)
//   - el resto no contiene '/'
// ---------------------------------------------------------------------------
static int tar_vfs_readdir(vfs_node_t *dir, uint64_t index, vfs_dirent_t *out) {
  if (!dir || !dir->priv || !out)
    return -EINVAL;
  tar_node_t *tn = (tar_node_t *)dir->priv;
  if (!tn->is_dir)
    return -ENOTDIR;

  // Prefijo a matchear. Para la raíz ("") es cadena vacía.
  size_t plen = strlen(tn->name);
  char prefix[260];
  size_t prefix_len;
  if (plen == 0) {
    prefix[0] = '\0';
    prefix_len = 0;
  } else {
    if (plen + 2 > sizeof(prefix))
      return -ENAMETOOLONG;
    memcpy(prefix, tn->name, plen);
    prefix[plen] = '/';
    prefix[plen + 1] = '\0';
    prefix_len = plen + 1;
  }

  size_t n = tarfs_get_node_count();
  uint64_t seen = 0;
  for (size_t i = 0; i < n; i++) {
    tar_node_t *child = tarfs_get_node(i);
    if (!child)
      continue;
    if (child == tn)
      continue;

    const char *name = child->name;
    if (prefix_len > 0) {
      if (strncmp(name, prefix, prefix_len) != 0)
        continue;
      name += prefix_len;
    }
    if (*name == '\0')
      continue;
    int has_slash = 0;
    for (const char *p = name; *p; p++) {
      if (*p == '/') {
        has_slash = 1;
        break;
      }
    }
    if (has_slash)
      continue;

    if (seen == index) {
      size_t rlen = strlen(name);
      if (rlen >= sizeof(out->name))
        rlen = sizeof(out->name) - 1;
      for (size_t k = 0; k < rlen; k++)
        out->name[k] = name[k];
      out->name[rlen] = '\0';
      out->type = child->is_dir ? VFS_DIRECTORY : VFS_FILE;
      out->size = child->is_dir ? 0 : child->size;
      return 0;
    }
    seen++;
  }

  // Fin de directorio: nombre vacío.
  out->name[0] = '\0';
  out->type = 0;
  out->size = 0;
  return 0;
}

// ---------------------------------------------------------------------------
// [PIVOT] Nodo sintético para la raíz de tarfs ("/initrd").
//
// tarfs no tiene entry para "". Lo fabricamos estáticamente. Vive en
// .data y no se libera nunca.
// ---------------------------------------------------------------------------
static tar_node_t g_tarfs_root = {
    .name = "",
    .data = NULL,
    .size = 0,
    .is_dir = 1,
};

// Ops para ficheros y directorios de tarfs. Los directorios llevan
// readdir; los ficheros, no. read/write/open/close son los mismos.
static vfs_ops_t tar_ops = {
    .read = tar_vfs_read,
    .write = tar_vfs_write,
    .open = tar_vfs_open,
    .close = tar_vfs_close,
    .readable = NULL,
};

static vfs_ops_t tar_dir_ops = {
    .read = tar_vfs_read,
    .write = tar_vfs_write,
    .open = tar_vfs_open,
    .close = tar_vfs_close,
    .readable = NULL,
    .readdir = tar_vfs_readdir,
};

// Forward declaration: tarfs_fs_lookup asigna node->fs = &tarfs_fs_ops,
// pero el struct se define al final del fichero.
static vfs_fs_ops_t tarfs_fs_ops;

static vfs_node_t *tarfs_fs_lookup(void *fs_priv, const char *path) {
  (void)fs_priv;

  // Caso raíz del mount: rel es "/" o "". Devolvemos un directorio
  // sintético sobre g_tarfs_root, con tar_dir_ops para poder listarlo.
  if (!path || path[0] == '\0' || (path[0] == '/' && path[1] == '\0')) {
    vfs_node_t *node = (vfs_node_t *)kzalloc(sizeof(vfs_node_t));
    if (!node)
      return NULL;
    node->name[0] = '/';
    node->name[1] = '\0';
    node->flags = VFS_DIRECTORY;
    node->size = 0;
    node->inode = 0;
    node->ops = &tar_dir_ops;
    node->fs = &tarfs_fs_ops;
    node->priv = &g_tarfs_root;
    return node;
  }

  // Path normal: buscar el nodo real en el tar.
  tar_node_t *tn = tarfs_open(path);
  if (!tn)
    return NULL;

  vfs_node_t *node = (vfs_node_t *)kmalloc(sizeof(vfs_node_t));
  if (!node)
    return NULL;

  memset(node, 0, sizeof(vfs_node_t));
  size_t nlen = 0;
  while (tn->name[nlen] && nlen < sizeof(node->name) - 1) {
    node->name[nlen] = tn->name[nlen];
    nlen++;
  }
  node->name[nlen] = '\0';
  node->flags = tn->is_dir ? VFS_DIRECTORY : VFS_FILE;
  node->size = tn->size;
  node->inode = 0;
  node->ops = tn->is_dir ? &tar_dir_ops : &tar_ops;
  node->fs = &tarfs_fs_ops;
  node->priv = tn;
  return node;
}

// ===========================================================================
// Consola: stdin/stdout/stderr
// ===========================================================================
// El nodo consola ahora delega TODO en el tty. La lógica de blocking,
// ring buffer, canonical, echo, ioctl y poll vive en tty.c.

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

  // Verificar que source_path existe y es directorio.
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

  // Comprobar mounts anidados.
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
//
// Encuentra el mount cuyo path es prefijo más largo del path consultado,
// delega en fs->lookup con el path relativo al mount.
// ===========================================================================
static vfs_node_t *vfs_lookup_rec(const char *path, int depth) {
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

  // [BIND] Reescribir path y recurrir.
  if (is_bind) {
    size_t src_len = strlen(bind_source);
    size_t suffix_len = norm_len - best_len;
    if (src_len + suffix_len + 1 > sizeof(bind_source))
      return NULL;
    char new_path[VFS_PATH_MAX];
    memcpy(new_path, bind_source, src_len);
    memcpy(new_path + src_len, norm + best_len, suffix_len);
    new_path[src_len + suffix_len] = '\0';
    return vfs_lookup_rec(new_path, depth + 1);
  }

  const char *rel;
  if (mount_is_root) {
    rel = norm;
  } else {
    rel = norm + best_len;
    if (*rel == '\0')
      rel = "/";
  }
  if (ops->lookup) {
    vfs_node_t *node = ops->lookup(fs_priv, rel);
    if (node)
      return node;
  }

  if (strcmp(norm, "/") == 0) {
    vfs_node_t *root = (vfs_node_t *)kzalloc(sizeof(vfs_node_t));
    if (!root)
      return NULL;
    root->name[0] = '/';
    root->name[1] = '\0';
    root->flags = VFS_DIRECTORY;
    root->ops = NULL;
    root->fs = NULL;
    root->priv = NULL;
    return root;
  }

  return NULL;
}

vfs_node_t *vfs_lookup(const char *path) { return vfs_lookup_rec(path, 0); }

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

// Helper común: split + lookup del padre.
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
int vfs_create(const char *path, int flags) {
  if (!path)
    return -EINVAL;

  char base[VFS_PATH_MAX];
  int rc;
  vfs_node_t *parent = resolve_parent(path, base, sizeof(base), &rc);
  if (!parent)
    return rc;

  if (!parent->ops || !parent->ops->create) {
    vfs_node_free(parent);
    return -EROFS;
  }
  rc = parent->ops->create(parent, base, flags);
  vfs_node_free(parent);
  return rc;
}

int vfs_mkdir(const char *path) {
  if (!path)
    return -EINVAL;

  char base[VFS_PATH_MAX];
  int rc;
  vfs_node_t *parent = resolve_parent(path, base, sizeof(base), &rc);
  if (!parent)
    return rc;

  if (!parent->ops || !parent->ops->mkdir) {
    vfs_node_free(parent);
    return -EROFS;
  }
  rc = parent->ops->mkdir(parent, base);
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

int vfs_readdir(const char *path, uint64_t index, vfs_dirent_t *out) {
  if (!path || !out)
    return -EINVAL;
  vfs_node_t *node = vfs_lookup(path);
  if (!node)
    return -ENOENT;
  if (!(node->flags & VFS_DIRECTORY)) {
    vfs_node_free(node);
    return -ENOTDIR;
  }
  if (!node->ops || !node->ops->readdir) {
    vfs_node_free(node);
    return -EROFS;
  }
  int rc = node->ops->readdir(node, index, out);
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
// Registra nodos estáticos en una tabla. vfs_lookup() lo consulta cuando
// el path cae bajo /dev (mount más específico que /). readdir del
// directorio raíz devuelve los nombres de la tabla.
// ===========================================================================

static int64_t dev_null_read(vfs_node_t *node, uint64_t offset, size_t size,
                             void *buf) {
  (void)node;
  (void)offset;
  (void)size;
  (void)buf;
  return 0; // EOF inmediato
}

static int64_t dev_null_write(vfs_node_t *node, uint64_t offset, size_t size,
                              const void *buf) {
  (void)node;
  (void)offset;
  (void)buf;
  return (int64_t)size; // descarta
}

static vfs_ops_t dev_null_ops = {
    .read = dev_null_read,
    .write = dev_null_write,
    .open = NULL,
    .close = NULL,
    .readable = NULL,
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
    .open = NULL,
    .close = NULL,
    .readable = NULL,
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
  // El caller (vfs_write_for_proc) hace stac() alrededor, así que
  // podemos leer directamente del buffer de userland.
  klog_write_raw((const char *)buf, size);
  return (int64_t)size;
}

static vfs_ops_t dev_kmsg_ops = {
    .read = dev_kmsg_read,
    .write = dev_kmsg_write,
    .open = NULL,
    .close = NULL,
    .readable = NULL,
};

typedef struct {
  const char *name;
  vfs_ops_t *ops;
  uint32_t type; // VFS_CHARDEVICE o VFS_DIRECTORY
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
      // formatear "%d"
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
// [CTTY] /dev/tty: nodo mágico que resuelve al terminal de control del
// proceso actual en cada operación. Si no hay ctty, ENXIO.
// ---------------------------------------------------------------------------
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
    {"null", &dev_null_ops, VFS_CHARDEVICE},
    {"zero", &dev_zero_ops, VFS_CHARDEVICE},
    {"kmsg", &dev_kmsg_ops, VFS_CHARDEVICE},
    {"tty", &dev_tty_ops, VFS_CHARDEVICE},
    {"pts", &devfs_pts_dir_ops, VFS_DIRECTORY},
};

static int devfs_readdir(vfs_node_t *dir, uint64_t index, vfs_dirent_t *out) {
  if (!dir || !out)
    return -EINVAL;
  if (!(dir->flags & VFS_DIRECTORY))
    return -ENOTDIR;

  // Solo el directorio raíz de devfs tiene entradas.
  if (!(dir->name[0] == '/' && dir->name[1] == '\0')) {
    out->name[0] = '\0';
    return 0;
  }

  size_t n_entries = sizeof(devfs_entries) / sizeof(devfs_entries[0]);
  if (index >= n_entries) {
    out->name[0] = '\0';
    out->type = 0;
    out->size = 0;
    return 0;
  }

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

static vfs_ops_t devfs_dir_ops = {
    .read = NULL,
    .write = NULL,
    .open = NULL,
    .close = NULL,
    .readable = NULL,
    .readdir = devfs_readdir,
};

static vfs_fs_ops_t devfs_fs_ops;

static vfs_node_t *devfs_lookup(void *fs_priv, const char *path) {
  (void)fs_priv;
  if (!path || path[0] != '/')
    return NULL;

  // Raíz
  if (path[1] == '\0') {
    vfs_node_t *n = kzalloc(sizeof(vfs_node_t));
    if (!n)
      return NULL;
    n->name[0] = '/';
    n->name[1] = '\0';
    n->flags = VFS_DIRECTORY;
    n->ops = &devfs_dir_ops;
    return n;
  }

  const char *name = path + 1;

  // [PTY] /pts/N
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
    return n;
  }

  // Sin subdirectorios más allá de pts/N.
  for (const char *p = name; *p; p++) {
    if (*p == '/')
      return NULL;
  }

  // Entradas estáticas.
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
      return n;
    }
  }
  return NULL;
}

static vfs_fs_ops_t devfs_fs_ops = {
    .lookup = devfs_lookup,
    .name = "devfs",
};

// ===========================================================================
// Init
// ===========================================================================
void vfs_init(void) {
  memset(&stdin_node, 0, sizeof(stdin_node));
  strcpy(stdin_node.name, "stdin");
  stdin_node.flags = VFS_CHARDEVICE;
  stdin_node.ops = &console_ops;

  memset(&stdout_node, 0, sizeof(stdout_node));
  strcpy(stdout_node.name, "stdout");
  stdout_node.flags = VFS_CHARDEVICE;
  stdout_node.ops = &console_ops;

  memset(&stderr_node, 0, sizeof(stderr_node));
  strcpy(stderr_node.name, "stderr");
  stderr_node.flags = VFS_CHARDEVICE;
  stderr_node.ops = &console_ops;

  spin_init(&g_mounts_lock);
  g_mounts = NULL;

  int rc = vfs_mount("/", &tarfs_fs_ops, NULL);
  if (rc != 0) {
    LOG_ERR("[VFS] fallo al montar tarfs en /: %d", rc);
  } else {
    LOG_INFO("[VFS] VFS inicializado (tarfs en /)");
  }
  // [libc] devfs en /dev. Registra null, zero, kmsg.
  int rc2 = vfs_mount("/dev", &devfs_fs_ops, NULL);
  if (rc2 != 0) {
    LOG_ERR("[VFS] fallo al montar devfs en /dev: %d", rc2);
  } else {
    LOG_INFO("[VFS] devfs montado en /dev");
  }
}

// ===========================================================================
// Helpers internos
// ===========================================================================
static void fd_init_wqs(file_descriptor_t *fd) {
  wait_queue_init(&fd->read_wq);
  wait_queue_init(&fd->write_wq);
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
    return -1;

  int free_fd = -1;
  for (int i = 0; i < MAX_PROCESS_FDS; i++) {
    if (!proc->fds[i]) {
      free_fd = i;
      break;
    }
  }
  if (free_fd == -1)
    return -1;

  // [PTY Fase 3] /dev/ptmx y /dev/pts/N necesitan recursos frescos en
  // cada open (un PTY nuevo, o un incremento de refcount del slave).
  // vfs_lookup devuelve un nodo nuevo cada vez, pero no hay forma de
  // pasar un contexto por-open a ops->open con la firma actual, así
  // que interceptamos aquí y delegamos a pty.c.
  if (strcmp(path, "/dev/ptmx") == 0) {
    int rc = pty_open_master_fd(proc, free_fd);
    return rc == 0 ? free_fd : rc;
  }
  if (strncmp(path, "/dev/pts/", 9) == 0) {
    const char *p = path + 9;
    if (*p < '0' || *p > '9')
      return -1;
    int idx = 0;
    while (*p >= '0' && *p <= '9') {
      idx = idx * 10 + (*p - '0');
      if (idx > 1000)
        return -1;
      p++;
    }
    if (*p != '\0')
      return -1;
    int rc = pty_open_slave_fd(proc, free_fd, idx);
    return rc == 0 ? free_fd : rc;
  }

  vfs_node_t *node = vfs_lookup(path);
  if (!node)
    return -1;

  if (node->ops && node->ops->open) {
    if (node->ops->open(node, flags) != 0) {
      vfs_node_free(node);
      return -1;
    }
  }

  file_descriptor_t *fd_entry =
      (file_descriptor_t *)kmalloc(sizeof(file_descriptor_t));
  if (!fd_entry) {
    vfs_node_free(node);
    return -1;
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

  f->ref_count--;
  if (f->ref_count <= 0) {
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

  // Las operaciones de filesystem escriben sobre memoria de kernel.
  // Copiar después al buffer de userland evita que un #PF por una página
  // ausente/no válida llegue al handler fatal del kernel.
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

  // Copiar primero a memoria de kernel. Así una fuente de userland
  // inválida devuelve -EFAULT mediante exception-fixup antes de entrar
  // en la operación del filesystem.
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
  // Fichero regular: siempre listo para leer y escribir.
  int r = 0;
  if (events & 1 /*POLLIN*/)
    r |= 1;
  if (events & 4 /*POLLOUT*/)
    r |= 4;
  return r;
}

int64_t vfs_node_ioctl(vfs_node_t *node, unsigned long req, uint64_t arg) {
  if (!node || !node->ops || !node->ops->ioctl)
    return -ENOTTY;
  return node->ops->ioctl(node, req, arg);
}

// ===========================================================================
// Definición del fs_ops de TarFS (forward-declared arriba).
// ===========================================================================
static vfs_fs_ops_t tarfs_fs_ops = {
    .lookup = tarfs_fs_lookup,
    .name = "tarfs",
};