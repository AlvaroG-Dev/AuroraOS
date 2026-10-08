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
#include "block.h"
#include "cpu.h"
#include "heap.h"
#include "klog.h"
#include "pf.h" // vma_t, VMA_ELF, VMA_STACK, VMA_FILE, VMA_ANON
#include "pmm.h"
#include "process.h"
#include "sched.h"
#include "string.h"
#include "swap.h"
#include "sysctl.h"
#include "sysfs.h" // [FIX] sysfs_pci_count / sysfs_pci_get
#include "time.h"
#include "uaccess.h"
#include "vfs.h"
#include <stddef.h>
#include <stdint.h>

// ===========================================================================
// Nodo priv: buffer de contenido + longitud.
// ===========================================================================
typedef struct {
  char *buf;
  size_t len;
} procfs_data_t;

extern uint64_t fs_dev_id(const void *fs);

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
  LOG_TRACE("[PROCFS-OPEN] path='%s' flags=0x%x", node ? node->name : "?",
            flags);
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

typedef struct {
  char *buf;
  size_t len;
  const sysctl_entry_t *entry; // solo lo usa procfs_sysctl_write
} procfs_sysctl_data_t;

static int64_t procfs_sysctl_write(vfs_node_t *node, uint64_t offset,
                                   size_t size, const void *buf) {
  if (!node || !node->priv || !buf)
    return -EINVAL;
  procfs_sysctl_data_t *sd = (procfs_sysctl_data_t *)node->priv;
  if (!sd->entry || !sd->entry->write)
    return -EIO;

  // Quitar '\n' final si lo hay (Linux sysctl no lo incluye)
  size_t n = size;
  if (n > 0 && ((const char *)buf)[n - 1] == '\n')
    n--;

  int rc = sd->entry->write((const char *)buf, n);
  if (rc != 0)
    return rc;
  return (int64_t)size;
}

static int procfs_sysctl_close(vfs_node_t *node) {
  if (!node)
    return 0;
  procfs_sysctl_data_t *sd = (procfs_sysctl_data_t *)node->priv;
  if (sd) {
    if (sd->buf)
      kfree(sd->buf);
    kfree(sd);
    node->priv = NULL;
  }
  return 0;
}

// [FIX] procfs_open rechaza cualquier O_WRONLY (comportamiento correcto
// para los ficheros read-only de procfs). Los nodos sysctl rw deben
// usar este open permisivo, que deja pasar lectura y escritura.
static int procfs_sysctl_open(vfs_node_t *node, int flags) {
  (void)node;
  (void)flags;
  return 0;
}

static vfs_ops_t procfs_sysctl_rw_ops = {
    .read = procfs_read,
    .write = procfs_sysctl_write,
    .open = procfs_sysctl_open, // ← antes procfs_open
    .close = procfs_sysctl_close,
};

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

// Formato hex con ancho mínimo (min_width dígitos, relleno con '0').
// Linux usa "%08lx" para direcciones: mínimo 8, natural arriba de 8.
static size_t kfmt_hex_min(char *out, size_t cap, uint64_t v, int min_width) {
  static const char hex[] = "0123456789abcdef";
  char tmp[16];
  int n = 0;
  if (v == 0) {
    tmp[n++] = '0';
  } else {
    while (v && n < 16) {
      tmp[n++] = hex[v & 0xF];
      v >>= 4;
    }
  }
  while (n < min_width && n < 16)
    tmp[n++] = '0';
  size_t o = 0;
  while (n > 0 && o + 1 < cap)
    out[o++] = tmp[--n];
  out[o] = '\0';
  return o;
}

static size_t __attribute__((unused)) kfmt_i64(char *out, size_t cap,
                                               int64_t v) {
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

  uint64_t total_pages = pmm_total_usable_pages(); // [FIX B]
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
  APPEND_KV("SwapTotal:      ", swap_total_bytes() / 1024);
  APPEND_KV("SwapFree:       ", swap_free_bytes() / 1024);
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
// /proc/loadavg.
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

  o = kappend(buf, o, CAP, " 1\n");

  buf[o] = '\0';
  *out_len = o;
  return buf;
}

// ===========================================================================
// /proc/<pid>/*.
// ===========================================================================
struct pid_ctx {
  uint32_t target_pid;
  char *buf;
  size_t cap;
  size_t len;
  int found;
};

static int gen_pid_stat_cb(process_t *p, void *arg) {
  struct pid_ctx *c = (struct pid_ctx *)arg;
  if (p->pid != c->target_pid)
    return 0;

  char *buf = c->buf;
  size_t cap = c->cap;
  size_t o = 0;

  o += kfmt_u64(buf + o, cap - o, p->pid);
  o = kappend(buf, o, cap, " ");

  buf[o++] = '(';
  size_t nl = strlen(p->name);
  if (nl > 15)
    nl = 15;
  for (size_t i = 0; i < nl && o + 2 < cap; i++)
    buf[o++] = p->name[i];
  buf[o++] = ')';
  o = kappend(buf, o, cap, " ");

  char st = procfs_state_char(p, NULL);
  buf[o++] = st;
  o = kappend(buf, o, cap, " ");

  o += kfmt_u64(buf + o, cap - o, p->ppid);
  o = kappend(buf, o, cap, " ");
  o += kfmt_u64(buf + o, cap - o, p->pgid);
  o = kappend(buf, o, cap, " ");
  o += kfmt_u64(buf + o, cap - o, p->sid);
  o = kappend(buf, o, cap, " 0 0 0 0 0 0 0 ");
  o += kfmt_u64(buf + o, cap - o, p->cpu_ticks_user);
  o = kappend(buf, o, cap, " ");
  o = kappend(buf, o, cap, "0 ");
  o = kappend(buf, o, cap, "0 0 20 0 1 0 0 ");
  o = kappend(buf, o, cap, "0 ");
  o = kappend(buf, o, cap, "0 ");
  for (int i = 0; i < 28; i++)
    o = kappend(buf, o, cap, "0 ");
  if (o > 0 && buf[o - 1] == ' ')
    o--;
  buf[o++] = '\n';

  c->len = o;
  c->found = 1;
  return 1;
}

