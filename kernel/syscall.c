// kernel/syscall.c
//
// ABI doble: rango 0..500 (Linux) y rango 0x1000+ (Aurora-only).
//
// Todos los handlers tienen prefijo k_ (Linux) o a_ (Aurora) para
// distinguir de un vistazo a qué ABI pertenecen.

#include "syscall.h"
#include "cpu.h"
#include "fat32.h"
#include "futex.h"
#include "gdt.h"
#include "gfx/winsrv.h"
#include "heap.h"
#include "io.h"
#include "ipc.h"
#include "klog.h"
#include "paging.h"
#include "pf.h"
#include "pmm.h"
#include "process.h"
#include "rtc.h"
#include "sched.h"
#include "serial.h"
#include "signal.h"
#include "spinlock.h"
#include "string.h"
#include "swap.h"
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
// [setdomainname] Dominio del sistema. Cosmético: NIS lo usa, pero
// nada en Aurora depende de él. Lo devuelve k_uname().
static char g_domainname[64] = "(none)";

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

// ---------------------------------------------------------------------------
// [df] Device id estable por filesystem montado.
//
// df, findmnt y mount deduplican y emparejan mounts por el número de
// device (st_dev en stat, columna maj:min en /proc/self/mountinfo). Si
// todos los FS comparten el mismo id, df colapsa /, /data, /proc, /sys
// y /dev a uno solo, y luego filtra los que reportan 0 bloques → "no
// file systems processed".
//
// Asignamos un id único por puntero a vfs_fs_ops. Todos los nodos de un
// mismo FS comparten el mismo ops, así que todos comparten el device.
// El primer slot es 32 (antes de 32 reservamos para el FS "desconocido").
// ---------------------------------------------------------------------------
#define FS_DEV_MAX 16
static const void *g_fs_dev_tab[FS_DEV_MAX];

uint64_t fs_dev_id(const void *fs) {
  if (!fs)
    return 31;
  for (int i = 0; i < FS_DEV_MAX; i++) {
    const void *cur = __atomic_load_n(&g_fs_dev_tab[i], __ATOMIC_ACQUIRE);
    if (cur == fs)
      return 32 + i;
    if (cur == NULL) {
      const void *expected = NULL;
      if (__atomic_compare_exchange_n(&g_fs_dev_tab[i], &expected, fs, 0,
                                      __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE) ||
          expected == fs)
        return 32 + i;
    }
  }
  return 32 + FS_DEV_MAX;
}

