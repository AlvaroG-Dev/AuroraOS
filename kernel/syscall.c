// kernel/syscall.c
//
// ABI doble: rango 0..500 (Linux) y rango 0x1000+ (Aurora-only).
//
// Todos los handlers tienen prefijo k_ (Linux) o a_ (Aurora) para
// distinguir de un vistazo a qué ABI pertenecen.

#include "syscall.h"
#include "cpu.h"
#include "gdt.h"
#include "gfx/winsrv.h"
#include "heap.h"
#include "ipc.h"
#include "klog.h"
#include "paging.h"
#include "pf.h"
#include "process.h"
#include "sched.h"
#include "serial.h"
#include "signal.h"
#include "spinlock.h"
#include "string.h"
#include "uaccess.h"
#include "vfs.h"
#include <stddef.h>
#include <stdint.h>

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

#define SPAWN_ARGS_MAX PROCESS_ARGV_MAX
#define SPAWN_ARG_STR_MAX 128

extern void syscall_entry(void);

// ---------------------------------------------------------------------------
// Trap frame del syscall en curso. Almacenamiento per-task. Ver
// sched.h:task_t.syscall_regs para el porqué del cambio desde global.
// ---------------------------------------------------------------------------
registers_t *syscall_current_regs(void) {
  task_t *t = sched_current();
  return t ? t->syscall_regs : NULL;
}

// ---------- helper de sleep ----------
//
// Duerme `ticks` sin wait queue permanente. Registra la tarea en una wq
// local que nunca cumple condición: solo sale por timeout del scheduler
// (sched_wake_expired, vía wait_event_interruptible_timeout).
static bool sleep_never_true(void *arg) {
  (void)arg;
  return false;
}

static void sched_sleep_ticks(uint64_t ticks) {
  if (ticks == 0)
    return;
  wait_queue_t wq;
  wait_queue_init(&wq);
  wait_event_interruptible_timeout(&wq, sleep_never_true, NULL, ticks);
}

// ---------------------------------------------------------------------------
// Estructuras auxiliares
// ---------------------------------------------------------------------------
typedef struct {
  char name[32];
  uint32_t task_id; // 0 = slot libre
} kernel_service_t;

typedef struct {
  char name[128];
  uint32_t type;
  uint32_t _pad;
  uint64_t size;
} user_dirent_t;

static kernel_service_t g_services[KERNEL_SERVICES_MAX];
static spinlock_t g_services_lock;

// ---------- readv / writev ----------
//
// struct iovec { void *iov_base; size_t iov_len; }  // 16 bytes en x86_64
//
// Los usa musl en __stdio_read/__stdio_write. Si no existen, printf() no
// vuelca (falla silenciosamente porque el error se ignora) y fgets() no
// funciona en absoluto.
struct k_iovec {
  uint64_t iov_base;
  uint64_t iov_len;
};

// ---------------------------------------------------------------------------
// struct rusage de Linux x86_64. Layout exacto, 144 bytes.
// ---------------------------------------------------------------------------
struct k_timeval_rs {
  int64_t tv_sec;
  int64_t tv_usec;
};

struct k_rusage {
  struct k_timeval_rs ru_utime; // 0x00
  struct k_timeval_rs ru_stime; // 0x10
  int64_t ru_maxrss;            // 0x20
  int64_t ru_ixrss;             // 0x28
  int64_t ru_idrss;             // 0x30
  int64_t ru_isrss;             // 0x38
  int64_t ru_minflt;            // 0x40
  int64_t ru_majflt;            // 0x48
  int64_t ru_nswap;             // 0x50
  int64_t ru_inblock;           // 0x58
  int64_t ru_oublock;           // 0x60
  int64_t ru_msgsnd;            // 0x68
  int64_t ru_msgrcv;            // 0x70
  int64_t ru_nsignals;          // 0x78
  int64_t ru_nvcsw;             // 0x80
  int64_t ru_nivcsw;            // 0x88
};
_Static_assert(sizeof(struct k_rusage) == 144, "rusage layout x86_64");

// ---------------------------------------------------------------------------
// Servicios IPC (Aurora-specific)
// ---------------------------------------------------------------------------
void syscall_register_service(const char *name, uint32_t task_id) {
  if (!name || !name[0] || task_id == 0) {
    LOG_WARN("[SYS] register_service: parámetros inválidos");
    return;
  }

  unsigned long flags = spin_lock_irqsave(&g_services_lock);

  for (int i = 0; i < KERNEL_SERVICES_MAX; i++) {
    if (g_services[i].task_id != 0 &&
        strncmp(g_services[i].name, name, sizeof(g_services[i].name)) == 0) {
      g_services[i].task_id = task_id;
      spin_unlock_irqrestore(&g_services_lock, flags);
      LOG_INFO("[SYS] Servicio '%s' re-registrado con Task ID=%u", name,
               task_id);
      return;
    }
  }

  for (int i = 0; i < KERNEL_SERVICES_MAX; i++) {
    if (g_services[i].task_id == 0) {
      size_t j = 0;
      while (name[j] && j < sizeof(g_services[i].name) - 1) {
        g_services[i].name[j] = name[j];
        j++;
      }
      g_services[i].name[j] = '\0';
      g_services[i].task_id = task_id;
      spin_unlock_irqrestore(&g_services_lock, flags);
      LOG_INFO("[SYS] Servicio '%s' registrado con Task ID=%u", name, task_id);
      return;
    }
  }

  spin_unlock_irqrestore(&g_services_lock, flags);
  LOG_WARN("[SYS] No hay slots para registrar servicio '%s'", name);
}

