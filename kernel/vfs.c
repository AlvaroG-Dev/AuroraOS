// kernel/vfs.c
#include "vfs.h"
#include "cpu.h"
#include "gfx/winsrv.h"
#include "heap.h"
#include "klog.h"
#include "process.h"
#include "serial.h"
#include "spinlock.h"
#include "string.h"
#include "tarfs.h"
#include "tty.h"
#include "uaccess.h" // EINVAL, EEXIST, ENOENT, ENOMEM, EISDIR, EIO, EBUSY,
                     // ENAMETOOLONG
#include <stddef.h>

#define EINTR 4

// ===========================================================================
// Node-level ops de TarFS. Cada nodo devuelto por tarfs_fs_lookup lleva
// estos ops. Son específicos de TarFS y no dependen del mount.
// ===========================================================================
static int64_t tar_vfs_read(vfs_node_t *node, uint64_t offset, size_t size,
                            void *buf) {
  if (!node || !node->priv || !buf)
    return -1;
  tar_node_t *tn = (tar_node_t *)node->priv;
  if (offset >= tn->size)
    return 0;

  size_t to_read = size;
  if (offset + to_read > tn->size) {
    to_read = tn->size - offset;
  }
  memcpy(buf, tn->data + offset, to_read);
  return (int64_t)to_read;
}

static int64_t tar_vfs_write(vfs_node_t *node, uint64_t offset, size_t size,
                             const void *buf) {
  (void)node;
  (void)offset;
  (void)size;
  (void)buf;
  return -1;
}

static int tar_vfs_open(vfs_node_t *node, int flags) {
  (void)node;
  if ((flags & O_WRONLY) || (flags & O_RDWR))
    return -1;
  return 0;
}

static int tar_vfs_close(vfs_node_t *node) {
  (void)node;
  return 0;
}

static vfs_ops_t tar_ops = {
    .read = tar_vfs_read,
    .write = tar_vfs_write,
    .open = tar_vfs_open,
    .close = tar_vfs_close,
    .readable = NULL,
};

// ---------------------------------------------------------------------------
// Adapter: tar_node_t → vfs_node_t. Crea un vfs_node_t nuevo kmalloc'd.
// El VFS es responsable de liberarlo (vfs_node_free → kfree).
// ---------------------------------------------------------------------------
// Forward declaration: tarfs_fs_lookup() asigna node->fs = &tarfs_fs_ops,
// pero el struct se rellena al final del archivo. Declaramos aquí para
// que el compilador conozca el símbolo.
static vfs_fs_ops_t tarfs_fs_ops;

static vfs_node_t *tarfs_fs_lookup(void *fs_priv, const char *path) {
  (void)fs_priv;
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
  node->ops = &tar_ops;
  node->fs = &tarfs_fs_ops;
  node->priv = tn;
  return node;
}

// ===========================================================================
// Consola: stdin/stdout/stderr
// ===========================================================================
static int64_t console_vfs_read(vfs_node_t *node, uint64_t offset, size_t size,
                                void *buf) {
  (void)node;
  (void)offset;
  (void)size;
  (void)buf;
  return 0;
}

static int64_t console_vfs_write(vfs_node_t *node, uint64_t offset, size_t size,
                                 const void *buf) {
  (void)node;
  (void)offset;
  if (!buf || size == 0)
    return 0;
  const char *str = (const char *)buf;
  unsigned long flags;
  serial_lock_acquire(&flags);
  for (size_t i = 0; i < size; i++) {
    serial_putc_locked(str[i]);
    winsrv_console_output(str[i]);
  }
  serial_lock_release(flags);
  return (int64_t)size;
}

static vfs_ops_t console_ops = {
    .read = console_vfs_read,
    .write = console_vfs_write,
    .open = NULL,
    .close = NULL,
    .readable = NULL,
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
  struct vfs_mount *next;
};

static struct vfs_mount *g_mounts = NULL;
static spinlock_t g_mounts_lock;

// ===========================================================================
// Path normalization
//
// Reglas:
//   - El resultado siempre empieza por '/' y no termina por '/' (excepto "/").
//   - Múltiples '/' se colapsan.
//   - "./" y "/." se eliminan.
//   - ".." se rechaza (no permitimos upward traversal en el VFS todavía).
//   - Si el path de entrada es vacío o solo '/', el resultado es "/".
//
// Devuelve 0 si OK, negativo en error.
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
    if (seglen == 2 && seg[0] == '.' && seg[1] == '.')
      return -EINVAL;

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

  // ¿Ya hay un mount en esa ruta exacta?
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

  // norm cabe en VFS_PATH_MAX (verificado por normalize_path).
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

