// kernel/procfs.c
//
// procfs: /proc, generado on-demand. Cada lookup construye el
// contenido del fichero en un buffer kmalloc'd y lo guarda en
// node->priv. ops->read lo copia, ops->close lo libera.
//
// No usamos inodos ni caching: cada vfs_lookup() regenera. Es simple,
// correcto, y coherente con el modelo "reconstruir el árbol en cada
// acceso" del VFS de Aurora.

#include "procfs.h"
#include "heap.h"
#include "klog.h"
#include "pmm.h"
#include "process.h"
#include "sched.h"
#include "string.h"
#include "time.h"
#include "uaccess.h"
#include "vfs.h"
#include <stddef.h>
#include <stdint.h>

// ===========================================================================
// Nodo priv: buffer de contenido + longitud.
// ===========================================================================
typedef struct {
  char *buf; // kmalloc'd, NUL-terminated
  size_t len;
} procfs_data_t;

// ===========================================================================
// Node ops
// ===========================================================================
static int64_t procfs_read(vfs_node_t *node, uint64_t offset, size_t size,
                           void *dst) {
  if (!node || !node->priv || !dst)
    return -EINVAL;
  procfs_data_t *pd = (procfs_data_t *)node->priv;
  if (offset >= pd->len)
    return 0;

  size_t n = pd->len - offset;
  if (n > size)
    n = size;
  memcpy(dst, pd->buf + offset, n);
  return (int64_t)n;
}

static int64_t procfs_write(vfs_node_t *node, uint64_t offset, size_t size,
                            const void *buf) {
  (void)node;
  (void)offset;
  (void)size;
  (void)buf;
  return -EROFS;
}

static int procfs_open(vfs_node_t *node, int flags) {
  (void)node;
  if ((flags & O_WRONLY) || (flags & O_RDWR))
    return -EROFS;
  return 0;
}

static int procfs_close(vfs_node_t *node) {
  if (!node)
    return 0;
  procfs_data_t *pd = (procfs_data_t *)node->priv;
  if (pd) {
    if (pd->buf)
      kfree(pd->buf);
    kfree(pd);
    node->priv = NULL;
  }
  return 0;
}

static vfs_ops_t procfs_file_ops = {
    .read = procfs_read,
    .write = procfs_write,
    .open = procfs_open,
    .close = procfs_close,
    .readable = NULL,
};

// ===========================================================================
// Helpers de formato
// ===========================================================================
static size_t kfmt_u64(char *out, size_t cap, uint64_t v) {
  char tmp[24];
  int n = 0;
  if (v == 0) {
    tmp[n++] = '0';
  } else {
    while (v > 0 && n < (int)sizeof(tmp)) {
      tmp[n++] = (char)('0' + (v % 10));
      v /= 10;
    }
  }
  size_t o = 0;
  while (n > 0 && o + 1 < cap)
    out[o++] = tmp[--n];
  out[o] = '\0';
  return o;
}

static size_t kfmt_str(char *out, size_t cap, const char *s) {
  size_t o = 0;
  while (*s && o + 1 < cap)
    out[o++] = *s++;
  out[o] = '\0';
  return o;
}

// Escribe `s` al final del buffer (respeta cap). Devuelve la nueva
// longitud o `cap` si se truncó.
static size_t kappend(char *buf, size_t len, size_t cap, const char *s) {
  while (*s && len + 1 < cap)
    buf[len++] = *s++;
  buf[len] = '\0';
  return len;
}

// ===========================================================================
// Constructores de contenido (cada uno devuelve un buffer kmalloc'd
// con su longitud en *out_len, o NULL).
// ===========================================================================
static char *gen_uptime(size_t *out_len) {
  char *buf = (char *)kmalloc(64);
  if (!buf)
    return NULL;
  uint64_t ticks = sched_get_ticks();
  uint64_t sec = ticks / 1000;
  uint64_t centi = (ticks % 1000) / 10;

  size_t o = 0;
  o += kfmt_u64(buf + o, 64 - o, sec);
  buf[o++] = '.';
  if (centi < 10)
    buf[o++] = '0';
  o += kfmt_u64(buf + o, 64 - o, centi);
  o = kappend(buf, o, 64, " ");
  // idle: no lo medimos todavía; reportamos 0.00 como en Linux con idle=0.
  o = kappend(buf, o, 64, "0.00\n");
  buf[o] = '\0';
  *out_len = o;
  return buf;
}