// ---------------------------------------------------------------------------
// [2.1] /proc/vmstat — vmstat(8) lo lee junto con /proc/stat y /proc/meminfo.
// Sin él: "vmstat: Unable to create vmstat structure".
//
// Solo rellenamos con valores reales lo que podemos sacar de PMM y swap.
// El resto a 0. vmstat no se queja si las claves existen.
// ---------------------------------------------------------------------------
static char *gen_vmstat(size_t *out_len) {
  const size_t CAP = 8 * 1024;
  char *buf = (char *)kmalloc(CAP);
  if (!buf)
    return NULL;

  uint64_t free_pages = pmm_free_pages_count();
  uint64_t total_pages = pmm_total_usable_pages();
  uint64_t used_pages =
      (total_pages > free_pages) ? (total_pages - free_pages) : 0;

  size_t o = 0;
#define VM(key, val)                                                           \
  do {                                                                         \
    o = kappend(buf, o, CAP, key " ");                                         \
    o += kfmt_u64(buf + o, CAP - o, (val));                                    \
    buf[o++] = '\n';                                                           \
  } while (0)

  // --- Zonas / free lists ---
  VM("nr_free_pages", free_pages);
  VM("nr_zone_inactive_anon", 0);
  VM("nr_zone_active_anon", 0);
  VM("nr_zone_inactive_file", 0);
  VM("nr_zone_active_file", 0);
  VM("nr_zone_unevictable", 0);
  VM("nr_zone_write_pending", 0);
  VM("nr_mlock", 0);
  VM("nr_page_table_pages", 0);
  VM("nr_kernel_stack", 0);
  VM("nr_bounce", 0);
  VM("nr_zspages", 0);
  VM("nr_free_cma", 0);

  // --- NUMA (mononodo ficticio) ---
  VM("numa_hit", total_pages);
  VM("numa_miss", 0);
  VM("numa_foreign", 0);
  VM("numa_interleave", 0);
  VM("numa_local", total_pages);
  VM("numa_other", 0);

  // --- LRU / slab ---
  VM("nr_inactive_anon", 0);
  VM("nr_active_anon", 0);
  VM("nr_inactive_file", 0);
  VM("nr_active_file", 0);
  VM("nr_unevictable", 0);
  VM("nr_slab_reclaimable", 0);
  VM("nr_slab_unreclaimable", 0);
  VM("nr_isolated_anon", 0);
  VM("nr_isolated_file", 0);

  // --- Working set / anon / file pages ---
  VM("workingset_nodes", 0);
  VM("workingset_refault", 0);
  VM("workingset_activate", 0);
  VM("workingset_restore", 0);
  VM("workingset_nodereclaim", 0);
  VM("nr_anon_pages", 0);
  VM("nr_mapped", 0);
  VM("nr_file_pages", 0);
  VM("nr_dirty", 0);
  VM("nr_writeback", 0);
  VM("nr_writeback_temp", 0);
  VM("nr_shmem", 0);
  VM("nr_shmem_hugepages", 0);
  VM("nr_shmem_pmdmapped", 0);
  VM("nr_file_hugepages", 0);
  VM("nr_file_pmdmapped", 0);
  VM("nr_anon_transparent_hugepages", 0);

  // --- Reclaim / scan ---
  VM("nr_vmscan_write", 0);
  VM("nr_vmscan_immediate_reclaim", 0);
  VM("nr_dirtied", 0);
  VM("nr_written", 0);
  VM("nr_kernel_misc_reclaimable", 0);
  VM("nr_foll_pin_acquired", 0);
  VM("nr_foll_pin_released", 0);

  // --- Page in/out, swap ---
  VM("pgpgin", 0);
  VM("pgpgout", 0);
  VM("pswpin", 0);
  VM("pswpout", 0);

  // --- Alloc / free paths (per zone) ---
  VM("pgalloc_dma", 0);
  VM("pgalloc_dma32", 0);
  VM("pgalloc_normal", total_pages);
  VM("pgalloc_movable", 0);
  VM("allocstall_dma", 0);
  VM("allocstall_dma32", 0);
  VM("allocstall_normal", 0);
  VM("allocstall_movable", 0);
  VM("pgskip_dma", 0);
  VM("pgskip_dma32", 0);
  VM("pgskip_normal", 0);
  VM("pgskip_movable", 0);
  VM("pgfree", total_pages);
  VM("pgactivate", 0);
  VM("pgdeactivate", 0);
  VM("pglazyfree", 0);
  VM("pglazyfreed", 0);
  VM("pgrefill", 0);
  VM("pgreuse", 0);
  VM("pgsteal_kswapd", 0);
  VM("pgsteal_direct", 0);
  VM("pgscan_kswapd", 0);
  VM("pgscan_direct", 0);
  VM("pgscan_direct_throttle", 0);
  VM("pgscan_anon", 0);
  VM("pgscan_file", 0);
  VM("pgsteal_anon", 0);
  VM("pgsteal_file", 0);

  // --- Kswapd / reclaim events ---
  VM("zone_reclaim_failed", 0);
  VM("pginodesteal", 0);
  VM("slabs_scanned", 0);
  VM("kswapd_inodesteal", 0);
  VM("kswapd_low_wmark_hit_quickly", 0);
  VM("kswapd_high_wmark_hit_quickly", 0);
  VM("pageoutrun", 0);
  VM("allocstall", 0);
  VM("pgrotated", 0);

  // --- Debug / admin ---
  VM("drop_pagecache", 0);
  VM("drop_slab", 0);
  VM("oom_kill", 0);

  // --- Faults (los que vmstat muestra como bi/bo y si/so) ---
  VM("pgmajfault", 0);
  VM("pgfault", used_pages); // aproximación: 1 fault por página mapeada

  // --- THP ---
  VM("thp_fault_alloc", 0);
  VM("thp_collapse_alloc", 0);
  VM("thp_collapse_alloc_failed", 0);
  VM("thp_file_alloc", 0);
  VM("thp_file_mapped", 0);
  VM("thp_split_page", 0);
  VM("thp_split_page_failed", 0);
  VM("thp_deferred_split_page", 0);
  VM("thp_split_pmd", 0);
  VM("thp_split_pud", 0);
  VM("thp_zero_page_alloc", 0);
  VM("thp_zero_page_alloc_failed", 0);
  VM("thp_swpout", 0);
  VM("thp_swpout_fallback", 0);

  // --- Balloon (virtio) ---
  VM("balloon_inflate", 0);
  VM("balloon_deflate", 0);

  // --- Swap readahead ---
  VM("swap_ra", 0);
  VM("swap_ra_hit", 0);

  // --- Direct map splits ---
  VM("direct_map_level2_splits", 0);
  VM("direct_map_level3_splits", 0);

  // --- Compact (fragmentación) ---
  VM("compact_migrate_scanned", 0);
  VM("compact_free_scanned", 0);
  VM("compact_isolated", 0);
  VM("compact_stall", 0);
  VM("compact_fail", 0);
  VM("compact_success", 0);

  // --- KSM ---
  VM("ksm_rmap_items", 0);
  VM("ksm_rmap_pages", 0);
  VM("ksm_pages_sharing", 0);
  VM("ksm_pages_unshared", 0);

#undef VM

  buf[o] = '\0';
  *out_len = o;
  return buf;
}