static uint32_t syscall_lookup_service(const char *name) {
  if (!name || !name[0])
    return 0;

  unsigned long flags = spin_lock_irqsave(&g_services_lock);
  uint32_t found = 0;
  for (int i = 0; i < KERNEL_SERVICES_MAX; i++) {
    if (g_services[i].task_id != 0 &&
        strncmp(g_services[i].name, name, sizeof(g_services[i].name)) == 0) {
      found = g_services[i].task_id;
      break;
    }
  }
  spin_unlock_irqrestore(&g_services_lock, flags);
  return found;
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------
void syscall_init(void) {
  for (int i = 0; i < KERNEL_SERVICES_MAX; i++) {
    g_services[i].name[0] = '\0';
    g_services[i].task_id = 0;
  }
  spin_init(&g_services_lock);

  uint64_t efer = rdmsr(MSR_EFER);
  efer |= EFER_SCE;
  wrmsr(MSR_EFER, efer);

  uint64_t star = ((uint64_t)KERNEL_CS << 32) | ((uint64_t)USER_CS << 48);
  wrmsr(MSR_STAR, star);

  wrmsr(MSR_LSTAR, (uint64_t)syscall_entry);
  wrmsr(MSR_SFMASK, MSR_SFMASK_BITS);

  LOG_INFO("[SYSCALL] Syscalls inicializados (SCE, STAR, LSTAR).");
}

void syscall_init_ap(void) {
  uint64_t efer = rdmsr(MSR_EFER);
  efer |= EFER_SCE;
  wrmsr(MSR_EFER, efer);

  uint64_t star = ((uint64_t)KERNEL_CS << 32) | ((uint64_t)USER_CS << 48);
  wrmsr(MSR_STAR, star);
  wrmsr(MSR_LSTAR, (uint64_t)syscall_entry);
  wrmsr(MSR_SFMASK, MSR_SFMASK_BITS);
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static inline uint64_t err(int e) { return (uint64_t)(-(int64_t)e); }

typedef int64_t (*syscall_fn_t)(uint64_t, uint64_t, uint64_t, uint64_t,
                                uint64_t);

typedef struct {
  syscall_fn_t fn;
  const char *name;
} syscall_entry_t;

static int resolve_user_path(process_t *proc, const char *uptr, char *out,
                             size_t outlen) {
  if (!uptr || !out || outlen < 2)
    return -EINVAL;
  char raw[VFS_PATH_MAX];
  long n = strncpy_from_user(raw, uptr, sizeof(raw));
  if (n < 0)
    return -EFAULT;
  if (n == 0)
    return -EINVAL;
  const char *cwd = (proc && proc->cwd[0]) ? proc->cwd : "/";
  return vfs_resolve_path(cwd, raw, out, outlen);
}

// Traduce un vfs_stat_t de Aurora al struct stat de Linux.
static void vfs_to_linux_stat(const vfs_stat_t *vs, uint64_t size,
                              linux_stat_t *out) {
  memset(out, 0, sizeof(*out));
  uint32_t mode;
  if (vs->flags & VFS_DIRECTORY)
    mode = S_IFDIR | 0755;
  else if (vs->flags & VFS_CHARDEVICE)
    mode = S_IFCHR | 0666;
  else
    mode = S_IFREG | 0644;
  out->st_dev = 1;
  out->st_ino = vs->inode;
  out->st_nlink = (vs->flags & VFS_DIRECTORY) ? 2 : 1;
  out->st_mode = mode;
  out->st_uid = 0;
  out->st_gid = 0;
  out->st_size = (int64_t)size;
  out->st_blksize = 512;
  out->st_blocks = (int64_t)((size + 511) / 512);
}

// ===========================================================================
//  LINUX ABI HANDLERS  (rango 0..500)
// ===========================================================================

// ---------- read/write ----------
static int64_t k_read(uint64_t fd, uint64_t buf, uint64_t count, uint64_t a4,
                      uint64_t a5) {
  (void)a4;
  (void)a5;
  process_t *proc = process_current();
  if (!proc)
    return -EFAULT;
  if (!access_ok((void *)buf, (size_t)count))
    return -EFAULT;
  return vfs_read_for_proc(proc, (int)fd, (void *)buf, (size_t)count);
}

static int64_t k_write(uint64_t fd, uint64_t buf, uint64_t count, uint64_t a4,
                       uint64_t a5) {
  (void)a4;
  (void)a5;
  process_t *proc = process_current();
  if (!proc)
    return -EFAULT;
  if (!access_ok((void *)buf, (size_t)count))
    return -EFAULT;
  return vfs_write_for_proc(proc, (int)fd, (const void *)buf, (size_t)count);
}

// ---------- close ----------
static int64_t k_close(uint64_t fd, uint64_t a2, uint64_t a3, uint64_t a4,
                       uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  process_t *proc = process_current();
  if (!proc)
    return -EFAULT;
  return vfs_close_for_proc(proc, (int)fd);
}

// ---------- open / openat ----------
static int64_t do_open_common(process_t *proc, const char *raw_path,
                              int linux_flags) {
  char path[VFS_PATH_MAX];
  int rc = resolve_user_path(proc, raw_path, path, sizeof(path));
  if (rc != 0)
    return rc;

  int aflags;
  switch (linux_flags & LINUX_O_ACCMODE) {
  case LINUX_O_RDONLY:
    aflags = O_RDONLY;
    break;
  case LINUX_O_WRONLY:
    aflags = O_WRONLY;
    break;
  case LINUX_O_RDWR:
    aflags = O_RDWR;
    break;
  default:
    aflags = O_RDONLY;
    break;
  }

  int created_here = 0;
  if (linux_flags & LINUX_O_CREAT) {
    int crc = vfs_create(path, linux_flags);
    if (crc == 0) {
      created_here = 1;
    } else if (crc != -EEXIST) {
      return crc;
    }
  }

  int fd = vfs_open_for_proc(proc, path, aflags);
  if (fd < 0)
    return -ENOENT;

  if (!created_here && (linux_flags & LINUX_O_TRUNC)) {
    file_descriptor_t *f = proc->fds[fd];
    if (f && f->node && f->node->ops && f->node->ops->truncate)
      (void)f->node->ops->truncate(f->node, 0);
  }
  if (linux_flags & LINUX_O_APPEND) {
    file_descriptor_t *f = proc->fds[fd];
    if (f && f->node)
      f->offset = f->node->size;
  }
  return fd;
}

static int64_t k_open(uint64_t path, uint64_t flags, uint64_t a3, uint64_t a4,
                      uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;
  process_t *proc = process_current();
  if (!proc)
    return -EFAULT;
  return do_open_common(proc, (const char *)path, (int)flags);
}

static int64_t k_openat(uint64_t dfd, uint64_t path, uint64_t flags,
                        uint64_t mode, uint64_t a5) {
  (void)mode;
  (void)a5;
  process_t *proc = process_current();
  if (!proc)
    return -EFAULT;
  if ((int64_t)dfd != AT_FDCWD)
    return -EINVAL;
  return do_open_common(proc, (const char *)path, (int)flags);
}

// ---------- lseek ----------
static int64_t k_lseek(uint64_t fd, uint64_t offset, uint64_t whence,
                       uint64_t a4, uint64_t a5) {
  (void)a4;
  (void)a5;
  process_t *proc = process_current();
  if (!proc)
    return -EFAULT;
  return vfs_seek_for_proc(proc, (int)fd, (int64_t)offset, (int)whence);
}

// ---------- fstat / stat / fstatat ----------
static int64_t k_fstat(uint64_t fd, uint64_t statbuf, uint64_t a3, uint64_t a4,
                       uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;
  process_t *proc = process_current();
  if (!proc)
    return -EFAULT;
  if (!access_ok((void *)statbuf, sizeof(linux_stat_t)))
    return -EFAULT;

  vfs_stat_t vs;
  int rc = vfs_fstat_for_proc(proc, (int)fd, &vs);
  if (rc != 0)
    return rc;

  linux_stat_t ls;
  vfs_to_linux_stat(&vs, vs.size, &ls);
  if (copy_to_user((void *)statbuf, &ls, sizeof(ls)) < 0)
    return -EFAULT;
  return 0;
}

static int64_t do_stat_path(process_t *proc, const char *upath,
                            uint64_t statbuf, uint64_t flags) {
  (void)flags;
  if (!access_ok((void *)statbuf, sizeof(linux_stat_t)))
    return -EFAULT;

  char path[VFS_PATH_MAX];
  int rc = resolve_user_path(proc, upath, path, sizeof(path));
  if (rc != 0)
    return rc;

  vfs_node_t *node = vfs_lookup(path);
  if (!node)
    return -ENOENT;

  vfs_stat_t vs = {
      .flags = node->flags,
      .size = node->size,
      .inode = node->inode,
  };
  vfs_node_free(node);

  linux_stat_t ls;
  vfs_to_linux_stat(&vs, vs.size, &ls);
  if (copy_to_user((void *)statbuf, &ls, sizeof(ls)) < 0)
    return -EFAULT;
  return 0;
}

static int64_t k_stat(uint64_t path, uint64_t statbuf, uint64_t a3, uint64_t a4,
                      uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;
  process_t *proc = process_current();
  if (!proc)
    return -EFAULT;
  return do_stat_path(proc, (const char *)path, statbuf, 0);
}

static int64_t k_lstat(uint64_t path, uint64_t statbuf, uint64_t a3,
                       uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;
  process_t *proc = process_current();
  if (!proc)
    return -EFAULT;
  return do_stat_path(proc, (const char *)path, statbuf, AT_SYMLINK_NOFOLLOW);
}

static int64_t k_fstatat(uint64_t dfd, uint64_t path, uint64_t statbuf,
                         uint64_t flags, uint64_t a5) {
  (void)a5;
  process_t *proc = process_current();
  if (!proc)
    return -EFAULT;
  if ((int64_t)dfd != AT_FDCWD)
    return -EINVAL;
  return do_stat_path(proc, (const char *)path, statbuf, flags);
}

// ---------- access ----------
static int64_t k_access(uint64_t path, uint64_t mode, uint64_t a3, uint64_t a4,
                        uint64_t a5) {
  (void)mode;
  (void)a3;
  (void)a4;
  (void)a5;
  process_t *proc = process_current();
  if (!proc)
    return -EFAULT;

  char p[VFS_PATH_MAX];
  int rc = resolve_user_path(proc, (const char *)path, p, sizeof(p));
  if (rc != 0)
    return rc;

  vfs_node_t *node = vfs_lookup(p);
  if (!node)
    return -ENOENT;
  vfs_node_free(node);
  return 0;
}

// ---------- mkdir / unlink / rename ----------
static int64_t k_mkdir(uint64_t path, uint64_t mode, uint64_t a3, uint64_t a4,
                       uint64_t a5) {
  (void)mode;
  (void)a3;
  (void)a4;
  (void)a5;
  process_t *proc = process_current();
  if (!proc)
    return -EFAULT;
  char p[VFS_PATH_MAX];
  int rc = resolve_user_path(proc, (const char *)path, p, sizeof(p));
  if (rc != 0)
    return rc;
  return vfs_mkdir(p);
}

static int64_t k_mkdirat(uint64_t dfd, uint64_t path, uint64_t mode,
                         uint64_t a4, uint64_t a5) {
  (void)mode;
  (void)a4;
  (void)a5;
  if ((int64_t)dfd != AT_FDCWD)
    return -EINVAL;
  return k_mkdir(path, 0, 0, 0, 0);
}

static int64_t k_unlink(uint64_t path, uint64_t a2, uint64_t a3, uint64_t a4,
                        uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  process_t *proc = process_current();
  if (!proc)
    return -EFAULT;
  char p[VFS_PATH_MAX];
  int rc = resolve_user_path(proc, (const char *)path, p, sizeof(p));
  if (rc != 0)
    return rc;
  return vfs_unlink(p);
}

static int64_t k_unlinkat(uint64_t dfd, uint64_t path, uint64_t flags,
                          uint64_t a4, uint64_t a5) {
  (void)flags;
  (void)a4;
  (void)a5;
  if ((int64_t)dfd != AT_FDCWD)
    return -EINVAL;
  return k_unlink(path, 0, 0, 0, 0);
}

static int64_t k_rename(uint64_t oldp, uint64_t newp, uint64_t a3, uint64_t a4,
                        uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;
  process_t *proc = process_current();
  if (!proc)
    return -EFAULT;
  char op[VFS_PATH_MAX], np[VFS_PATH_MAX];
  int rc = resolve_user_path(proc, (const char *)oldp, op, sizeof(op));
  if (rc != 0)
    return rc;
  rc = resolve_user_path(proc, (const char *)newp, np, sizeof(np));
  if (rc != 0)
    return rc;
  return vfs_rename(op, np);
}

static int64_t k_renameat(uint64_t odfd, uint64_t oldp, uint64_t ndfd,
                          uint64_t newp, uint64_t a5) {
  (void)a5;
  if ((int64_t)odfd != AT_FDCWD || (int64_t)ndfd != AT_FDCWD)
    return -EINVAL;
  return k_rename(oldp, newp, 0, 0, 0);
}

// ---------- chdir / getcwd ----------
static int64_t k_chdir(uint64_t path, uint64_t a2, uint64_t a3, uint64_t a4,
                       uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  process_t *proc = process_current();
  if (!proc)
    return -EFAULT;
  char p[VFS_PATH_MAX];
  int rc = resolve_user_path(proc, (const char *)path, p, sizeof(p));
  if (rc != 0)
    return rc;

  vfs_node_t *node = vfs_lookup(p);
  if (!node)
    return -ENOENT;
  if (!(node->flags & VFS_DIRECTORY)) {
    vfs_node_free(node);
    return -ENOTDIR;
  }
  vfs_node_free(node);

  size_t plen = strlen(p);
  if (plen >= sizeof(proc->cwd))
    return -ENAMETOOLONG;
  for (size_t i = 0; i < plen; i++)
    proc->cwd[i] = p[i];
  proc->cwd[plen] = '\0';
  return 0;
}

static int64_t k_getcwd(uint64_t buf, uint64_t size, uint64_t a3, uint64_t a4,
                        uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;
  process_t *proc = process_current();
  if (!proc)
    return -EFAULT;
  if (!buf || size == 0)
    return -EINVAL;
  size_t len = strlen(proc->cwd) + 1;
  if (len > (size_t)size)
    return -ERANGE;
  if (copy_to_user((void *)buf, proc->cwd, len) < 0)
    return -EFAULT;
  return (int64_t)len;
}

// ---------- mmap / munmap / mprotect / brk ----------
static int64_t k_mmap(uint64_t addr, uint64_t length, uint64_t prot,
                      uint64_t flags, uint64_t a5) {
  (void)a5;
  process_t *proc = process_current();
  if (!proc)
    return -EFAULT;

  // sys_mmap() recibe `prot` y `flags` de Linux tal cual: los traduce
  // internamente a PTE_*. Antes este wrapper hacía la traducción y
  // sys_mmap recibía solo flags PTE, perdiendo MAP_FIXED,
  // MAP_ANONYMOUS y MAP_SHARED.
  return sys_mmap(proc, addr, length, prot, flags, -1, 0);
}

static int64_t k_munmap(uint64_t addr, uint64_t length, uint64_t a3,
                        uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;
  process_t *proc = process_current();
  if (!proc)
    return -EFAULT;
  return sys_munmap(proc, addr, length);
}

static int64_t k_mprotect(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                          uint64_t a5) {
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  // Aurora no cambia permisos post-mmap todavía. musl lo usa para
  // protección de stack/páginas; devolver 0 es suficiente para arrancar.
  return 0;
}

static int64_t k_brk(uint64_t addr, uint64_t a2, uint64_t a3, uint64_t a4,
                     uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  process_t *proc = process_current();
  if (!proc)
    return -EFAULT;

  uint64_t old = proc->heap_end;
  if (addr == 0)
    return (int64_t)old;

  if (addr < proc->heap_start || addr > proc->heap_max)
    return (int64_t)old;

  int64_t delta = (int64_t)(addr - old);
  void *r = process_sbrk(proc, delta);
  if (r == (void *)-1)
    return (int64_t)old;
  return (int64_t)addr;
}

// ---------- getpid / set_tid_address / gettid ----------
static int64_t k_getpid(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                        uint64_t a5) {
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  process_t *proc = process_current();
  return proc ? (int64_t)proc->pid : -EFAULT;
}

static int64_t k_set_tid_address(uint64_t a1, uint64_t a2, uint64_t a3,
                                 uint64_t a4, uint64_t a5) {
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  process_t *proc = process_current();
  return proc ? (int64_t)proc->pid : -EFAULT;
}

// ---------- getuid/getgid/geteuid/getegid ----------
//
// Aurora no tiene usuarios. Todos los procesos son "root" (uid 0).
// Busybox usa estas en whoami, id, ls -l, sh. Devolver 0 es correcto.
static int64_t k_getuid(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                        uint64_t a5) {
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  return 0;
}
static int64_t k_getgid(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                        uint64_t a5) {
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  return 0;
}
static int64_t k_geteuid(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                         uint64_t a5) {
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  return 0;
}
static int64_t k_getegid(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                         uint64_t a5) {
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  return 0;
}

// ---------- getppid ----------
static int64_t k_getppid(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                         uint64_t a5) {
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  process_t *proc = process_current();
  return proc ? (int64_t)proc->ppid : -EFAULT;
}

// ---------- process groups / sessions ----------

static int64_t k_setpgid(uint64_t pid, uint64_t pgid, uint64_t a3, uint64_t a4,
                         uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;

  process_t *self = process_current();
  if (!self)
    return -EFAULT;

  uint32_t target_pid = (pid == 0) ? self->pid : (uint32_t)pid;
  uint32_t target_pgid = (pgid == 0) ? target_pid : (uint32_t)pgid;

  LOG_INFO("[SETPGID] self=%u target=%u pgid=%u", self->pid, target_pid,
           target_pgid);

  if (target_pgid == 0)
    return -EINVAL;

  process_t *target = process_find_by_pid(target_pid);
  if (!target)
    return -ESRCH;

  // Solo puedes cambiar tu propio grupo, o el de un hijo directo.
  if (target_pid != self->pid && target->ppid != self->pid)
    return -EPERM;

  // No puedes mover un proceso a otra sesión.
  if (target->sid != self->sid)
    return -EPERM;

  // Un session leader no puede cambiar su grupo.
  if (target->sid == target->pid)
    return -EPERM;

  // El pgid debe existir: o es el pid del target mismo (crea grupo),
  // o hay algún proceso en ese grupo dentro de la misma sesión.
  if (target_pgid != target_pid) {
    process_t *p = process_find_by_pid(target_pgid);
    if (!p || p->pgid != target_pgid || p->sid != target->sid)
      return -EPERM;
  }

  target->pgid = target_pgid;
  LOG_INFO("[SETPGID] -> OK pid=%u pgid=%u", target_pid, target_pgid);
  return 0;
}

static int64_t k_getpgid(uint64_t pid, uint64_t a2, uint64_t a3, uint64_t a4,
                         uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  process_t *self = process_current();
  if (!self)
    return -EFAULT;

  uint32_t target_pid = (pid == 0) ? self->pid : (uint32_t)pid;
  process_t *target = process_find_by_pid(target_pid);
  if (!target)
    return -ESRCH;
  return (int64_t)target->pgid;
}

static int64_t k_setsid(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                        uint64_t a5) {
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  process_t *self = process_current();
  if (!self)
    return -EFAULT;

  // Si ya es líder de sesión, no puede volver a crearla.
  if (self->sid == self->pid)
    return -EPERM;

  self->sid = self->pid;
  self->pgid = self->pid;
  self->ctty = NULL; // [CTTY] setsid desasocia el terminal de control
  return (int64_t)self->pid;
}

static int64_t k_getsid(uint64_t pid, uint64_t a2, uint64_t a3, uint64_t a4,
                        uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  process_t *self = process_current();
  if (!self)
    return -EFAULT;

  uint32_t target_pid = (pid == 0) ? self->pid : (uint32_t)pid;
  process_t *target = process_find_by_pid(target_pid);
  if (!target)
    return -ESRCH;
  return (int64_t)target->sid;
}

// Aurora no tiene grupos suplementarios. Con size==0 devolvemos 0 (no
// hay ninguno). Con size>0 devolvemos 0 (no escribimos nada).
static int64_t k_getgroups(uint64_t size, uint64_t list, uint64_t a3,
                           uint64_t a4, uint64_t a5) {
  (void)list;
  (void)a3;
  (void)a4;
  (void)a5;
  return 0;
}

// ---------- prctl ----------
//
// Multiplexado. Aurora no tiene namespaces, capabilities, core dumps
// ni nombres de thread. Aceptamos todo con 0 (éxito), salvo
// PR_GET_DUMPABLE (3) que devolvemos 1 como hace Linux por defecto.
//
// Opciones que musl/busybox usan realmente:
//   1  PR_SET_PDEATHSIG   → ignorar
//   3  PR_GET_DUMPABLE    → devolver 1
//   4  PR_SET_DUMPABLE    → ignorar
//   15 PR_SET_NAME        → ignorar
//   16 PR_GET_NAME        → escribir "" en el buffer del usuario
//   38 PR_SET_NO_NEW_PRIVS → ignorar
static int64_t k_prctl(uint64_t option, uint64_t arg2, uint64_t arg3,
                       uint64_t arg4, uint64_t arg5) {
  (void)arg3;
  (void)arg4;
  (void)arg5;
  switch (option) {
  case 3: /* PR_GET_DUMPABLE */
    return 1;
  case 16: /* PR_GET_NAME */ {
    if (arg2 && access_ok((void *)arg2, 16)) {
      // Copiamos 16 bytes de ceros (Linux pone el nombre del proceso
      // ahí, 16 bytes con terminador).
      char empty[16] = {0};
      if (copy_to_user((void *)arg2, empty, 16) < 0)
        return -EFAULT;
    }
    return 0;
  }
  default:
    return 0;
  }
}

// ---------- kill ----------
static int64_t k_kill(uint64_t pid, uint64_t sig, uint64_t a3, uint64_t a4,
                      uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;
  int32_t kpid = (int32_t)pid;
  int ksig = (int)sig;

  // Señal 0 = comprobar existencia, sin enviar. Linux la acepta.
  if (ksig == 0) {
    if (kpid > 0)
      return process_find_by_pid((uint32_t)kpid) ? 0 : -ESRCH;
    if (kpid == 0)
      return process_current() ? 0 : -ESRCH;
    return process_pgrp_exists((uint32_t)(-kpid)) ? 0 : -ESRCH;
  }

  if (ksig < 1 || ksig >= SIG_MAX)
    return -EINVAL;

  process_t *self = process_current();
  LOG_INFO("[KILL] self=%u kpid=%d sig=%d", self ? self->pid : 0, (int)kpid,
           ksig);

  if (kpid > 0) {
    int need_intr = 0;
    task_t *t = process_signal_pid_ex((uint32_t)kpid, 1ULL << ksig, &need_intr);
    if (!t)
      return -ESRCH;
    if (need_intr)
      wait_queue_interrupt_task(t);
    task_put(t);
    return 0;
  }
  if (kpid == 0) {
    process_t *target = process_current();
    if (!target || !target->task)
      return -ESRCH;
    task_t *t = target->task;
    task_get(t);
    uint64_t mask = signal_filter_ignored(target, 1ULL << ksig);
    if (mask) {
      __atomic_fetch_or(&target->pending_signals, mask, __ATOMIC_RELEASE);
      wait_queue_interrupt_task(t);
    }
    task_put(t);
    return 0;
  }

  // kpid < 0: enviar a todos los procesos del grupo -kpid.
  uint32_t grp = (uint32_t)(-kpid);
  int n = process_signal_pgrp(grp, 1ULL << ksig);
  return n < 0 ? n : 0;
}

// ---------- pipe / dup2 ----------
static int64_t k_pipe(uint64_t fds_ptr, uint64_t a2, uint64_t a3, uint64_t a4,
                      uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  if (!fds_ptr)
    return -EINVAL;
  if (!access_ok((void *)fds_ptr, 2 * sizeof(int)))
    return -EFAULT;

  process_t *proc = process_current();
  if (!proc)
    return -EFAULT;

  int rd = -1, wr = -1;
  for (int i = 0; i < MAX_PROCESS_FDS; i++) {
    if (!proc->fds[i]) {
      if (rd < 0)
        rd = i;
      else {
        wr = i;
        break;
      }
    }
  }
  if (rd < 0 || wr < 0)
    return -EMFILE;

  vfs_node_t *re = NULL, *we = NULL;
  int rc = vfs_pipe_create(&re, &we);
  if (rc != 0)
    return rc;

  file_descriptor_t *rfd = (file_descriptor_t *)kzalloc(sizeof(*rfd));
  file_descriptor_t *wfd = (file_descriptor_t *)kzalloc(sizeof(*wfd));
  if (!rfd || !wfd) {
    kfree(rfd);
    kfree(wfd);
    vfs_node_free(re);
    vfs_node_free(we);
    return -ENOMEM;
  }
  rfd->node = re;
  rfd->flags = O_RDONLY;
  rfd->ref_count = 1;
  wait_queue_init(&rfd->read_wq);
  wait_queue_init(&rfd->write_wq);
  wfd->node = we;
  wfd->flags = O_WRONLY;
  wfd->ref_count = 1;
  wait_queue_init(&wfd->read_wq);
  wait_queue_init(&wfd->write_wq);

  proc->fds[rd] = rfd;
  proc->fds[wr] = wfd;

  int user_fds[2] = {rd, wr};
  if (copy_to_user((void *)fds_ptr, user_fds, sizeof(user_fds)) < 0) {
    vfs_close_for_proc(proc, rd);
    vfs_close_for_proc(proc, wr);
    return -EFAULT;
  }
  return 0;
}

static int64_t k_dup2(uint64_t oldfd, uint64_t newfd, uint64_t a3, uint64_t a4,
                      uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;
  int ofd = (int)oldfd, nfd = (int)newfd;
  process_t *proc = process_current();
  if (!proc)
    return -EFAULT;
  if (ofd < 0 || ofd >= MAX_PROCESS_FDS || !proc->fds[ofd])
    return -EBADF;
  if (nfd < 0 || nfd >= MAX_PROCESS_FDS)
    return -EBADF;
  if (ofd == nfd)
    return nfd;
  if (proc->fds[nfd])
    vfs_close_for_proc(proc, nfd);
  proc->fds[ofd]->ref_count++;
  proc->fds[nfd] = proc->fds[ofd];
  return nfd;
}

static int64_t k_writev(uint64_t fd, uint64_t iov_uptr, uint64_t iovcnt,
                        uint64_t a4, uint64_t a5) {
  (void)a4;
  (void)a5;
  process_t *proc = process_current();
  if (!proc)
    return -EFAULT;
  if (iovcnt == 0)
    return 0;
  if (iovcnt > 1024)
    return -EINVAL;
  if (!access_ok((void *)iov_uptr, iovcnt * sizeof(struct k_iovec)))
    return -EFAULT;

  struct k_iovec local[16];
  int64_t total = 0;
  uint64_t done = 0;
  while (done < iovcnt) {
    uint64_t batch = iovcnt - done;
    if (batch > 16)
      batch = 16;
    if (copy_from_user(local,
                       (void *)(iov_uptr + done * sizeof(struct k_iovec)),
                       batch * sizeof(struct k_iovec)) < 0)
      return total > 0 ? total : -EFAULT;

    for (uint64_t i = 0; i < batch; i++) {
      if (local[i].iov_len == 0)
        continue;
      if (!access_ok((void *)local[i].iov_base, (size_t)local[i].iov_len))
        return total > 0 ? total : -EFAULT;
      int64_t r =
          vfs_write_for_proc(proc, (int)fd, (const void *)local[i].iov_base,
                             (size_t)local[i].iov_len);
      if (r < 0)
        return total > 0 ? total : r;
      total += r;
      // Short write: paramos y devolvemos lo que llevamos.
      if ((uint64_t)r < local[i].iov_len)
        return total;
    }
    done += batch;
  }
  return total;
}

static int64_t k_readv(uint64_t fd, uint64_t iov_uptr, uint64_t iovcnt,
                       uint64_t a4, uint64_t a5) {
  (void)a4;
  (void)a5;
  process_t *proc = process_current();
  if (!proc)
    return -EFAULT;
  if (iovcnt == 0)
    return 0;
  if (iovcnt > 1024)
    return -EINVAL;
  if (!access_ok((void *)iov_uptr, iovcnt * sizeof(struct k_iovec)))
    return -EFAULT;

  struct k_iovec local[16];
  int64_t total = 0;
  uint64_t done = 0;
  while (done < iovcnt) {
    uint64_t batch = iovcnt - done;
    if (batch > 16)
      batch = 16;
    if (copy_from_user(local,
                       (void *)(iov_uptr + done * sizeof(struct k_iovec)),
                       batch * sizeof(struct k_iovec)) < 0)
      return total > 0 ? total : -EFAULT;

    for (uint64_t i = 0; i < batch; i++) {
      if (local[i].iov_len == 0)
        continue;
      if (!access_ok((void *)local[i].iov_base, (size_t)local[i].iov_len))
        return total > 0 ? total : -EFAULT;
      int64_t r = vfs_read_for_proc(proc, (int)fd, (void *)local[i].iov_base,
                                    (size_t)local[i].iov_len);
      if (r < 0)
        return total > 0 ? total : r;
      total += r;
      if (r == 0)
        return total; // EOF
      if ((uint64_t)r < local[i].iov_len)
        return total;
    }
    done += batch;
  }
  return total;
}

// ---------- sched_yield ----------
static int64_t k_sched_yield(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                             uint64_t a5) {
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  sched_yield();
  return 0;
}

// ---------- exit_group ----------
static int64_t k_exit_group(uint64_t code, uint64_t a2, uint64_t a3,
                            uint64_t a4, uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  process_exit_current((int)code);
  // no retorna
}

// ---------- arch_prctl (CRÍTICO para musl) ----------
static int64_t k_arch_prctl(uint64_t code, uint64_t addr, uint64_t a3,
                            uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;
  process_t *proc = process_current();
  if (!proc)
    return -EFAULT;

  switch (code) {
  case ARCH_SET_FS:
    proc->fs_base = addr;
    if (proc->task)
      proc->task->fs_base = addr;
    // Escribimos directamente el MSR. En SMP, switch.asm restaura el
    // valor correcto al cambiar de tarea.
    wrmsr(0xC0000100, addr);
    return 0;

  case ARCH_GET_FS: {
    uint64_t val = proc->fs_base;
    if (!access_ok((void *)addr, sizeof(uint64_t)))
      return -EFAULT;
    if (copy_to_user((void *)addr, &val, sizeof(val)) < 0)
      return -EFAULT;
    return 0;
  }

  case ARCH_SET_GS:
  case ARCH_GET_GS:
    return -EINVAL;

  default:
    return -EINVAL;
  }
}

// ---------- getdents64 ----------
//
// Escribe el linux_dirent64 en un buffer del kernel y lo copia con
// copy_to_user. Escribir directamente al VA del usuario (memcpy o
// asignación de campos) funciona en CPUs sin SMAP, pero en CPUs con
// SMAP (Haswell+, o QEMU con -cpu host) dispara un #PF en modo kernel
// porque el kernel no puede tocar páginas de userland sin stac().
static int64_t k_getdents64(uint64_t fd, uint64_t dirp, uint64_t count,
                            uint64_t a4, uint64_t a5) {
  (void)a4;
  (void)a5;
  process_t *proc = process_current();
  if (!proc)
    return -EFAULT;
  if ((int)fd < 0 || (int)fd >= MAX_PROCESS_FDS || !proc->fds[fd])
    return -EBADF;

  file_descriptor_t *f = proc->fds[fd];
  if (!f->node || !(f->node->flags & VFS_DIRECTORY))
    return -ENOTDIR;
  if (!f->node->ops || !f->node->ops->readdir)
    return -ENOSYS;

  if (!access_ok((void *)dirp, count))
    return -EFAULT;

  // Buffer en pila para un linux_dirent64 con nombre de hasta
  // VFS_PATH_MAX-1 bytes. sizeof(linux_dirent64_t)=19 + 128 + padding ≈ 160.
  uint8_t kbuf[256];
  size_t pos = 0;
  uint64_t index = f->offset;

  while (pos + sizeof(linux_dirent64_t) + 8 <= count) {
    vfs_dirent_t ent;
    int rc = f->node->ops->readdir(f->node, index, &ent);
    if (rc != 0)
      break;
    if (ent.name[0] == '\0')
      break;

    size_t namelen = strlen(ent.name);
    size_t reclen = sizeof(linux_dirent64_t) + namelen + 1;
    reclen = (reclen + 7) & ~7ULL;
    if (pos + reclen > count)
      break;
    if (reclen > sizeof(kbuf))
      break; // nombre imposible, no debería pasar

    memset(kbuf, 0, reclen);
    linux_dirent64_t *d = (linux_dirent64_t *)kbuf;
    d->d_ino = (uint64_t)(index + 1);
    d->d_off = (int64_t)(index + 1);
    d->d_reclen = (uint16_t)reclen;
    if (ent.type == VFS_DIRECTORY)
      d->d_type = DT_DIR;
    else if (ent.type == VFS_CHARDEVICE)
      d->d_type = DT_CHR;
    else
      d->d_type = DT_REG;
    memcpy(d->d_name, ent.name, namelen);
    d->d_name[namelen] = '\0';

    // Copia al VA de userland. copy_to_user usa stac/clac y, si la
    // página no es accesible, el fixup devuelve -EFAULT sin panickear.
    if (copy_to_user((uint8_t *)dirp + pos, kbuf, reclen) < 0)
      return pos > 0 ? (int64_t)pos : -EFAULT;

    pos += reclen;
    index++;
  }

  f->offset = index;
  return (int64_t)pos;
}

// ---------- uname ----------
static int64_t k_uname(uint64_t buf, uint64_t a2, uint64_t a3, uint64_t a4,
                       uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  struct {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
    char domainname[65];
  } u;
  memset(&u, 0, sizeof(u));
  strcpy(u.sysname, "Aurora");
  strcpy(u.nodename, "aurora");
  strcpy(u.release, "0.1.0");
  strcpy(u.version, "Aurora OS v0.1.0");
  strcpy(u.machine, "x86_64");
  u.domainname[0] = '\0';

  if (!access_ok((void *)buf, sizeof(u)))
    return -EFAULT;
  if (copy_to_user((void *)buf, &u, sizeof(u)) < 0)
    return -EFAULT;
  return 0;
}

// ---------- readlink ----------
static int64_t k_readlink(uint64_t path, uint64_t buf, uint64_t bufsiz,
                          uint64_t a4, uint64_t a5) {
  (void)path;
  (void)buf;
  (void)bufsiz;
  (void)a4;
  (void)a5;
  return -ENOENT;
}

// ---------- ioctl ----------
static int64_t k_ioctl(uint64_t fd, uint64_t req, uint64_t arg, uint64_t a4,
                       uint64_t a5) {
  (void)a4;
  (void)a5;
  process_t *proc = process_current();
  if (!proc)
    return -EFAULT;
  int kfd = (int)fd;
  if (kfd < 0 || kfd >= MAX_PROCESS_FDS || !proc->fds[kfd])
    return -EBADF;
  // [ioctl] musl pasa `req` como int (sign-extended en x86_64). Los
  // ioctls con bit 31 puesto (todos los _IOR/_IOW modernos, p. ej.
  // TIOCGPTN, TIOCSPTLCK, DRM, V4L2, ...) llegan como
  // 0xffffffffXXXXXXXX. Truncamos a 32 bits, que es lo que hacen
  // Linux y cualquier kernel POSIX en el syscall ioctl.
  unsigned long kreq = (unsigned long)(uint32_t)req;
  return vfs_node_ioctl(proc->fds[kfd]->node, kreq, arg);
}

// ---------- clock_gettime ----------
static int64_t k_clock_gettime(uint64_t clockid, uint64_t tp, uint64_t a3,
                               uint64_t a4, uint64_t a5) {
  (void)clockid;
  (void)a3;
  (void)a4;
  (void)a5;
  if (!access_ok((void *)tp, 16))
    return -EFAULT;
  struct {
    int64_t sec;
    int64_t nsec;
  } ts;
  extern uint64_t sched_get_ticks(void);
  uint64_t ticks = sched_get_ticks();
  ts.sec = (int64_t)(ticks / 1000);
  ts.nsec = (int64_t)((ticks % 1000) * 1000000);
  if (copy_to_user((void *)tp, &ts, sizeof(ts)) < 0)
    return -EFAULT;
  return 0;
}

// ---------- set_robust_list ----------
static int64_t k_set_robust_list(uint64_t a1, uint64_t a2, uint64_t a3,
                                 uint64_t a4, uint64_t a5) {
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  return 0;
}

// ---------- prlimit64 ----------
static int64_t k_prlimit64(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                           uint64_t a5) {
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  return -ENOSYS;
}

// ---------- getrandom ----------
static int64_t k_getrandom(uint64_t buf, uint64_t buflen, uint64_t flags,
                           uint64_t a4, uint64_t a5) {
  (void)flags;
  (void)a4;
  (void)a5;
  if (buflen == 0)
    return 0;
  if (!access_ok((void *)buf, buflen))
    return -EFAULT;

  size_t written = 0;
  uint8_t out[8];
  while (written < buflen) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    uint64_t r = ((uint64_t)hi << 32) | lo;
    r ^= (r << 13);
    r ^= (r >> 7);
    r ^= (r << 17);
    size_t n = buflen - written;
    if (n > 8)
      n = 8;
    memcpy(out, &r, n);
    if (copy_to_user((uint8_t *)buf + written, out, n) < 0)
      return -EFAULT;
    written += n;
  }
  return (int64_t)buflen;
}

// ---------- rseq ----------
static int64_t k_rseq(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                      uint64_t a5) {
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  return -ENOSYS;
}

// ---------- fcntl ----------
//
// musl lo usa en __stdio_init_file (F_GETFD, F_GETFL) y en las rutas de
// dup2/O_APPEND/O_NONBLOCK. Sin él, printf() inicializa el FILE con
// flags basura y revienta al primer flush.
//
// Comandos Linux x86_64:
//   F_DUPFD=0, F_GETFD=1, F_SETFD=2, F_GETFL=3, F_SETFL=4,
//   F_DUPFD_CLOEXEC=1030.
static int64_t k_fcntl(uint64_t fd, uint64_t cmd, uint64_t arg, uint64_t a4,
                       uint64_t a5) {
  (void)a4;
  (void)a5;
  process_t *proc = process_current();
  if (!proc)
    return -EFAULT;
  int kfd = (int)fd;
  if (kfd < 0 || kfd >= MAX_PROCESS_FDS || !proc->fds[kfd])
    return -EBADF;

  file_descriptor_t *f = proc->fds[kfd];

  switch (cmd) {
  case 0:    /* F_DUPFD */
  case 1030: /* F_DUPFD_CLOEXEC */
  {
    int minfd = (int)arg;
    if (minfd < 0 || minfd >= MAX_PROCESS_FDS)
      return -EINVAL;
    int free_fd = -1;
    for (int i = minfd; i < MAX_PROCESS_FDS; i++) {
      if (!proc->fds[i]) {
        free_fd = i;
        break;
      }
    }
    if (free_fd < 0)
      return -EMFILE;
    f->ref_count++;
    proc->fds[free_fd] = f;
    return (int64_t)free_fd;
  }
  case 1: /* F_GETFD */
    return 0;
  case 2: /* F_SETFD */
    return 0;
  case 3: /* F_GETFL */
    return (int64_t)f->flags;
  case 4: /* F_SETFL */
    return 0;
  default:
    return -EINVAL;
  }
}

// ---------- poll / ppoll / select / pselect6 ----------

struct k_pollfd {
  int32_t fd;
  int16_t events;
  int16_t revents;
};

struct k_timespec {
  int64_t tv_sec;
  int64_t tv_nsec;
};

// POLL* bits (Linux).
#define POLLIN 0x0001
#define POLLPRI 0x0002
#define POLLOUT 0x0004
#define POLLERR 0x0008
#define POLLHUP 0x0010
#define POLLNVAL 0x0020

static uint64_t ts_to_ticks(int64_t sec, int64_t nsec) {
  if (sec < 0 || nsec < 0)
    return 0;
  uint64_t s = (uint64_t)sec;
  uint64_t ns = (uint64_t)nsec;
  // KERNEL_HZ = 1000 en Aurora.
  uint64_t ticks = s * 1000ULL + ns / 1000000ULL;
  return ticks;
}

// Cond para wait_event: ¿alguno de los fds está listo?
struct poll_ctx {
  struct k_pollfd *fds;
  uint32_t nfds;
  int any_ready;
};

static bool poll_any_ready(void *arg) {
  struct poll_ctx *ctx = (struct poll_ctx *)arg;
  ctx->any_ready = 0;
  for (uint32_t i = 0; i < ctx->nfds; i++) {
    if (ctx->fds[i].fd < 0)
      continue;
    if (ctx->fds[i].revents) {
      ctx->any_ready = 1;
      break;
    }
  }
  return ctx->any_ready;
}

// Núcleo. timeouts en ticks: 0 = no bloquear, >0 = bloquear hasta N ticks,
// UINT64_MAX = bloquear indefinidamente.
#define POLL_BLOCK_FOREVER UINT64_MAX

static int64_t do_poll_common(struct k_pollfd *kfds, uint32_t nfds,
                              uint64_t timeout_ticks) {
  process_t *proc = process_current();
  if (!proc)
    return -EFAULT;

  int forever = (timeout_ticks == POLL_BLOCK_FOREVER);
  uint64_t deadline = 0;
  if (!forever)
    deadline = sched_get_ticks() + timeout_ticks;

  for (;;) {
    int ready_count = 0;
    wait_queue_t *wq = NULL;

    for (uint32_t i = 0; i < nfds; i++) {
      int32_t fd = kfds[i].fd;
      kfds[i].revents = 0;
      if (fd < 0)
        continue;
      if (fd >= MAX_PROCESS_FDS || !proc->fds[fd]) {
        kfds[i].revents = POLLNVAL;
        ready_count++;
        continue;
      }
      file_descriptor_t *f = proc->fds[fd];
      int rev = vfs_node_poll(f->node, kfds[i].events);
      if (rev) {
        kfds[i].revents = (int16_t)rev;
        ready_count++;
      } else if (!wq && f->node && f->node->ops && f->node->ops->poll) {
        if (f->node->ops->poll_wq)
          wq = f->node->ops->poll_wq(f->node);
        if (!wq)
          wq = &f->read_wq;
      }
    }

    if (ready_count > 0)
      return ready_count;

    // Nadie listo. Calcular cuánto dormir sin perder el deadline.
    uint64_t now = sched_get_ticks();
    uint64_t wait_ticks;
    if (forever) {
      wait_ticks = 100; // 100 ms máx por iteración, para no quedar atrapados
                        // si la wq nunca se despierta por otra vía.
    } else {
      if (now >= deadline)
        return 0;
      wait_ticks = deadline - now;
      if (wait_ticks > 100)
        wait_ticks = 100;
    }

    if (wq) {
      struct poll_ctx ctx = {.fds = kfds, .nfds = nfds, .any_ready = 0};
      long r = wait_event_interruptible_timeout(wq, poll_any_ready, &ctx,
                                                wait_ticks);
      if (r < 0)
        return -EINTR;
    } else {
      sched_sleep_ticks(wait_ticks);
    }

    // Si el timeout total ya se agotó, salir.
    if (!forever && sched_get_ticks() >= deadline)
      return 0;
  }
}

static int64_t k_poll(uint64_t fds_ptr, uint64_t nfds, uint64_t timeout_ms,
                      uint64_t a4, uint64_t a5) {
  (void)a4;
  (void)a5;
  if (nfds == 0) {
    // Poll sin fds: solo esperar.
    uint64_t t = (int64_t)timeout_ms < 0 ? POLL_BLOCK_FOREVER
                                         : (uint64_t)(int64_t)timeout_ms;
    if (t == POLL_BLOCK_FOREVER)
      return 0;
    sched_sleep_ticks(t);
    return 0;
  }
  if (nfds > 1024)
    return -EINVAL;
  if (!access_ok((void *)fds_ptr, nfds * sizeof(struct k_pollfd)))
    return -EFAULT;
  struct k_pollfd kfds[16];
  struct k_pollfd *big = NULL;
  if (nfds > 16) {
    big = (struct k_pollfd *)kmalloc(nfds * sizeof(struct k_pollfd));
    if (!big)
      return -ENOMEM;
  }
  struct k_pollfd *dst = big ? big : kfds;
  if (copy_from_user(dst, (void *)fds_ptr, nfds * sizeof(struct k_pollfd)) <
      0) {
    if (big)
      kfree(big);
    return -EFAULT;
  }
  uint64_t ticks;
  int64_t t = (int64_t)timeout_ms;
  if (t < 0)
    ticks = POLL_BLOCK_FOREVER;
  else
    ticks = (uint64_t)t; /* ms == ticks en Aurora (1000 Hz) */
  int64_t r = do_poll_common(dst, (uint32_t)nfds, ticks);
  if (r >= 0) {
    if (copy_to_user((void *)fds_ptr, dst, nfds * sizeof(struct k_pollfd)) < 0)
      r = -EFAULT;
  }
  if (big)
    kfree(big);
  return r;
}

static int64_t k_ppoll(uint64_t fds_ptr, uint64_t nfds, uint64_t ts_ptr,
                       uint64_t a4, uint64_t a5) {
  (void)a4;
  (void)a5; /* a4=sigmask, a5=sigsetsize: ignorados */
  uint64_t ticks = POLL_BLOCK_FOREVER;
  if (ts_ptr) {
    if (!access_ok((void *)ts_ptr, sizeof(struct k_timespec)))
      return -EFAULT;
    struct k_timespec ts;
    if (copy_from_user(&ts, (void *)ts_ptr, sizeof(ts)) < 0)
      return -EFAULT;
    ticks = ts_to_ticks(ts.tv_sec, ts.tv_nsec);
  }
  if (nfds == 0) {
    if (ticks == POLL_BLOCK_FOREVER)
      return 0;
    sched_sleep_ticks(ticks);
    return 0;
  }
  if (nfds > 1024)
    return -EINVAL;
  if (!access_ok((void *)fds_ptr, nfds * sizeof(struct k_pollfd)))
    return -EFAULT;
  struct k_pollfd kfds[16];
  struct k_pollfd *big = NULL;
  if (nfds > 16) {
    big = (struct k_pollfd *)kmalloc(nfds * sizeof(struct k_pollfd));
    if (!big)
      return -ENOMEM;
  }
  struct k_pollfd *dst = big ? big : kfds;
  if (copy_from_user(dst, (void *)fds_ptr, nfds * sizeof(struct k_pollfd)) <
      0) {
    if (big)
      kfree(big);
    return -EFAULT;
  }
  int64_t r = do_poll_common(dst, (uint32_t)nfds, ticks);
  if (r >= 0)
    if (copy_to_user((void *)fds_ptr, dst, nfds * sizeof(struct k_pollfd)) < 0)
      r = -EFAULT;
  if (big)
    kfree(big);
  return r;
}

// select/pselect6: convertimos fd_set a un array de pollfd y reusamos
// do_poll_common. No implementamos el enmascarado de señales (sigmask
// se ignora). El tamaño de fd_set en x86_64 Linux es 128 bytes (1024 bits).
#define FD_SET_BYTES 128

static int64_t do_select_common(uint64_t nfds, uint64_t readfds,
                                uint64_t writefds, uint64_t exceptfds,
                                uint64_t ticks) {
  if (nfds == 0) {
    if (ticks == POLL_BLOCK_FOREVER)
      return 0;
    sched_sleep_ticks(ticks);
    return 0;
  }
  if (nfds > 1024)
    nfds = 1024;

  uint8_t kr[FD_SET_BYTES], kw[FD_SET_BYTES], ke[FD_SET_BYTES];
  memset(kr, 0, sizeof(kr));
  memset(kw, 0, sizeof(kw));
  memset(ke, 0, sizeof(ke));
  if (readfds)
    if (copy_from_user(kr, (void *)readfds, sizeof(kr)) < 0)
      return -EFAULT;
  if (writefds)
    if (copy_from_user(kw, (void *)writefds, sizeof(kw)) < 0)
      return -EFAULT;
  if (exceptfds)
    if (copy_from_user(ke, (void *)exceptfds, sizeof(ke)) < 0)
      return -EFAULT;

  // Contar fds únicos y construir pollfds.
  struct k_pollfd *pfds = (struct k_pollfd *)kmalloc(nfds * sizeof(*pfds));
  if (!pfds)
    return -ENOMEM;
  uint32_t npfd = 0;
  for (uint64_t i = 0; i < nfds; i++) {
    short ev = 0;
    if (readfds && (kr[i / 8] & (1u << (i % 8))))
      ev |= POLLIN;
    if (writefds && (kw[i / 8] & (1u << (i % 8))))
      ev |= POLLOUT;
    if (exceptfds && (ke[i / 8] & (1u << (i % 8))))
      ev |= POLLPRI;
    if (!ev)
      continue;
    pfds[npfd].fd = (int32_t)i;
    pfds[npfd].events = ev;
    pfds[npfd].revents = 0;
    npfd++;
  }

  int64_t r = do_poll_common(pfds, npfd, ticks);
  if (r < 0) {
    kfree(pfds);
    return r;
  }

  // Escribir de vuelta los fd_sets.
  uint8_t or_[FD_SET_BYTES], ow[FD_SET_BYTES], oe[FD_SET_BYTES];
  memset(or_, 0, sizeof(or_));
  memset(ow, 0, sizeof(ow));
  memset(oe, 0, sizeof(oe));
  int total = 0;
  for (uint32_t k = 0; k < npfd; k++) {
    int fd = pfds[k].fd;
    int rev = pfds[k].revents;
    if (!rev)
      continue;
    if ((rev & (POLLIN | POLLHUP | POLLERR)) && readfds) {
      or_[fd / 8] |= (1u << (fd % 8));
      total++;
    }
    if ((rev & POLLOUT) && writefds) {
      ow[fd / 8] |= (1u << (fd % 8));
      total++;
    }
    if ((rev & POLLPRI) && exceptfds) {
      oe[fd / 8] |= (1u << (fd % 8));
      total++;
    }
  }
  if (readfds && copy_to_user((void *)readfds, or_, sizeof(or_)) < 0) {
    kfree(pfds);
    return -EFAULT;
  }
  if (writefds && copy_to_user((void *)writefds, ow, sizeof(ow)) < 0) {
    kfree(pfds);
    return -EFAULT;
  }
  if (exceptfds && copy_to_user((void *)exceptfds, oe, sizeof(oe)) < 0) {
    kfree(pfds);
    return -EFAULT;
  }
  kfree(pfds);
  return total;
}

static int64_t k_select(uint64_t nfds, uint64_t r, uint64_t w, uint64_t e,
                        uint64_t tv_ptr) {
  uint64_t ticks = POLL_BLOCK_FOREVER;
  if (tv_ptr) {
    if (!access_ok((void *)tv_ptr, 16))
      return -EFAULT;
    struct {
      int64_t sec;
      int64_t usec;
    } tv;
    if (copy_from_user(&tv, (void *)tv_ptr, sizeof(tv)) < 0)
      return -EFAULT;
    if (tv.sec < 0 || tv.usec < 0)
      return -EINVAL;
    ticks = (uint64_t)tv.sec * 1000ULL + (uint64_t)tv.usec / 1000ULL;
  }
  return do_select_common(nfds, r, w, e, ticks);
}

static int64_t k_pselect6(uint64_t nfds, uint64_t r, uint64_t w, uint64_t e,
                          uint64_t ts_ptr) {
  /* sigmask (r9 en el ABI) ignorado: no entregamos señales todavía. */
  uint64_t ticks = POLL_BLOCK_FOREVER;
  if (ts_ptr) {
    if (!access_ok((void *)ts_ptr, sizeof(struct k_timespec)))
      return -EFAULT;
    struct k_timespec ts;
    if (copy_from_user(&ts, (void *)ts_ptr, sizeof(ts)) < 0)
      return -EFAULT;
    ticks = ts_to_ticks(ts.tv_sec, ts.tv_nsec);
  }
  return do_select_common(nfds, r, w, e, ticks);
}

// ---------- nanosleep ----------
static int64_t k_nanosleep(uint64_t req_ptr, uint64_t rem_ptr, uint64_t a3,
                           uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;
  if (!access_ok((void *)req_ptr, sizeof(struct k_timespec)))
    return -EFAULT;
  struct k_timespec ts;
  if (copy_from_user(&ts, (void *)req_ptr, sizeof(ts)) < 0)
    return -EFAULT;
  if (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000LL)
    return -EINVAL;

  uint64_t ticks = ts_to_ticks(ts.tv_sec, ts.tv_nsec);
  uint64_t deadline = sched_get_ticks() + ticks;
  int interrupted = 0;

  process_t *self = process_current();
  uint32_t spid = self ? self->pid : 0;

  // Bucle hasta agotar el deadline. Si se interrumpe, devolvemos en rem el
  // tiempo REAL restante para que BusyBox retome donde lo dejó.
  while (1) {
    uint64_t now = sched_get_ticks();
    if (now >= deadline)
      break;
    uint64_t remaining = deadline - now;

    wait_queue_t wq;
    wait_queue_init(&wq);
    long r = wait_event_interruptible_timeout(&wq, sleep_never_true, NULL,
                                              remaining);
    if (r < 0) {
      interrupted = 1;
      break;
    }
  }

  uint64_t now_end = sched_get_ticks();
  uint64_t left = (now_end < deadline) ? (deadline - now_end) : 0;

  // [DIAG] Si esto sale con left≈100000 e interrupted=0 el bug está en el
  // wait; con interrupted=1 fue una señal (mira pending/blocked).
  LOG_INFO("[NANOSLEEP] pid=%u req=%lu ticks left=%lu interrupted=%d "
           "pending=0x%lx blocked=0x%lx",
           spid, (unsigned long)ticks, (unsigned long)left, interrupted,
           self ? (unsigned long)__atomic_load_n(&self->pending_signals,
                                                 __ATOMIC_ACQUIRE)
                : 0UL,
           self ? (unsigned long)self->blocked_signals : 0UL);

  if (rem_ptr) {
    struct k_timespec rem;
    if (interrupted) {
      rem.tv_sec = (int64_t)(left / 1000);
      rem.tv_nsec = (int64_t)((left % 1000) * 1000000);
    } else {
      rem.tv_sec = 0;
      rem.tv_nsec = 0;
    }
    if (copy_to_user((void *)rem_ptr, &rem, sizeof(rem)) < 0)
      return -EFAULT;
  }

  return interrupted ? -EINTR : 0;
}

// ---------- fork / clone / wait4 ----------
static int64_t k_fork(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                      uint64_t a5) {
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  return sys_fork();
}

// ---------------------------------------------------------------------------
// vfork (syscall 58).
//
// En Linux comparte el address space del padre y lo bloquea hasta que
// el hijo hace execve() o _exit(). Busybox lo usa en `time`, `nice`,
// y el spawn de ash para ahorrar la copia de memoria en el caso
// "fork + execve inmediato".
//
// Aquí lo implementamos como fork(). El hijo tendrá su propia copia
// del espacio, lo cual es correcto y algo más lento pero sin efectos
// observables para el patrón fork+execve: el hijo llama a execve
// justo después y descarta su copia.
//
// La diferencia real (padre bloqueado hasta execve) la omitimos por
// ahora: busybox `time` espera con wait4 inmediatamente después, así
// que no depende de ese bloqueo.
// ---------------------------------------------------------------------------
static int64_t k_vfork(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                       uint64_t a5) {
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  return sys_fork();
}

// ---------- pivot_root ----------
static int64_t k_pivot_root(uint64_t new_root_uptr, uint64_t put_old_uptr,
                            uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;
  process_t *proc = process_current();
  if (!proc)
    return -EFAULT;

  char nr[VFS_PATH_MAX];
  int rc = resolve_user_path(proc, (const char *)new_root_uptr, nr, sizeof(nr));
  if (rc != 0)
    return rc;

  char po[VFS_PATH_MAX];
  rc = resolve_user_path(proc, (const char *)put_old_uptr, po, sizeof(po));
  if (rc != 0)
    return rc;

  return vfs_pivot_root(nr, po);
}

// En x86_64 Linux, clone(flags, stack, ptid, tls, ctid). Solo soportamos
// el caso "fork-like": flags cuyo byte bajo es la señal de salida y
// ningún bit CLONE_* activo. Cualquier otro uso devuelve -ENOSYS.
#define CLONE_FLAG_MASK 0xFFFFFF00ULL // bits ≥ 8

static int64_t k_clone(uint64_t flags, uint64_t stack, uint64_t ptid,
                       uint64_t tls, uint64_t ctid) {
  (void)stack;
  (void)ptid;
  (void)tls;
  (void)ctid;
  if (flags & CLONE_FLAG_MASK) {
    LOG_WARN("[CLONE] flags=%lx no soportados", (unsigned long)flags);
    return -ENOSYS;
  }
  return sys_fork();
}

static int64_t k_wait4(uint64_t pid, uint64_t status_ptr, uint64_t options,
                       uint64_t rusage_ptr, uint64_t a5) {
  (void)a5;
  process_t *self = process_current();
  if (!self)
    return -EFAULT;

  LOG_INFO("[WAIT4] self=%u pid=%ld options=0x%lx", self->pid,
           (long)(int32_t)pid, (unsigned long)options);

  int32_t aurora_status = 0;
  proc_rusage_t ru = {0};
  int r =
      process_waitpid(self, (int32_t)pid, &aurora_status, &ru, (int)options);
  LOG_INFO("[WAIT4] -> %d", r);
  if (r < 0)
    return r;

  // r == 0 → WNOHANG sin hijos listos. No tocar status/rusage.
  // r > 0 → pid de un hijo recogido.
  if (r > 0) {
    if (status_ptr) {
      if (!access_ok((void *)status_ptr, sizeof(int32_t)))
        return -EFAULT;
      // process_waitpid ya escribió el status en formato Linux:
      //   exited:    (code & 0xff) << 8
      //   stopped:   ((sig & 0xff) << 8) | 0x7f
      //   continued: 0xffff
      if (put_user_u32((uint32_t *)status_ptr, (uint32_t)aurora_status) < 0)
        return -EFAULT;
    }
    if (rusage_ptr) {
      if (!access_ok((void *)rusage_ptr, sizeof(struct k_rusage)))
        return -EFAULT;

      struct k_rusage kr;
      memset(&kr, 0, sizeof(kr));

      // Kernel HZ = 1000 (LAPIC a 1 kHz).
      //   ticks → timeval = (sec = ticks/1000, usec = (ticks%1000)*1000)
      kr.ru_utime.tv_sec = (int64_t)(ru.utime_ticks / 1000);
      kr.ru_utime.tv_usec = (int64_t)((ru.utime_ticks % 1000) * 1000);
      kr.ru_stime.tv_sec = (int64_t)(ru.stime_ticks / 1000);
      kr.ru_stime.tv_usec = (int64_t)((ru.stime_ticks % 1000) * 1000);
      kr.ru_maxrss = ru.maxrss_kb;
      kr.ru_minflt = (int64_t)ru.minflt;
      kr.ru_majflt = (int64_t)ru.majflt;
      kr.ru_nvcsw = (int64_t)ru.nvcsw;
      kr.ru_nivcsw = (int64_t)ru.nivcsw;

      if (copy_to_user((void *)rusage_ptr, &kr, sizeof(kr)) < 0)
        return -EFAULT;
    }
  }
  return r;
}

// ---------- execve ----------
//
// Reemplaza el binario del proceso actual. No crea tarea nueva.
// El "retorno" del syscall aterriza en el entry point del nuevo ELF:
// el dispatcher devuelve 0 → RAX=0, y regs->rip/rsp redirigen el
// sysretq/iretq final.
static int64_t k_execve(uint64_t path_ptr, uint64_t argv_ptr, uint64_t envp_ptr,
                        uint64_t a4, uint64_t a5) {
  (void)a4;
  (void)a5;
  process_t *proc = process_current();
  if (!proc)
    return -EFAULT;

  char path[VFS_PATH_MAX];
  int rc = resolve_user_path(proc, (const char *)path_ptr, path, sizeof(path));
  if (rc != 0)
    return rc;

  // Buffers en heap: 16*128 + 32*256 = 10 KB no caben cómodos en el
  // stack de kernel sin disparar -Wframe-larger-than.
  enum {
    ARGV_BYTES = PROCESS_ARGV_MAX * SPAWN_ARG_STR_MAX,
    ENVP_BYTES = PROCESS_ENVP_MAX * PROCESS_ENV_STR_MAX
  };
  char *buf = (char *)kmalloc(ARGV_BYTES + ENVP_BYTES);
  if (!buf)
    return -ENOMEM;
  char *argv_storage = buf;
  char *env_storage = buf + ARGV_BYTES;

  const char *kargv[PROCESS_ARGV_MAX];
  int argc = 0;
  if (argv_ptr) {
    for (int i = 0; i < PROCESS_ARGV_MAX; i++) {
      uint64_t uptr;
      if (get_user_u64(&uptr, (const uint64_t *)(argv_ptr + (uint64_t)i * 8)) <
          0) {
        kfree(buf);
        return -EFAULT;
      }
      if (uptr == 0)
        break;
      long m = strncpy_from_user(&argv_storage[i * SPAWN_ARG_STR_MAX],
                                 (const char *)uptr, SPAWN_ARG_STR_MAX);
      if (m < 0) {
        kfree(buf);
        return -EFAULT;
      }
      kargv[i] = &argv_storage[i * SPAWN_ARG_STR_MAX];
      argc++;
    }
  }

  const char *kenvp[PROCESS_ENVP_MAX];
  int envc = 0;
  if (envp_ptr) {
    for (int i = 0; i < PROCESS_ENVP_MAX; i++) {
      uint64_t uptr;
      if (get_user_u64(&uptr, (const uint64_t *)(envp_ptr + (uint64_t)i * 8)) <
          0) {
        kfree(buf);
        return -EFAULT;
      }
      if (uptr == 0)
        break;
      long m = strncpy_from_user(&env_storage[i * PROCESS_ENV_STR_MAX],
                                 (const char *)uptr, PROCESS_ENV_STR_MAX);
      if (m < 0) {
        kfree(buf);
        return -EFAULT;
      }
      kenvp[i] = &env_storage[i * PROCESS_ENV_STR_MAX];
      envc++;
    }
  }

  uint64_t new_entry = 0, new_rsp = 0;
  rc = process_execve_prepare(path, argc, kargv, envc, kenvp, &new_entry,
                              &new_rsp);
  kfree(buf);
  if (rc != 0)
    return rc;

  registers_t *regs = syscall_current_regs();
  if (!regs)
    return -EINVAL;
  regs->rip = new_entry;
  regs->rsp = new_rsp;

  return 0;
}

// ---------- gettid ----------
static int64_t k_gettid(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                        uint64_t a5) {
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  process_t *proc = process_current();
  return proc ? (int64_t)proc->pid : -EFAULT;
}

static int64_t k_sendfile(uint64_t out_fd, uint64_t in_fd, uint64_t offset_ptr,
                          uint64_t count, uint64_t a5) {
  (void)a5;
  process_t *proc = process_current();
  if (!proc)
    return -EFAULT;
  if ((int)out_fd < 0 || (int)out_fd >= MAX_PROCESS_FDS || !proc->fds[out_fd])
    return -EBADF;
  if ((int)in_fd < 0 || (int)in_fd >= MAX_PROCESS_FDS || !proc->fds[in_fd])
    return -EBADF;

  file_descriptor_t *out = proc->fds[out_fd];
  file_descriptor_t *in = proc->fds[in_fd];
  if (!out->node || !out->node->ops || !out->node->ops->write)
    return -EINVAL;
  if (!in->node || !in->node->ops || !in->node->ops->read)
    return -EINVAL;

  // Ignoramos offset_ptr por simplicidad: usamos el offset del fd de
  // entrada (que es lo que hace sendfile con offset==NULL).
  uint8_t buf[4096];
  size_t total = 0;
  while (total < count) {
    size_t chunk = count - total;
    if (chunk > sizeof(buf))
      chunk = sizeof(buf);
    int64_t r = in->node->ops->read(in->node, in->offset, chunk, buf);
    if (r <= 0)
      break;
    int64_t w = out->node->ops->write(out->node, out->offset, r, buf);
    if (w < 0) {
      if (total == 0)
        return w;
      break;
    }
    in->offset += r;
    out->offset += w;
    total += w;
    if (w < r)
      break;
  }
  return (int64_t)total;
}

// ===========================================================================
//  AURORA-ONLY HANDLERS  (rango 0x1000+)
// ===========================================================================

static int64_t a_print(uint64_t arg1, uint64_t a2, uint64_t a3, uint64_t a4,
                       uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  char buffer[256];
  long n = strncpy_from_user(buffer, (const char *)arg1, sizeof(buffer));
  if (n < 0)
    return -EFAULT;
  serial_puts(buffer);
  return 0;
}

static int64_t a_get_task_id(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                             uint64_t a5) {
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  task_t *cur = sched_current();
  return cur ? (int64_t)cur->id : -EFAULT;
}

static int64_t a_spawn(uint64_t path, uint64_t a2, uint64_t a3, uint64_t a4,
                       uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  process_t *proc = process_current();
  if (!proc)
    return -EFAULT;
  char p[VFS_PATH_MAX];
  int rc = resolve_user_path(proc, (const char *)path, p, sizeof(p));
  if (rc != 0)
    return rc;
  process_t *child = process_spawn_child(proc, p);
  return child ? (int64_t)child->pid : -ENOENT;
}

static int64_t a_spawn_args(uint64_t path, uint64_t argv, uint64_t argc,
                            uint64_t envp_ptr, uint64_t a5) {
  (void)a5;
  process_t *proc = process_current();
  if (!proc || !path)
    return -EFAULT;

  char p[VFS_PATH_MAX];
  int rc = resolve_user_path(proc, (const char *)path, p, sizeof(p));
  if (rc != 0)
    return rc;

  int ac = (int)argc;
  if (ac < 0 || ac > SPAWN_ARGS_MAX)
    return -EINVAL;

  enum {
    ARGV_BYTES = SPAWN_ARGS_MAX * SPAWN_ARG_STR_MAX,
    ENVP_BYTES = PROCESS_ENVP_MAX * PROCESS_ENV_STR_MAX
  };
  char *buf = (char *)kmalloc(ARGV_BYTES + ENVP_BYTES);
  if (!buf)
    return -ENOMEM;
  char *argv_storage = buf;
  char *env_storage = buf + ARGV_BYTES;

  const char *kargv[SPAWN_ARGS_MAX];
  if (ac > 0) {
    if (!argv) {
      kfree(buf);
      return -EINVAL;
    }
    if (!access_ok((void *)argv, (size_t)ac * sizeof(uint64_t))) {
      kfree(buf);
      return -EFAULT;
    }
    uint64_t uargv[SPAWN_ARGS_MAX];
    if (copy_from_user(uargv, (void *)argv, (size_t)ac * sizeof(uint64_t)) <
        0) {
      kfree(buf);
      return -EFAULT;
    }
    for (int i = 0; i < ac; i++) {
      if (!uargv[i]) {
        kfree(buf);
        return -EINVAL;
      }
      long m = strncpy_from_user(&argv_storage[i * SPAWN_ARG_STR_MAX],
                                 (const char *)uargv[i], SPAWN_ARG_STR_MAX);
      if (m < 0) {
        kfree(buf);
        return -EFAULT;
      }
      kargv[i] = &argv_storage[i * SPAWN_ARG_STR_MAX];
    }
  }

  const char *kenvp[PROCESS_ENVP_MAX];
  int envc = 0;
  if (envp_ptr) {
    for (int i = 0; i < PROCESS_ENVP_MAX; i++) {
      uint64_t uptr;
      if (get_user_u64(&uptr, (const uint64_t *)(envp_ptr + (uint64_t)i * 8)) <
          0) {
        kfree(buf);
        return -EFAULT;
      }
      if (uptr == 0)
        break;
      long m = strncpy_from_user(&env_storage[i * PROCESS_ENV_STR_MAX],
                                 (const char *)uptr, PROCESS_ENV_STR_MAX);
      if (m < 0) {
        kfree(buf);
        return -EFAULT;
      }
      kenvp[i] = &env_storage[i * PROCESS_ENV_STR_MAX];
      envc++;
    }
  }

  process_t *child =
      process_spawn_child_args_env(proc, p, ac, kargv, envc, kenvp);
  kfree(buf);
  return child ? (int64_t)child->pid : -ENOENT;
}

static int64_t a_spawn_args_fds(uint64_t path, uint64_t argv, uint64_t argc,
                                uint64_t fds_ptr, uint64_t envp_ptr) {
  process_t *proc = process_current();
  if (!proc || !path)
    return -EFAULT;

  char p[VFS_PATH_MAX];
  int rc = resolve_user_path(proc, (const char *)path, p, sizeof(p));
  if (rc != 0)
    return rc;

  int ac = (int)argc;
  if (ac < 0 || ac > SPAWN_ARGS_MAX)
    return -EINVAL;

  spawn_fds_t kfds = {-1, -1, -1};
  if (fds_ptr) {
    if (!access_ok((void *)fds_ptr, sizeof(spawn_fds_t)))
      return -EFAULT;
    if (copy_from_user(&kfds, (void *)fds_ptr, sizeof(kfds)) < 0)
      return -EFAULT;
  }

  if (kfds.fd_in != -1 && (kfds.fd_in < 0 || kfds.fd_in >= MAX_PROCESS_FDS ||
                           !proc->fds[kfds.fd_in]))
    return -EBADF;
  if (kfds.fd_out != -1 && (kfds.fd_out < 0 || kfds.fd_out >= MAX_PROCESS_FDS ||
                            !proc->fds[kfds.fd_out]))
    return -EBADF;
  if (kfds.fd_err != -1 && (kfds.fd_err < 0 || kfds.fd_err >= MAX_PROCESS_FDS ||
                            !proc->fds[kfds.fd_err]))
    return -EBADF;

  enum {
    ARGV_BYTES = SPAWN_ARGS_MAX * SPAWN_ARG_STR_MAX,
    ENVP_BYTES = PROCESS_ENVP_MAX * PROCESS_ENV_STR_MAX
  };
  char *buf = (char *)kmalloc(ARGV_BYTES + ENVP_BYTES);
  if (!buf)
    return -ENOMEM;
  char *argv_storage = buf;
  char *env_storage = buf + ARGV_BYTES;

  const char *kargv[SPAWN_ARGS_MAX];
  if (ac > 0) {
    if (!argv) {
      kfree(buf);
      return -EINVAL;
    }
    if (!access_ok((void *)argv, (size_t)ac * sizeof(uint64_t))) {
      kfree(buf);
      return -EFAULT;
    }
    uint64_t uargv[SPAWN_ARGS_MAX];
    if (copy_from_user(uargv, (void *)argv, (size_t)ac * sizeof(uint64_t)) <
        0) {
      kfree(buf);
      return -EFAULT;
    }
    for (int i = 0; i < ac; i++) {
      if (!uargv[i]) {
        kfree(buf);
        return -EINVAL;
      }
      long m = strncpy_from_user(&argv_storage[i * SPAWN_ARG_STR_MAX],
                                 (const char *)uargv[i], SPAWN_ARG_STR_MAX);
      if (m < 0) {
        kfree(buf);
        return -EFAULT;
      }
      kargv[i] = &argv_storage[i * SPAWN_ARG_STR_MAX];
    }
  }

  const char *kenvp[PROCESS_ENVP_MAX];
  int envc = 0;
  if (envp_ptr) {
    for (int i = 0; i < PROCESS_ENVP_MAX; i++) {
      uint64_t uptr;
      if (get_user_u64(&uptr, (const uint64_t *)(envp_ptr + (uint64_t)i * 8)) <
          0) {
        kfree(buf);
        return -EFAULT;
      }
      if (uptr == 0)
        break;
      long m = strncpy_from_user(&env_storage[i * PROCESS_ENV_STR_MAX],
                                 (const char *)uptr, PROCESS_ENV_STR_MAX);
      if (m < 0) {
        kfree(buf);
        return -EFAULT;
      }
      kenvp[i] = &env_storage[i * PROCESS_ENV_STR_MAX];
      envc++;
    }
  }

  process_t *child =
      process_spawn_child_args_fds_env(proc, p, ac, kargv, envc, kenvp, &kfds);
  kfree(buf);
  return child ? (int64_t)child->pid : -ENOENT;
}

static int64_t a_waitpid(uint64_t pid, uint64_t status, uint64_t options,
                         uint64_t a4, uint64_t a5) {
  (void)a4;
  (void)a5;
  process_t *proc = process_current();
  if (!proc)
    return -EFAULT;
  int32_t kstatus = 0;
  int res = process_waitpid(proc, (int32_t)pid, &kstatus, NULL, (int)options);
  if (res < 0)
    return res;
  if (status != 0) {
    if (!access_ok((void *)status, sizeof(int32_t)))
      return -EFAULT;
    if (put_user_u32((uint32_t *)status, (uint32_t)kstatus) < 0)
      return -EFAULT;
  }
  return res;
}

// [LEGACY] readdir con API Aurora. Se mantiene para no reescribir todo
// el userland. Migrará a getdents64 cuando se porte musl.
static int64_t a_readdir_legacy(uint64_t path, uint64_t index, uint64_t out,
                                uint64_t a4, uint64_t a5) {
  (void)a4;
  (void)a5;
  if (!path || !out)
    return -EINVAL;
  process_t *proc = process_current();
  if (!proc)
    return -EFAULT;
  char p[VFS_PATH_MAX];
  int prc = resolve_user_path(proc, (const char *)path, p, sizeof(p));
  if (prc != 0)
    return prc;
  if (!access_ok((void *)out, sizeof(user_dirent_t)))
    return -EFAULT;

  vfs_dirent_t kout;
  int rc = vfs_readdir(p, index, &kout);
  if (rc != 0)
    return rc;

  user_dirent_t uout;
  size_t l = strlen(kout.name);
  if (l >= sizeof(uout.name))
    l = sizeof(uout.name) - 1;
  for (size_t i = 0; i < l; i++)
    uout.name[i] = kout.name[i];
  uout.name[l] = '\0';
  uout.type = kout.type;
  uout._pad = 0;
  uout.size = kout.size;
  if (copy_to_user((void *)out, &uout, sizeof(uout)) < 0)
    return -EFAULT;
  return kout.name[0] ? 1 : 0;
}

// ---------- winsrv ----------
static int64_t a_win_create(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                            uint64_t a5) {
  process_t *proc = process_current();
  if (!proc || !proc->task)
    return -EFAULT;
  return winsrv_create_window(proc->task, (int)a1, (int)a2, (int)a3, (int)a4,
                              (const char *)a5);
}
static int64_t a_win_destroy(uint64_t id, uint64_t a2, uint64_t a3, uint64_t a4,
                             uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  process_t *proc = process_current();
  if (!proc || !proc->task)
    return -EFAULT;
  return winsrv_destroy_window(proc->task, (int)id);
}
static int64_t a_win_blit(uint64_t args_ptr, uint64_t a2, uint64_t a3,
                          uint64_t a4, uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  process_t *proc = process_current();
  if (!proc || !proc->task)
    return -EFAULT;
  if (!access_ok((const void *)args_ptr, sizeof(winsrv_blit_args_t)))
    return -EFAULT;
  winsrv_blit_args_t args;
  if (copy_from_user(&args, (const void *)args_ptr, sizeof(args)) < 0)
    return -EFAULT;
  if (args.w <= 0 || args.h <= 0 || args.src_stride <= 0)
    return -EINVAL;
  if (args.src_x < 0 || args.src_y < 0)
    return -EINVAL;
  if (args.src_x + args.w > args.src_stride)
    return -EINVAL;

  uint64_t rows = (uint64_t)args.src_y + (uint64_t)args.h;
  if (rows == 0)
    return -EINVAL;
  uint64_t last_row_start;
  if (__builtin_mul_overflow(rows - 1, (uint64_t)args.src_stride,
                             &last_row_start))
    return -EINVAL;
  uint64_t last_pixel_offset;
  if (__builtin_add_overflow(last_row_start, (uint64_t)args.src_x,
                             &last_pixel_offset))
    return -EINVAL;
  if (__builtin_add_overflow(last_pixel_offset, (uint64_t)args.w,
                             &last_pixel_offset))
    return -EINVAL;
  uint64_t size_bytes;
  if (__builtin_mul_overflow(last_pixel_offset, sizeof(uint32_t), &size_bytes))
    return -EINVAL;
  if (!access_ok(args.pixels, (size_t)size_bytes))
    return -EFAULT;

  int rc = winsrv_blit(proc->task, args.win_id, args.x, args.y, args.w, args.h,
                       args.src_x, args.src_y, args.src_stride, args.pixels);
  return (uint64_t)rc;
}
static int64_t a_win_poll_event(uint64_t win_id, uint64_t ev_ptr,
                                uint64_t blocking, uint64_t a4, uint64_t a5) {
  (void)a4;
  (void)a5;
  process_t *proc = process_current();
  if (!proc || !proc->task)
    return -EFAULT;
  if (!access_ok((void *)ev_ptr, sizeof(winsrv_event_t)))
    return -EFAULT;
  winsrv_event_t ev;
  int rc = winsrv_poll_event(proc->task, (int)win_id, &ev, (int)blocking);
  if (rc <= 0)
    return rc;
  if (copy_to_user((void *)ev_ptr, &ev, sizeof(ev)) < 0)
    return -EFAULT;
  return 1;
}
static int64_t a_win_register_console(uint64_t win_id, uint64_t a2, uint64_t a3,
                                      uint64_t a4, uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  process_t *proc = process_current();
  if (!proc || !proc->task)
    return -EFAULT;
  return winsrv_register_console(proc->task, (int)win_id);
}
static int64_t a_win_set_icon(uint64_t win_id, uint64_t path, uint64_t a3,
                              uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;
  process_t *proc = process_current();
  if (!proc || !proc->task)
    return -EFAULT;
  if (path == 0)
    return -EINVAL;
  char p[VFS_PATH_MAX];
  int rc = resolve_user_path(proc, (const char *)path, p, sizeof(p));
  if (rc != 0)
    return rc;
  return winsrv_set_icon(proc->task, (int)win_id, p);
}

// ---------- IPC ----------
static int64_t a_ipc_send(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                          uint64_t a5) {
  (void)a5;
  uint8_t kbuf[IPC_MAX_PAYLOAD];
  size_t size = (size_t)a4;
  size_t copy_len = size > IPC_MAX_PAYLOAD ? IPC_MAX_PAYLOAD : size;
  if (a3 != 0 && copy_len > 0) {
    if (copy_from_user(kbuf, (const void *)a3, copy_len) < 0)
      return -EFAULT;
  } else {
    copy_len = 0;
  }
  return ipc_send((uint32_t)a1, (uint32_t)a2, kbuf, copy_len);
}
static int64_t a_ipc_recv(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                          uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;
  if (!access_ok((void *)a1, sizeof(ipc_msg_t)))
    return -EFAULT;
  ipc_msg_t kmsg;
  int rc = ipc_recv(&kmsg, (int)a2);
  if (rc != 0)
    return rc;
  if (copy_to_user((void *)a1, &kmsg, sizeof(kmsg)) < 0)
    return -EFAULT;
  return 0;
}
static int64_t a_get_service_id(uint64_t name, uint64_t a2, uint64_t a3,
                                uint64_t a4, uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  if (name == 0)
    return -EINVAL;
  char n[32];
  long m = strncpy_from_user(n, (const char *)name, sizeof(n));
  if (m < 0)
    return -EFAULT;
  if (m == 0)
    return -EINVAL;
  uint32_t id = syscall_lookup_service(n);
  return id ? (int64_t)id : -ENOENT;
}

// ---------- debug ----------
static int64_t a_vm_debug_info(uint64_t virt, uint64_t info_ptr, uint64_t a3,
                               uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;
  process_t *proc = process_current();
  if (!proc || proc->pml4_phys == 0)
    return -EFAULT;
  if (virt >= USER_LIMIT ||
      !access_ok((const void *)info_ptr, sizeof(vm_debug_info_t)))
    return -EFAULT;

  uint64_t phys =
      paging_get_phys_in((uint64_t *)phys_to_virt(proc->pml4_phys), virt);
  if (phys == 0)
    return -EFAULT;

  vm_debug_info_t info = {
      .cr3_phys = proc->pml4_phys,
      .virt = virt,
      .phys = phys,
  };
  if (copy_to_user((void *)info_ptr, &info, sizeof(info)) < 0)
    return -EFAULT;
  return 0;
}

// ===========================================================================
// Tablas y dispatcher
// ===========================================================================
static const syscall_entry_t linux_table[] = {
    [SYS_READ] = {k_read, "read"},
    [SYS_WRITE] = {k_write, "write"},
    [SYS_OPEN] = {k_open, "open"},
    [SYS_CLOSE] = {k_close, "close"},
    [SYS_STAT] = {k_stat, "stat"},
    [SYS_FSTAT] = {k_fstat, "fstat"},
    [SYS_LSTAT] = {k_lstat, "lstat"},
    [SYS_LSEEK] = {k_lseek, "lseek"},
    [SYS_MMAP] = {k_mmap, "mmap"},
    [SYS_MPROTECT] = {k_mprotect, "mprotect"},
    [SYS_MUNMAP] = {k_munmap, "munmap"},
    [SYS_BRK] = {k_brk, "brk"},
    [SYS_RT_SIGACTION] = {k_rt_sigaction, "rt_sigaction"},
    [SYS_RT_SIGRETURN] = {k_rt_sigreturn, "rt_sigreturn"},
    [SYS_RT_SIGPROCMASK] = {k_rt_sigprocmask, "rt_sigprocmask"},
    [SYS_IOCTL] = {k_ioctl, "ioctl"},
    [SYS_POLL] = {k_poll, "poll"},
    [SYS_SELECT] = {k_select, "select"},
    [SYS_NANOSLEEP] = {k_nanosleep, "nanosleep"},
    [SYS_PSELECT6] = {k_pselect6, "pselect6"},
    [SYS_PPOLL] = {k_ppoll, "ppoll"},
    [SYS_FCNTL] = {k_fcntl, "fcntl"},
    [SYS_RENAME] = {k_rename, "rename"},
    [SYS_MKDIR] = {k_mkdir, "mkdir"},
    [SYS_UNLINK] = {k_unlink, "unlink"},
    [SYS_READV] = {k_readv, "readv"},
    [SYS_WRITEV] = {k_writev, "writev"},
    [SYS_ACCESS] = {k_access, "access"},
    [SYS_PIPE] = {k_pipe, "pipe"},
    [SYS_SCHED_YIELD] = {k_sched_yield, "sched_yield"},
    [SYS_DUP2] = {k_dup2, "dup2"},
    [SYS_GETPID] = {k_getpid, "getpid"},
    [SYS_SENDFILE] = {k_sendfile, "sendfile"},
    [SYS_GETUID] = {k_getuid, "getuid"},
    [SYS_GETGID] = {k_getgid, "getgid"},
    [SYS_GETEUID] = {k_geteuid, "geteuid"},
    [SYS_GETEGID] = {k_getegid, "getegid"},
    [SYS_GETPPID] = {k_getppid, "getppid"},
    [SYS_SETPGID] = {k_setpgid, "setpgid"},
    [SYS_GETPGID] = {k_getpgid, "getpgid"},
    [SYS_GETSID] = {k_getsid, "getsid"},
    [SYS_SETSID] = {k_setsid, "setsid"},
    [SYS_GETGROUPS] = {k_getgroups, "getgroups"},
    [SYS_PRCTL] = {k_prctl, "prctl"},
    [SYS_KILL] = {k_kill, "kill"},
    [SYS_UNAME] = {k_uname, "uname"},
    [SYS_GETCWD] = {k_getcwd, "getcwd"},
    [SYS_CHDIR] = {k_chdir, "chdir"},
    [SYS_READLINK] = {k_readlink, "readlink"},
    [SYS_ARCH_PRCTL] = {k_arch_prctl, "arch_prctl"},
    [SYS_GETDENTS64] = {k_getdents64, "getdents64"},
    [SYS_SET_TID_ADDRESS] = {k_set_tid_address, "set_tid_address"},
    [SYS_CLOCK_GETTIME] = {k_clock_gettime, "clock_gettime"},
    [SYS_EXIT_GROUP] = {k_exit_group, "exit_group"},
    [SYS_OPENAT] = {k_openat, "openat"},
    [SYS_MKDIRAT] = {k_mkdirat, "mkdirat"},
    [SYS_FSTATAT] = {k_fstatat, "fstatat"},
    [SYS_UNLINKAT] = {k_unlinkat, "unlinkat"},
    [SYS_RENAMEAT] = {k_renameat, "renameat"},
    [SYS_SET_ROBUST_LIST] = {k_set_robust_list, "set_robust_list"},
    [SYS_PRLIMIT64] = {k_prlimit64, "prlimit64"},
    [SYS_GETRANDOM] = {k_getrandom, "getrandom"},
    [SYS_RSEQ] = {k_rseq, "rseq"},
    [SYS_CLONE] = {k_clone, "clone"},
    [SYS_FORK] = {k_fork, "fork"},
    [SYS_VFORK] = {k_vfork, "vfork"},
    [SYS_PIVOT_ROOT] = {k_pivot_root, "pivot_root"},
    [SYS_EXECVE] = {k_execve, "execve"},
    [SYS_GETTID] = {k_gettid, "gettid"},
    [SYS_WAIT4] = {k_wait4, "wait4"},
};
#define LINUX_TABLE_N ((int)ARRAY_SIZE(linux_table))

static const syscall_entry_t aurora_table[] = {
    [ASYS_SPAWN - ASYS_BASE] = {a_spawn, "asys_spawn"},
    [ASYS_WAITPID - ASYS_BASE] = {a_waitpid, "asys_waitpid"},
    [ASYS_SPAWN_ARGS - ASYS_BASE] = {a_spawn_args, "asys_spawn_args"},
    [ASYS_SPAWN_ARGS_FDS -
        ASYS_BASE] = {a_spawn_args_fds, "asys_spawn_args_fds"},
    [ASYS_GET_TASK_ID - ASYS_BASE] = {a_get_task_id, "asys_get_task_id"},
    [ASYS_READDIR_LEGACY - ASYS_BASE] = {a_readdir_legacy, "asys_readdir"},

    [ASYS_WIN_CREATE - ASYS_BASE] = {a_win_create, "asys_win_create"},
    [ASYS_WIN_DESTROY - ASYS_BASE] = {a_win_destroy, "asys_win_destroy"},
    [ASYS_WIN_BLIT - ASYS_BASE] = {a_win_blit, "asys_win_blit"},
    [ASYS_WIN_POLL_EVENT -
        ASYS_BASE] = {a_win_poll_event, "asys_win_poll_event"},
    [ASYS_WIN_REGISTER_CONSOLE -
        ASYS_BASE] = {a_win_register_console, "asys_win_register_console"},
    [ASYS_WIN_SET_ICON - ASYS_BASE] = {a_win_set_icon, "asys_win_set_icon"},

    [ASYS_IPC_SEND - ASYS_BASE] = {a_ipc_send, "asys_ipc_send"},
    [ASYS_IPC_RECV - ASYS_BASE] = {a_ipc_recv, "asys_ipc_recv"},
    [ASYS_GET_SERVICE_ID -
        ASYS_BASE] = {a_get_service_id, "asys_get_service_id"},

    [ASYS_VM_DEBUG_INFO - ASYS_BASE] = {a_vm_debug_info, "asys_vm_debug_info"},

    [ASYS_PRINT - ASYS_BASE] = {a_print, "asys_print"},
};
#define AURORA_TABLE_N ((int)ARRAY_SIZE(aurora_table))

uint64_t syscall_handler_c(registers_t *regs) {
  uint64_t num = regs->rax;
  uint64_t arg1 = regs->rdi;
  uint64_t arg2 = regs->rsi;
  uint64_t arg3 = regs->rdx;
  uint64_t arg4 = regs->r10;
  uint64_t arg5 = regs->r8;

  syscall_fn_t fn = NULL;
  if (num < (uint64_t)LINUX_TABLE_N) {
    fn = linux_table[num].fn;
  } else if (num >= ASYS_BASE && num < ASYS_MAX) {
    int idx = (int)(num - ASYS_BASE);
    if (idx < AURORA_TABLE_N)
      fn = aurora_table[idx].fn;
  }

  if (!fn) {
    LOG_WARN("[SYSCALL] num desconocido: %lu", (unsigned long)num);
    return err(ENOSYS);
  }

  task_t *cur = sched_current();
  registers_t *saved = cur ? cur->syscall_regs : NULL;
  if (cur)
    cur->syscall_regs = regs;

  uint64_t ret = (uint64_t)fn(arg1, arg2, arg3, arg4, arg5);

  // Publicar el valor de retorno en el trap frame ANTES de la entrega
  // de señales, para que el ucontext que construyamos capture el rax
  // correcto (valor de retorno de la syscall).
  regs->rax = ret;

  signal_check_pending();

  if (cur)
    cur->syscall_regs = saved;

  // Devolvemos regs->rax (que pudo cambiar si k_rt_sigreturn restauró
  // un rax distinto). El asm lo escribirá en [frame+0x70].
  return regs->rax;
}