static char *gen_version(size_t *out_len) {
  static const char v[] = "Aurora OS version 0.1.0 (x86_64) "
                          "(built with aurora-gcc, musl userland)\n";
  size_t n = sizeof(v) - 1;
  char *buf = (char *)kmalloc(n + 1);
  if (!buf)
    return NULL;
  memcpy(buf, v, n);
  buf[n] = '\0';
  *out_len = n;
  return buf;
}

static char *gen_meminfo(size_t *out_len) {
  // 16 KiB es de sobra para ~15 líneas.
  const size_t CAP = 16 * 1024;
  char *buf = (char *)kmalloc(CAP);
  if (!buf)
    return NULL;

  uint64_t total_pages = pmm_total_pages();
  uint64_t free_pages = pmm_free_pages_count();
  uint64_t used_pages =
      (total_pages > free_pages) ? (total_pages - free_pages) : 0;

  uint64_t page_kb = PAGE_SIZE / 1024;
  uint64_t total_kb = total_pages * page_kb;
  uint64_t free_kb = free_pages * page_kb;
  uint64_t used_kb = used_pages * page_kb;

  size_t o = 0;
#define APPEND_KV(key, val)                                                    \
  do {                                                                         \
    o = kappend(buf, o, CAP, key);                                             \
    o += kfmt_u64(buf + o, CAP - o, (val));                                    \
    o = kappend(buf, o, CAP, " kB\n");                                         \
  } while (0)

  APPEND_KV("MemTotal:       ", total_kb);
  APPEND_KV("MemFree:        ", free_kb);
  APPEND_KV("MemAvailable:   ", free_kb);
  APPEND_KV("MemUsed:        ", used_kb);
  APPEND_KV("Buffers:        ", 0);
  APPEND_KV("Cached:         ", 0);
  APPEND_KV("SwapTotal:      ", 0);
  APPEND_KV("SwapFree:       ", 0);
  APPEND_KV("Dirty:          ", 0);
  APPEND_KV("Writeback:      ", 0);
  APPEND_KV("Shmem:          ", 0);
  APPEND_KV("Slab:           ", 0);
#undef APPEND_KV

  buf[o] = '\0';
  *out_len = o;
  return buf;
}

static char *gen_stat(size_t *out_len) {
  // Formato muy reducido de /proc/stat. BusyBox `top` lo usa para el
  // timestamp, `uptime` no lo toca. Sin contadores de CPU todavía.
  const size_t CAP = 512;
  char *buf = (char *)kmalloc(CAP);
  if (!buf)
    return NULL;

  uint64_t ticks = sched_get_ticks();
  uint64_t sec = ticks / 1000;

  size_t o = 0;
  o = kappend(buf, o, CAP, "cpu  0 0 0 0 0 0 0 0 0 0\n");
  o = kappend(buf, o, CAP, "btime 0\n");
  o = kappend(buf, o, CAP, "processes 0\n");
  o = kappend(buf, o, CAP, "procs_running 1\n");
  o = kappend(buf, o, CAP, "procs_blocked 0\n");
  (void)sec;
  buf[o] = '\0';
  *out_len = o;
  return buf;
}

// ===========================================================================
// readdir: solo el directorio raíz tiene entradas estáticas.
// ===========================================================================
static const char *procfs_root_entries[] = {
    "uptime", "version", "meminfo", "stat", "self",
};
#define PROCFS_N_ROOT_ENTRIES                                                  \
  (sizeof(procfs_root_entries) / sizeof(procfs_root_entries[0]))

static int procfs_readdir(vfs_node_t *dir, uint64_t index, vfs_dirent_t *out) {
  if (!dir || !out)
    return -EINVAL;
  if (!(dir->flags & VFS_DIRECTORY))
    return -ENOTDIR;

  // Solo el directorio raíz. Subdirectorios (/proc/<pid>) llegarán en
  // 3.3.d.
  if (!(dir->name[0] == '/' && dir->name[1] == '\0')) {
    out->name[0] = '\0';
    return 0;
  }

  if (index >= PROCFS_N_ROOT_ENTRIES) {
    out->name[0] = '\0';
    out->type = 0;
    out->size = 0;
    return 0;
  }

  const char *n = procfs_root_entries[index];
  size_t len = strlen(n);
  if (len >= sizeof(out->name))
    len = sizeof(out->name) - 1;
  memcpy(out->name, n, len);
  out->name[len] = '\0';
  out->type = VFS_FILE;
  out->size = 0;
  return 0;
}

