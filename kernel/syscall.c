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
  (void)flags;
  (void)a5;
  process_t *proc = process_current();
  if (!proc)
    return -EFAULT;

  // prot es Linux (PROT_READ=1, PROT_WRITE=2, PROT_EXEC=4).
  uint64_t pte_flags = 0;
  if (prot & 0x2) // PROT_WRITE → página escribible
    pte_flags |= PTE_WRITABLE;
  if (!(prot & 0x4)) // !PROT_EXEC → NX
    pte_flags |= PTE_NX;

  LOG_DEBUG("[MMAP] addr=%p len=%p prot=%lx flags=%lx pte=%lx", (void *)addr,
            (void *)length, prot, flags, pte_flags);

  int64_t r = sys_mmap(proc, addr, length, pte_flags, 0, -1, 0);
  return r;
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

// ---------- kill ----------
static int64_t k_kill(uint64_t pid, uint64_t sig, uint64_t a3, uint64_t a4,
                      uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;
  int32_t kpid = (int32_t)pid;
  int ksig = (int)sig;
  if (ksig < 1 || ksig >= SIG_MAX)
    return -EINVAL;

  task_t *target_task = NULL;
  if (kpid > 0) {
    target_task = process_signal_pid((uint32_t)kpid, 1ULL << ksig);
    if (!target_task)
      return -ENOENT;
  } else if (kpid == 0) {
    process_t *target = process_current();
    if (!target || !target->task)
      return -ENOENT;
    target_task = target->task;
    task_get(target_task);
    __atomic_fetch_or(&target->pending_signals, 1ULL << ksig, __ATOMIC_RELEASE);
  } else {
    return -EINVAL;
  }

  wait_queue_interrupt_task(target_task);
  task_put(target_task);
  return 0;
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
  (void)fd;
  (void)req;
  (void)arg;
  (void)a4;
  (void)a5;
  return -ENOTTY;
}

// ---------- rt_sigaction / rt_sigprocmask (stubs) ----------
static int64_t k_rt_sigaction(uint64_t a1, uint64_t a2, uint64_t a3,
                              uint64_t a4, uint64_t a5) {
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  return 0;
}
static int64_t k_rt_sigprocmask(uint64_t a1, uint64_t a2, uint64_t a3,
                                uint64_t a4, uint64_t a5) {
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  return 0;
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
                            uint64_t a4, uint64_t a5) {
  (void)a4;
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

  char storage[SPAWN_ARGS_MAX][SPAWN_ARG_STR_MAX];
  const char *kargv[SPAWN_ARGS_MAX];

  if (ac > 0) {
    if (!argv)
      return -EINVAL;
    if (!access_ok((void *)argv, (size_t)ac * sizeof(uint64_t)))
      return -EFAULT;
    uint64_t uargv[SPAWN_ARGS_MAX];
    if (copy_from_user(uargv, (void *)argv, (size_t)ac * sizeof(uint64_t)) < 0)
      return -EFAULT;
    for (int i = 0; i < ac; i++) {
      if (!uargv[i])
        return -EINVAL;
      long m = strncpy_from_user(storage[i], (const char *)uargv[i],
                                 SPAWN_ARG_STR_MAX);
      if (m < 0)
        return -EFAULT;
      kargv[i] = storage[i];
    }
  }

  process_t *child = process_spawn_child_args(proc, p, ac, kargv);
  return child ? (int64_t)child->pid : -ENOENT;
}

static int64_t a_spawn_args_fds(uint64_t path, uint64_t argv, uint64_t argc,
                                uint64_t fds_ptr, uint64_t a5) {
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

  char storage[SPAWN_ARGS_MAX][SPAWN_ARG_STR_MAX];
  const char *kargv[SPAWN_ARGS_MAX];

  if (ac > 0) {
    if (!argv)
      return -EINVAL;
    if (!access_ok((void *)argv, (size_t)ac * sizeof(uint64_t)))
      return -EFAULT;
    uint64_t uargv[SPAWN_ARGS_MAX];
    if (copy_from_user(uargv, (void *)argv, (size_t)ac * sizeof(uint64_t)) < 0)
      return -EFAULT;
    for (int i = 0; i < ac; i++) {
      if (!uargv[i])
        return -EINVAL;
      long m = strncpy_from_user(storage[i], (const char *)uargv[i],
                                 SPAWN_ARG_STR_MAX);
      if (m < 0)
        return -EFAULT;
      kargv[i] = storage[i];
    }
  }

  process_t *child = process_spawn_child_args_fds(proc, p, ac, kargv, &kfds);
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
  int res = process_waitpid(proc, (int32_t)pid, &kstatus, (int)options);
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
    [SYS_RT_SIGPROCMASK] = {k_rt_sigprocmask, "rt_sigprocmask"},
    [SYS_IOCTL] = {k_ioctl, "ioctl"},
    [SYS_FCNTL] = {k_fcntl, "fcntl"},
    [SYS_READV] = {k_readv, "readv"},
    [SYS_WRITEV] = {k_writev, "writev"},
    [SYS_ACCESS] = {k_access, "access"},
    [SYS_PIPE] = {k_pipe, "pipe"},
    [SYS_SCHED_YIELD] = {k_sched_yield, "sched_yield"},
    [SYS_DUP2] = {k_dup2, "dup2"},
    [SYS_GETPID] = {k_getpid, "getpid"},
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

  LOG_TRACE("[SYSCALL] cpu=%u num=%lu arg1=%p arg2=%p",
            (unsigned)smp_processor_id(), (unsigned long)num, (void *)arg1,
            (void *)arg2);

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

  signal_check_pending();

  uint64_t ret = (uint64_t)fn(arg1, arg2, arg3, arg4, arg5);

  signal_check_pending();

  return ret;
}