// Traduce un vfs_stat_t de Aurora al struct stat de Linux.
//
// `dev` debe ser el device id del FS (fs_dev_id(node->fs)), no un valor
// fijo, o df confunde todos los mounts con el mismo FS.
static void vfs_to_linux_stat(const vfs_stat_t *vs, uint64_t size, uint64_t dev,
                              linux_stat_t *out) {
  memset(out, 0, sizeof(*out));
  uint32_t mode = vs->mode;
  if (mode == 0) {
    if (vs->flags & VFS_DIRECTORY)
      mode = S_IFDIR | 0755;
    else if (vs->flags & VFS_CHARDEVICE)
      mode = S_IFCHR | 0666;
    else
      mode = S_IFREG | 0644;
  }
  out->st_dev = (int64_t)dev;
  out->st_ino = vs->inode;
  out->st_nlink = (vs->flags & VFS_DIRECTORY) ? 2 : 1;
  out->st_mode = mode;
  out->st_uid = vs->uid;
  out->st_gid = vs->gid;
  out->st_rdev = (int64_t)vs->rdev;
  out->st_size = (int64_t)size;
  out->st_blksize = 512;
  out->st_blocks = (int64_t)((size + 511) / 512);
  out->st_mtime_sec = vs->mtime_sec;
  out->st_mtime_nsec = 0;
  out->st_atime_sec = vs->mtime_sec;
  out->st_atime_nsec = 0;
  out->st_ctime_sec = vs->mtime_sec;
  out->st_ctime_nsec = 0;
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

// ---------- pread64 / pwrite64 ----------
//
// [glibc] pread/pwrite = read/write con offset explícito, sin mover
// f->offset. glibc los usa para leer la cabecera ELF de un .so antes
// de decidir cómo mmap'earlo.
static int64_t k_pread64(uint64_t fd, uint64_t buf, uint64_t count,
                         uint64_t offset, uint64_t a5) {
  (void)a5;
  process_t *proc = process_current();
  if (!proc)
    return -EFAULT;
  if (!access_ok((void *)buf, (size_t)count))
    return -EFAULT;
  return vfs_pread_for_proc(proc, (int)fd, (void *)buf, (size_t)count,
                            (uint64_t)offset);
}

static int64_t k_pwrite64(uint64_t fd, uint64_t buf, uint64_t count,
                          uint64_t offset, uint64_t a5) {
  (void)a5;
  process_t *proc = process_current();
  if (!proc)
    return -EFAULT;
  if (!access_ok((void *)buf, (size_t)count))
    return -EFAULT;
  return vfs_pwrite_for_proc(proc, (int)fd, (const void *)buf, (size_t)count,
                             (uint64_t)offset);
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
// [glibc] Núcleo reutilizable: toma un path ABSOLUTO ya resuelto.
static int64_t do_open_resolved(process_t *proc, const char *kpath,
                                int linux_flags) {
  // [fix] O_TMPFILE = __O_TMPFILE (0x400000) | O_DIRECTORY (0x10000).
  // Hay que comprobar AMBOS bits: O_DIRECTORY solo (0x10000) es un
  // flag válido que `ls .` y `ls /` pasan en cada invocación, y un
  // check con disyunción (`flags & 0x410000`) los rechazaría por error.
  //
  // Devolvemos -EISDIR (NO -EOPNOTSUPP). glibc, en sysdeps/posix/
  // tempname.c (el que usan mkstemp/tmpfile), solo cae a su fallback
  // (open con nombre random + O_CREAT|O_EXCL) si el errno es EISDIR o
  // ENOENT. Con EOPNOTSUPP aborta y no prueba el fallback, así que
  // ed/nano/less morían con "?" o no guardaban.
  //
  // El path que llega aquí es un directorio (el usuario pidió "un
  // fichero anónimo dentro de este directorio"), así que devolver
  // EISDIR es además semánticamente correcto: coincide con lo que
  // Linux devuelve cuando el FS no implementa O_TMPFILE.
  if ((linux_flags & 0x410000) == 0x410000) {
    // Devolvemos -ENOENT (NO -EISDIR ni -EOPNOTSUPP).
    //
    // glibc, en sysdeps/posix/tempname.c, acepta tanto EISDIR como
    // ENOENT como señal de "O_TMPFILE no soportado por este FS, cae
    // al fallback de generar nombre random + open(O_CREAT|O_EXCL)".
    // ENOENT es la constante universal — la aceptan todas las
    // versiones de glibc desde hace 20 años. EISDIR depende de la
    // versión y algunos builds la rechazan.
    //
    // Devolver ENOENT para un path que existe es raro, pero glibc no
    // valida la semántica: solo lo usa como señal interna para
    // decidir el fallback. Es un uso de ENOENT como "feature not
    // available", no como "file not found".
    LOG_TRACE("[OPEN-TMPFILE] path='%s' flags=0x%x -> ENOENT", kpath,
              linux_flags);
    return -ENOENT;
  }

  // [fix] O_DIRECTORY (0x10000): falla con ENOTDIR si el path no es
  // directorio. O_NOFOLLOW (0x20000): falla con ELOOP si el path final
  // es un symlink. Ambos se comprueban con un lookup previo.
  int want_dir = (linux_flags & 0x10000) != 0;
  int no_follow = (linux_flags & 0x20000) != 0;
  if (want_dir || no_follow) {
    vfs_node_t *probe =
        no_follow ? vfs_lookup_nofollow(kpath) : vfs_lookup(kpath);
    if (!probe)
      return -ENOENT;
    int is_dir = (probe->flags & VFS_DIRECTORY) != 0;
    int is_sym = probe->is_symlink;
    vfs_node_free(probe);

    if (no_follow && is_sym)
      return -ELOOP;
    if (want_dir && !is_dir)
      return -ENOTDIR;
  }

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
    vfs_node_t *probe = vfs_lookup(kpath);
    if (probe) {
      vfs_node_free(probe);
      if (linux_flags & LINUX_O_EXCL)
        return -EEXIST;
    } else {
      uint32_t create_mode = 0666 & ~proc->umask;
      int crc = vfs_create(kpath, create_mode);
      if (crc == 0)
        created_here = 1;
      else if (crc != -EEXIST)
        return crc;
    }
  }

  int fd = vfs_open_for_proc(proc, kpath, aflags);
  if (fd < 0) {
    LOG_INFO("[OPEN-FAIL] path='%s' flags=0x%x rc=%d", kpath, linux_flags, fd);
    return fd;
  }

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

// Ahora do_open_common es solo "resolver user-ptr → path absoluto → open".
static int64_t do_open_common(process_t *proc, const char *raw_path,
                              int linux_flags) {
  char path[VFS_PATH_MAX];
  int rc = resolve_user_path(proc, raw_path, path, sizeof(path));
  if (rc != 0)
    return rc;
  return do_open_resolved(proc, path, linux_flags);
}

static int64_t k_openat(uint64_t dfd, uint64_t path, uint64_t flags,
                        uint64_t mode, uint64_t a5) {
  (void)mode;
  (void)a5;
  process_t *proc = process_current();
  if (!proc)
    return -EFAULT;

  // [glibc] El cast debe ser a (int), NO a (int64_t). glibc pasa
  // AT_FDCWD cero-extendido (0x00000000FFFFFF9C), y el cast a int64_t
  // daría 4294967196 en lugar de -100, entrando al branch por error.
  if ((int)dfd != AT_FDCWD) {
    int kfd = (int)dfd;
    if (kfd < 0 || kfd >= MAX_PROCESS_FDS || !proc->fds[kfd])
      return -EBADF;
    vfs_node_t *dir_node = proc->fds[kfd]->node;
    if (!dir_node || !(dir_node->flags & VFS_DIRECTORY))
      return -ENOTDIR;

    char rel[VFS_PATH_MAX];
    long n = strncpy_from_user(rel, (const char *)path, sizeof(rel));
    if (n < 0)
      return -EFAULT;
    if (n == 0)
      return -EINVAL;

    char full[VFS_PATH_MAX];
    if (rel[0] == '/') {
      size_t rl = strlen(rel);
      if (rl >= sizeof(full))
        return -ENAMETOOLONG;
      memcpy(full, rel, rl + 1);
    } else {
      const char *base = dir_node->name;
      size_t bl = strlen(base);
      size_t rl = strlen(rel);
      int base_is_root = (bl == 1 && base[0] == '/');
      size_t needed = base_is_root ? (1 + rl + 1) : (bl + 1 + rl + 1);
      if (needed > sizeof(full))
        return -ENAMETOOLONG;
      if (base_is_root) {
        full[0] = '/';
        memcpy(full + 1, rel, rl + 1);
      } else {
        memcpy(full, base, bl);
        full[bl] = '/';
        memcpy(full + bl + 1, rel, rl + 1);
      }
    }

    char norm[VFS_PATH_MAX];
    int rc = vfs_resolve_path("/", full, norm, sizeof(norm));
    if (rc != 0)
      return rc;
    return do_open_resolved(proc, norm, (int)flags);
  }

  return do_open_common(proc, (const char *)path, (int)flags);
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

  // [df] Device del FS subyacente, no un 1 fijo.
  uint64_t dev = 31;
  if ((int)fd >= 0 && (int)fd < MAX_PROCESS_FDS && proc->fds[fd] &&
      proc->fds[fd]->node)
    dev = fs_dev_id(proc->fds[fd]->node->fs);

  linux_stat_t ls;
  vfs_to_linux_stat(&vs, vs.size, dev, &ls);
  if (copy_to_user((void *)statbuf, &ls, sizeof(ls)) < 0)
    return -EFAULT;
  return 0;
}

static int64_t do_stat_path(process_t *proc, const char *upath,
                            uint64_t statbuf, uint64_t flags) {
  if (!access_ok((void *)statbuf, sizeof(linux_stat_t)))
    return -EFAULT;

  char path[VFS_PATH_MAX];
  int rc = resolve_user_path(proc, upath, path, sizeof(path));
  if (rc != 0)
    return rc;

  int no_follow = (flags & AT_SYMLINK_NOFOLLOW) ? 1 : 0;
  vfs_node_t *node = no_follow ? vfs_lookup_nofollow(path) : vfs_lookup(path);
  if (!node)
    return -ENOENT;

  vfs_stat_t vs = {
      .flags = node->flags,
      .size = node->size,
      .inode = node->inode,
      .mtime_sec = node->mtime_sec,
      .mode = node->mode,
      .uid = node->uid,
      .gid = node->gid,
      .rdev = node->rdev,
  };
  // [df] Capturar el device ANTES de liberar el nodo.
  uint64_t dev = fs_dev_id(node->fs);
  vfs_node_free(node);

  linux_stat_t ls;
  vfs_to_linux_stat(&vs, vs.size, dev, &ls);
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

  if ((int)dfd == AT_FDCWD)
    return do_stat_path(proc, (const char *)path, statbuf, flags);

  // [FIX] dirfd real: combinar node->name + rel, como en k_openat.
  int kfd = (int)dfd;
  if (kfd < 0 || kfd >= MAX_PROCESS_FDS || !proc->fds[kfd])
    return -EBADF;
  vfs_node_t *dir_node = proc->fds[kfd]->node;
  if (!dir_node || !(dir_node->flags & VFS_DIRECTORY))
    return -ENOTDIR;

  char rel[VFS_PATH_MAX];
  long n = strncpy_from_user(rel, (const char *)path, sizeof(rel));
  if (n < 0)
    return -EFAULT;
  if (n == 0)
    return -EINVAL;

  char full[VFS_PATH_MAX];
  if (rel[0] == '/') {
    size_t rl = strlen(rel);
    if (rl >= sizeof(full))
      return -ENAMETOOLONG;
    memcpy(full, rel, rl + 1);
  } else {
    const char *base = dir_node->name;
    size_t bl = strlen(base);
    size_t rl = strlen(rel);
    int base_is_root = (bl == 1 && base[0] == '/');
    size_t needed = base_is_root ? (1 + rl + 1) : (bl + 1 + rl + 1);
    if (needed > sizeof(full))
      return -ENAMETOOLONG;
    if (base_is_root) {
      full[0] = '/';
      memcpy(full + 1, rel, rl + 1);
    } else {
      memcpy(full, base, bl);
      full[bl] = '/';
      memcpy(full + bl + 1, rel, rl + 1);
    }
  }

  char norm[VFS_PATH_MAX];
  int rc = vfs_resolve_path("/", full, norm, sizeof(norm));
  if (rc != 0)
    return rc;

  if (!access_ok((void *)statbuf, sizeof(linux_stat_t)))
    return -EFAULT;

  int no_follow = (flags & AT_SYMLINK_NOFOLLOW) ? 1 : 0;
  vfs_node_t *node = no_follow ? vfs_lookup_nofollow(norm) : vfs_lookup(norm);
  if (!node)
    return -ENOENT;

  vfs_stat_t vs = {
      .flags = node->flags,
      .size = node->size,
      .inode = node->inode,
      .mtime_sec = node->mtime_sec,
      .mode = node->mode,
      .uid = node->uid,
      .gid = node->gid,
      .rdev = node->rdev,
  };
  // [df] Capturar el device ANTES de liberar el nodo.
  uint64_t dev = fs_dev_id(node->fs);
  vfs_node_free(node);

  linux_stat_t ls;
  vfs_to_linux_stat(&vs, vs.size, dev, &ls);
  if (copy_to_user((void *)statbuf, &ls, sizeof(ls)) < 0)
    return -EFAULT;
  return 0;
}

// ---------- access ----------
static int64_t k_access(uint64_t path, uint64_t mode, uint64_t a3, uint64_t a4,
                        uint64_t a5) {
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

  // [3.4.d] access(2) usa REAL uid/gid, no efectivos. Como hoy no hay
  // setuid activo, euid == uid siempre; cuando lo haya, habrá que
  // cambiar vfs_check_access para aceptar un flag.
  int acc = vfs_check_access(node, (int)mode);
  vfs_node_free(node);
  return acc;
}

// ---------- mkdir / unlink / rename ----------
static int64_t k_mkdir(uint64_t path, uint64_t mode, uint64_t a3, uint64_t a4,
                       uint64_t a5) {
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
  // [3.4.c] El mode de mkdir(2) ya viene filtrado por umask. Si el
  // caller pasa 0, usamos 0777 & ~umask (como hace glibc).
  uint32_t m = (uint32_t)mode;
  if (m == 0)
    m = 0777;
  m &= ~proc->umask;
  return vfs_mkdir(p, m);
}

static int64_t k_mkdirat(uint64_t dfd, uint64_t path, uint64_t mode,
                         uint64_t a4, uint64_t a5) {
  (void)mode;
  (void)a4;
  (void)a5;
  if ((int)dfd != AT_FDCWD)
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
  if ((int)dfd != AT_FDCWD)
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
  if ((int)odfd != AT_FDCWD || (int)ndfd != AT_FDCWD)
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

  // [3.4.d] Requerir X para entrar al directorio.
  int acc = vfs_check_access(node, VFS_X_OK);
  if (acc != 0) {
    vfs_node_free(node);
    return acc;
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

static int64_t k_fchdir(uint64_t fd, uint64_t a2, uint64_t a3, uint64_t a4,
                        uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  process_t *proc = process_current();
  if (!proc)
    return -EFAULT;
  int kfd = (int)fd;
  if (kfd < 0 || kfd >= MAX_PROCESS_FDS || !proc->fds[kfd])
    return -EBADF;
  vfs_node_t *node = proc->fds[kfd]->node;
  if (!node || !(node->flags & VFS_DIRECTORY))
    return -ENOTDIR;

  // Copiamos node->name (path completo del directorio) a proc->cwd.
  size_t len = strlen(node->name);
  if (len >= sizeof(proc->cwd))
    return -ENAMETOOLONG;
  memcpy(proc->cwd, node->name, len + 1);
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
//
// Linux x86_64: mmap(addr, length, prot, flags, fd, offset)
//   rdi=addr, rsi=length, rdx=prot, r10=flags, r8=fd, r9=offset
//
// El dispatcher pasa 5 args (rdi,rsi,rdx,r10,r8) → fd llega como a5.
// El offset (r9) hay que leerlo desde el trap frame; el dispatcher no
// lo propaga.
static int64_t k_mmap(uint64_t addr, uint64_t length, uint64_t prot,
                      uint64_t flags, uint64_t fd) {
  process_t *proc = process_current();
  if (!proc)
    return -EFAULT;

  registers_t *regs = syscall_current_regs();
  uint64_t offset = regs ? regs->r9 : 0;

  return sys_mmap(proc, addr, length, prot, flags, (int)fd, offset);
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

static int64_t k_mremap(uint64_t old_addr, uint64_t old_size, uint64_t new_size,
                        uint64_t flags, uint64_t new_addr) {
  process_t *proc = process_current();
  if (!proc)
    return -EFAULT;
  return sys_mremap(proc, old_addr, old_size, new_size, flags, new_addr);
}

// ---------- madvise ----------
//
// madvise(addr, len, advice). Linux lo usa para hints de VM. Ninguno
// de los advices cambia el comportamiento visible para Aurora hoy:
//   MADV_NORMAL/RANDOM/SEQUENTIAL/WILLNEED: hints, no-op.
//   MADV_DONTNEED/MADV_FREE: liberar páginas. Implementarlo bien
//   requiere partir VMAs a nivel de página. De momento aceptamos y no
//   hacemos nada (el contenido sigue accesible — degradación benigna).
static int64_t k_madvise(uint64_t addr, uint64_t len, uint64_t advice,
                         uint64_t a4, uint64_t a5) {
  (void)addr;
  (void)len;
  (void)advice;
  (void)a4;
  (void)a5;
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

static int64_t k_set_tid_address(uint64_t tidptr, uint64_t a2, uint64_t a3,
                                 uint64_t a4, uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  task_t *t = sched_current();
  if (!t)
    return -EFAULT;
  t->clear_child_tid = (uint32_t *)tidptr;
  return (int64_t)t->id;
}

// ---------- sysinfo (99) ----------
//
// Estructura Linux x86_64, 112 bytes. Rellenamos uptime, totalram,
// freeram, procs. loads[] a 0 (no medimos load average real). Los
// campos que no tocamos van a 0.
struct k_sysinfo {
  int64_t uptime;
  uint64_t loads[3];
  uint64_t totalram;
  uint64_t freeram;
  uint64_t sharedram;
  uint64_t bufferram;
  uint64_t totalswap;
  uint64_t freeswap;
  uint16_t procs;
  uint16_t pad;
  uint64_t totalhigh;
  uint64_t freehigh;
  uint32_t mem_unit;
  uint8_t _f[0];
};
_Static_assert(sizeof(struct k_sysinfo) == 112, "sysinfo layout x86_64");

struct count_ctx {
  uint32_t n;
};
static int count_proc_cb(process_t *p, void *arg) {
  (void)p;
  ((struct count_ctx *)arg)->n++;
  return 0;
}

static int64_t k_sysinfo(uint64_t info_ptr, uint64_t a2, uint64_t a3,
                         uint64_t a4, uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  if (!info_ptr)
    return -EFAULT;
  if (!access_ok((void *)info_ptr, sizeof(struct k_sysinfo)))
    return -EFAULT;

  struct k_sysinfo si;
  memset(&si, 0, sizeof(si));
  si.uptime = (int64_t)(sched_get_ticks() / 1000);
  si.loads[0] = si.loads[1] = si.loads[2] = 0;
  si.totalram = pmm_total_usable_pages() * (uint64_t)PAGE_SIZE; // [FIX B]
  si.freeram = pmm_free_pages_count() * (uint64_t)PAGE_SIZE;
  si.totalswap = swap_total_bytes();
  si.freeswap = swap_free_bytes();
  si.mem_unit = 1;

  struct count_ctx cc = {.n = 0};
  extern void process_for_each(int (*cb)(process_t *, void *), void *arg);
  process_for_each(count_proc_cb, &cc);
  si.procs = (uint16_t)(cc.n > 0xFFFF ? 0xFFFF : cc.n);

  if (copy_to_user((void *)info_ptr, &si, sizeof(si)) < 0)
    return -EFAULT;
  return 0;
}

// ---------- get/set uid/gid (POSIX) ----------
//
// [3.4.b] Aurora arranca como root. La semántica es la de Linux sin
// capabilities: uid==0 puede todo; el resto solo puede cambiar a
// valores que ya tiene.

static int64_t k_getuid(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                        uint64_t a5) {
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  process_t *p = process_current();
  return p ? (int64_t)p->uid : -EFAULT;
}

static int64_t k_geteuid(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                         uint64_t a5) {
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  process_t *p = process_current();
  return p ? (int64_t)p->euid : -EFAULT;
}

static int64_t k_getgid(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                        uint64_t a5) {
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  process_t *p = process_current();
  return p ? (int64_t)p->gid : -EFAULT;
}

static int64_t k_getegid(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                         uint64_t a5) {
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  process_t *p = process_current();
  return p ? (int64_t)p->egid : -EFAULT;
}

// setuid(uid). Linux x86_64:
//   - root: uid = euid = suid = uid_new
//   - no-root, uid_new == uid || euid || suid: euid = uid_new
//   - no-root, otro: EPERM
static int64_t k_setuid(uint64_t uid, uint64_t a2, uint64_t a3, uint64_t a4,
                        uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  process_t *p = process_current();
  if (!p)
    return -EFAULT;
  uint32_t u = (uint32_t)uid;
  if (p->euid == 0) {
    p->uid = u;
    p->euid = u;
    p->suid = u;
    p->fsuid = u;
    return 0;
  }
  if (u == p->uid || u == p->euid || u == p->suid) {
    p->euid = u;
    p->fsuid = u;
    return 0;
  }
  return -EPERM;
}

static int64_t k_setgid(uint64_t gid, uint64_t a2, uint64_t a3, uint64_t a4,
                        uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  process_t *p = process_current();
  if (!p)
    return -EFAULT;
  uint32_t g = (uint32_t)gid;
  if (p->euid == 0) {
    p->gid = g;
    p->egid = g;
    p->sgid = g;
    p->fsgid = g;
    return 0;
  }
  if (g == p->gid || g == p->egid || g == p->sgid) {
    p->egid = g;
    p->fsgid = g;
    return 0;
  }
  return -EPERM;
}

// setreuid(ruid, euid). -1 significa "no cambiar".
static int64_t k_setreuid(uint64_t ruid, uint64_t euid, uint64_t a3,
                          uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;
  process_t *p = process_current();
  if (!p)
    return -EFAULT;
  int32_t r = (int32_t)ruid;
  int32_t e = (int32_t)euid;
  int was_root = (p->euid == 0);

  if (r != -1) {
    if (!was_root && (uint32_t)r != p->uid && (uint32_t)r != p->euid)
      return -EPERM;
    p->uid = (uint32_t)r;
    // Linux actualiza suid si ruid != old euid o euid cambia.
    if (was_root || (uint32_t)r != p->suid)
      p->suid = (uint32_t)r;
  }
  if (e != -1) {
    if (!was_root && (uint32_t)e != p->uid && (uint32_t)e != p->euid &&
        (uint32_t)e != p->suid)
      return -EPERM;
    p->euid = (uint32_t)e;
    p->fsuid = (uint32_t)e;
  }
  return 0;
}

static int64_t k_setregid(uint64_t rgid, uint64_t egid, uint64_t a3,
                          uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;
  process_t *p = process_current();
  if (!p)
    return -EFAULT;
  int32_t r = (int32_t)rgid;
  int32_t e = (int32_t)egid;
  int was_root = (p->euid == 0);

  if (r != -1) {
    if (!was_root && (uint32_t)r != p->gid && (uint32_t)r != p->egid)
      return -EPERM;
    p->gid = (uint32_t)r;
    if (was_root || (uint32_t)r != p->sgid)
      p->sgid = (uint32_t)r;
  }
  if (e != -1) {
    if (!was_root && (uint32_t)e != p->gid && (uint32_t)e != p->egid &&
        (uint32_t)e != p->sgid)
      return -EPERM;
    p->egid = (uint32_t)e;
    p->fsgid = (uint32_t)e;
  }
  return 0;
}

// setresuid(ruid, euid, suid). -1 = no cambiar.
// Linux: si el proceso no es root, cada valor solo puede ir a uid/euid/suid
// actuales. Si lo es, puede ir a cualquier valor.
static int64_t k_setresuid(uint64_t ruid, uint64_t euid, uint64_t suid,
                           uint64_t a4, uint64_t a5) {
  (void)a4;
  (void)a5;
  process_t *p = process_current();
  if (!p)
    return -EFAULT;
  int32_t r = (int32_t)ruid, e = (int32_t)euid, s = (int32_t)suid;
  int was_root = (p->euid == 0);

  if (!was_root) {
    // Cada valor debe estar entre los uid/euid/suid actuales.
    if (r != -1 && (uint32_t)r != p->uid && (uint32_t)r != p->euid &&
        (uint32_t)r != p->suid)
      return -EPERM;
    if (e != -1 && (uint32_t)e != p->uid && (uint32_t)e != p->euid &&
        (uint32_t)e != p->suid)
      return -EPERM;
    if (s != -1 && (uint32_t)s != p->uid && (uint32_t)s != p->euid &&
        (uint32_t)s != p->suid)
      return -EPERM;
  }
  if (r != -1)
    p->uid = (uint32_t)r;
  if (e != -1) {
    p->euid = (uint32_t)e;
    p->fsuid = (uint32_t)e;
  }
  if (s != -1)
    p->suid = (uint32_t)s;
  return 0;
}

static int64_t k_setresgid(uint64_t rgid, uint64_t egid, uint64_t sgid,
                           uint64_t a4, uint64_t a5) {
  (void)a4;
  (void)a5;
  process_t *p = process_current();
  if (!p)
    return -EFAULT;
  int32_t r = (int32_t)rgid, e = (int32_t)egid, s = (int32_t)sgid;
  int was_root = (p->euid == 0);

  if (!was_root) {
    if (r != -1 && (uint32_t)r != p->gid && (uint32_t)r != p->egid &&
        (uint32_t)r != p->sgid)
      return -EPERM;
    if (e != -1 && (uint32_t)e != p->gid && (uint32_t)e != p->egid &&
        (uint32_t)e != p->sgid)
      return -EPERM;
    if (s != -1 && (uint32_t)s != p->gid && (uint32_t)s != p->egid &&
        (uint32_t)s != p->sgid)
      return -EPERM;
  }
  if (r != -1)
    p->gid = (uint32_t)r;
  if (e != -1) {
    p->egid = (uint32_t)e;
    p->fsgid = (uint32_t)e;
  }
  if (s != -1)
    p->sgid = (uint32_t)s;
  return 0;
}

// getresuid(&ruid, &euid, &suid): escribe tres uint32_t en userland.
static int64_t k_getresuid(uint64_t r_ptr, uint64_t e_ptr, uint64_t s_ptr,
                           uint64_t a4, uint64_t a5) {
  (void)a4;
  (void)a5;
  process_t *p = process_current();
  if (!p)
    return -EFAULT;
  uint32_t vals[3] = {p->uid, p->euid, p->suid};
  uint64_t ptrs[3] = {r_ptr, e_ptr, s_ptr};
  for (int i = 0; i < 3; i++) {
    if (ptrs[i] == 0)
      continue;
    if (!access_ok((void *)ptrs[i], sizeof(uint32_t)))
      return -EFAULT;
    if (copy_to_user((void *)ptrs[i], &vals[i], sizeof(uint32_t)) < 0)
      return -EFAULT;
  }
  return 0;
}

static int64_t k_getresgid(uint64_t r_ptr, uint64_t e_ptr, uint64_t s_ptr,
                           uint64_t a4, uint64_t a5) {
  (void)a4;
  (void)a5;
  process_t *p = process_current();
  if (!p)
    return -EFAULT;
  uint32_t vals[3] = {p->gid, p->egid, p->sgid};
  uint64_t ptrs[3] = {r_ptr, e_ptr, s_ptr};
  for (int i = 0; i < 3; i++) {
    if (ptrs[i] == 0)
      continue;
    if (!access_ok((void *)ptrs[i], sizeof(uint32_t)))
      return -EFAULT;
    if (copy_to_user((void *)ptrs[i], &vals[i], sizeof(uint32_t)) < 0)
      return -EFAULT;
  }
  return 0;
}

// setfsuid / setfsgid. Solo un proceso con euid==0 puede cambiarlos a
// algo distinto de los uid/gid actuales. Los usan algunas libc para
// cambiar credenciales temporalmente.
static int64_t k_setfsuid(uint64_t uid, uint64_t a2, uint64_t a3, uint64_t a4,
                          uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  process_t *p = process_current();
  if (!p)
    return -EFAULT;
  uint32_t old = p->fsuid;
  uint32_t u = (uint32_t)uid;
  if (p->euid == 0 || u == p->uid || u == p->euid || u == p->suid ||
      u == p->fsuid)
    p->fsuid = u;
  return (int64_t)old; // Linux devuelve el valor anterior
}

static int64_t k_setfsgid(uint64_t gid, uint64_t a2, uint64_t a3, uint64_t a4,
                          uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  process_t *p = process_current();
  if (!p)
    return -EFAULT;
  uint32_t old = p->fsgid;
  uint32_t g = (uint32_t)gid;
  if (p->euid == 0 || g == p->gid || g == p->egid || g == p->sgid ||
      g == p->fsgid)
    p->fsgid = g;
  return (int64_t)old;
}

// getgroups(size, list). Sin grupos suplementarios, size==0 devuelve 0.
// Con size>0 devuelve -EINVAL si size < ngroups, o escribe los grupos.
static int64_t k_getgroups(uint64_t size, uint64_t list, uint64_t a3,
                           uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;
  process_t *p = process_current();
  if (!p)
    return -EFAULT;
  if (size == 0)
    return p->ngroups;
  if ((int)size < p->ngroups)
    return -EINVAL;
  if (p->ngroups == 0)
    return 0;
  if (!access_ok((void *)list, (size_t)p->ngroups * sizeof(uint32_t)))
    return -EFAULT;
  if (copy_to_user((void *)list, p->groups,
                   (size_t)p->ngroups * sizeof(uint32_t)) < 0)
    return -EFAULT;
  return p->ngroups;
}

// setgroups(size, list). Solo root. Máximo NGROUPS_MAX.
static int64_t k_setgroups(uint64_t size, uint64_t list, uint64_t a3,
                           uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;
  process_t *p = process_current();
  if (!p)
    return -EFAULT;
  if (p->euid != 0)
    return -EPERM;
  if (size > NGROUPS_MAX)
    return -EINVAL;
  if (size == 0) {
    p->ngroups = 0;
    return 0;
  }
  if (!access_ok((void *)list, (size_t)size * sizeof(uint32_t)))
    return -EFAULT;
  uint32_t tmp[NGROUPS_MAX];
  if (copy_from_user(tmp, (void *)list, (size_t)size * sizeof(uint32_t)) < 0)
    return -EFAULT;
  for (uint64_t i = 0; i < size; i++)
    p->groups[i] = tmp[i];
  p->ngroups = (int)size;
  return 0;
}

// umask(mask): devuelve el anterior y establece el nuevo.
static int64_t k_umask(uint64_t mask, uint64_t a2, uint64_t a3, uint64_t a4,
                       uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  process_t *p = process_current();
  if (!p)
    return -EFAULT;
  uint32_t old = p->umask;
  p->umask = (uint32_t)mask & 0777;
  return (int64_t)old;
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

static int64_t k_getpgrp(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                         uint64_t a5) {
  return k_getpgid(0, a2, a3, a4, a5);
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
static int64_t k_pipe_impl(uint64_t fds_ptr, int *out_rd, int *out_wr) {
  if (!fds_ptr) {
    LOG_ERR("[PIPE] return -EINVAL (fds_ptr=NULL)");
    return -EINVAL;
  }
  if (!access_ok((void *)fds_ptr, 2 * sizeof(int))) {
    LOG_ERR("[PIPE] return -EFAULT (access_ok falla, fds_ptr=%p)",
            (void *)fds_ptr);
    return -EFAULT;
  }

  process_t *proc = process_current();
  if (!proc) {
    LOG_ERR("[PIPE] return -EFAULT (no current process)");
    return -EFAULT;
  }

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
  if (rd < 0 || wr < 0) {
    LOG_ERR("[PIPE] return -EMFILE (rd=%d wr=%d)", rd, wr);
    return -EMFILE;
  }

  vfs_node_t *re = NULL, *we = NULL;
  int rc = vfs_pipe_create(&re, &we);
  if (rc != 0) {
    LOG_ERR("[PIPE] return rc=%d (vfs_pipe_create falló)", rc);
    return rc;
  }

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
    LOG_ERR("[PIPE] return -EFAULT (copy_to_user falló, fds_ptr=%p)",
            (void *)fds_ptr);
    vfs_close_for_proc(proc, rd);
    vfs_close_for_proc(proc, wr);
    return -EFAULT;
  }

  LOG_INFO("[PIPE] OK rd=%d wr=%d", rd, wr);
  if (out_rd)
    *out_rd = rd;
  if (out_wr)
    *out_wr = wr;
  return 0;
}

static int64_t k_pipe(uint64_t fds_ptr, uint64_t a2, uint64_t a3, uint64_t a4,
                      uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  return k_pipe_impl(fds_ptr, NULL, NULL);
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

  file_descriptor_t *f = proc->fds[ofd];

  // Tomar la referencia ANTES de cerrar nfd: si nfd apuntaba al mismo
  // file_descriptor_t, el contador no puede llegar a 0 por el camino.
  __atomic_fetch_add(&f->ref_count, 1, __ATOMIC_ACQ_REL);

  if (proc->fds[nfd]) {
    vfs_close_for_proc(proc, nfd);
  }

  proc->fds[nfd] = f;
  proc->fd_cloexec_mask &= ~(1u << nfd);
  // [FIX] ELIMINADA esta línea: el ref ya se incrementó arriba con
  // __atomic_fetch_add. Antes estaba duplicada y cada dup2 dejaba el
  // file_descriptor_t con un ref huérfano, así que el write end de un
  // pipe nunca llegaba a 0 y el reader bloqueaba en read() para siempre
  // (dmesg | wc -c).
  // f->ref_count++;
  return nfd;
}

static int64_t k_dup3(uint64_t oldfd, uint64_t newfd, uint64_t flags,
                      uint64_t a4, uint64_t a5) {
  (void)a4;
  (void)a5;
  // dup3 es dup2 + flag opcional. Solo O_CLOEXEC (0x80000) es válido.
  if (flags & ~0x80000ULL)
    return -EINVAL;
  // A diferencia de dup2, dup3(old, new) con old == new es EINVAL.
  if ((int)oldfd == (int)newfd)
    return -EINVAL;

  int64_t r = k_dup2(oldfd, newfd, 0, 0, 0);
  if (r < 0)
    return r;
  if (flags & 0x80000ULL) {
    process_t *p = process_current();
    if (p)
      p->fd_cloexec_mask |= (1u << (int)newfd);
  }
  return r;
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

  if (!access_ok((void *)dirp, count))
    return -EFAULT;

  // Buffer en pila para un linux_dirent64 con nombre de hasta
  // VFS_PATH_MAX-1 bytes. sizeof(linux_dirent64_t)=19 + 128 + padding ≈ 160.
  uint8_t kbuf[256];
  size_t pos = 0;
  uint64_t index = f->offset;

  while (pos + sizeof(linux_dirent64_t) + 8 <= count) {
    vfs_dirent_t ent;
    // [3.3.c] vía vfs_readdir_node (no node->ops->readdir directo)
    // para que las mounts hijas (/dev, /proc) aparezcan en `ls /`.
    int rc = vfs_readdir_node(f->node, index, &ent);
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
  size_t dnl = strlen(g_domainname);
  if (dnl >= sizeof(u.domainname))
    dnl = sizeof(u.domainname) - 1;
  memcpy(u.domainname, g_domainname, dnl);
  u.domainname[dnl] = '\0';

  if (!access_ok((void *)buf, sizeof(u)))
    return -EFAULT;
  if (copy_to_user((void *)buf, &u, sizeof(u)) < 0)
    return -EFAULT;
  return 0;
}

// ---------- readlink ----------
static int64_t k_readlink(uint64_t path, uint64_t buf, uint64_t bufsiz,
                          uint64_t a4, uint64_t a5) {
  (void)a4;
  (void)a5;
  process_t *proc = process_current();
  if (!proc)
    return -EFAULT;
  if (!buf || bufsiz == 0)
    return -EINVAL;
  if (!access_ok((void *)buf, bufsiz))
    return -EFAULT;

  char p[VFS_PATH_MAX];
  int rc = resolve_user_path(proc, (const char *)path, p, sizeof(p));
  if (rc != 0)
    return rc;

  char target[VFS_PATH_MAX];
  rc = vfs_readlink(p, target, sizeof(target));
  if (rc < 0)
    return rc;

  size_t n = (size_t)rc;
  if (n > bufsiz)
    n = bufsiz;
  if (copy_to_user((void *)buf, target, n) < 0)
    return -EFAULT;
  return (int64_t)n;
}

// [3.1] symlink(target, linkpath).
static int64_t k_symlink(uint64_t target_ptr, uint64_t linkpath_ptr,
                         uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;
  process_t *proc = process_current();
  if (!proc)
    return -EFAULT;

  char target[VFS_PATH_MAX];
  long tl = strncpy_from_user(target, (const char *)target_ptr, sizeof(target));
  if (tl < 0)
    return -EFAULT;
  if (tl == 0)
    return -EINVAL;

  char linkpath[VFS_PATH_MAX];
  int rc = resolve_user_path(proc, (const char *)linkpath_ptr, linkpath,
                             sizeof(linkpath));
  if (rc != 0)
    return rc;

  return vfs_symlink(target, linkpath);
}

static int64_t k_symlinkat(uint64_t target_ptr, uint64_t newdfd,
                           uint64_t linkpath_ptr, uint64_t a4, uint64_t a5) {
  (void)a4;
  (void)a5;
  // Aurora no soporta dirfd != AT_FDCWD todavía. Fallback a k_symlink.
  if ((int)newdfd != AT_FDCWD)
    return -EINVAL;
  return k_symlink(target_ptr, linkpath_ptr, 0, 0, 0);
}

// ---------- tkill ----------
//
// En Linux, tkill(tid, sig) envía a un thread concreto. Aurora es
// single-threaded: tid == pid. BusyBox ash lo usa en job control.
static int64_t k_tkill(uint64_t tid, uint64_t sig, uint64_t a3, uint64_t a4,
                       uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;
  return k_kill(tid, sig, 0, 0, 0);
}

// ---------- tgkill ----------
//
// tgkill(tgid, tid, sig). En Linux, envía una señal a un thread
// específico. En Aurora no hay thread groups separados (cada thread es
// un task_t y un process_t), así que tid == pid y tgid == pid.
//
// glibc lo usa en raise() y pthread_kill(). Sin esto, abort() no puede
// matar el proceso y sigue ejecutando con estado corrupto → hlt.
static int64_t k_tgkill(uint64_t tgid, uint64_t tid, uint64_t sig, uint64_t a4,
                        uint64_t a5) {
  (void)a4;
  (void)a5;
  process_t *self = process_current();
  task_t *cur = sched_current();
  if (!self || !cur)
    return -EFAULT;
  if ((uint32_t)tgid != self->pid)
    return -EINVAL;
  // Aceptar tanto el pid (thread principal) como el task->id crudo.
  if ((uint32_t)tid != self->pid && (uint32_t)tid != cur->id)
    return -EINVAL;
  return k_kill(tid, sig, 0, 0, 0);
}

// ---------- readlinkat ----------
//
// readlinkat(dirfd, path, buf, bufsiz). AT_FDCWD o dirfd numérico.
// Reutilizamos k_readlink resolviendo el path con el cwd correcto.
static int64_t k_readlinkat(uint64_t dirfd, uint64_t path, uint64_t buf,
                            uint64_t bufsiz, uint64_t a5) {
  (void)a5;
  if ((int)dirfd != AT_FDCWD) {
    // Path relativo a un dirfd abierto: no soportado todavía.
    // BusyBox rara vez lo usa (suele pasar AT_FDCWD).
    return -EINVAL;
  }
  return k_readlink(path, buf, bufsiz, 0, 0);
}

// ---------- faccessat ----------
//
// faccessat(dirfd, path, mode, flags). Con flags=0 se comporta como
// access(path, mode). Con AT_EACCESS no cambia nada aquí porque todo
// corre como root. Ignoramos `mode` como hace k_access (no hay permisos
// reales, todo es accesible si existe).
static int64_t k_faccessat(uint64_t dirfd, uint64_t path, uint64_t mode,
                           uint64_t flags, uint64_t a5) {
  (void)flags;
  (void)a5;
  if ((int)dirfd != AT_FDCWD)
    return -EINVAL;
  return k_access(path, mode, 0, 0, 0);
}

// ---------- pipe2 ----------
//
// Implementamos O_CLOEXEC de verdad porque los descriptores de un pipe
// se usan normalmente como fds temporales de un pipeline. Así, un
// fork()+execve() no puede conservar accidentalmente un extremo del pipe
// y retrasar el EOF del lector.
//
// O_NONBLOCK todavía no está implementado por el pipe/VFS; devolver EINVAL
// es preferible a fingir que la operación es no bloqueante.
static int64_t k_pipe2(uint64_t fds_ptr, uint64_t flags, uint64_t a3,
                       uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;

  const uint64_t supported = LINUX_O_CLOEXEC;
  if (flags & ~supported)
    return -EINVAL;

  process_t *proc = process_current();
  if (!proc)
    return -EFAULT;

  int rd = -1, wr = -1;
  int64_t rc = k_pipe_impl(fds_ptr, &rd, &wr);
  if (rc < 0)
    return rc;

  if (flags & LINUX_O_CLOEXEC)
    proc->fd_cloexec_mask |= (1u << rd) | (1u << wr);

  return 0;
}

// [3.2] link(oldpath, newpath): hard link. FAT32 → -EPERM, tarfs → -EROFS.
static int64_t k_link(uint64_t oldp, uint64_t newp, uint64_t a3, uint64_t a4,
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
  return vfs_link(op, np);
}

// linkat(olddirfd, oldpath, newdirfd, newpath, flags).
// Ignoramos flags (AT_SYMLINK_FOLLOW, AT_EMPTY_PATH no soportados).
static int64_t k_linkat(uint64_t olddfd, uint64_t oldp, uint64_t newdfd,
                        uint64_t newp, uint64_t flags) {
  (void)flags;
  if ((int)olddfd != AT_FDCWD || (int)newdfd != AT_FDCWD)
    return -EINVAL;
  return k_link(oldp, newp, 0, 0, 0);
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
//
// CLOCK_REALTIME (0) debe devolver epoch UNIX — es lo que usan
// touch/date/gettimeofday para saber "ahora".
// CLOCK_MONOTONIC (1), CLOCK_PROCESS_CPUTIME_ID (2), CLOCK_BOOTTIME (7)
// devuelven tiempo desde el boot, que es lo que ya hacíamos.
#define CLOCK_REALTIME_ 0
#define CLOCK_MONOTONIC_ 1

static int64_t k_clock_gettime(uint64_t clockid, uint64_t tp, uint64_t a3,
                               uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;
  if (!access_ok((void *)tp, 16))
    return -EFAULT;

  struct {
    int64_t sec;
    int64_t nsec;
  } ts;

  if (clockid == CLOCK_REALTIME_) {
    int64_t epoch = rtc_get_epoch();
    if (epoch == 0) {
      // RTC no disponible: cae a segundos desde boot. Mejor que 0.
      uint64_t t = sched_get_ticks();
      ts.sec = (int64_t)(t / 1000);
      ts.nsec = (int64_t)((t % 1000) * 1000000);
    } else {
      ts.sec = epoch;
      ts.nsec = 0;
    }
  } else {
    uint64_t t = sched_get_ticks();
    ts.sec = (int64_t)(t / 1000);
    ts.nsec = (int64_t)((t % 1000) * 1000000);
  }

  if (copy_to_user((void *)tp, &ts, sizeof(ts)) < 0)
    return -EFAULT;
  return 0;
}

static int64_t k_gettimeofday(uint64_t tv_ptr, uint64_t tz_ptr, uint64_t a3,
                              uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;

  if (tv_ptr) {
    struct {
      int64_t sec;
      int64_t usec;
    } tv;
    if (!access_ok((void *)tv_ptr, sizeof(tv)))
      return -EFAULT;

    int64_t epoch = rtc_get_epoch();
    if (epoch == 0)
      epoch = (int64_t)(sched_get_ticks() / 1000);
    tv.sec = epoch;
    tv.usec = 0;
    if (copy_to_user((void *)tv_ptr, &tv, sizeof(tv)) < 0)
      return -EFAULT;
  }

  if (tz_ptr) {
    struct {
      int32_t minutes_west;
      int32_t dst_time;
    } tz = {0};
    if (!access_ok((void *)tz_ptr, sizeof(tz)))
      return -EFAULT;
    if (copy_to_user((void *)tz_ptr, &tz, sizeof(tz)) < 0)
      return -EFAULT;
  }
  return 0;
}

static int64_t k_time(uint64_t tloc, uint64_t a2, uint64_t a3, uint64_t a4,
                      uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;

  int64_t epoch = rtc_get_epoch();
  if (epoch == 0)
    epoch = (int64_t)(sched_get_ticks() / 1000);
  if (tloc && copy_to_user((void *)tloc, &epoch, sizeof(epoch)) < 0)
    return -EFAULT;
  return epoch;
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

// ---------- clock_settime / settimeofday / adjtimex ----------

struct k_timeval {
  int64_t tv_sec;
  int64_t tv_usec;
};

extern int rtc_set_epoch(int64_t epoch);

static int64_t k_clock_settime(uint64_t clockid, uint64_t tp, uint64_t a3,
                               uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;
  // Solo soportamos CLOCK_REALTIME (0). El resto son inmutables
  // (MONOTONIC, BOOTTIME) o por-proceso (CPUTIME).
  if (clockid != 0)
    return -EINVAL;
  if (!access_ok((void *)tp, 16))
    return -EFAULT;
  struct k_timespec ts;
  if (copy_from_user(&ts, (void *)tp, sizeof(ts)) < 0)
    return -EFAULT;
  if (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000LL)
    return -EINVAL;
  if (!rtc_set_epoch(ts.tv_sec))
    return -EIO;
  return 0;
}

static int64_t k_settimeofday(uint64_t tv_ptr, uint64_t tz_ptr, uint64_t a3,
                              uint64_t a4, uint64_t a5) {
  (void)tz_ptr;
  (void)a3;
  (void)a4;
  (void)a5;
  if (tv_ptr) {
    if (!access_ok((void *)tv_ptr, sizeof(struct k_timeval)))
      return -EFAULT;
    struct k_timeval tv;
    if (copy_from_user(&tv, (void *)tv_ptr, sizeof(tv)) < 0)
      return -EFAULT;
    if (tv.tv_sec < 0)
      return -EINVAL;
    if (!rtc_set_epoch(tv.tv_sec))
      return -EIO;
  }
  return 0;
}

// adjtimex: hwclock -a lo usa. No soportamos ajuste de deriva; devolvemos
// valores razonables (Linux: struct timex relleno). hwclock solo comprueba
// el return, así que con 0 basta.
static int64_t k_adjtimex(uint64_t tx_ptr, uint64_t a2, uint64_t a3,
                          uint64_t a4, uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  if (tx_ptr) {
    // Rellenamos 128 bytes con ceros como hace Linux en modo
    // "solo lectura". Es suficiente para que hwclock no se queje.
    if (!access_ok((void *)tx_ptr, 128))
      return -EFAULT;
    uint8_t zeros[128] = {0};
    if (copy_to_user((void *)tx_ptr, zeros, sizeof(zeros)) < 0)
      return -EFAULT;
  }
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
  case F_DUPFD:
  case F_DUPFD_CLOEXEC: {
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
    __atomic_fetch_add(&f->ref_count, 1, __ATOMIC_ACQ_REL);
    proc->fds[free_fd] = f;
    // [FD_CLOEXEC] POSIX: F_DUPFD limpia CLOEXEC en el nuevo fd.
    // F_DUPFD_CLOEXEC lo pone.
    if (cmd == F_DUPFD_CLOEXEC)
      proc->fd_cloexec_mask |= (1u << free_fd);
    else
      proc->fd_cloexec_mask &= ~(1u << free_fd);
    return (int64_t)free_fd;
  }
  case F_GETFD:
    return (proc->fd_cloexec_mask & (1u << kfd)) ? FD_CLOEXEC : 0;
  case F_SETFD:
    if ((int)arg & FD_CLOEXEC)
      proc->fd_cloexec_mask |= (1u << kfd);
    else
      proc->fd_cloexec_mask &= ~(1u << kfd);
    return 0;
  case F_GETFL:
    return (int64_t)f->flags;
  case F_SETFL:
    return 0;
  default:
    return -EINVAL;
  }
}

static int64_t k_dup(uint64_t fd, uint64_t a2, uint64_t a3, uint64_t a4,
                     uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  return k_fcntl(fd, F_DUPFD, 0, 0, 0);
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
  LOG_TRACE("[NANOSLEEP] pid=%u req=%lu ticks left=%lu interrupted=%d "
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

// ---------- [4.1] statfs / fstatfs ----------
struct k_statfs {
  int64_t f_type;
  int64_t f_bsize;
  uint64_t f_blocks;
  uint64_t f_bfree;
  uint64_t f_bavail;
  uint64_t f_files;
  uint64_t f_ffree;
  int32_t f_fsid[2];
  int64_t f_namelen;
  int64_t f_frsize;
  int64_t f_flags;
  int64_t f_spare[4];
};
_Static_assert(sizeof(struct k_statfs) == 120, "statfs layout x86_64");

static int64_t do_statfs_path(const char *path, uint64_t buf_uptr) {
  if (!access_ok((void *)buf_uptr, sizeof(struct k_statfs)))
    return -EFAULT;
  struct vfs_statfs vs;
  int rc = vfs_statfs(path, &vs);
  if (rc != 0)
    return rc;
  struct k_statfs ks;
  memset(&ks, 0, sizeof(ks));
  ks.f_type = (int64_t)vs.f_type;
  ks.f_bsize = (int64_t)vs.f_bsize;
  ks.f_blocks = vs.f_blocks;
  ks.f_bfree = vs.f_bfree;
  ks.f_bavail = vs.f_bavail;
  ks.f_files = vs.f_files;
  ks.f_ffree = vs.f_ffree;
  ks.f_namelen = (int64_t)vs.f_namelen;
  ks.f_frsize = (int64_t)vs.f_frsize;
  if (copy_to_user((void *)buf_uptr, &ks, sizeof(ks)) < 0)
    return -EFAULT;
  return 0;
}

static int64_t k_statfs(uint64_t path_uptr, uint64_t buf_uptr, uint64_t a3,
                        uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;
  process_t *proc = process_current();
  if (!proc)
    return -EFAULT;
  char path[VFS_PATH_MAX];
  int rc = resolve_user_path(proc, (const char *)path_uptr, path, sizeof(path));
  if (rc != 0)
    return rc;
  return do_statfs_path(path, buf_uptr);
}

static int64_t k_fstatfs(uint64_t fd, uint64_t buf_uptr, uint64_t a3,
                         uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;
  process_t *proc = process_current();
  if (!proc)
    return -EFAULT;
  if ((int)fd < 0 || (int)fd >= MAX_PROCESS_FDS || !proc->fds[fd])
    return -EBADF;
  return do_statfs_path(proc->fds[fd]->node->name, buf_uptr);
}

// ---------------------------------------------------------------------------
// [statx] struct statx de Linux x86_64, 256 bytes.
//
// Layout exacto (include/uapi/linux/stat.h):
//   0x00 stx_mask (u32)          0x04 stx_blksize (u32)
//   0x08 stx_attributes (u64)    0x10 stx_nlink (u32)
//   0x14 stx_uid (u32)           0x18 stx_gid (u32)
//   0x1c stx_mode (u16)          0x1e __spare0 (u16)
//   0x20 stx_ino (u64)           0x28 stx_size (u64)
//   0x30 stx_blocks (u64)        0x38 stx_attributes_mask (u64)
//   0x40 stx_atime (16B)         0x50 stx_btime (16B)
//   0x60 stx_ctime (16B)         0x70 stx_mtime (16B)
//   0x80 stx_rdev_major/minor    0x88 stx_dev_major/minor
//   0x90 stx_mnt_id              0x98 stx_dio_mem_align
//   0x9c stx_dio_offset_align    0xa0 __spare3[12] (96B)
// ---------------------------------------------------------------------------
struct k_statx_ts {
  int64_t tv_sec;
  uint32_t tv_nsec;
  int32_t __reserved;
};

struct k_statx {
  uint32_t stx_mask;
  uint32_t stx_blksize;
  uint64_t stx_attributes;
  uint32_t stx_nlink;
  uint32_t stx_uid;
  uint32_t stx_gid;
  uint16_t stx_mode;
  uint16_t __spare0;
  uint64_t stx_ino;
  uint64_t stx_size;
  uint64_t stx_blocks;
  uint64_t stx_attributes_mask;
  struct k_statx_ts stx_atime;
  struct k_statx_ts stx_btime;
  struct k_statx_ts stx_ctime;
  struct k_statx_ts stx_mtime;
  uint32_t stx_rdev_major;
  uint32_t stx_rdev_minor;
  uint32_t stx_dev_major;
  uint32_t stx_dev_minor;
  uint64_t stx_mnt_id;
  uint32_t stx_dio_mem_align;
  uint32_t stx_dio_offset_align;
  uint64_t __spare3[12];
};
_Static_assert(sizeof(struct k_statx) == 256, "statx layout x86_64");

#define STATX_BASIC_STATS 0x000007ffU

#ifndef AT_EMPTY_PATH
#define AT_EMPTY_PATH 0x1000
#endif

// Traduce un vfs_stat_t al struct statx. Reutiliza el mismo esquema de
// device que vfs_to_linux_stat (0:<minor>) para que coincida con el
// resto del kernel (df/mountinfo/maps usan el mismo).
static void vfs_to_statx(const vfs_stat_t *vs, uint64_t size, uint64_t dev,
                         struct k_statx *out) {
  memset(out, 0, sizeof(*out));

  uint32_t mode = vs->mode;
  if (mode == 0) {
    if (vs->flags & VFS_DIRECTORY)
      mode = S_IFDIR | 0755;
    else if (vs->flags & VFS_CHARDEVICE)
      mode = S_IFCHR | 0666;
    else
      mode = S_IFREG | 0644;
  }

  out->stx_mask = STATX_BASIC_STATS;
  out->stx_blksize = 512;
  out->stx_nlink = (vs->flags & VFS_DIRECTORY) ? 2 : 1;
  out->stx_uid = vs->uid;
  out->stx_gid = vs->gid;
  out->stx_mode = (uint16_t)(mode & 0xFFFF);
  out->stx_ino = vs->inode;
  out->stx_size = size;
  out->stx_blocks = (size + 511) / 512;
  out->stx_atime.tv_sec = vs->mtime_sec;
  out->stx_mtime.tv_sec = vs->mtime_sec;
  out->stx_ctime.tv_sec = vs->mtime_sec;

  uint32_t ftype = mode & S_IFMT;
  if (ftype == S_IFCHR || ftype == S_IFBLK) {
    out->stx_rdev_major = (vs->rdev >> 8) & 0xFF;
    out->stx_rdev_minor = vs->rdev & 0xFF;
  }
  out->stx_dev_major = 0;
  out->stx_dev_minor = (uint32_t)dev;
}

// ---------------------------------------------------------------------------
// statx(dirfd, path, flags, mask, statxbuf). Linux 332.
//
// Implementación mínima, suficiente para glibc y coreutils 9.x:
//   - AT_FDCWD + path absoluto o relativo al cwd: camino normal.
//   - dirfd real + path relativo: combina como fstatat.
//   - AT_EMPTY_PATH con fd real: stat del nodo del fd.
//   - AT_SYMLINK_NOFOLLOW: no sigue el symlink final.
//
// El `mask` del llamante se ignora: siempre rellenamos STATX_BASIC_STATS.
// Es lo que hace Linux para la mayoría de FS: devuelve más de lo pedido
// y el llamante filtra.
// ---------------------------------------------------------------------------
static int64_t k_statx(uint64_t dfd, uint64_t path_uptr, uint64_t flags,
                       uint64_t mask, uint64_t statxbuf) {
  (void)mask;
  process_t *proc = process_current();
  if (!proc)
    return -EFAULT;
  if (!access_ok((void *)statxbuf, sizeof(struct k_statx)))
    return -EFAULT;

  int32_t kdfd = (int32_t)dfd;
  int no_follow = (flags & AT_SYMLINK_NOFOLLOW) ? 1 : 0;

  // AT_EMPTY_PATH con fd real: stat del nodo del fd.
  if ((flags & AT_EMPTY_PATH) && kdfd != AT_FDCWD) {
    if (kdfd < 0 || kdfd >= MAX_PROCESS_FDS || !proc->fds[kdfd])
      return -EBADF;
    char chk[2];
    long n = strncpy_from_user(chk, (const char *)path_uptr, sizeof(chk));
    if (n < 0)
      return -EFAULT;
    if (n != 0)
      return -EINVAL;

    vfs_stat_t vs;
    int rc = vfs_fstat_for_proc(proc, kdfd, &vs);
    if (rc != 0)
      return rc;
    uint64_t dev = fs_dev_id(proc->fds[kdfd]->node->fs);
    struct k_statx sx;
    vfs_to_statx(&vs, vs.size, dev, &sx);
    if (copy_to_user((void *)statxbuf, &sx, sizeof(sx)) < 0)
      return -EFAULT;
    return 0;
  }

  char path[VFS_PATH_MAX];
  if (kdfd == AT_FDCWD) {
    int rc =
        resolve_user_path(proc, (const char *)path_uptr, path, sizeof(path));
    if (rc != 0)
      return rc;
  } else {
    if (kdfd < 0 || kdfd >= MAX_PROCESS_FDS || !proc->fds[kdfd])
      return -EBADF;
    vfs_node_t *dir_node = proc->fds[kdfd]->node;
    if (!dir_node || !(dir_node->flags & VFS_DIRECTORY))
      return -ENOTDIR;

    char rel[VFS_PATH_MAX];
    long n = strncpy_from_user(rel, (const char *)path_uptr, sizeof(rel));
    if (n < 0)
      return -EFAULT;
    if (n == 0)
      return -EINVAL;

    char full[VFS_PATH_MAX];
    if (rel[0] == '/') {
      size_t rl = strlen(rel);
      if (rl >= sizeof(full))
        return -ENAMETOOLONG;
      memcpy(full, rel, rl + 1);
    } else {
      const char *base = dir_node->name;
      size_t bl = strlen(base);
      size_t rl = strlen(rel);
      int base_is_root = (bl == 1 && base[0] == '/');
      size_t needed = base_is_root ? (1 + rl + 1) : (bl + 1 + rl + 1);
      if (needed > sizeof(full))
        return -ENAMETOOLONG;
      if (base_is_root) {
        full[0] = '/';
        memcpy(full + 1, rel, rl + 1);
      } else {
        memcpy(full, base, bl);
        full[bl] = '/';
        memcpy(full + bl + 1, rel, rl + 1);
      }
    }
    int rc = vfs_resolve_path("/", full, path, sizeof(path));
    if (rc != 0)
      return rc;
  }

  vfs_node_t *node = no_follow ? vfs_lookup_nofollow(path) : vfs_lookup(path);
  if (!node)
    return -ENOENT;

  vfs_stat_t vs = {
      .flags = node->flags,
      .size = node->size,
      .inode = node->inode,
      .mtime_sec = node->mtime_sec,
      .mode = node->mode,
      .uid = node->uid,
      .gid = node->gid,
      .rdev = node->rdev,
  };
  uint64_t dev = fs_dev_id(node->fs);
  vfs_node_free(node);

  struct k_statx sx;
  vfs_to_statx(&vs, vs.size, dev, &sx);
  if (copy_to_user((void *)statxbuf, &sx, sizeof(sx)) < 0)
    return -EFAULT;
  return 0;
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
// ---------------------------------------------------------------------------
// clone(2) x86_64.
//   rdi = flags
//   rsi = stack
//   rdx = parent_tidptr
//   r10 = child_tidptr
//   r8  = tls
//
// Flags soportados:
//   CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD |
//   CLONE_SYSVSEM | CLONE_SETTLS | CLONE_PARENT_SETTID | CLONE_CHILD_SETTID |
//   CLONE_CHILD_CLEARTID
// Cualquier otro bit (namespaces, CLONE_VFORK, CLONE_PTRACE, ...) → -EINVAL.
//
// El byte bajo son los bits de señal de salida (SIGCHLD típicamente);
// los ignoramos porque ya enviamos SIGCHLD al padre en process_exit.
//
// Sin CLONE_THREAD (aunque tenga CLONE_VM) → fork() normal.
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// clone(2) x86_64.
//   rdi = flags
//   rsi = stack
//   rdx = parent_tidptr
//   r10 = child_tidptr
//   r8  = tls
//
// Convención musl 1.2.x: además de esos 5 args del syscall, musl deja
// r9 = fn (el entrypoint del thread) y el arg en [stack]. El kernel
// solo tiene que preservar los registros tal cual (los pasa al hijo
// vía sched_create_forked_user_task_ex) y poner rax=0 en el hijo. El
// hijo ejecuta `call *%r9` él solo.
// ---------------------------------------------------------------------------
#define CLONE_VM 0x00000100ULL
#define CLONE_FS 0x00000200ULL
#define CLONE_FILES 0x00000400ULL
#define CLONE_SIGHAND 0x00000800ULL
#define CLONE_THREAD 0x00010000ULL
#define CLONE_SYSVSEM 0x00040000ULL
#define CLONE_SETTLS 0x00080000ULL
#define CLONE_PARENT_SETTID 0x00100000ULL
#define CLONE_CHILD_CLEARTID 0x00200000ULL
#define CLONE_DETACHED 0x00400000ULL
#define CLONE_CHILD_SETTID 0x01000000ULL

static int64_t k_clone(uint64_t flags, uint64_t stack, uint64_t ptid,
                       uint64_t ctid, uint64_t tls) {
  process_t *parent = process_current();
  if (!parent)
    return -EINVAL;
  registers_t *r = syscall_current_regs();
  if (!r)
    return -EINVAL;

  uint64_t supported = CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND |
                       CLONE_THREAD | CLONE_SYSVSEM | CLONE_SETTLS |
                       CLONE_PARENT_SETTID | CLONE_CHILD_SETTID |
                       CLONE_CHILD_CLEARTID | CLONE_DETACHED;
  uint64_t unknown = flags & ~(supported | 0xffULL);
  if (unknown) {
    LOG_WARN("[CLONE] flags no soportados: 0x%lx", (unsigned long)unknown);
    return -EINVAL;
  }

  if (!(flags & CLONE_THREAD))
    return sys_fork();
  if (!(flags & CLONE_VM))
    return -EINVAL;

  uint64_t tls_val = (flags & CLONE_SETTLS) ? tls : 0;
  uint64_t *ptid_ptr = (flags & CLONE_PARENT_SETTID) ? (uint64_t *)ptid : NULL;
  // [FIX] CHILD_SETTID y CHILD_CLEARTID son flags INDEPENDIENTES:
  //   CHILD_SETTID   → escribe el tid al clonar
  //   CHILD_CLEARTID → limpia a 0 y futex-wake al morir
  // Antes los mezclábamos y machacábamos __thread_list_lock de musl.
  uint64_t *ctid_set_ptr =
      (flags & CLONE_CHILD_SETTID) ? (uint64_t *)ctid : NULL;
  uint64_t *ctid_clear_ptr =
      (flags & CLONE_CHILD_CLEARTID) ? (uint64_t *)ctid : NULL;

  task_t *child = process_clone_thread(parent, r, stack, tls_val, ptid_ptr,
                                       ctid_set_ptr, ctid_clear_ptr);
  if (!child)
    return -ENOMEM;
  return (int64_t)child->id;
}

static int64_t k_exit(uint64_t code, uint64_t a2, uint64_t a3, uint64_t a4,
                      uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  process_exit_current((int)code);
  // no retorna
}

static int64_t k_wait4(uint64_t pid, uint64_t status_ptr, uint64_t options,
                       uint64_t rusage_ptr, uint64_t a5) {
  (void)a5;
  process_t *self = process_current();
  if (!self)
    return -EFAULT;

  LOG_TRACE("[WAIT4] self=%u pid=%ld options=0x%lx", self->pid,
            (long)(int32_t)pid, (unsigned long)options);

  int32_t aurora_status = 0;
  proc_rusage_t ru = {0};
  int r =
      process_waitpid(self, (int32_t)pid, &aurora_status, &ru, (int)options);
  LOG_TRACE("[WAIT4] -> %d", r);
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

// [EXECVE-LOCK] process_execve_prepare cambia el address space del
// proceso actual y toca estructuras VMA/PMM compartidas. Mantener los
// execve serializados evita que dos CPUs entren simultáneamente en esa
// ruta. El lock se inicializa estáticamente: NO puede haber un "lazy init"
// concurrente que reinicialice el spinlock mientras otra CPU ya lo está
// usando.
static spinlock_t g_execve_lock = {0};

static int64_t k_execve(uint64_t path_ptr, uint64_t argv_ptr, uint64_t envp_ptr,
                        uint64_t a4, uint64_t a5) {
  (void)a4;
  (void)a5;
  process_t *proc = process_current();
  if (!proc)
    return -EFAULT;

  unsigned long exec_flags = spin_lock_irqsave(&g_execve_lock);
  int64_t ret = -EFAULT;

  char path[VFS_PATH_MAX];
  int rc = resolve_user_path(proc, (const char *)path_ptr, path, sizeof(path));
  if (rc != 0) {
    ret = rc;
    goto out;
  }
  process_set_exe_path(proc, path); // ← NUEVO
  LOG_INFO("[EXECVE] pid=%u path=%s", proc->pid, path);

  // Buffers en heap: 16*128 + 32*256 = 10 KB no caben cómodos en el
  // stack de kernel sin disparar -Wframe-larger-than.
  enum {
    ARGV_BYTES = PROCESS_ARGV_MAX * SPAWN_ARG_STR_MAX,
    ENVP_BYTES = PROCESS_ENVP_MAX * PROCESS_ENV_STR_MAX
  };
  char *buf = (char *)kmalloc(ARGV_BYTES + ENVP_BYTES);
  if (!buf) {
    ret = -ENOMEM;
    goto out;
  }
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
        ret = -EFAULT;
        goto out;
      }
      if (uptr == 0)
        break;
      long m = strncpy_from_user(&argv_storage[i * SPAWN_ARG_STR_MAX],
                                 (const char *)uptr, SPAWN_ARG_STR_MAX);
      if (m < 0) {
        kfree(buf);
        ret = -EFAULT;
        goto out;
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
        ret = -EFAULT;
        goto out;
      }
      if (uptr == 0)
        break;
      long m = strncpy_from_user(&env_storage[i * PROCESS_ENV_STR_MAX],
                                 (const char *)uptr, PROCESS_ENV_STR_MAX);
      if (m < 0) {
        kfree(buf);
        ret = -EFAULT;
        goto out;
      }
      kenvp[i] = &env_storage[i * PROCESS_ENV_STR_MAX];
      envc++;
    }
  }

  uint64_t new_entry = 0, new_rsp = 0;
  rc = process_execve_prepare(path, argc, kargv, envc, kenvp, &new_entry,
                              &new_rsp);
  kfree(buf);
  if (rc != 0) {
    ret = rc;
    goto out;
  }
  LOG_INFO("[EXECVE] pid=%u rc=%lld", proc->pid, (long long)rc);

  registers_t *regs = syscall_current_regs();
  if (!regs) {
    ret = -EINVAL;
    goto out;
  }
  regs->rip = new_entry;
  regs->rsp = new_rsp;
  LOG_INFO("[EXECVE-IRET] pid=%u rip=%p rsp=%p", proc->pid, (void *)regs->rip,
           (void *)regs->rsp);

  ret = 0;

out:
  spin_unlock_irqrestore(&g_execve_lock, exec_flags);
  return ret;
}

// ---------- gettid ----------
static int64_t k_gettid(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                        uint64_t a5) {
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  task_t *t = sched_current();
  if (!t)
    return -EFAULT;
  // [glibc] En un proceso sin threads, gettid() == getpid() (thread
  // principal). Aurora tiene 1 task_t por proceso, así que el pid es
  // el tid correcto. Devolver t->id rompía abort() de glibc:
  // getpid()=3, gettid()=38, tgkill(3, 38, SIGABRT) → EINVAL.
  process_t *p = t->proc;
  return p ? (int64_t)p->pid : (int64_t)t->id;
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
  // [fix] kmalloc en vez de buffer de pila: 4 KB en el stack del
  // kernel dispara -Wframe-larger-than=2048 y en SMP reduce el margen
  // antes del overflow de la pila de la tarea.
  const size_t CHUNK = 4096;
  uint8_t *buf = (uint8_t *)kmalloc(CHUNK);
  if (!buf)
    return -ENOMEM;

  size_t total = 0;
  while (total < count) {
    size_t chunk = count - total;
    if (chunk > CHUNK)
      chunk = CHUNK;
    int64_t r = in->node->ops->read(in->node, in->offset, chunk, buf);
    if (r <= 0)
      break;
    int64_t w = out->node->ops->write(out->node, out->offset, r, buf);
    if (w < 0) {
      if (total == 0) {
        kfree(buf);
        return w;
      }
      break;
    }
    in->offset += r;
    out->offset += w;
    total += w;
    if (w < r)
      break;
  }

  kfree(buf);
  return (int64_t)total;
}

// ---------- [4.2] fsync / chmod / fchmod / chown / fchown ----------
static int64_t k_fsync(uint64_t fd, uint64_t a2, uint64_t a3, uint64_t a4,
                       uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  process_t *proc = process_current();
  if (!proc)
    return -EFAULT;
  if ((int)fd < 0 || (int)fd >= MAX_PROCESS_FDS || !proc->fds[fd])
    return -EBADF;
  // FAT32 sincroniza en cada write. tarfs es RO. Nada que hacer.
  return 0;
}

// ---------- chmod / fchmod / fchmodat ----------
//
// [3.4.c] Implementación real. El VFS comprueba permisos:
// root o owner pueden chmod. FS sin soporte devuelven -EPERM/-EROFS.

static int64_t k_chmod(uint64_t path_ptr, uint64_t mode, uint64_t a3,
                       uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;
  process_t *proc = process_current();
  if (!proc)
    return -EFAULT;
  char path[VFS_PATH_MAX];
  int rc = resolve_user_path(proc, (const char *)path_ptr, path, sizeof(path));
  if (rc != 0)
    return rc;
  return vfs_chmod(path, (uint32_t)mode);
}

static int64_t k_fchmod(uint64_t fd, uint64_t mode, uint64_t a3, uint64_t a4,
                        uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;
  process_t *proc = process_current();
  if (!proc)
    return -EFAULT;
  if ((int)fd < 0 || (int)fd >= MAX_PROCESS_FDS || !proc->fds[fd])
    return -EBADF;
  return vfs_fchmod(proc->fds[fd], (uint32_t)mode);
}

static int64_t k_fchmodat(uint64_t dfd, uint64_t path_ptr, uint64_t mode,
                          uint64_t flags, uint64_t a5) {
  (void)flags;
  (void)a5;
  if ((int)dfd != AT_FDCWD)
    return -EINVAL;
  return k_chmod(path_ptr, mode, 0, 0, 0);
}

// ---------- chown / fchown / lchown ----------
//
// [3.4.c] uid/gid = -1 deja el campo intacto. Solo root.

static int64_t k_chown(uint64_t path_ptr, uint64_t uid, uint64_t gid,
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
  return vfs_chown(path, (uint32_t)uid, (uint32_t)gid);
}

static int64_t k_fchown(uint64_t fd, uint64_t uid, uint64_t gid, uint64_t a4,
                        uint64_t a5) {
  (void)a4;
  (void)a5;
  process_t *proc = process_current();
  if (!proc)
    return -EFAULT;
  if ((int)fd < 0 || (int)fd >= MAX_PROCESS_FDS || !proc->fds[fd])
    return -EBADF;
  return vfs_fchown(proc->fds[fd], (uint32_t)uid, (uint32_t)gid);
}

static int64_t k_lchown(uint64_t path_ptr, uint64_t uid, uint64_t gid,
                        uint64_t a4, uint64_t a5) {
  // lchown == chown para FS que no soportan symlink-follow at chown.
  // tarfs es RO, así que devolverá -EROFS igual.
  return k_chown(path_ptr, uid, gid, a4, a5);
}

// ---------- [4.2] utimensat ----------
#define UTIME_NOW 0x3FFFFFFFLL
#define UTIME_OMIT 0x3FFFFFFELL

struct k_ts64 {
  int64_t tv_sec;
  int64_t tv_nsec;
};

static int64_t k_utimensat(uint64_t dfd, uint64_t path_uptr,
                           uint64_t times_uptr, uint64_t flags, uint64_t a5) {
  (void)a5;
  process_t *proc = process_current();
  if (!proc)
    return -EFAULT;
  if (flags & ~0x100ULL)
    return -EINVAL; // solo AT_SYMLINK_NOFOLLOW

  int64_t mtime_sec = 0;
  if (times_uptr) {
    struct k_ts64 times[2];
    if (!access_ok((void *)times_uptr, sizeof(times)))
      return -EFAULT;
    if (copy_from_user(times, (void *)times_uptr, sizeof(times)) < 0)
      return -EFAULT;
    if (times[1].tv_nsec == UTIME_OMIT)
      return 0;
    if (times[1].tv_nsec == UTIME_NOW)
      mtime_sec = rtc_get_epoch();
    else
      mtime_sec = times[1].tv_sec;
  } else {
    mtime_sec = rtc_get_epoch();
  }

  // [4.2] futimens(fd, times) → utimensat(fd, NULL, times, 0).
  // BusyBox touch abre el fichero con O_CREAT y luego aplica futimens.
  // Sin este path, dfd != AT_FDCWD → EINVAL → mtime se queda en 1980.
  if ((int)dfd != AT_FDCWD) {
    if (path_uptr != 0)
      return -EINVAL; // path relativo a dfd: no soportado todavía
    int kfd = (int)dfd;
    if (kfd < 0 || kfd >= MAX_PROCESS_FDS || !proc->fds[kfd])
      return -EBADF;
    vfs_node_t *node = proc->fds[kfd]->node;
    if (!node)
      return -EBADF;
    if (node->ops && node->ops->utimes)
      return node->ops->utimes(node, mtime_sec);
    return 0;
  }

  if (path_uptr == 0)
    return -EINVAL;

  char path[VFS_PATH_MAX];
  int rc = resolve_user_path(proc, (const char *)path_uptr, path, sizeof(path));
  if (rc != 0) {
    return rc;
  }
  return vfs_utimes(path, mtime_sec);
}

// ---------- [4.3] mprotect ----------
static int64_t k_mprotect(uint64_t addr, uint64_t length, uint64_t prot,
                          uint64_t a4, uint64_t a5) {
  (void)a4;
  (void)a5;
  process_t *proc = process_current();
  if (!proc)
    return -EFAULT;
  return sys_mprotect(proc, addr, length, prot);
}

// ---------- [4.4] mknod ----------
static int64_t k_mknod(uint64_t path_uptr, uint64_t mode, uint64_t dev,
                       uint64_t a4, uint64_t a5) {
  (void)dev;
  (void)a4;
  (void)a5;
  process_t *proc = process_current();
  if (!proc)
    return -EFAULT;

  int type = (int)(mode & S_IFMT);
  if (type == 0 || type == S_IFREG) {
    char path[VFS_PATH_MAX];
    int rc =
        resolve_user_path(proc, (const char *)path_uptr, path, sizeof(path));
    if (rc != 0)
      return rc;
    // [3.4.c] mknod(PATH, mode) usa `mode & 0777` filtrado por umask.
    uint32_t m = ((uint32_t)mode & 0777) & ~proc->umask;
    if (m == 0)
      m = 0666 & ~proc->umask;
    return vfs_create(path, m);
  }
  return -EPERM;
}

// ---------- [4.5] prlimit64 ----------
struct k_rlimit64 {
  uint64_t rlim_cur;
  uint64_t rlim_max;
};
#define RLIM_INFINITY 0xFFFFFFFFFFFFFFFFULL

// ---------- [4.5] prlimit64 / getrlimit / setrlimit ----------
//
// Linux x86_64: prlimit64(pid, resource, new_limit, old_limit).
//   pid == 0 → el actual.
//   resource >= RLIM_NLIMITS → EINVAL.
//   new_limit: si != NULL, escribir rlim_cur/rlim_max.
//   old_limit: si != NULL, copiar los actuales antes de cambiar.
//
// Reglas:
//   - new.rlim_cur > new.rlim_max → EINVAL.
//   - new.rlim_max > old.rlim_max → EPERM (solo CAP_SYS_RESOURCE).
//     Sin capabilities, todos somos root → permitido.
//   - RLIMIT_NOFILE: aplicar cambio si cur > MAX_PROCESS_FDS? No
//     podemos bajar fds abiertos. Linux permite poner cualquier
//     valor hasta rlim_max. Aquí, si cur > MAX_PROCESS_FDS, hay que
//     rechazar o clamp. Clampeamos a MAX_PROCESS_FDS.
static int64_t k_prlimit64(uint64_t pid, uint64_t resource, uint64_t new_uptr,
                           uint64_t old_uptr, uint64_t a5) {
  (void)a5;
  process_t *self = process_current();
  if (!self)
    return -EFAULT;

  if (resource >= RLIM_NLIMITS)
    return -EINVAL;

  process_t *target = self;
  if (pid != 0) {
    if ((uint32_t)pid != self->pid)
      return -ESRCH;
  }
  (void)target;

  // Copia de los actuales si el caller lo pide.
  if (old_uptr) {
    if (!access_ok((void *)old_uptr, sizeof(struct k_rlimit64)))
      return -EFAULT;
    struct k_rlimit64 o = {
        .rlim_cur = self->rlimits[resource].rlim_cur,
        .rlim_max = self->rlimits[resource].rlim_max,
    };
    if (copy_to_user((void *)old_uptr, &o, sizeof(o)) < 0)
      return -EFAULT;
  }

  // Aplicar nuevo valor.
  if (new_uptr) {
    if (!access_ok((void *)new_uptr, sizeof(struct k_rlimit64)))
      return -EFAULT;
    struct k_rlimit64 nr;
    if (copy_from_user(&nr, (void *)new_uptr, sizeof(nr)) < 0)
      return -EFAULT;
    if (nr.rlim_cur > nr.rlim_max)
      return -EINVAL;
    // NOFILE: clamp a MAX_PROCESS_FDS. Sin capabilities, no podemos
    // rechazar solo por bajar max; pero no podemos permitir cur >
    // MAX_PROCESS_FDS porque rompería vfs_open_for_proc.
    if (resource == RLIMIT_NOFILE) {
      if (nr.rlim_cur > MAX_PROCESS_FDS)
        nr.rlim_cur = MAX_PROCESS_FDS;
      if (nr.rlim_max > MAX_PROCESS_FDS)
        nr.rlim_max = MAX_PROCESS_FDS;
    }
    self->rlimits[resource].rlim_cur = nr.rlim_cur;
    self->rlimits[resource].rlim_max = nr.rlim_max;
  }
  return 0;
}

// Linux: getrlimit(resource, &rlim). rlim es 2x uint64 (16 bytes).
// Deprecado en x86_64 (musl usa prlimit64), pero algunos binarios
// viejos lo llaman directo.
static int64_t k_getrlimit(uint64_t resource, uint64_t rlim_ptr, uint64_t a3,
                           uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;
  return k_prlimit64(0, resource, 0, rlim_ptr, 0);
}

static int64_t k_setrlimit(uint64_t resource, uint64_t rlim_ptr, uint64_t a3,
                           uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;
  return k_prlimit64(0, resource, rlim_ptr, 0, 0);
}

// ---------- [4.6] sethostname / getrusage / times ----------
char g_hostname[64] = "aurora";

static int64_t k_sethostname(uint64_t name_uptr, uint64_t len, uint64_t a3,
                             uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;
  if (len > sizeof(g_hostname) - 1)
    return -EINVAL;
  if (len == 0)
    return -EINVAL;
  if (!access_ok((void *)name_uptr, len))
    return -EFAULT;
  char buf[64];
  if (copy_from_user(buf, (void *)name_uptr, len) < 0)
    return -EFAULT;
  if (memchr(buf, '\0', len))
    return -EINVAL;
  memcpy(g_hostname, buf, len);
  g_hostname[len] = '\0';
  return 0;
}

static int64_t k_getrusage(uint64_t who, uint64_t usage_ptr, uint64_t a3,
                           uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;
  process_t *proc = process_current();
  if (!proc)
    return -EFAULT;
  if (!access_ok((void *)usage_ptr, sizeof(struct k_rusage)))
    return -EFAULT;
  struct k_rusage ru;
  memset(&ru, 0, sizeof(ru));
  if (who == 0 || who == 1) {
    uint64_t ticks = proc->cpu_ticks_user;
    ru.ru_utime.tv_sec = (int64_t)(ticks / 1000);
    ru.ru_utime.tv_usec = (int64_t)((ticks % 1000) * 1000);
  }
  if (copy_to_user((void *)usage_ptr, &ru, sizeof(ru)) < 0)
    return -EFAULT;
  return 0;
}

struct k_tms {
  int64_t tms_utime;
  int64_t tms_stime;
  int64_t tms_cutime;
  int64_t tms_cstime;
};

static int64_t k_times(uint64_t buf_uptr, uint64_t a2, uint64_t a3, uint64_t a4,
                       uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  process_t *proc = process_current();
  if (!proc)
    return -EFAULT;
  if (buf_uptr) {
    if (!access_ok((void *)buf_uptr, sizeof(struct k_tms)))
      return -EFAULT;
    struct k_tms t = {0};
    t.tms_utime = (int64_t)proc->cpu_ticks_user;
    if (copy_to_user((void *)buf_uptr, &t, sizeof(t)) < 0)
      return -EFAULT;
  }
  extern uint64_t sched_get_ticks(void);
  return (int64_t)sched_get_ticks();
}

// ---------- klogctl / syslog (103) ----------
//
// Es la syscall que busybox `dmesg` usa para leer el ring de klog.
// Tipos (Linux <sys/klog.h>):
//   0 CLOSE            no-op
//   1 OPEN             no-op
//   2 READ             destructivo, lee y consume hasta len bytes
//   3 READ_ALL         no destructivo, copia todo el ring (hasta len)
//   4 READ_CLEAR       destructivo
//   5 CLEAR            vacía el ring
//   6 CONSOLE_OFF      no-op
//   7 CONSOLE_ON       no-op
//   8 CONSOLE_LEVEL    no-op
//   9 SIZE_UNREAD      bytes pendientes
//  10 SIZE_BUFFER      capacidad total del ring
#define SYSLOG_ACTION_CLOSE 0
#define SYSLOG_ACTION_OPEN 1
#define SYSLOG_ACTION_READ 2
#define SYSLOG_ACTION_READ_ALL 3
#define SYSLOG_ACTION_READ_CLEAR 4
#define SYSLOG_ACTION_CLEAR 5
#define SYSLOG_ACTION_CONSOLE_OFF 6
#define SYSLOG_ACTION_CONSOLE_ON 7
#define SYSLOG_ACTION_CONSOLE_LEVEL 8
#define SYSLOG_ACTION_SIZE_UNREAD 9
#define SYSLOG_ACTION_SIZE_BUFFER 10

static int64_t k_klogctl(uint64_t type, uint64_t buf, uint64_t len, uint64_t a4,
                         uint64_t a5) {
  (void)a4;
  (void)a5;

  switch ((int)type) {
  case SYSLOG_ACTION_CLOSE:
  case SYSLOG_ACTION_OPEN:
  case SYSLOG_ACTION_CONSOLE_OFF:
  case SYSLOG_ACTION_CONSOLE_ON:
  case SYSLOG_ACTION_CONSOLE_LEVEL:
    return 0;

  case SYSLOG_ACTION_READ:
  case SYSLOG_ACTION_READ_ALL:
  case SYSLOG_ACTION_READ_CLEAR: {
    if (len == 0) {
      return 0;
    }
    if (!buf) {
      return -EINVAL;
    }
    if (!access_ok((void *)buf, (size_t)len)) {
      return -EFAULT;
    }
    size_t n = (size_t)len;
    if (n > 16384)
      n = 16384;
    char *tmp = (char *)kmalloc(n);
    if (!tmp) {
      return -ENOMEM;
    }
    size_t got;
    if ((int)type == SYSLOG_ACTION_READ_ALL)
      got = klog_peek(tmp, n);
    else
      got = klog_read(tmp, n);
    int rc = 0;
    if (got > 0 && copy_to_user((void *)buf, tmp, got) < 0)
      rc = -EFAULT;
    kfree(tmp);
    return rc ? rc : (int64_t)got;
  }

  case SYSLOG_ACTION_CLEAR:
    klog_clear();
    return 0;

  case SYSLOG_ACTION_SIZE_UNREAD:
    return (int64_t)klog_available();

  case SYSLOG_ACTION_SIZE_BUFFER:
    return (int64_t)klog_size();

  default:
    return -EINVAL;
  }
}

// ---------- setdomainname (171) ----------
//
// Cosmético. Guardamos el valor para que uname lo devuelva; nada más
// depende de él. Si len == 0, limpia el dominio.
static int64_t k_setdomainname(uint64_t name_uptr, uint64_t len, uint64_t a3,
                               uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;
  if (len >= sizeof(g_domainname))
    return -EINVAL;
  if (len == 0) {
    g_domainname[0] = '\0';
    return 0;
  }
  if (!access_ok((void *)name_uptr, (size_t)len))
    return -EFAULT;
  if (copy_from_user(g_domainname, (void *)name_uptr, (size_t)len) < 0)
    return -EFAULT;
  g_domainname[len] = '\0';
  return 0;
}

// ---------- waitid (247) ----------
//
// Traducción directa a process_waitpid + relleno de siginfo_t.
// idtype:  0=P_ALL, 1=P_PID, 2=P_PGID
// options: WNOHANG(1) | WSTOPPED(2) | WEXITED(4) | WCONTINUED(8) |
//          WNOWAIT(0x01000000, no soportado → ignorado)
//
// siginfo_t Linux x86_64 (128 bytes):
//   0  si_signo   (int)
//   4  si_errno   (int)
//   8  si_code    (int)
//  12  __pad0
//  16  si_pid     (_sigchld)
//  20  si_uid
//  24  si_status
//  28  (pad)
//  32  si_utime
//  40  si_stime
//  48..127 relleno
#define P_ALL_ 0
#define P_PID_ 1
#define P_PGID_ 2

#define CLD_EXITED 1
#define CLD_KILLED 2
#define CLD_DUMPED 3
#define CLD_TRAPPED 4
#define CLD_STOPPED 5
#define CLD_CONTINUED 6

#define WNOHANG_ 0x00000001
#define WSTOPPED_ 0x00000002
#define WEXITED_ 0x00000004
#define WCONTINUED_ 0x00000008
#define WNOWAIT_ 0x01000000

static int64_t k_waitid(uint64_t idtype, uint64_t id, uint64_t infop,
                        uint64_t options, uint64_t a5) {
  (void)a5;
  process_t *self = process_current();
  if (!self)
    return -EFAULT;
  if (!infop)
    return -EINVAL;

  // Linux x86_64: waitid exige (options & (WEXITED|WSTOPPED|WCONTINUED))
  // != 0, salvo que WNOHANG solo no cuenta. BusyBox pasa WEXITED|WNOWAIT.
  // Somos laxos: si no hay ningún bit de evento, asumimos WEXITED.
  if ((options & (WEXITED_ | WSTOPPED_ | WCONTINUED_)) == 0)
    options |= WEXITED_;

  int32_t pid_filter;
  switch ((int)idtype) {
  case P_ALL_:
    pid_filter = -1;
    break;
  case P_PID_:
    pid_filter = (int32_t)id;
    break;
  case P_PGID_:
    pid_filter = -(int32_t)id;
    break;
  default:
    return -EINVAL;
  }

  int woptions = 0;
  if (options & WNOHANG_)
    woptions |= WNOHANG;
  if (options & WSTOPPED_)
    woptions |= WUNTRACED;
  if (options & WCONTINUED_)
    woptions |= WCONTINUED;
  // WNOWAIT: no soportado. Ignoramos (reapamos igual). Documentado.

  int status = 0;
  proc_rusage_t ru = {0};
  int r = process_waitpid(self, pid_filter, &status, &ru, woptions);
  if (r < 0)
    return r;
  if (r == 0)
    return 0; // WNOHANG sin hijos listos

  // r es el pid del hijo. Decodificar status Linux.
  int si_code, si_status;
  if (status == 0xffff) {
    si_code = CLD_CONTINUED;
    si_status = SIGCONT;
  } else if ((status & 0xff) == 0x7f) {
    si_code = CLD_STOPPED;
    si_status = (status >> 8) & 0xff;
  } else if ((status & 0x7f) == 0) {
    si_code = CLD_EXITED;
    si_status = (status >> 8) & 0xff;
  } else {
    si_code = CLD_KILLED;
    si_status = status & 0x7f;
  }

  // Construir siginfo en buffer local y copiar. Escribir campo a campo
  // en userland directamente se rompe con SMAP.
  uint8_t info[128];
  memset(info, 0, sizeof(info));
  *(int32_t *)(info + 0) = SIGCHLD;
  *(int32_t *)(info + 4) = 0;
  *(int32_t *)(info + 8) = si_code;
  *(int32_t *)(info + 16) = (int32_t)r; // si_pid
  *(int32_t *)(info + 20) = 0;          // si_uid
  *(int32_t *)(info + 24) = si_status;
  *(int64_t *)(info + 32) = (int64_t)ru.utime_ticks; // si_utime (ticks)
  *(int64_t *)(info + 40) = (int64_t)ru.stime_ticks; // si_stime

  if (!access_ok((void *)infop, sizeof(info)))
    return -EFAULT;
  if (copy_to_user((void *)infop, info, sizeof(info)) < 0)
    return -EFAULT;
  return 0;
}

// ---------- flock (73) ----------
//
// Advisory lock por fd. NO es fcntl(F_SETLK): flock es un lock entero
// sobre el inode, no byte-range, y no es POSIX.
//
// Implementación: el modo vive en file_descriptor_t.flock_mode (0/1/2).
// Para detectar conflictos escaneamos los fds de TODOS los procesos
// vía process_for_each() y comparamos (node->fs, node->inode). No hay
// tabla global aparte — el coste del scan es despreciable con <100 fds.
//
// Semántica soportada:
//   LOCK_SH        compartido. Coexiste con otros SH. Bloquea EX.
//   LOCK_EX        exclusivo. Bloquea SH y EX de otros.
//   LOCK_UN        libera.
//   LOCK_NB        falla con EWOULDBLOCK si hay conflicto.
//
// No soportado (documentado):
//   - No hay wake-up dirigido: si un flock sin NB encuentra conflicto,
//     hace polling cada 10 ms. Funciona pero es menos elegante que
//     una wait queue por inode. Aceptable para scripts.
//   - No se limpia al cerrar el fd: un fd cerrado con flock_mode != 0
//     seguirá apareciendo como lockholder. Mitigación: cerrar con
//     flock(fd, LOCK_UN) explícito o dejar morir al proceso. Pendiente
//     un cleanup en vfs_close_for_proc si se necesita.
#define LINUX_LOCK_SH 1
#define LINUX_LOCK_EX 2
#define LINUX_LOCK_NB 4
#define LINUX_LOCK_UN 8

struct flock_scan_ctx {
  vfs_node_t *node;
  int want_mode; // 1=SH, 2=EX
  process_t *self;
  file_descriptor_t *self_fd;
  int conflict;
};

static int flock_scan_cb(process_t *p, void *arg) {
  struct flock_scan_ctx *ctx = (struct flock_scan_ctx *)arg;
  for (int i = 0; i < MAX_PROCESS_FDS; i++) {
    file_descriptor_t *f = p->fds[i];
    if (!f || !f->node || f->flock_mode == 0)
      continue;
    if (p == ctx->self && f == ctx->self_fd)
      continue;
    if (f->node->inode != ctx->node->inode)
      continue;
    if (f->node->fs != ctx->node->fs)
      continue;
    // Mismo inodo, otro fd con lock.
    if (f->flock_mode == 2) { // EX ajeno
      ctx->conflict = 1;
      return 1;
    }
    if (ctx->want_mode == 2 && f->flock_mode == 1) { // quiero EX, hay SH
      ctx->conflict = 1;
      return 1;
    }
  }
  return 0;
}

static int flock_has_conflict(process_t *proc, file_descriptor_t *fd,
                              vfs_node_t *node, int want_mode) {
  struct flock_scan_ctx ctx = {
      .node = node,
      .want_mode = want_mode,
      .self = proc,
      .self_fd = fd,
      .conflict = 0,
  };
  process_for_each(flock_scan_cb, &ctx);
  return ctx.conflict;
}

static int64_t k_flock(uint64_t fd, uint64_t operation, uint64_t a3,
                       uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;
  process_t *proc = process_current();
  if (!proc)
    return -EFAULT;
  int kfd = (int)fd;
  if (kfd < 0 || kfd >= MAX_PROCESS_FDS || !proc->fds[kfd])
    return -EBADF;
  file_descriptor_t *f = proc->fds[kfd];
  if (!f->node)
    return -EBADF;

  int op = (int)operation;
  int want = op & (LINUX_LOCK_SH | LINUX_LOCK_EX);
  int nb = (op & LINUX_LOCK_NB) != 0;

  if (op & LINUX_LOCK_UN) {
    f->flock_mode = 0;
    return 0;
  }
  if (want != LINUX_LOCK_SH && want != LINUX_LOCK_EX)
    return -EINVAL;

  int want_mode = (want == LINUX_LOCK_EX) ? 2 : 1;

  // Soltar el lock previo antes de re-chequear. Si vamos a subir
  // SH→EX, esto libera momentáneamente el SH, pero es la única forma
  // sin mantener un estado de "upgrade pendiente".
  f->flock_mode = 0;

  for (;;) {
    if (!flock_has_conflict(proc, f, f->node, want_mode)) {
      f->flock_mode = want_mode;
      return 0;
    }
    if (nb)
      return -EAGAIN;      // == EWOULDBLOCK
    sched_sleep_ticks(10); // 10 ms, cede CPU
  }
}

// ---------- Familia socket (Fase 5 pendiente) ----------
//
// Aurora no tiene stack de red todavía. Devolvemos -EAFNOSUPPORT
// (el errno que da Linux cuando no hay familia de direcciones soportada)
// en lugar de -ENOSYS: así dnsdomainname, hostname -d, getaddrinfo y
// demás reciben un error informativo y tienen oportunidad de caer a un
// fallback limpio, en vez de "syscall desconocida".
//
// Cuando Fase 5 (networking) llegue, se sustituye esto por la
// implementación real; los números de syscall ya están reservados.
static int64_t k_net_nosupport(uint64_t a1, uint64_t a2, uint64_t a3,
                               uint64_t a4, uint64_t a5) {
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  return -EAFNOSUPPORT;
}

static int64_t k_futex(uint64_t uaddr, uint64_t op, uint64_t val,
                       uint64_t timeout, uint64_t uaddr2) {
  registers_t *regs = syscall_current_regs();
  uint64_t val3 = regs ? regs->r9 : 0;
  return sys_futex(uaddr, op, val, timeout, uaddr2, val3);
}

static int64_t k_sync(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                      uint64_t a5) {
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  // No tenemos write-back cache: FAT32 sincroniza en cada write,
  // tarfs es RO, procfs/sysfs/devfs no escriben a disco. No hay nada
  // que volcar. Devolver 0 es correcto (Linux también lo hace en
  // sistemas sin dirty pages).
  return 0;
}

#define LINUX_REBOOT_MAGIC1 0xfee1dead
#define LINUX_REBOOT_MAGIC2 672274793
#define LINUX_REBOOT_CMD_RESTART 0x01234567
#define LINUX_REBOOT_CMD_HALT 0xcdef0123
#define LINUX_REBOOT_CMD_POWER_OFF 0x4321fedc

static int64_t k_reboot(uint64_t m1, uint64_t m2, uint64_t cmd, uint64_t arg,
                        uint64_t a5) {
  (void)arg;
  (void)a5;
  if (m1 != LINUX_REBOOT_MAGIC1 || m2 != LINUX_REBOOT_MAGIC2)
    return -EINVAL;
  if (cmd == LINUX_REBOOT_CMD_RESTART) {
    outb(0x64, 0xFE); // 8042 reset
    return 0;
  }
  if (cmd == LINUX_REBOOT_CMD_HALT || cmd == LINUX_REBOOT_CMD_POWER_OFF) {
    for (;;)
      __asm__ volatile("cli; hlt");
  }
  return -EINVAL;
}

static int64_t k_swapon(uint64_t path_uptr, uint64_t flags, uint64_t a3,
                        uint64_t a4, uint64_t a5) {
  (void)flags;
  (void)a3;
  (void)a4;
  (void)a5;
  process_t *proc = process_current();
  if (!proc)
    return -EFAULT;

  char path[VFS_PATH_MAX];
  int rc = resolve_user_path(proc, (const char *)path_uptr, path, sizeof(path));
  if (rc != 0)
    return rc;

  return swap_on(path);
}

static int64_t k_swapoff(uint64_t path_uptr, uint64_t a2, uint64_t a3,
                         uint64_t a4, uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  process_t *proc = process_current();
  if (!proc)
    return -EFAULT;

  char path[VFS_PATH_MAX];
  int rc = resolve_user_path(proc, (const char *)path_uptr, path, sizeof(path));
  if (rc != 0)
    return rc;

  return swap_off(path);
}

// ---------- getcpu (309) ----------
static int64_t k_getcpu(uint64_t cpu_ptr, uint64_t node_ptr, uint64_t a3,
                        uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;
  int cpu = smp_processor_id();
  if (cpu_ptr && access_ok((void *)cpu_ptr, sizeof(uint32_t))) {
    if (copy_to_user((void *)cpu_ptr, &cpu, sizeof(uint32_t)) < 0)
      return -EFAULT;
  }
  if (node_ptr) {
    uint32_t node = 0;
    if (access_ok((void *)node_ptr, sizeof(uint32_t)))
      copy_to_user((void *)node_ptr, &node, sizeof(uint32_t));
  }
  return 0;
}

// ---------- membarrier (324) ----------
// Solo soportamos QUERY (0) y PRIVATE_EXPEDITED (1). El resto EINVAL.
static int64_t k_membarrier(uint64_t cmd, uint64_t flags, uint64_t a3,
                            uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;
  if (flags != 0)
    return -EINVAL;
  switch (cmd) {
  case 0:       // MEMBARRIER_CMD_QUERY
    return 0x1; // soportamos PRIVATE_EXPEDITED
  case 1:       // MEMBARRIER_CMD_PRIVATE_EXPEDITED
    __asm__ volatile("mfence" ::: "memory");
    return 0;
  default:
    return -EINVAL;
  }
}

// ---------- sched_getaffinity (204) / setaffinity (203) ----------
static int64_t k_sched_getaffinity(uint64_t pid, uint64_t cpusetsize,
                                   uint64_t mask_ptr, uint64_t a4,
                                   uint64_t a5) {
  (void)pid;
  (void)a4;
  (void)a5;
  if (cpusetsize < 8)
    return -EINVAL;
  if (!access_ok((void *)mask_ptr, 8))
    return -EFAULT;
  uint64_t mask = 0;
  for (int i = 0; i < MAX_CPUS; i++)
    mask |= (1ULL << i);
  if (copy_to_user((void *)mask_ptr, &mask, 8) < 0)
    return -EFAULT;
  return 8;
}

static int64_t k_sched_setaffinity(uint64_t pid, uint64_t cpusetsize,
                                   uint64_t mask_ptr, uint64_t a4,
                                   uint64_t a5) {
  (void)pid;
  (void)cpusetsize;
  (void)mask_ptr;
  (void)a4;
  (void)a5;
  // No-op: aceptamos cualquier máscara. El scheduler ignora afinidad.
  return 0;
}

// ---------- sched_getparam (143) / getscheduler (145) ----------
struct k_sched_param {
  int32_t sched_priority;
};
static int64_t k_sched_getparam(uint64_t pid, uint64_t param_ptr, uint64_t a3,
                                uint64_t a4, uint64_t a5) {
  (void)pid;
  (void)a3;
  (void)a4;
  (void)a5;
  struct k_sched_param p = {0};
  if (!access_ok((void *)param_ptr, sizeof(p)))
    return -EFAULT;
  if (copy_to_user((void *)param_ptr, &p, sizeof(p)) < 0)
    return -EFAULT;
  return 0;
}
static int64_t k_sched_getscheduler(uint64_t pid, uint64_t a2, uint64_t a3,
                                    uint64_t a4, uint64_t a5) {
  (void)pid;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  return 0; // SCHED_OTHER
}

// ---------- getpriority (140) / setpriority (141) ----------
static int64_t k_getpriority(uint64_t which, uint64_t who, uint64_t a3,
                             uint64_t a4, uint64_t a5) {
  (void)which;
  (void)who;
  (void)a3;
  (void)a4;
  (void)a5;
  return 20; // nice(0) == priority 20 (Linux)
}
static int64_t k_setpriority(uint64_t which, uint64_t who, uint64_t prio,
                             uint64_t a4, uint64_t a5) {
  (void)which;
  (void)who;
  (void)prio;
  (void)a4;
  (void)a5;
  return 0;
}

// ---------- clock_nanosleep (230) ----------
static int64_t k_clock_nanosleep(uint64_t clockid, uint64_t flags,
                                 uint64_t req_ptr, uint64_t rem_ptr,
                                 uint64_t a5) {
  (void)a5;
  if (clockid != 0 && clockid != 1)
    return -EINVAL;
  if (flags & ~1ULL) // TIMER_ABSTIME=1
    return -EINVAL;
  return k_nanosleep(req_ptr, rem_ptr, 0, 0, 0);
}

static int64_t k_fadvise64(uint64_t fd, uint64_t offset, uint64_t len,
                           uint64_t advice, uint64_t a5) {
  (void)fd;
  (void)offset;
  (void)len;
  (void)advice;
  (void)a5;
  return 0; // no-op: somos un FS en RAM
}

// ---------- xattr family ----------
// Linux's xattr API. Aurora no tiene xattrs en ningún FS. Devolvemos
// -EOPNOTSUPP (95) para que glibc/coreutils lo interpreten como
// "no soportado por el FS" y sigan sin quejarse. Si devolviéramos
// -ENOSYS, ls -l imprimiría "Function not implemented".
static int64_t k_xattr_notsup(uint64_t a, uint64_t b, uint64_t c, uint64_t d,
                              uint64_t e) {
  (void)a;
  (void)b;
  (void)c;
  (void)d;
  (void)e;
  return -EOPNOTSUPP;
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

// [2.4] Scan activo de un FS montado. Por ahora solo FAT32 lo
// implementa. Resultado en struct fat32_check_result (definido en
// fat32.h; el binario aurora-fsck replica el mismo layout).
static int64_t a_fs_check(uint64_t path_ptr, uint64_t result_ptr, uint64_t a3,
                          uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;
  process_t *proc = process_current();
  if (!proc)
    return -EFAULT;
  if (!result_ptr)
    return -EINVAL;

  char path[VFS_PATH_MAX];
  int rc = resolve_user_path(proc, (const char *)path_ptr, path, sizeof(path));
  if (rc != 0)
    return rc;

  struct vfs_fs_ops *ops = vfs_get_mount_ops(path);
  void *priv = vfs_get_mount_priv(path);
  if (!ops || !priv)
    return -ENOENT;
  if (!ops->name || strcmp(ops->name, "fat32") != 0)
    return -ENOTSUP;

  struct fat32_check_result res;
  rc = fat32_check(priv, &res);
  if (rc != 0)
    return rc;

  if (copy_to_user((void *)result_ptr, &res, sizeof(res)) < 0)
    return -EFAULT;
  return 0;
}

// ===========================================================================
// Tablas y dispatcher
// ===========================================================================
static const syscall_entry_t linux_table[] = {
    [SYS_READ] = {k_read, "read"},
    [SYS_WRITE] = {k_write, "write"},
    [SYS_PREAD64] = {k_pread64, "pread64"},
    [SYS_PWRITE64] = {k_pwrite64, "pwrite64"},
    [SYS_OPEN] = {k_open, "open"},
    [SYS_CLOSE] = {k_close, "close"},
    [SYS_STAT] = {k_stat, "stat"},
    [SYS_FSTAT] = {k_fstat, "fstat"},
    [SYS_FCHMODAT] = {k_fchmodat, "fchmodat"},
    [SYS_LSTAT] = {k_lstat, "lstat"},
    [SYS_LSEEK] = {k_lseek, "lseek"},
    [SYS_MMAP] = {k_mmap, "mmap"},
    [SYS_MREMAP] = {k_mremap, "mremap"},
    [SYS_MPROTECT] = {k_mprotect, "mprotect"},
    [SYS_MADVISE] = {k_madvise, "madvise"},
    [SYS_FSTATFS] = {k_fstatfs, "fstatfs"},
    [SYS_STATFS] = {k_statfs, "statfs"},
    [SYS_FSYNC] = {k_fsync, "fsync"},
    [SYS_CHMOD] = {k_chmod, "chmod"},
    [SYS_FCHMOD] = {k_fchmod, "fchmod"},
    [SYS_CHOWN] = {k_chown, "chown"},
    [SYS_FCHOWN] = {k_fchown, "fchown"},
    [SYS_LCHOWN] = {k_lchown, "lchown"},
    [SYS_GETRUSAGE] = {k_getrusage, "getrusage"},
    [SYS_TIMES] = {k_times, "times"},
    [SYS_MKNOD] = {k_mknod, "mknod"},
    [SYS_SETHOSTNAME] = {k_sethostname, "sethostname"},
    [SYS_UTIMENSAT] = {k_utimensat, "utimensat"},
    [SYS_SOCKET] = {k_net_nosupport, "socket"},
    [SYS_CONNECT] = {k_net_nosupport, "connect"},
    [SYS_ACCEPT] = {k_net_nosupport, "accept"},
    [SYS_SENDTO] = {k_net_nosupport, "sendto"},
    [SYS_RECVFROM] = {k_net_nosupport, "recvfrom"},
    [SYS_SENDMSG] = {k_net_nosupport, "sendmsg"},
    [SYS_RECVMSG] = {k_net_nosupport, "recvmsg"},
    [SYS_SHUTDOWN] = {k_net_nosupport, "shutdown"},
    [SYS_BIND] = {k_net_nosupport, "bind"},
    [SYS_LISTEN] = {k_net_nosupport, "listen"},
    [SYS_GETSOCKNAME] = {k_net_nosupport, "getsockname"},
    [SYS_GETPEERNAME] = {k_net_nosupport, "getpeername"},
    [SYS_SOCKETPAIR] = {k_net_nosupport, "socketpair"},
    [SYS_SETSOCKOPT] = {k_net_nosupport, "setsockopt"},
    [SYS_GETSOCKOPT] = {k_net_nosupport, "getsockopt"},
    [SYS_ACCEPT4] = {k_net_nosupport, "accept4"},
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
    [SYS_DUP] = {k_dup, "dup"},
    [SYS_DUP3] = {k_dup3, "dup3"},
    [SYS_GETPID] = {k_getpid, "getpid"},
    [SYS_SENDFILE] = {k_sendfile, "sendfile"},
    [SYS_SYSINFO] = {k_sysinfo, "sysinfo"},
    [SYS_GETUID] = {k_getuid, "getuid"},
    [SYS_GETGID] = {k_getgid, "getgid"},
    [SYS_GETEUID] = {k_geteuid, "geteuid"},
    [SYS_GETEGID] = {k_getegid, "getegid"},
    [SYS_GETPPID] = {k_getppid, "getppid"},
    [SYS_SETPGID] = {k_setpgid, "setpgid"},
    [SYS_GETPGID] = {k_getpgid, "getpgid"},
    [SYS_GETPGRP] = {k_getpgrp, "getpgrp"},
    [SYS_GETSID] = {k_getsid, "getsid"},
    [SYS_SETSID] = {k_setsid, "setsid"},
    [SYS_UMASK] = {k_umask, "umask"},
    [SYS_SETUID] = {k_setuid, "setuid"},
    [SYS_SETGID] = {k_setgid, "setgid"},
    [SYS_SETREUID] = {k_setreuid, "setreuid"},
    [SYS_SETREGID] = {k_setregid, "setregid"},
    [SYS_GETGROUPS] = {k_getgroups, "getgroups"},
    [SYS_SETGROUPS] = {k_setgroups, "setgroups"},
    [SYS_SETRESUID] = {k_setresuid, "setresuid"},
    [SYS_GETRESUID] = {k_getresuid, "getresuid"},
    [SYS_SETRESGID] = {k_setresgid, "setresgid"},
    [SYS_GETRESGID] = {k_getresgid, "getresgid"},
    [SYS_SETFSUID] = {k_setfsuid, "setfsuid"},
    [SYS_SETFSGID] = {k_setfsgid, "setfsgid"},
    [SYS_PRCTL] = {k_prctl, "prctl"},
    [SYS_KILL] = {k_kill, "kill"},
    [SYS_UNAME] = {k_uname, "uname"},
    [SYS_GETCWD] = {k_getcwd, "getcwd"},
    [SYS_CHDIR] = {k_chdir, "chdir"},
    [SYS_FCHDIR] = {k_fchdir, "fchdir"},
    [SYS_LINK] = {k_link, "link"},
    [SYS_LINKAT] = {k_linkat, "linkat"},
    [SYS_SYMLINK] = {k_symlink, "symlink"},
    [SYS_SYMLINKAT] = {k_symlinkat, "symlinkat"},
    [SYS_READLINK] = {k_readlink, "readlink"},
    [SYS_TKILL] = {k_tkill, "tkill"},
    [SYS_READLINKAT] = {k_readlinkat, "readlinkat"},
    [SYS_FACCESSAT] = {k_faccessat, "faccessat"},
    [SYS_PIPE2] = {k_pipe2, "pipe2"},
    [SYS_ARCH_PRCTL] = {k_arch_prctl, "arch_prctl"},
    [SYS_GETDENTS64] = {k_getdents64, "getdents64"},
    [SYS_SET_TID_ADDRESS] = {k_set_tid_address, "set_tid_address"},
    [SYS_CLOCK_GETTIME] = {k_clock_gettime, "clock_gettime"},
    [SYS_GETTIMEOFDAY] = {k_gettimeofday, "gettimeofday"},
    [SYS_TIME] = {k_time, "time"},
    [SYS_CLOCK_SETTIME] = {k_clock_settime, "clock_settime"},
    [SYS_SETTIMEOFDAY] = {k_settimeofday, "settimeofday"},
    [SYS_ADJTIMEX] = {k_adjtimex, "adjtimex"},
    [SYS_EXIT] = {k_exit, "exit"},
    [SYS_EXIT_GROUP] = {k_exit_group, "exit_group"},
    [SYS_OPENAT] = {k_openat, "openat"},
    [SYS_MKDIRAT] = {k_mkdirat, "mkdirat"},
    [SYS_FSTATAT] = {k_fstatat, "fstatat"},
    [SYS_STATX] = {k_statx, "statx"}, // 332
    [SYS_UNLINKAT] = {k_unlinkat, "unlinkat"},
    [SYS_RENAMEAT] = {k_renameat, "renameat"},
    [SYS_SET_ROBUST_LIST] = {k_set_robust_list, "set_robust_list"},
    [SYS_PRLIMIT64] = {k_prlimit64, "prlimit64"},
    [SYS_GETRLIMIT] = {k_getrlimit, "getrlimit"},
    [SYS_SETRLIMIT] = {k_setrlimit, "setrlimit"},
    [SYS_GETRANDOM] = {k_getrandom, "getrandom"},
    [SYS_RSEQ] = {k_rseq, "rseq"},
    [SYS_CLONE] = {k_clone, "clone"},
    [SYS_FORK] = {k_fork, "fork"},
    [SYS_VFORK] = {k_vfork, "vfork"},
    [SYS_PIVOT_ROOT] = {k_pivot_root, "pivot_root"},
    [SYS_EXECVE] = {k_execve, "execve"},
    [SYS_GETTID] = {k_gettid, "gettid"},
    [SYS_WAIT4] = {k_wait4, "wait4"},
    [SYS_FLOCK] = {k_flock, "flock"},
    [SYS_KLOGCTL] = {k_klogctl, "klogctl"},
    [SYS_SETDOMAINNAME] = {k_setdomainname, "setdomainname"},
    [SYS_WAITID] = {k_waitid, "waitid"},
    [SYS_FUTEX] = {k_futex, "futex"},
    [SYS_SYNC] = {k_sync, "sync"},
    [SYS_REBOOT] = {k_reboot, "reboot"},
    [SYS_SWAPON] = {k_swapon, "swapon"},
    [SYS_SWAPOFF] = {k_swapoff, "swapoff"},
    [SYS_GETCPU] = {k_getcpu, "getcpu"},                                  // 309
    [SYS_MEMBARRIER] = {k_membarrier, "membarrier"},                      // 324
    [SYS_SCHED_GETAFFINITY] = {k_sched_getaffinity, "sched_getaffinity"}, // 204
    [SYS_SCHED_SETAFFINITY] = {k_sched_setaffinity, "sched_setaffinity"}, // 203
    [SYS_SCHED_GETPARAM] = {k_sched_getparam, "sched_getparam"},          // 143
    [SYS_SCHED_GETSCHEDULER] = {k_sched_getscheduler,
                                "sched_getscheduler"},              // 145
    [SYS_GETPRIORITY] = {k_getpriority, "getpriority"},             // 140
    [SYS_SETPRIORITY] = {k_setpriority, "setpriority"},             // 141
    [SYS_CLOCK_NANOSLEEP] = {k_clock_nanosleep, "clock_nanosleep"}, // 230
    [SYS_FADVISE64] = {k_fadvise64, "fadvise64"},                   // 291
    [SYS_TGKILL] = {k_tgkill, "tgkill"},                            // 234
    [SYS_SETXATTR] = {k_xattr_notsup, "setxattr"},
    [SYS_LSETXATTR] = {k_xattr_notsup, "lsetxattr"},
    [SYS_FSETXATTR] = {k_xattr_notsup, "fsetxattr"},
    [SYS_GETXATTR] = {k_xattr_notsup, "getxattr"},
    [SYS_LGETXATTR] = {k_xattr_notsup, "lgetxattr"},
    [SYS_FGETXATTR] = {k_xattr_notsup, "fgetxattr"},
    [SYS_LISTXATTR] = {k_xattr_notsup, "listxattr"},
    [SYS_LLISTXATTR] = {k_xattr_notsup, "llistxattr"},
    [SYS_FLISTXATTR] = {k_xattr_notsup, "flistxattr"},
    [SYS_REMOVEXATTR] = {k_xattr_notsup, "removexattr"},
    [SYS_LREMOVEXATTR] = {k_xattr_notsup, "lremovexattr"},
    [SYS_FREMOVXATTR] = {k_xattr_notsup, "fremovexattr"},
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
    [ASYS_FS_CHECK - ASYS_BASE] = {a_fs_check, "asys_fs_check"},
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

  task_t *cur = sched_current();

  syscall_fn_t fn = NULL;
  if (num < (uint64_t)LINUX_TABLE_N) {
    fn = linux_table[num].fn;
  } else if (num >= ASYS_BASE && num < ASYS_MAX) {
    int idx = (int)(num - ASYS_BASE);
    if (idx < AURORA_TABLE_N)
      fn = aurora_table[idx].fn;
  }

  if (!fn) {
    // [DEBUG] Loguear solo en el primer hit por syscall.
    static uint8_t seen[512] = {0};
    if (num < 512 && !seen[num]) {
      seen[num] = 1;
      LOG_ERR("[SYSCALL-GAP] num=%lu (0x%lx) pid=%u comm=%s",
              (unsigned long)num, (unsigned long)num,
              process_current() ? process_current()->pid : 0,
              process_current() ? process_current()->name : "?");
    }
    return err(ENOSYS);
  }

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

  // [DIAG PIPE] Log temporal de pipe/pipe2 + cualquier retorno negativo.
  // Quitar cuando se cierre el bug.
  if ((num == SYS_PIPE || num == SYS_PIPE2) ||
      ((int64_t)ret < 0 && (int64_t)ret > -4096)) {
    if (num == SYS_PIPE || num == SYS_PIPE2) {
      LOG_TRACE("[SYSCALL-PIPE] num=%lu a1=%p a2=%lx ret=%ld",
                (unsigned long)num, (void *)arg1, (unsigned long)arg2,
                (long)ret);
    } else {
      LOG_INFO("[SYSCALL-ERR] num=%lu ret=%ld", (unsigned long)num, (long)ret);
    }
  }

  // Devolvemos regs->rax (que pudo cambiar si k_rt_sigreturn restauró
  // un rax distinto). El asm lo escribirá en [frame+0x70].
  return regs->rax;
}