static vfs_ops_t procfs_dir_ops = {
    .readdir = procfs_readdir,
};

// ===========================================================================
// Construcción de nodos
// ===========================================================================
static vfs_node_t *make_root_node(void) {
  vfs_node_t *n = (vfs_node_t *)kzalloc(sizeof(vfs_node_t));
  if (!n)
    return NULL;
  n->name[0] = '/';
  n->name[1] = '\0';
  n->flags = VFS_DIRECTORY;
  n->ops = &procfs_dir_ops;
  return n;
}

static vfs_node_t *make_file_node(const char *name, char *buf, size_t len) {
  if (!buf)
    return NULL;
  vfs_node_t *n = (vfs_node_t *)kzalloc(sizeof(vfs_node_t));
  if (!n) {
    kfree(buf);
    return NULL;
  }
  procfs_data_t *pd = (procfs_data_t *)kzalloc(sizeof(*pd));
  if (!pd) {
    kfree(buf);
    kfree(n);
    return NULL;
  }
  pd->buf = buf;
  pd->len = len;

  size_t nl = strlen(name);
  if (nl >= sizeof(n->name))
    nl = sizeof(n->name) - 1;
  memcpy(n->name, name, nl);
  n->name[nl] = '\0';
  n->flags = VFS_FILE;
  n->size = len;
  n->ops = &procfs_file_ops;
  n->priv = pd;
  return n;
}

// ===========================================================================
// lookup
// ===========================================================================
static vfs_node_t *procfs_lookup(void *fs_priv, const char *path) {
  (void)fs_priv;
  if (!path || path[0] != '/')
    return NULL;

  // Raíz.
  if (path[1] == '\0')
    return make_root_node();

  // Sin subdirectorios todavía. Un '/' extra → rechazar.
  for (const char *p = path + 1; *p; p++) {
    if (*p == '/')
      return NULL;
  }

  const char *name = path + 1;

  // [FIX] Evaluar en dos líneas separadas. El orden de evaluación de
  // argumentos en C no está garantizado: si `*len_out` se evalúa antes
  // que `gen_XXX(len_out)`, se pasa un `len` sin inicializar y el
  // fichero queda truncado a basura.
  if (strcmp(name, "uptime") == 0) {
    size_t len = 0;
    char *buf = gen_uptime(&len);
    return make_file_node("uptime", buf, len);
  }
  if (strcmp(name, "version") == 0) {
    size_t len = 0;
    char *buf = gen_version(&len);
    return make_file_node("version", buf, len);
  }
  if (strcmp(name, "meminfo") == 0) {
    size_t len = 0;
    char *buf = gen_meminfo(&len);
    return make_file_node("meminfo", buf, len);
  }
  if (strcmp(name, "stat") == 0) {
    size_t len = 0;
    char *buf = gen_stat(&len);
    return make_file_node("stat", buf, len);
  }
  if (strcmp(name, "self") == 0) {
    // [3.3.a] /proc/self: symlink al pid del proceso actual. La
    // resolución la hará vfs_lookup_rec() cuando llegue a un
    // /proc/<pid>/... real; por ahora devolvemos el nodo del symlink
    // con is_symlink=1 y link_target relleno.
    process_t *p = process_current();
    if (!p)
      return NULL;
    vfs_node_t *n = (vfs_node_t *)kzalloc(sizeof(vfs_node_t));
    if (!n)
      return NULL;
    n->name[0] = 's';
    n->name[1] = 'e';
    n->name[2] = 'l';
    n->name[3] = 'f';
    n->name[4] = '\0';
    n->flags = VFS_FILE;
    n->is_symlink = 1;
    // "/proc/" + pid
    size_t o = 0;
    const char *pfx = "/proc/";
    while (*pfx && o + 1 < sizeof(n->link_target))
      n->link_target[o++] = *pfx++;
    o += kfmt_u64(n->link_target + o, sizeof(n->link_target) - o, p->pid);
    n->link_target[o] = '\0';
    return n;
  }

  return NULL;
}

static vfs_fs_ops_t procfs_fs_ops = {
    .lookup = procfs_lookup,
    .name = "procfs",
};

struct vfs_fs_ops *procfs_get_vfs_ops(void) {
  return (struct vfs_fs_ops *)&procfs_fs_ops;
}