static int gen_pid_status_cb(process_t *p, void *arg) {
  struct pid_ctx *c = (struct pid_ctx *)arg;
  if (p->pid != c->target_pid)
    return 0;

  char *buf = c->buf;
  size_t cap = c->cap;
  size_t o = 0;

  const char *desc = "running";
  char st = procfs_state_char(p, &desc);

  // --- Memoria agregada por VMA ---
  uint64_t vm_size = 0, vm_rss = 0;
  uint64_t vm_data = 0, vm_stk = 0, vm_exe = 0, vm_lib = 0;
  uint64_t *pml4 = p->pml4_phys ? (uint64_t *)phys_to_virt(p->pml4_phys) : NULL;

  for (vma_t *v = p->vma_list; v; v = v->next) {
    uint64_t sz = v->end - v->start;
    vm_size += sz;
    if (pml4) {
      uint64_t present = 0;
      for (uint64_t pg = v->start; pg < v->end; pg += PAGE_SIZE) {
        if (paging_get_phys_in(pml4, pg))
          present++;
      }
      vm_rss += present * PAGE_SIZE;
    }
    switch (v->type) {
    case VMA_STACK:
      vm_stk += sz;
      break;
    case VMA_ELF:
      vm_exe += sz;
      break;
    case VMA_FILE:
      vm_lib += sz;
      break;
    default:
      vm_data += sz;
      break;
    }
  }

  int fds_open = 0;
  for (int i = 0; i < MAX_PROCESS_FDS; i++)
    if (p->fds[i])
      fds_open++;

  uint64_t sig_pnd = __atomic_load_n(&p->pending_signals, __ATOMIC_ACQUIRE);
  uint64_t sig_blk = p->blocked_signals;

  uint64_t cpu_mask = 0;
  for (int i = 0; i < MAX_CPUS; i++)
    cpu_mask |= (1ULL << i);

  // ===== Name / Umask =====
  o = kappend(buf, o, cap, "Name:\t");
  o = kappend(buf, o, cap, p->name);
  buf[o++] = '\n';

  o = kappend(buf, o, cap, "Umask:\t");
  {
    char tmp[8];
    tmp[0] = '0';
    tmp[1] = '0' + ((p->umask >> 6) & 7);
    tmp[2] = '0' + ((p->umask >> 3) & 7);
    tmp[3] = '0' + (p->umask & 7);
    tmp[4] = '\0';
    o = kappend(buf, o, cap, tmp);
    buf[o++] = '\n';
  }

  // ===== State =====
  o = kappend(buf, o, cap, "State:\t");
  buf[o++] = st;
  o = kappend(buf, o, cap, " (");
  o = kappend(buf, o, cap, desc);
  o = kappend(buf, o, cap, ")\n");

  o = kappend(buf, o, cap, "Tgid:\t");
  o += kfmt_u64(buf + o, cap - o, p->pid);
  buf[o++] = '\n';

  o = kappend(buf, o, cap, "Pid:\t");
  o += kfmt_u64(buf + o, cap - o, p->pid);
  buf[o++] = '\n';

  o = kappend(buf, o, cap, "PPid:\t");
  o += kfmt_u64(buf + o, cap - o, p->ppid);
  buf[o++] = '\n';

  o = kappend(buf, o, cap, "TracerPid:\t0\n");

  // ===== Uid / Gid =====
  o = kappend(buf, o, cap, "Uid:\t");
  o += kfmt_u64(buf + o, cap - o, p->uid);
  buf[o++] = '\t';
  o += kfmt_u64(buf + o, cap - o, p->euid);
  buf[o++] = '\t';
  o += kfmt_u64(buf + o, cap - o, p->suid);
  buf[o++] = '\t';
  o += kfmt_u64(buf + o, cap - o, p->fsuid);
  buf[o++] = '\n';

  o = kappend(buf, o, cap, "Gid:\t");
  o += kfmt_u64(buf + o, cap - o, p->gid);
  buf[o++] = '\t';
  o += kfmt_u64(buf + o, cap - o, p->egid);
  buf[o++] = '\t';
  o += kfmt_u64(buf + o, cap - o, p->sgid);
  buf[o++] = '\t';
  o += kfmt_u64(buf + o, cap - o, p->fsgid);
  buf[o++] = '\n';

  // ===== FDSize =====
  o = kappend(buf, o, cap, "FDSize:\t");
  o += kfmt_u64(buf + o, cap - o, fds_open);
  buf[o++] = '\n';

  // ===== Groups =====
  o = kappend(buf, o, cap, "Groups:\t");
  for (int i = 0; i < p->ngroups && i < NGROUPS_MAX; i++) {
    o += kfmt_u64(buf + o, cap - o, p->groups[i]);
    if (i + 1 < p->ngroups)
      buf[o++] = ' ';
  }
  buf[o++] = '\n';

  // ===== Memoria =====
#define KB_LINE(key, bytes)                                                    \
  do {                                                                         \
    o = kappend(buf, o, cap, key);                                             \
    o += kfmt_u64(buf + o, cap - o, (uint64_t)((bytes) / 1024));               \
    o = kappend(buf, o, cap, " kB\n");                                         \
  } while (0)

  KB_LINE("VmPeak:\t", vm_size);
  KB_LINE("VmSize:\t", vm_size);
  o = kappend(buf, o, cap, "VmLck:\t0 kB\n");
  o = kappend(buf, o, cap, "VmPin:\t0 kB\n");
  KB_LINE("VmHWM:\t", vm_rss);
  KB_LINE("VmRSS:\t", vm_rss);
  KB_LINE("RssAnon:\t", vm_data);
  KB_LINE("RssFile:\t", vm_lib);
  o = kappend(buf, o, cap, "RssShmem:\t0 kB\n");
  KB_LINE("VmData:\t", vm_data);
  KB_LINE("VmStk:\t", vm_stk);
  KB_LINE("VmExe:\t", vm_exe);
  KB_LINE("VmLib:\t", vm_lib);
  o = kappend(buf, o, cap, "VmPTE:\t0 kB\n");
  o = kappend(buf, o, cap, "VmSwap:\t0 kB\n");
#undef KB_LINE

  // ===== Threads =====
  o = kappend(buf, o, cap, "Threads:\t1\n");

  // ===== Señales =====
  int n_pnd = 0;
  for (uint64_t m = sig_pnd; m; m >>= 1)
    n_pnd += (int)(m & 1);

  o = kappend(buf, o, cap, "SigQ:\t");
  o += kfmt_u64(buf + o, cap - o, (uint64_t)n_pnd);
  buf[o++] = '/';
  o += kfmt_u64(buf + o, cap - o, 1024);
  buf[o++] = '\n';

#define MASK_LINE(key, val)                                                    \
  do {                                                                         \
    o = kappend(buf, o, cap, key);                                             \
    o += kfmt_hex_min(buf + o, cap - o, (val), 16);                            \
    buf[o++] = '\n';                                                           \
  } while (0)

  MASK_LINE("SigPnd:\t", sig_pnd);
  MASK_LINE("ShdPnd:\t", 0);
  MASK_LINE("SigBlk:\t", sig_blk);
  MASK_LINE("SigIgn:\t", 0);
  MASK_LINE("SigCgt:\t", 0);
#undef MASK_LINE

  // ===== Cpus_allowed =====
  o = kappend(buf, o, cap, "Cpus_allowed:\t");
  o += kfmt_hex_min(buf + o, cap - o, cpu_mask, 1);
  buf[o++] = '\n';

  o = kappend(buf, o, cap, "Cpus_allowed_list:\t");
  {
    int hi = 0;
    for (int i = 0; i < MAX_CPUS; i++)
      if (cpu_mask & (1ULL << i))
        hi = i;
    o += kfmt_u64(buf + o, cap - o, 0);
    if (hi > 0) {
      buf[o++] = '-';
      o += kfmt_u64(buf + o, cap - o, (uint64_t)hi);
    }
    buf[o++] = '\n';
  }

  o = kappend(buf, o, cap, "Mems_allowed:\t00000001\n");
  o = kappend(buf, o, cap, "Mems_allowed_list:\t0\n");

  o = kappend(buf, o, cap, "voluntary_ctxt_switches:\t0\n");
  o = kappend(buf, o, cap, "nonvoluntary_ctxt_switches:\t0\n");

  c->len = o;
  c->found = 1;
  return 1;
}

// [FIX] p->argv es un array, nunca es NULL. El check va sobre argv[0].
static int gen_pid_cmdline_cb(process_t *p, void *arg) {
  struct pid_ctx *c = (struct pid_ctx *)arg;
  if (p->pid != c->target_pid)
    return 0;

  char *buf = c->buf;
  size_t cap = c->cap;
  size_t o = 0;

  if (p->argc > 0 && p->argv[0]) {
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
  } else {
    size_t n = strlen(p->name);
    if (n + 1 > cap)
      n = cap - 1;
    memcpy(buf, p->name, n);
    buf[n] = '\0';
    o = n + 1;
  }

  c->len = o;
  c->found = 1;
  return 1;
}

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

static int gen_pid_statm_cb(process_t *p, void *arg) {
  struct pid_ctx *c = (struct pid_ctx *)arg;
  if (p->pid != c->target_pid)
    return 0;

  size_t o = kappend(c->buf, 0, c->cap, "0 0 0 0 0 0 0\n");
  c->len = o;
  c->found = 1;
  return 1;
}