int vfs_umount(const char *path) {
  if (!path)
    return -EINVAL;

  char norm[VFS_PATH_MAX];
  int rc = normalize_path(path, norm, sizeof(norm));
  if (rc != 0)
    return rc;

  unsigned long flags = spin_lock_irqsave(&g_mounts_lock);

  size_t nlen = strlen(norm);

  // Comprobar si hay mounts anidados bajo este path. Caso especial:
  // unmount de "/" prohibido si hay otros mounts.
  for (struct vfs_mount *m = g_mounts; m; m = m->next) {
    if (strcmp(m->path, norm) == 0)
      continue;
    // ¿m->path empieza con norm + '/'? (o norm == "/" y m->path != "/")
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

  // Buscar y desenlazar.
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
// Estrategia: encontrar el mount cuyo path es el prefijo más largo de
// `path` que matchea con frontera de componente ("/mnt" matchea "/mnt/foo"
// pero no "/mntx"). Delegar en fs->lookup con el path relativo.
// ===========================================================================
vfs_node_t *vfs_lookup(const char *path) {
  if (!path)
    return NULL;

  char norm[VFS_PATH_MAX];
  if (normalize_path(path, norm, sizeof(norm)) != 0)
    return NULL;

  // Buscar el mount más específico.
  unsigned long flags = spin_lock_irqsave(&g_mounts_lock);
  struct vfs_mount *best = NULL;
  size_t best_len = 0;
  size_t norm_len = strlen(norm);

  for (struct vfs_mount *m = g_mounts; m; m = m->next) {
    size_t ml = strlen(m->path);
    if (ml > best_len) {
      // Matchea si:
      //   m->path == "/" (root, todo matchea)
      //   O strncmp(norm, m->path, ml) == 0 con frontera:
      //     norm[ml] == '\0' (mismo path) o norm[ml] == '/'
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

  vfs_fs_ops_t *ops = best->ops;
  void *fs_priv = best->fs_priv;
  spin_unlock_irqrestore(&g_mounts_lock, flags);

  // Path relativo al mount, empezando por '/'.
  const char *rel = norm + best_len;
  if (*rel == '\0')
    rel = "/";
  // Si best_len == 1 (root), rel ya apunta al siguiente char.
  // Si best->path == "/mnt/x" y norm == "/mnt/x", rel = "" → "/".
  // Si norm == "/mnt/x/foo", rel = "/foo".

  if (!ops->lookup)
    return NULL;
  return ops->lookup(fs_priv, rel);
}

// ===========================================================================
// [PR 4.2] Split de un path normalizado en (dirname, basename).
//
// Asume que `path` ya pasó por normalize_path: empieza por '/', no
// termina por '/', sin '.' ni '..'.
//
// Ejemplos:
//   "/foo"        → ("/",    "foo")
//   "/a/b/c"      → ("/a/b", "c")
//
// Devuelve 0 si OK, -EINVAL si path es "/" o termina en '/' (no
// debería pasar tras normalize), -ENAMETOOLONG si algo no cabe.
// ===========================================================================
static int split_dirname(const char *path, char *dir_out, size_t dir_outlen,
                         char *base_out, size_t base_outlen) {
  if (!path || path[0] != '/' || !dir_out || !base_out)
    return -EINVAL;

  // Buscar el último '/'.
  const char *last = NULL;
  for (const char *p = path; *p; p++) {
    if (*p == '/')
      last = p;
  }
  if (!last)
    return -EINVAL;

  const char *base_start = last + 1;
  if (*base_start == '\0')
    return -EINVAL; // path termina en '/'

  size_t blen = strlen(base_start);
  if (blen + 1 > base_outlen)
    return -ENAMETOOLONG;
  for (size_t i = 0; i < blen; i++)
    base_out[i] = base_start[i];
  base_out[blen] = '\0';

  // dir = path[0..last]. Si last == path, dir = "/".
  size_t dlen = (size_t)(last - path);
  if (dlen == 0)
    dlen = 1; // raíz
  if (dlen + 1 > dir_outlen)
    return -ENAMETOOLONG;
  for (size_t i = 0; i < dlen; i++)
    dir_out[i] = path[i];
  dir_out[dlen] = '\0';

  return 0;
}

// Helper común: split + lookup del padre + dispatch.
// Devuelve el nodo del padre o NULL con *rc_out rellenado.
static vfs_node_t *resolve_parent(const char *path, char *base_out,
                                  size_t base_outlen, int *rc_out) {
  char norm[VFS_PATH_MAX];
  int rc = normalize_path(path, norm, sizeof(norm));
  if (rc != 0) {
    *rc_out = rc;
    return NULL;
  }

  // Casos triviales: "/", "/foo/" no llegan aquí porque normalize los
  // colapsa o rechaza, pero por si acaso.
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
//
// Abre `path` vía vfs_lookup, lee el archivo entero a un buffer kmalloc'd,
// libera el nodo. El llamante debe kfree(out_buf) cuando termine.
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

  // Montar TarFS en /.
  int rc = vfs_mount("/", &tarfs_fs_ops, NULL);
  if (rc != 0) {
    LOG_ERR("[VFS] fallo al montar tarfs en /: %d", rc);
  } else {
    LOG_INFO("[VFS] VFS inicializado (root=tarfs).");
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

  stac();
  int64_t bytes = f->node->ops->read(f->node, f->offset, count, buf);
  clac();

  if (bytes > 0)
    f->offset += (uint64_t)bytes;
  return bytes;
}

int64_t vfs_write_for_proc(void *proc_ptr, int fd, const void *buf,
                           size_t count) {
  process_t *proc = (process_t *)proc_ptr;
  if (!proc || fd < 0 || fd >= MAX_PROCESS_FDS || !proc->fds[fd] || !buf)
    return -1;

  file_descriptor_t *f = proc->fds[fd];
  if (!f->node || !f->node->ops || !f->node->ops->write)
    return -1;

  stac();
  int64_t bytes = f->node->ops->write(f->node, f->offset, count, buf);
  clac();

  if (bytes > 0)
    f->offset += (uint64_t)bytes;
  return bytes;
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

// ===========================================================================
// Definición del fs_ops de TarFS (forward declared arriba).
// ===========================================================================
static vfs_fs_ops_t tarfs_fs_ops = {
    .lookup = tarfs_fs_lookup,
    .name = "tarfs",
};