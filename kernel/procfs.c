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

static size_t kfmt_i64(char *out, size_t cap, int64_t v) {
  if (v < 0) {
    if (cap < 2)
      return 0;
    out[0] = '-';
    size_t n = kfmt_u64(out + 1, cap - 1, (uint64_t)(-v));
    return n + 1;
  }
  return kfmt_u64(out, cap, (uint64_t)v);
}

static size_t kappend(char *buf, size_t len, size_t cap, const char *s) {
  while (*s && len + 1 < cap)
    buf[len++] = *s++;
  buf[len] = '\0';
  return len;
}

// ===========================================================================
// Estado del proceso en formato Linux.
//
//   R running        (RUNNING o READY)
//   S sleeping       (BLOCKED)
//   T stopped        (job control)
//   Z zombie
// ===========================================================================
static char procfs_state_char(const process_t *p, const char **desc_out) {
  if (p->is_zombie) {
    if (desc_out)
      *desc_out = "zombie";
    return 'Z';
  }
  if (p->stopped) {
    if (desc_out)
      *desc_out = "stopped";
    return 'T';
  }
  task_state_t ts = p->task ? p->task->state : TASK_RUNNING;
  if (ts == TASK_BLOCKED) {
    if (desc_out)
      *desc_out = "sleeping";
    return 'S';
  }
  if (desc_out)
    *desc_out = "running";
  return 'R';
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

// ---------------------------------------------------------------------------
// [3.3.b] /proc/loadavg.
//
// Formato Linux: "load1 load5 load15 running/total last_pid".
// Como no medimos load average, reportamos 0.00 en los tres. La pareja
// running/total es útil para top/busybox.
// ---------------------------------------------------------------------------
struct loadavg_ctx {
  uint32_t total;
  uint32_t running;
};

static int count_loadavg_cb(process_t *p, void *arg) {
  struct loadavg_ctx *c = (struct loadavg_ctx *)arg;
  c->total++;

  if (p->is_zombie || p->stopped)
    return 0;

  if (p->task &&
      (p->task->state == TASK_RUNNING || p->task->state == TASK_READY))
    c->running++;

  return 0;
}

static char *gen_loadavg(size_t *out_len) {
  const size_t CAP = 64;
  char *buf = (char *)kmalloc(CAP);
  if (!buf)
    return NULL;

  struct loadavg_ctx c = {0, 0};
  process_for_each(count_loadavg_cb, &c);

  size_t o = 0;
  o = kappend(buf, o, CAP, "0.00 0.00 0.00 ");

  if (c.running == 0)
    buf[o++] = '0';
  else
    o += kfmt_u64(buf + o, CAP - o, c.running);
  buf[o++] = '/';
  if (c.total == 0)
    buf[o++] = '0';
  else
    o += kfmt_u64(buf + o, CAP - o, c.total);

  // last_pid: no lo llevamos, ponemos 1.
  o = kappend(buf, o, CAP, " 1\n");

  buf[o] = '\0';
  *out_len = o;
  return buf;
}

// ===========================================================================
// [3.3.d] /proc/<pid>/*.
// ===========================================================================

// Genera contenido bajo process_lock. cb corre bajo lock: no debe dormir.
struct pid_ctx {
  uint32_t target_pid;
  char *buf;
  size_t cap;
  size_t len;
  int found;
};

// ---------------------------------------------------------------------------
// /proc/<pid>/stat: formato Linux de EXACTAMENTE 52 campos.
//
// Campos reales: pid, comm, state, ppid, pgrp, session, utime, stime.
// El resto van a 0. BusyBox `ps` los lee como columnas; contar mal
// desplaza todos los campos siguientes.
//
// Posiciones:
//   1 pid, 2 comm, 3 state, 4 ppid, 5 pgrp, 6 session,
//   7 tty_nr, 8 tpgid, 9 flags, 10 minflt, 11 cminflt, 12 majflt, 13 cmajflt,
//   14 utime, 15 stime,
//   16 cutime, 17 cstime, 18 priority, 19 nice, 20 num_threads,
//   21 itrealvalue, 22 starttime,
//   23 vsize, 24 rss,
//   25..52 (28 campos) relleno.
// ---------------------------------------------------------------------------
static int gen_pid_stat_cb(process_t *p, void *arg) {
  struct pid_ctx *c = (struct pid_ctx *)arg;
  if (p->pid != c->target_pid)
    return 0;

  char *buf = c->buf;
  size_t cap = c->cap;
  size_t o = 0;

  // 1: pid
  o += kfmt_u64(buf + o, cap - o, p->pid);
  o = kappend(buf, o, cap, " ");

  // 2: comm (entre paréntesis). Hasta 15 chars + paréntesis.
  buf[o++] = '(';
  size_t nl = strlen(p->name);
  if (nl > 15)
    nl = 15;
  for (size_t i = 0; i < nl && o + 2 < cap; i++)
    buf[o++] = p->name[i];
  buf[o++] = ')';
  o = kappend(buf, o, cap, " ");

  // 3: state
  char st = procfs_state_char(p, NULL);
  buf[o++] = st;
  o = kappend(buf, o, cap, " ");

  // 4: ppid
  o += kfmt_u64(buf + o, cap - o, p->ppid);
  o = kappend(buf, o, cap, " ");
  // 5: pgrp
  o += kfmt_u64(buf + o, cap - o, p->pgid);
  o = kappend(buf, o, cap, " ");
  // 6: session
  o += kfmt_u64(buf + o, cap - o, p->sid);
  // 7..13: tty_nr tpgid flags minflt cminflt majflt cmajflt (7 campos)
  o = kappend(buf, o, cap, " 0 0 0 0 0 0 0 ");
  // 14: utime (ticks)
  o += kfmt_u64(buf + o, cap - o, p->cpu_ticks_user);
  o = kappend(buf, o, cap, " ");
  // 15: stime
  o = kappend(buf, o, cap, "0 ");
  // 16..22: cutime cstime priority nice num_threads itrealvalue starttime
  // (7 campos). priority=20, nice=0, num_threads=1 como un proceso normal.
  o = kappend(buf, o, cap, "0 0 20 0 1 0 0 ");
  // 23: vsize (bytes). No lo medimos.
  o = kappend(buf, o, cap, "0 ");
  // 24: rss (pages). No lo medimos.
  o = kappend(buf, o, cap, "0 ");
  // 25..52: 28 campos más a 0.
  for (int i = 0; i < 28; i++)
    o = kappend(buf, o, cap, "0 ");
  // Quitar el último espacio colgante y cerrar con '\n'.
  if (o > 0 && buf[o - 1] == ' ')
    o--;
  buf[o++] = '\n';

  c->len = o;
  c->found = 1;
  return 1;
}

// ---------------------------------------------------------------------------
// /proc/<pid>/status: key:value multi-línea.
// ---------------------------------------------------------------------------
static int gen_pid_status_cb(process_t *p, void *arg) {
  struct pid_ctx *c = (struct pid_ctx *)arg;
  if (p->pid != c->target_pid)
    return 0;

  char *buf = c->buf;
  size_t cap = c->cap;
  size_t o = 0;

  const char *desc = "running";
  char st = procfs_state_char(p, &desc);

  o = kappend(buf, o, cap, "Name:\t");
  o = kappend(buf, o, cap, p->name);
  o = kappend(buf, o, cap, "\n");

  o = kappend(buf, o, cap, "State:\t");
  buf[o++] = st;
  o = kappend(buf, o, cap, " (");
  o = kappend(buf, o, cap, desc);
  o = kappend(buf, o, cap, ")\n");

  o = kappend(buf, o, cap, "Tgid:\t");
  o += kfmt_u64(buf + o, cap - o, p->pid);
  o = kappend(buf, o, cap, "\n");

  o = kappend(buf, o, cap, "Pid:\t");
  o += kfmt_u64(buf + o, cap - o, p->pid);
  o = kappend(buf, o, cap, "\n");

  o = kappend(buf, o, cap, "PPid:\t");
  o += kfmt_u64(buf + o, cap - o, p->ppid);
  o = kappend(buf, o, cap, "\n");

  // [3.4.b] Uid real/effective/saved/fs.
  o = kappend(buf, o, cap, "Uid:\t");
  o += kfmt_u64(buf + o, cap - o, p->uid);
  o = kappend(buf, o, cap, "\t");
  o += kfmt_u64(buf + o, cap - o, p->euid);
  o = kappend(buf, o, cap, "\t");
  o += kfmt_u64(buf + o, cap - o, p->suid);
  o = kappend(buf, o, cap, "\t");
  o += kfmt_u64(buf + o, cap - o, p->fsuid);
  o = kappend(buf, o, cap, "\n");

  o = kappend(buf, o, cap, "Gid:\t");
  o += kfmt_u64(buf + o, cap - o, p->gid);
  o = kappend(buf, o, cap, "\t");
  o += kfmt_u64(buf + o, cap - o, p->egid);
  o = kappend(buf, o, cap, "\t");
  o += kfmt_u64(buf + o, cap - o, p->sgid);
  o = kappend(buf, o, cap, "\t");
  o += kfmt_u64(buf + o, cap - o, p->fsgid);
  o = kappend(buf, o, cap, "\n");
  o = kappend(buf, o, cap, "Threads:\t1\n");
  o = kappend(buf, o, cap, "VmSize:\t0 kB\n");
  o = kappend(buf, o, cap, "VmRSS:\t0 kB\n");

  c->len = o;
  c->found = 1;
  return 1;
}

// ---------------------------------------------------------------------------
// /proc/<pid>/cmdline: argv[0]\0argv[1]\0...\0
// ---------------------------------------------------------------------------
static int gen_pid_cmdline_cb(process_t *p, void *arg) {
  struct pid_ctx *c = (struct pid_ctx *)arg;
  if (p->pid != c->target_pid)
    return 0;

  char *buf = c->buf;
  size_t cap = c->cap;
  size_t o = 0;

  if (p->argc == 0) {
    // Proceso sin argv: Linux devuelve un único \0.
    buf[0] = '\0';
    c->len = 1;
    c->found = 1;
    return 1;
  }
  for (int i = 0; i < p->argc && i < PROCESS_ARGV_MAX; i++) {
    if (!p->argv[i])
      break;
    size_t n = strlen(p->argv[i]);
    if (o + n + 1 > cap)
      n = (o + 1 < cap) ? (cap - o - 1) : 0;
    memcpy(buf + o, p->argv[i], n);
    o += n;
    buf[o++] = '\0';
  }
  c->len = o;
  c->found = 1;
  return 1;
}

// ---------------------------------------------------------------------------
// /proc/<pid>/comm: nombre del proceso, terminado en '\n', hasta 15
// chars + '\n'. BusyBox `ps` prefiere comm sobre cmdline cuando está
// disponible.
// ---------------------------------------------------------------------------
static int gen_pid_comm_cb(process_t *p, void *arg) {
  struct pid_ctx *c = (struct pid_ctx *)arg;
  if (p->pid != c->target_pid)
    return 0;

  size_t n = strlen(p->name);
  if (n > 15)
    n = 15;
  if (n + 1 > c->cap)
    n = c->cap - 1;
  memcpy(c->buf, p->name, n);
  c->buf[n] = '\n';
  c->len = n + 1;
  c->found = 1;
  return 1;
}

// ---------------------------------------------------------------------------
// /proc/<pid>/statm: 7 enteros en una línea:
//   size resident shared text lib data dt
// Todo en páginas. Los dejamos a 0 hasta medir RSS.
// ---------------------------------------------------------------------------
static int gen_pid_statm_cb(process_t *p, void *arg) {
  struct pid_ctx *c = (struct pid_ctx *)arg;
  if (p->pid != c->target_pid)
    return 0;

  size_t o = kappend(c->buf, 0, c->cap, "0 0 0 0 0 0 0\n");
  c->len = o;
  c->found = 1;
  return 1;
}

// Envoltorio común: reserva buffer, itera bajo lock, devuelve el
// contenido o NULL si el pid no existe.
static char *gen_pid_file(uint32_t pid, size_t *out_len, process_iter_cb_t cb) {
  const size_t CAP = 2048;
  char *buf = (char *)kmalloc(CAP);
  if (!buf)
    return NULL;
  struct pid_ctx c = {.target_pid = pid, .buf = buf, .cap = CAP};
  process_for_each(cb, &c);
  if (!c.found) {
    kfree(buf);
    return NULL;
  }
  *out_len = c.len;
  return buf;
}

// ===========================================================================
// Parser de "<digits>"
// ===========================================================================
static int parse_pid(const char *s, uint32_t *out) {
  if (!s || !*s)
    return 0;
  uint32_t v = 0;
  while (*s) {
    if (*s < '0' || *s > '9')
      return 0;
    v = v * 10 + (uint32_t)(*s - '0');
    s++;
  }
  *out = v;
  return 1;
}

// ---------------------------------------------------------------------------
// [3.3.c] /proc/mounts.
// ---------------------------------------------------------------------------
struct mounts_ctx {
  char *buf;
  size_t len;
  size_t cap;
};

static int append_mount_line(const char *path, const char *fs_name, int is_bind,
                             const char *bind_source, void *arg) {
  struct mounts_ctx *mc = (struct mounts_ctx *)arg;
  const char *opts = (strcmp(fs_name, "tarfs") == 0) ? "ro" : "rw";

  mc->len = kappend(mc->buf, mc->len, mc->cap, "none ");
  mc->len = kappend(mc->buf, mc->len, mc->cap, path);
  mc->len = kappend(mc->buf, mc->len, mc->cap, " ");
  mc->len = kappend(mc->buf, mc->len, mc->cap, fs_name);
  mc->len = kappend(mc->buf, mc->len, mc->cap, " ");
  mc->len = kappend(mc->buf, mc->len, mc->cap, opts);
  mc->len = kappend(mc->buf, mc->len, mc->cap, " 0 0\n");

  if (is_bind && bind_source && bind_source[0]) {
    mc->len = kappend(mc->buf, mc->len, mc->cap, "# bind: ");
    mc->len = kappend(mc->buf, mc->len, mc->cap, path);
    mc->len = kappend(mc->buf, mc->len, mc->cap, " <- ");
    mc->len = kappend(mc->buf, mc->len, mc->cap, bind_source);
    mc->len = kappend(mc->buf, mc->len, mc->cap, "\n");
  }
  return 0;
}

static char *gen_mounts(size_t *out_len) {
  const size_t CAP = 4096;
  char *buf = (char *)kmalloc(CAP);
  if (!buf)
    return NULL;
  buf[0] = '\0';
  struct mounts_ctx mc = {.buf = buf, .len = 0, .cap = CAP};
  vfs_for_each_mount(append_mount_line, &mc);
  *out_len = mc.len;
  return buf;
}

// ===========================================================================
// readdir: solo el directorio raíz tiene entradas estáticas.
// ===========================================================================
static const char *procfs_root_entries[] = {
    "uptime", "version", "meminfo", "stat", "self", "mounts", "loadavg",
};
#define PROCFS_N_ROOT_ENTRIES                                                  \
  (sizeof(procfs_root_entries) / sizeof(procfs_root_entries[0]))

struct procfs_root_iter {
  uint64_t target;
  uint64_t current;
  vfs_dirent_t *out;
  int found;
};

static int procfs_root_pid_cb(process_t *p, void *arg) {
  struct procfs_root_iter *it = (struct procfs_root_iter *)arg;
  if (p->is_zombie)
    return 0;
  if (it->current == it->target) {
    int n = 0;
    char tmp[12];
    uint32_t v = p->pid;
    if (v == 0)
      tmp[n++] = '0';
    while (v > 0) {
      tmp[n++] = (char)('0' + (v % 10));
      v /= 10;
    }
    int i = 0;
    while (n > 0 && i < (int)sizeof(it->out->name) - 1)
      it->out->name[i++] = tmp[--n];
    it->out->name[i] = '\0';
    it->out->type = VFS_DIRECTORY;
    it->out->size = 0;
    it->found = 1;
    return 1;
  }
  it->current++;
  return 0;
}

static const char *procfs_pid_entries[] = {
    "stat", "status", "cmdline", "comm", "statm",
};
#define PROCFS_N_PID_ENTRIES                                                   \
  (sizeof(procfs_pid_entries) / sizeof(procfs_pid_entries[0]))

static int procfs_readdir(vfs_node_t *dir, uint64_t index, vfs_dirent_t *out) {
  if (!dir || !out)
    return -EINVAL;
  if (!(dir->flags & VFS_DIRECTORY))
    return -ENOTDIR;

  // [3.3.d] ¿Es /proc raíz o /proc/<pid>?
  uint32_t pid;
  if (strncmp(dir->name, "/proc/", 6) == 0 && parse_pid(dir->name + 6, &pid)) {
    if (index >= PROCFS_N_PID_ENTRIES) {
      out->name[0] = '\0';
      out->type = 0;
      out->size = 0;
      return 0;
    }
    const char *n = procfs_pid_entries[index];
    size_t l = strlen(n);
    memcpy(out->name, n, l);
    out->name[l] = '\0';
    out->type = VFS_FILE;
    out->size = 0;
    return 0;
  }

  if (index < PROCFS_N_ROOT_ENTRIES) {
    const char *n = procfs_root_entries[index];
    size_t l = strlen(n);
    if (l >= sizeof(out->name))
      l = sizeof(out->name) - 1;
    memcpy(out->name, n, l);
    out->name[l] = '\0';
    out->type = VFS_FILE;
    out->size = 0;
    return 0;
  }

  struct procfs_root_iter it = {
      .target = index - PROCFS_N_ROOT_ENTRIES,
      .current = 0,
      .out = out,
      .found = 0,
  };
  process_for_each(procfs_root_pid_cb, &it);
  if (!it.found) {
    out->name[0] = '\0';
    out->type = 0;
    out->size = 0;
  }
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
  n->mode = S_IFDIR | 0555;
  n->uid = 0;
  n->gid = 0;
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
  n->mode = S_IFREG | 0444;
  n->uid = 0;
  n->gid = 0;
  return n;
}

// ===========================================================================
// lookup
// ===========================================================================
static vfs_node_t *procfs_lookup(void *fs_priv, const char *path) {
  (void)fs_priv;
  if (!path || path[0] != '/')
    return NULL;

  if (path[1] == '\0')
    return make_root_node();

  const char *name = path + 1;

  const char *slash = NULL;
  for (const char *p = name; *p; p++) {
    if (*p == '/') {
      slash = p;
      break;
    }
  }

  if (slash) {
    // <pid>/<file>
    char pidbuf[16];
    size_t plen = (size_t)(slash - name);
    if (plen == 0 || plen >= sizeof(pidbuf))
      return NULL;
    memcpy(pidbuf, name, plen);
    pidbuf[plen] = '\0';
    uint32_t pid;
    if (strcmp(pidbuf, "self") == 0) {
      process_t *cur = process_current();
      if (!cur)
        return NULL;
      pid = cur->pid;
    } else if (!parse_pid(pidbuf, &pid)) {
      return NULL;
    }

    const char *file = slash + 1;
    for (const char *p = file; *p; p++)
      if (*p == '/')
        return NULL;

    size_t len = 0;
    char *buf = NULL;
    if (strcmp(file, "stat") == 0) {
      buf = gen_pid_file(pid, &len, gen_pid_stat_cb);
      return buf ? make_file_node("stat", buf, len) : NULL;
    }
    if (strcmp(file, "status") == 0) {
      buf = gen_pid_file(pid, &len, gen_pid_status_cb);
      return buf ? make_file_node("status", buf, len) : NULL;
    }
    if (strcmp(file, "cmdline") == 0) {
      buf = gen_pid_file(pid, &len, gen_pid_cmdline_cb);
      return buf ? make_file_node("cmdline", buf, len) : NULL;
    }
    if (strcmp(file, "comm") == 0) {
      buf = gen_pid_file(pid, &len, gen_pid_comm_cb);
      return buf ? make_file_node("comm", buf, len) : NULL;
    }
    if (strcmp(file, "statm") == 0) {
      buf = gen_pid_file(pid, &len, gen_pid_statm_cb);
      return buf ? make_file_node("statm", buf, len) : NULL;
    }
    return NULL;
  }

  // Un solo componente.
  if (strcmp(name, "uptime") == 0) {
    size_t len = 0;
    char *b = gen_uptime(&len);
    return make_file_node("uptime", b, len);
  }
  if (strcmp(name, "version") == 0) {
    size_t len = 0;
    char *b = gen_version(&len);
    return make_file_node("version", b, len);
  }
  if (strcmp(name, "meminfo") == 0) {
    size_t len = 0;
    char *b = gen_meminfo(&len);
    return make_file_node("meminfo", b, len);
  }
  if (strcmp(name, "stat") == 0) {
    size_t len = 0;
    char *b = gen_stat(&len);
    return make_file_node("stat", b, len);
  }
  if (strcmp(name, "mounts") == 0) {
    size_t len = 0;
    char *b = gen_mounts(&len);
    return make_file_node("mounts", b, len);
  }
  if (strcmp(name, "loadavg") == 0) {
    size_t len = 0;
    char *b = gen_loadavg(&len);
    return make_file_node("loadavg", b, len);
  }
  if (strcmp(name, "self") == 0) {
    process_t *p = process_current();
    if (!p)
      return NULL;
    vfs_node_t *n = (vfs_node_t *)kzalloc(sizeof(vfs_node_t));
    if (!n)
      return NULL;
    memcpy(n->name, "self", 5);
    n->flags = VFS_FILE;
    n->is_symlink = 1;
    size_t o = 0;
    const char *pfx = "/proc/";
    while (*pfx && o + 1 < sizeof(n->link_target))
      n->link_target[o++] = *pfx++;
    o += kfmt_u64(n->link_target + o, sizeof(n->link_target) - o, p->pid);
    n->link_target[o] = '\0';
    n->mode = S_IFLNK | 0777;
    n->uid = 0;
    n->gid = 0;
    return n;
  }

  uint32_t pid;
  if (parse_pid(name, &pid)) {
    process_t *p = process_find_by_pid(pid);
    if (!p)
      return NULL;
    vfs_node_t *n = (vfs_node_t *)kzalloc(sizeof(vfs_node_t));
    if (!n)
      return NULL;
    size_t l = strlen(name);
    if (l >= sizeof(n->name))
      l = sizeof(n->name) - 1;
    memcpy(n->name, name, l);
    n->name[l] = '\0';
    n->flags = VFS_DIRECTORY;
    n->ops = &procfs_dir_ops;
    n->mode = S_IFDIR | 0555;
    n->uid = 0;
    n->gid = 0;
    return n;
  }

  return NULL;
}

static int procfs_statfs(void *fs_priv, struct vfs_statfs *out) {
  (void)fs_priv;
  memset(out, 0, sizeof(*out));
  out->f_type = 0x9fa0; // PROC_SUPER_MAGIC
  out->f_bsize = 4096;
  out->f_frsize = 4096;
  out->f_namelen = 255;
  return 0;
}

static vfs_fs_ops_t procfs_fs_ops = {
    .lookup = procfs_lookup,
    .statfs = procfs_statfs,
    .name = "procfs",
};

struct vfs_fs_ops *procfs_get_vfs_ops(void) {
  return (struct vfs_fs_ops *)&procfs_fs_ops;
}