static char *gen_pid_file(uint32_t pid, size_t *out_len, process_iter_cb_t cb) {
  const size_t CAP = 4096;
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
// [FIX] /proc/bus/pci/devices — enumeración clásica de dispositivos PCI.
//
// Formato Linux:
//   <devfn:04x>\t<vendor:04x><device:04x>\t<irq:x>\t<BAR0>..<ROM>
// donde devfn = (bus << 8) | (slot << 3) | func.
// ===========================================================================
static char *gen_proc_bus_pci_devices(size_t *out_len) {
  const size_t CAP = 4096;
  char *buf = (char *)kmalloc(CAP);
  if (!buf)
    return NULL;
  static const char HEX[] = "0123456789abcdef";
  size_t o = 0;

  int n = sysfs_pci_count();
  for (int i = 0; i < n; i++) {
    const struct sysfs_pci_info *d = sysfs_pci_get(i);
    if (!d)
      continue;
    if (o + 64 > CAP)
      break;

    uint32_t devfn = ((uint32_t)d->bus << 8) | ((uint32_t)d->slot << 3) |
                     (uint32_t)(d->func & 0x7);

    // devfn: 4 hex
    buf[o++] = HEX[(devfn >> 12) & 0xF];
    buf[o++] = HEX[(devfn >> 8) & 0xF];
    buf[o++] = HEX[(devfn >> 4) & 0xF];
    buf[o++] = HEX[devfn & 0xF];
    buf[o++] = '\t';

    // vendor + device: 8 hex
    buf[o++] = HEX[(d->vendor >> 12) & 0xF];
    buf[o++] = HEX[(d->vendor >> 8) & 0xF];
    buf[o++] = HEX[(d->vendor >> 4) & 0xF];
    buf[o++] = HEX[d->vendor & 0xF];
    buf[o++] = HEX[(d->device >> 12) & 0xF];
    buf[o++] = HEX[(d->device >> 8) & 0xF];
    buf[o++] = HEX[(d->device >> 4) & 0xF];
    buf[o++] = HEX[d->device & 0xF];
    buf[o++] = '\t';

    // IRQ: no lo tenemos por dispositivo, ponemos 0
    buf[o++] = '0';

    // 7 BARs a 0
    for (int b = 0; b < 7; b++) {
      buf[o++] = '\t';
      buf[o++] = '0';
    }
    buf[o++] = '\n';
  }
  buf[o] = '\0';
  *out_len = o;
  return buf;
}

// ---------------------------------------------------------------------------
// /proc/swaps
// ---------------------------------------------------------------------------
struct swaps_ctx {
  char *buf;
  size_t len;
  size_t cap;
};

static void swaps_line_cb(const char *devname, uint64_t total_kb,
                          uint64_t used_kb, uint64_t free_kb, void *arg) {
  (void)free_kb;
  struct swaps_ctx *c = (struct swaps_ctx *)arg;
  char fname[64];
  int fn = 0;
  const char *pfx = "/dev/";
  while (*pfx && fn < 60)
    fname[fn++] = *pfx++;
  const char *d = devname;
  while (*d && fn < 60)
    fname[fn++] = *d++;
  fname[fn] = '\0';

  c->len = kappend(c->buf, c->len, c->cap, fname);
  while (c->len < 40)
    c->buf[c->len++] = ' ';
  c->len = kappend(c->buf, c->len, c->cap, "partition");
  while (c->len < 56)
    c->buf[c->len++] = ' ';
  c->len += kfmt_u64(c->buf + c->len, c->cap - c->len, total_kb);
  while (c->len < 64)
    c->buf[c->len++] = ' ';
  c->len += kfmt_u64(c->buf + c->len, c->cap - c->len, used_kb);
  while (c->len < 72)
    c->buf[c->len++] = ' ';
  c->len = kappend(c->buf, c->len, c->cap, "-2\n");
  c->buf[c->len] = '\0';
}

static char *gen_swaps(size_t *out_len) {
  const size_t CAP = 1024;
  char *buf = (char *)kmalloc(CAP);
  if (!buf)
    return NULL;
  buf[0] = '\0';
  struct swaps_ctx c = {.buf = buf, .len = 0, .cap = CAP};
  c.len = kappend(buf, c.len, CAP,
                  "Filename\t\t\t\tType\t\tSize\tUsed\tPriority\n");
  swap_for_each(swaps_line_cb, &c);
  *out_len = c.len;
  return buf;
}

// ---------------------------------------------------------------------------
// [FIX] /proc/partitions — lista de block devices.
// Formato Linux: "major minor  #blocks  name\n" con cabecera.
//
//   major minor  #blocks  name
//
//      8     0      65536 sda
//      8     1      16384 sda1
//      8     2      16384 sda2
//      8     3      31727 sda3
//
// Lo usan blkid (scan mode), mount, df, fdisk, parted, ...
// major=8 para todos los discos (SCSI/SATA en Linux). minor=índice.
// ---------------------------------------------------------------------------
static char *gen_partitions(size_t *out_len) {
  const size_t CAP = 4096;
  char *buf = (char *)kmalloc(CAP);
  if (!buf)
    return NULL;

  size_t o = 0;
  o = kappend(buf, o, CAP, "major minor  #blocks  name\n\n");

  int n = blk_count();
  for (int i = 0; i < n; i++) {
    block_device_t *b = blk_get_by_index(i);
    if (!b)
      continue;

    uint64_t size_kb = (b->num_sectors * (uint64_t)b->sector_size) / 1024;

    o = kappend(buf, o, CAP, "   8 ");
    // minor en formato de 5 chars (como Linux, no alineado estrictamente)
    if (i < 10) {
      buf[o++] = ' ';
      buf[o++] = (char)('0' + i);
    } else if (i < 100) {
      buf[o++] = (char)('0' + (i / 10));
      buf[o++] = (char)('0' + (i % 10));
    } else {
      buf[o++] = (char)('0' + (i / 100));
      buf[o++] = (char)('0' + ((i / 10) % 10));
      buf[o++] = (char)('0' + (i % 10));
    }
    o = kappend(buf, o, CAP, "   ");
    o += kfmt_u64(buf + o, CAP - o, size_kb);
    o = kappend(buf, o, CAP, " ");
    o = kappend(buf, o, CAP, b->name);
    o = kappend(buf, o, CAP, "\n");

    if (o + 64 > CAP)
      break;
  }
  buf[o] = '\0';
  *out_len = o;
  return buf;
}

// ---------------------------------------------------------------------------
// [FIX D] /proc/<pid>/maps — formato Linux:
//   00400000-00606000 r-xp 00000000 08:01 1234  /apps/init
//   00007ffff0000000-00007ffff0004000 rw-p 00000000 00:00 0  [stack]
//
// pmap, gdb y otros parsean esto. Los campos van separados por espacios
// (Linux usa uno o varios, da igual).
//
// Tipos de VMA (pf.h):
//   VMA_ANON=0, VMA_STACK=1, VMA_ELF=2, VMA_FILE=3
// ---------------------------------------------------------------------------
static int gen_pid_maps_cb(process_t *p, void *arg) {
  struct pid_ctx *c = (struct pid_ctx *)arg;
  if (p->pid != c->target_pid)
    return 0;

  char *buf = c->buf;
  size_t cap = c->cap;
  size_t o = 0;

  for (vma_t *v = p->vma_list; v; v = v->next) {
    if (o + 320 >= cap)
      break;

    // start-end
    o += kfmt_hex_min(buf + o, cap - o, v->start, 8);
    buf[o++] = '-';
    o += kfmt_hex_min(buf + o, cap - o, v->end, 8);
    buf[o++] = ' ';

    // perms
    buf[o++] = 'r';
    buf[o++] = (v->flags & PTE_WRITABLE) ? 'w' : '-';
    buf[o++] = (v->flags & PTE_NX) ? '-' : 'x';
    buf[o++] = 'p';
    buf[o++] = ' ';

    // offset dentro del fichero
    uint64_t off = (v->type == VMA_FILE) ? v->file_offset : 0;
    o += kfmt_hex_min(buf + o, cap - o, off, 8);
    buf[o++] = ' ';

    // [2.4] dev + inode vienen del file_node del VMA.
    uint64_t dev = 0;
    uint64_t ino = 0;
    if (v->file_node) {
      dev = fs_dev_id(v->file_node->fs);
      ino = v->file_node->inode;
    }
    o = kappend(buf, o, cap, "0:");
    o += kfmt_u64(buf + o, cap - o, dev);
    buf[o++] = ' ';
    o += kfmt_u64(buf + o, cap - o, ino);

    // path
    const char *path = NULL;
    if (v->type == VMA_STACK) {
      path = "[stack]";
    } else if (v->type == VMA_ELF) {
      if (v->file_node && v->file_node->name[0])
        path = v->file_node->name;
      else if (p->exe_path[0])
        path = p->exe_path;
      else
        path = p->name;
    } else if (v->type == VMA_FILE) {
      path =
          (v->file_node && v->file_node->name[0]) ? v->file_node->name : NULL;
    } else if (v->type == VMA_ANON) {
      path = "[anon]";
    }

    if (path && path[0]) {
      buf[o++] = ' ';
      o = kappend(buf, o, cap, path);
    }

    buf[o++] = '\n';
  }

  c->len = o;
  c->found = 1;
  return 1;
}

// ---------------------------------------------------------------------------
// [FIX D] /proc/<pid>/smaps.
//
// Igual que maps pero con campos extra por VMA. BusyBox pmap solo usa
// Size/Rss/Private_Dirty, el resto puede ir a 0.
//
// Formato Linux: header de VMA igual que maps, luego "Key: %lu kB\n".
// ---------------------------------------------------------------------------
static int gen_pid_smaps_cb(process_t *p, void *arg) {
  struct pid_ctx *c = (struct pid_ctx *)arg;
  if (p->pid != c->target_pid)
    return 0;

  char *buf = c->buf;
  size_t cap = c->cap;
  size_t o = 0;

  uint64_t *pml4 = NULL;
  if (p->pml4_phys)
    pml4 = (uint64_t *)phys_to_virt(p->pml4_phys);

  for (vma_t *v = p->vma_list; v; v = v->next) {
    if (o + 900 >= cap)
      break;

    // --- Header ---
    o += kfmt_hex_min(buf + o, cap - o, v->start, 8);
    buf[o++] = '-';
    o += kfmt_hex_min(buf + o, cap - o, v->end, 8);
    buf[o++] = ' ';
    buf[o++] = 'r';
    buf[o++] = (v->flags & PTE_WRITABLE) ? 'w' : '-';
    buf[o++] = (v->flags & PTE_NX) ? '-' : 'x';
    buf[o++] = 'p';
    buf[o++] = ' ';

    uint64_t off = (v->type == VMA_FILE) ? v->file_offset : 0;
    o += kfmt_hex_min(buf + o, cap - o, off, 8);
    buf[o++] = ' ';

    // [2.4] dev + inode del file_node
    uint64_t dev = 0;
    uint64_t ino = 0;
    if (v->file_node) {
      dev = fs_dev_id(v->file_node->fs);
      ino = v->file_node->inode;
    }
    o = kappend(buf, o, cap, "0:");
    o += kfmt_u64(buf + o, cap - o, dev);
    buf[o++] = ' ';
    o += kfmt_u64(buf + o, cap - o, ino);

    const char *path = NULL;
    if (v->type == VMA_STACK)
      path = "[stack]";
    else if (v->type == VMA_ELF) {
      if (v->file_node && v->file_node->name[0])
        path = v->file_node->name;
      else if (p->exe_path[0])
        path = p->exe_path;
      else
        path = p->name;
    } else if (v->type == VMA_FILE) {
      path =
          (v->file_node && v->file_node->name[0]) ? v->file_node->name : NULL;
    } else if (v->type == VMA_ANON)
      path = "[anon]";

    if (path && path[0]) {
      buf[o++] = ' ';
      o = kappend(buf, o, cap, path);
    }
    buf[o++] = '\n';

    // --- Rss ---
    uint64_t pages_present = 0;
    if (pml4) {
      for (uint64_t page = v->start; page < v->end; page += PAGE_SIZE) {
        if (paging_get_phys_in(pml4, page))
          pages_present++;
      }
    }
    uint64_t size_kb = (v->end - v->start) / 1024;
    uint64_t rss_kb = pages_present * 4;

    char tmp[64];
#define SMAPS_LINE(key, val)                                                   \
  do {                                                                         \
    int _n = 0;                                                                \
    const char *_k = (key);                                                    \
    while (*_k && _n < (int)sizeof(tmp) - 32)                                  \
      tmp[_n++] = *_k++;                                                       \
    for (int _s = _n; _s < 22 && _n < (int)sizeof(tmp) - 16; _s++)             \
      tmp[_n++] = ' ';                                                         \
    _n += kfmt_u64(tmp + _n, sizeof(tmp) - _n, (val));                         \
    tmp[_n++] = ' ';                                                           \
    tmp[_n++] = 'k';                                                           \
    tmp[_n++] = 'B';                                                           \
    tmp[_n++] = '\n';                                                          \
    tmp[_n] = '\0';                                                            \
    o = kappend(buf, o, cap, tmp);                                             \
  } while (0)

    SMAPS_LINE("Size:", size_kb);
    SMAPS_LINE("KernelPageSize:", 4);
    SMAPS_LINE("MMUPageSize:", 4);
    SMAPS_LINE("Rss:", rss_kb);
    SMAPS_LINE("Pss:", rss_kb);
    SMAPS_LINE("Shared_Clean:", 0);
    SMAPS_LINE("Shared_Dirty:", 0);
    SMAPS_LINE("Private_Clean:", 0);
    SMAPS_LINE("Private_Dirty:", rss_kb);
    SMAPS_LINE("Referenced:", rss_kb);
    SMAPS_LINE("Anonymous:", rss_kb);
    SMAPS_LINE("AnonHugePages:", 0);
    SMAPS_LINE("Swap:", 0);
    SMAPS_LINE("Locked:", 0);
#undef SMAPS_LINE

    o = kappend(buf, o, cap, "VmFlags: rd wr mr mw me\n");
  }

  c->len = o;
  c->found = 1;
  return 1;
}

// ---------------------------------------------------------------------------
// [FIX] /proc/bus/pci/<bus>/<dev>.<func> — config space (256 bytes).
// libpci (usado por busybox lspci) abre estos ficheros para leer la
// clase, revisión, subsys, etc. Sin ellos, lspci imprime todo a cero.
// ---------------------------------------------------------------------------
static int parse_hex_byte(const char *s, uint8_t *out) {
  uint8_t v = 0;
  for (int i = 0; i < 2; i++) {
    char c = s[i];
    int d;
    if (c >= '0' && c <= '9')
      d = c - '0';
    else if (c >= 'a' && c <= 'f')
      d = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F')
      d = c - 'A' + 10;
    else
      return -1;
    v = (uint8_t)((v << 4) | d);
  }
  *out = v;
  return 0;
}

static char *gen_proc_bus_pci_device_config(uint8_t bus, uint8_t slot,
                                            uint8_t func, size_t *out_len) {
  uint8_t *buf = (uint8_t *)kmalloc(256);
  if (!buf)
    return NULL;
  if (sysfs_pci_read_config(bus, slot, func, buf) != 0) {
    kfree(buf);
    return NULL;
  }
  *out_len = 256;
  return (char *)buf;
}

// readdir de /proc/bus/pci: "devices" + un directorio por bus (solo "00"
// en la práctica).
static int procfs_readdir_bus_pci(uint64_t index, vfs_dirent_t *out) {
  if (index == 0) {
    const char *n = "devices";
    size_t l = strlen(n);
    memcpy(out->name, n, l);
    out->name[l] = '\0';
    out->type = VFS_FILE;
    out->size = 0;
    return 0;
  }
  index--;

  uint8_t seen[256];
  memset(seen, 0, sizeof(seen));
  int n = sysfs_pci_count();
  for (int i = 0; i < n; i++) {
    const struct sysfs_pci_info *d = sysfs_pci_get(i);
    if (d)
      seen[d->bus] = 1;
  }
  int count = 0;
  for (int b = 0; b < 256; b++) {
    if (!seen[b])
      continue;
    if (count == (int)index) {
      static const char hex[] = "0123456789abcdef";
      out->name[0] = hex[(b >> 4) & 0xF];
      out->name[1] = hex[b & 0xF];
      out->name[2] = '\0';
      out->type = VFS_DIRECTORY;
      out->size = 0;
      return 0;
    }
    count++;
  }
  out->name[0] = '\0';
  out->type = 0;
  out->size = 0;
  return 0;
}

// readdir de /proc/bus/pci/<bus>: lista de "XX.Y" (slot.func).
static int procfs_readdir_bus_pci_bus(uint8_t bus, uint64_t index,
                                      vfs_dirent_t *out) {
  uint64_t seen = 0;
  int n = sysfs_pci_count();
  for (int i = 0; i < n; i++) {
    const struct sysfs_pci_info *d = sysfs_pci_get(i);
    if (!d || d->bus != bus)
      continue;
    if (seen == index) {
      static const char hex[] = "0123456789abcdef";
      out->name[0] = hex[(d->slot >> 4) & 0xF];
      out->name[1] = hex[d->slot & 0xF];
      out->name[2] = '.';
      out->name[3] = (char)('0' + (d->func & 0x7));
      out->name[4] = '\0';
      out->type = VFS_FILE;
      out->size = 256;
      return 0;
    }
    seen++;
  }
  out->name[0] = '\0';
  out->type = 0;
  out->size = 0;
  return 0;
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
// /proc/mounts.
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

// ---------------------------------------------------------------------------
// /proc/self/mountinfo.
//
// Formato Linux:
//   <id> <parent> <maj:min> <root> <mountpoint> <opts> - <fs> <src> <superopts>
//
// [FIX] libmount construye un árbol con mount_id/parent_id. La raíz del
// árbol DEBE tener id=1 y parent=1. vfs_for_each_mount devuelve los
// mounts en orden arbitrario (por eso antes '/' salía el último y
// libmount descartaba todo). Aquí colectamos primero, emitimos la raíz
// con id=1, y el resto con ids correlativos desde 2 y parent_id=1.
// ---------------------------------------------------------------------------

struct mountinfo_entry {
  char path[VFS_PATH_MAX];
  char fs_name[32];
  uint64_t dev; // [df] maj:min para mountinfo
};

struct mountinfo_collect_ctx {
  struct mountinfo_entry *entries;
  size_t n;
  size_t cap;
};

static int mountinfo_collect_cb(const char *path, const char *fs_name,
                                int is_bind, const char *bind_source,
                                void *arg) {
  (void)is_bind;
  (void)bind_source;
  struct mountinfo_collect_ctx *c = (struct mountinfo_collect_ctx *)arg;
  if (c->n >= c->cap)
    return 1;

  size_t pl = strlen(path);
  if (pl >= sizeof(c->entries[c->n].path))
    pl = sizeof(c->entries[c->n].path) - 1;
  memcpy(c->entries[c->n].path, path, pl);
  c->entries[c->n].path[pl] = '\0';

  size_t fl = strlen(fs_name);
  if (fl >= sizeof(c->entries[c->n].fs_name))
    fl = sizeof(c->entries[c->n].fs_name) - 1;
  memcpy(c->entries[c->n].fs_name, fs_name, fl);
  c->entries[c->n].fs_name[fl] = '\0';

  c->n++;
  return 0;
}

// [df] Escribe "0:<minor>" en buf. Usa el device id asignado por
// fs_dev_id: los majors son todos 0 (pseudo device), el minor es lo
// que distingue un FS de otro.
extern uint64_t fs_dev_id(const void *fs);

static size_t kappend_dev(char *buf, size_t len, size_t cap, uint64_t dev) {
  char tmp[24];
  int n = 0;
  if (dev == 0)
    tmp[n++] = '0';
  while (dev > 0) {
    tmp[n++] = (char)('0' + (dev % 10));
    dev /= 10;
  }
  len = kappend(buf, len, cap, "0:");
  while (n > 0 && len + 1 < cap)
    buf[len++] = tmp[--n];
  buf[len] = '\0';
  return len;
}

static char *gen_mountinfo(size_t *out_len) {
  const size_t CAP = 4096;
  char *buf = (char *)kmalloc(CAP);
  if (!buf)
    return NULL;
  buf[0] = '\0';
  size_t len = 0;

#define MAX_MOUNTS 32
  struct mountinfo_entry *entries =
      (struct mountinfo_entry *)kmalloc(MAX_MOUNTS * sizeof(*entries));
  if (!entries) {
    kfree(buf);
    return NULL;
  }
  struct mountinfo_collect_ctx cc = {
      .entries = entries, .n = 0, .cap = MAX_MOUNTS};
  vfs_for_each_mount(mountinfo_collect_cb, &cc);

  // [df] Calcular el device de cada mount FUERA del callback: for_each
  // mantiene g_mounts_lock tomado y vfs_lookup intentaría cogerlo otra
  // vez (deadlock).
  for (size_t i = 0; i < cc.n; i++) {
    vfs_node_t *n = vfs_lookup(entries[i].path);
    entries[i].dev = n ? fs_dev_id(n->fs) : 31;
    if (n)
      vfs_node_free(n);
  }

  // 1) Buscar la raíz real ("/").
  int root_idx = -1;
  for (size_t i = 0; i < cc.n; i++) {
    if (strcmp(entries[i].path, "/") == 0) {
      root_idx = (int)i;
      break;
    }
  }

  // 2) Emitir la raíz primero con id=1, parent=1.
  if (root_idx >= 0) {
    len = kappend(buf, len, CAP, "1 1 ");
    len = kappend_dev(buf, len, CAP, entries[root_idx].dev);
    len = kappend(buf, len, CAP, " / / rw - ");
    len = kappend(buf, len, CAP, entries[root_idx].fs_name);
    buf[len++] = ' ';
    len = kappend(buf, len, CAP, entries[root_idx].fs_name);
    len = kappend(buf, len, CAP, " rw\n");
  }

  // 3) Emitir el resto con parent_id=1, ids correlativos desde 2.
  int next_id = 2;
  for (size_t i = 0; i < cc.n; i++) {
    if ((int)i == root_idx)
      continue;
    if (len + 200 >= CAP)
      break;

    char tmp[12];
    int tn = 0;
    int v = next_id;
    if (v == 0)
      tmp[tn++] = '0';
    while (v > 0) {
      tmp[tn++] = (char)('0' + (v % 10));
      v /= 10;
    }
    while (tn > 0)
      buf[len++] = tmp[--tn];
    buf[len++] = ' ';
    buf[len++] = '1';
    buf[len++] = ' ';

    // [df] Device real, no 0:32 fijo.
    len = kappend_dev(buf, len, CAP, entries[i].dev);
    len = kappend(buf, len, CAP, " / ");
    len = kappend(buf, len, CAP, entries[i].path);
    len = kappend(buf, len, CAP, " rw - ");
    len = kappend(buf, len, CAP, entries[i].fs_name);
    buf[len++] = ' ';
    len = kappend(buf, len, CAP, entries[i].fs_name);
    len = kappend(buf, len, CAP, " rw\n");

    next_id++;
  }

  kfree(entries);
  *out_len = len;
  return buf;
}

static char *gen_filesystems(size_t *out_len) {
  static const char v[] = "nodev\ttarfs\n"
                          "nodev\tdevfs\n"
                          "nodev\tprocfs\n"
                          "nodev\tsysfs\n"
                          "nodev\ttmpfs\n"
                          "      \tfat32\n";
  size_t n = sizeof(v) - 1;
  char *buf = (char *)kmalloc(n + 1);
  if (!buf)
    return NULL;
  memcpy(buf, v, n);
  buf[n] = '\0';
  *out_len = n;
  return buf;
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
// readdir: /proc raíz, /proc/bus/*, /proc/<pid>/ y /proc/<pid>/fd/.
// ===========================================================================
static const char *procfs_root_entries[] = {
    "uptime", "version", "meminfo",     "stat",       "self",
    "mounts", "loadavg", "bus",         "partitions", "swaps",
    "sys",    "vmstat",  "filesystems",
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
    "stat", "status", "cmdline", "comm", "statm", "fd",
    "cwd",  "exe",    "root",    "maps", "smaps", "mountinfo",
};
#define PROCFS_N_PID_ENTRIES                                                   \
  (sizeof(procfs_pid_entries) / sizeof(procfs_pid_entries[0]))

// [FIX] /proc/<pid>/fd: helper para listar los fds abiertos del proceso.
static int procfs_readdir_fd(uint32_t pid, uint64_t index, vfs_dirent_t *out) {
  process_t *proc = process_find_by_pid(pid);
  if (!proc) {
    out->name[0] = '\0';
    out->type = 0;
    out->size = 0;
    return 0;
  }
  uint64_t seen = 0;
  for (int i = 0; i < MAX_PROCESS_FDS; i++) {
    if (!proc->fds[i])
      continue;
    if (seen == index) {
      char tmp[12];
      int n = 0;
      int v = i;
      if (v == 0)
        tmp[n++] = '0';
      while (v > 0) {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
      }
      int k = 0;
      while (n > 0 && k < (int)sizeof(out->name) - 1)
        out->name[k++] = tmp[--n];
      out->name[k] = '\0';
      out->type = VFS_FILE;
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

static int procfs_readdir(vfs_node_t *dir, uint64_t index, vfs_dirent_t *out) {
  if (!dir || !out)
    return -EINVAL;
  if (!(dir->flags & VFS_DIRECTORY))
    return -ENOTDIR;

  // [FIX] /proc/bus y /proc/bus/pci — antes del dispatcher de <pid>.
  if (strcmp(dir->name, "/proc/bus") == 0) {
    if (index >= 1) {
      out->name[0] = '\0';
      out->type = 0;
      out->size = 0;
      return 0;
    }
    out->name[0] = 'p';
    out->name[1] = 'c';
    out->name[2] = 'i';
    out->name[3] = '\0';
    out->type = VFS_DIRECTORY;
    out->size = 0;
    return 0;
  }
  if (strcmp(dir->name, "/proc/bus/pci") == 0)
    return procfs_readdir_bus_pci(index, out);

  // [FIX] /proc/bus/pci/<bus>/
  if (strncmp(dir->name, "/proc/bus/pci/", 14) == 0) {
    const char *rest = dir->name + 14;
    if (strlen(rest) == 2) {
      uint8_t bus;
      if (parse_hex_byte(rest, &bus) == 0)
        return procfs_readdir_bus_pci_bus(bus, index, out);
    }
    out->name[0] = '\0';
    out->type = 0;
    out->size = 0;
    return 0;
  }

  // /proc/sys
  if (strcmp(dir->name, "/proc/sys") == 0) {
    static const char *l[] = {"kernel", "vm"};
    if (index >= 2) {
      out->name[0] = '\0';
      out->type = 0;
      out->size = 0;
      return 0;
    }
    const char *n = l[index];
    size_t ln = strlen(n);
    memcpy(out->name, n, ln);
    out->name[ln] = '\0';
    out->type = VFS_DIRECTORY;
    out->size = 0;
    return 0;
  }
  if (strcmp(dir->name, "/proc/sys/kernel") == 0) {
    if (index >= 2) {
      out->name[0] = '\0';
      return 0;
    }
    const char *n = (index == 0) ? "hostname" : "printk";
    size_t ln = strlen(n);
    memcpy(out->name, n, ln);
    out->name[ln] = '\0';
    out->type = VFS_FILE;
    out->size = 0;
    return 0;
  }
  if (strcmp(dir->name, "/proc/sys/vm") == 0) {
    if (index >= 2) {
      out->name[0] = '\0';
      return 0;
    }
    const char *n = (index == 0) ? "swap_low_pct" : "swap_high_pct";
    size_t ln = strlen(n);
    memcpy(out->name, n, ln);
    out->name[ln] = '\0';
    out->type = VFS_FILE;
    out->size = 0;
    return 0;
  }

  // [FIX] /proc/<pid>/fd/ y /proc/<pid>/.
  if (strncmp(dir->name, "/proc/", 6) == 0) {
    const char *p = dir->name + 6;
    const char *slash = p;
    while (*slash && *slash != '/')
      slash++;

    int has_pid = (slash > p);
    uint32_t pid_v = 0;
    if (has_pid) {
      // [FIX] "self" se resuelve al pid del proceso actual. Lo
      // necesitamos aquí porque el VFS pasa a readdir el path
      // ORIGINAL (/proc/self/fd), no el resuelto.
      if ((slash - p) == 4 && memcmp(p, "self", 4) == 0) {
        process_t *cur = process_current();
        if (!cur)
          return -ESRCH;
        pid_v = cur->pid;
      } else {
        for (const char *q = p; q < slash; q++) {
          if (*q < '0' || *q > '9') {
            has_pid = 0;
            break;
          }
          pid_v = pid_v * 10 + (uint32_t)(*q - '0');
        }
      }
    }

    if (has_pid) {
      if (strcmp(slash, "/fd") == 0)
        return procfs_readdir_fd(pid_v, index, out);

      if (*slash == '\0') {
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
        out->type = (strcmp(n, "fd") == 0) ? VFS_DIRECTORY : VFS_FILE;
        out->size = 0;
        return 0;
      }
    }
  }

  // /proc/ raíz.
  if (index < PROCFS_N_ROOT_ENTRIES) {
    const char *n = procfs_root_entries[index];
    size_t l = strlen(n);
    if (l >= sizeof(out->name))
      l = sizeof(out->name) - 1;
    memcpy(out->name, n, l);
    out->name[l] = '\0';
    out->type = (strcmp(n, "bus") == 0) ? VFS_DIRECTORY : VFS_FILE;
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

// [FIX] Helper genérico para nodos de directorio en procfs.
// Antes solo existía `make_root_node` (hardcodeado a "/"), pero
// /proc/bus y /proc/bus/pci también son directorios.
static vfs_node_t *make_dir_node(const char *name) {
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

  // [FIX] /proc/bus/pci/devices — DESPUÉS de declarar `name`.
  if (strcmp(name, "bus") == 0)
    return make_dir_node("bus");
  if (strcmp(name, "bus/pci") == 0)
    return make_dir_node("pci");
  if (strcmp(name, "bus/pci/devices") == 0) {
    size_t len = 0;
    char *buf = gen_proc_bus_pci_devices(&len);
    return buf ? make_file_node("devices", buf, len) : NULL;
  }

  // ---- /proc/sys/... ----
  if (strncmp(name, "sys", 3) == 0 && (name[3] == '\0' || name[3] == '/')) {
    // Caso "/proc/sys": dir
    if (name[3] == '\0')
      return make_dir_node("sys");

    const char *rest = name + 4; // "kernel" o "kernel/hostname"

    // Subdirectorios hardcoded: kernel/, vm/
    if (strcmp(rest, "kernel") == 0)
      return make_dir_node("kernel");
    if (strcmp(rest, "vm") == 0)
      return make_dir_node("vm");

    // Fichero concreto: buscar en sysctl
    const sysctl_entry_t *e = sysctl_lookup(rest);
    if (!e)
      return NULL;

    // Generar contenido
    char buf[128];
    size_t len = 0;
    if (e->read)
      e->read(buf, sizeof(buf), &len);

    // Crear nodo. Si es rw, con ops->write; si es ro, con ops ro.
    vfs_node_t *n = (vfs_node_t *)kzalloc(sizeof(vfs_node_t));
    if (!n)
      return NULL;
    // El nombre debe ser el basename: extraerlo de `rest`
    const char *base = rest;
    for (const char *p = rest; *p; p++)
      if (*p == '/')
        base = p + 1;
    size_t nl = strlen(base);
    if (nl >= sizeof(n->name))
      nl = sizeof(n->name) - 1;
    memcpy(n->name, base, nl);
    n->name[nl] = '\0';
    n->flags = VFS_FILE;
    n->ops = (e->mode & 0222) ? &procfs_sysctl_rw_ops : &procfs_file_ops;
    n->mode = S_IFREG | (e->mode & 0777);
    n->uid = 0;
    n->gid = 0;

    // Para el rw, guardamos el entry en priv para el write.
    if (e->mode & 0222) {
      procfs_sysctl_data_t *sd = (procfs_sysctl_data_t *)kzalloc(sizeof(*sd));
      if (!sd) {
        kfree(n);
        return NULL;
      }
      sd->entry = e;
      sd->buf = (char *)kmalloc(len + 1);
      if (!sd->buf) {
        kfree(sd);
        kfree(n);
        return NULL;
      }
      memcpy(sd->buf, buf, len);
      sd->buf[len] = '\0';
      sd->len = len;
      n->size = len;
      n->priv = sd;
    } else {
      procfs_data_t *pd = (procfs_data_t *)kzalloc(sizeof(*pd));
      if (!pd) {
        kfree(n);
        return NULL;
      }
      pd->buf = (char *)kmalloc(len + 1);
      if (!pd->buf) {
        kfree(pd);
        kfree(n);
        return NULL;
      }
      memcpy(pd->buf, buf, len);
      pd->buf[len] = '\0';
      pd->len = len;
      n->size = len;
      n->priv = pd;
    }
    return n;
  }

  // [FIX] /proc/bus/pci/<bus>  y  /proc/bus/pci/<bus>/<dev>.<func>
  // libpci abre estos ficheros para leer el config space (256 bytes).
  if (strncmp(name, "bus/pci/", 8) == 0) {
    const char *rest = name + 8;
    const char *slash2 = NULL;
    for (const char *p = rest; *p; p++) {
      if (*p == '/') {
        slash2 = p;
        break;
      }
    }
    if (!slash2) {
      // /proc/bus/pci/<bus> — directorio
      if (strlen(rest) != 2)
        return NULL;
      uint8_t bus;
      if (parse_hex_byte(rest, &bus) != 0)
        return NULL;
      int n = sysfs_pci_count();
      int found = 0;
      for (int i = 0; i < n; i++) {
        const struct sysfs_pci_info *d = sysfs_pci_get(i);
        if (d && d->bus == bus) {
          found = 1;
          break;
        }
      }
      if (!found)
        return NULL;
      return make_dir_node(rest);
    }
    // /proc/bus/pci/<bus>/<devfn> — fichero con config space
    char bus_str[8];
    size_t blen = (size_t)(slash2 - rest);
    if (blen != 2 || blen >= sizeof(bus_str))
      return NULL;
    memcpy(bus_str, rest, blen);
    bus_str[blen] = '\0';
    uint8_t bus;
    if (parse_hex_byte(bus_str, &bus) != 0)
      return NULL;

    const char *devfn_str = slash2 + 1;
    // Formato "XX.Y"
    if (strlen(devfn_str) != 4 || devfn_str[2] != '.')
      return NULL;
    uint8_t slot;
    if (parse_hex_byte(devfn_str, &slot) != 0)
      return NULL;
    char fc = devfn_str[3];
    if (fc < '0' || fc > '7')
      return NULL;
    uint8_t func = (uint8_t)(fc - '0');

    size_t len = 0;
    char *buf = gen_proc_bus_pci_device_config(bus, slot, func, &len);
    if (!buf)
      return NULL;
    return make_file_node(devfn_str, buf, len);
  }

  const char *slash = NULL;
  for (const char *p = name; *p; p++) {
    if (*p == '/') {
      slash = p;
      break;
    }
  }

  if (slash) {
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

    const char *rest = slash + 1;

    // <pid>/fd/N — symlink al path del nodo subyacente.
    if (strncmp(rest, "fd/", 3) == 0) {
      int fd_num = 0;
      const char *q = rest + 3;
      while (*q >= '0' && *q <= '9') {
        fd_num = fd_num * 10 + (*q - '0');
        q++;
      }
      if (*q != '\0' || fd_num < 0 || fd_num >= MAX_PROCESS_FDS)
        return NULL;
      process_t *proc = process_find_by_pid(pid);
      if (!proc || !proc->fds[fd_num] || !proc->fds[fd_num]->node)
        return NULL;

      const char *target = proc->fds[fd_num]->node->name;
      if (!target || !target[0])
        target = "/";

      vfs_node_t *n = (vfs_node_t *)kzalloc(sizeof(vfs_node_t));
      if (!n)
        return NULL;
      n->is_symlink = 1;
      size_t tl = strlen(target);
      if (tl >= sizeof(n->link_target))
        tl = sizeof(n->link_target) - 1;
      memcpy(n->link_target, target, tl);
      n->link_target[tl] = '\0';
      n->flags = VFS_FILE;
      n->mode = S_IFLNK | 0777;
      n->uid = 0;
      n->gid = 0;
      n->name[0] = '\0';
      return n;
    }

    if (strcmp(rest, "cwd") == 0 || strcmp(rest, "exe") == 0 ||
        strcmp(rest, "root") == 0) {
      process_t *p = process_find_by_pid(pid);
      if (!p)
        return NULL;
      const char *target = "/";
      if (strcmp(rest, "cwd") == 0 && p->cwd[0])
        target = p->cwd;
      else if (strcmp(rest, "exe") == 0) {
        if (p->exe_path[0])
          target = p->exe_path;
        else if (p->name[0])
          target = p->name;
      }
      vfs_node_t *n = (vfs_node_t *)kzalloc(sizeof(vfs_node_t));
      if (!n)
        return NULL;
      n->is_symlink = 1;
      size_t tl = strlen(target);
      if (tl >= sizeof(n->link_target))
        tl = sizeof(n->link_target) - 1;
      memcpy(n->link_target, target, tl);
      n->link_target[tl] = '\0';
      n->flags = VFS_FILE;
      n->mode = S_IFLNK | 0777;
      n->uid = 0;
      n->gid = 0;
      return n;
    }

    // <pid>/fd — directorio.
    if (strcmp(rest, "fd") == 0) {
      process_t *proc = process_find_by_pid(pid);
      if (!proc)
        return NULL;
      return make_dir_node("fd");
    }

    const char *file = rest;
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
    if (strcmp(file, "maps") == 0) {
      buf = gen_pid_file(pid, &len, gen_pid_maps_cb);
      return buf ? make_file_node("maps", buf, len) : NULL;
    }
    if (strcmp(file, "smaps") == 0) {
      buf = gen_pid_file(pid, &len, gen_pid_smaps_cb);
      return buf ? make_file_node("smaps", buf, len) : NULL;
    }
    // [FIX] /proc/<pid>/mountinfo — lo pide coreutils df, findmnt, mount.
    if (strcmp(file, "mountinfo") == 0) {
      char *b = gen_mountinfo(&len);
      return b ? make_file_node("mountinfo", b, len) : NULL;
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
  if (strcmp(name, "partitions") == 0) {
    size_t len = 0;
    char *b = gen_partitions(&len);
    return make_file_node("partitions", b, len);
  }
  if (strcmp(name, "vmstat") == 0) {
    size_t len = 0;
    char *b = gen_vmstat(&len);
    return make_file_node("vmstat", b, len);
  }
  if (strcmp(name, "swaps") == 0) {
    size_t len = 0;
    char *b = gen_swaps(&len);
    return make_file_node("swaps", b, len);
  }
  // [FIX] /proc/filesystems — lo consulta ls, mount, blkid.
  if (strcmp(name, "filesystems") == 0) {
    size_t len = 0;
    char *b = gen_filesystems(&len);
    return make_file_node("filesystems", b, len);
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
  out->f_type = 0x9fa0;
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