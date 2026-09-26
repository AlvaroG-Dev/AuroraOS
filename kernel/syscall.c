// kernel/syscall.c
#include "syscall.h"
#include "cpu.h"
#include "gdt.h"
#include "gfx/winsrv.h"
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

// [Fase 3.2] Límites de argv en SYS_SPAWN_ARGS. Deben coincidir con
// PROCESS_ARGV_MAX de process.h.
#define SPAWN_ARGS_MAX PROCESS_ARGV_MAX
#define SPAWN_ARG_STR_MAX 128

extern void syscall_entry(void);

// ---------------------------------------------------------------------------
// Registro de servicios del kernel.
//
// Un servicio es una tarea que ofrece funcionalidad por nombre. Las apps
// de userland descubren su Task ID con SYS_GET_SERVICE_ID y luego se
// comunican con él por IPC.
//
// Protegido por g_services_lock porque varios CPUs pueden registrar o
// consultar servicios concurrentemente.
// ---------------------------------------------------------------------------
typedef struct {
  char name[32];
  uint32_t task_id; // 0 = slot libre
} kernel_service_t;

// Estructura que userland usa para readdir. Debe coincidir con
// `struct vfs_dirent` de kernel/vfs.h.
typedef struct {
  char name[128];
  uint32_t type;
  uint32_t _pad;
  uint64_t size;
} user_dirent_t;

static kernel_service_t g_services[KERNEL_SERVICES_MAX];
static spinlock_t g_services_lock;

void syscall_register_service(const char *name, uint32_t task_id) {
  if (!name || !name[0] || task_id == 0) {
    LOG_WARN("[SYS] register_service: parámetros inválidos");
    return;
  }

  unsigned long flags = spin_lock_irqsave(&g_services_lock);

  // ¿Ya existe? Actualizar.
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

  // Slot libre.
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

// Busca un servicio por nombre. Devuelve el Task ID o 0 si no existe.
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
  // Inicializar el registro de servicios.
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

// ===========================================================================
// Implementación de cada syscall
// ===========================================================================

// ---------------------------------------------------------------------------
// [cwd] Copia `uptr` desde userland y lo resuelve contra el cwd del
// proceso actual. Devuelve 0 si OK, negativo si falla. `out` debe tener
// al menos VFS_PATH_MAX bytes.
// ---------------------------------------------------------------------------
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

static int64_t sys_print_k(uint64_t arg1, uint64_t arg2, uint64_t arg3,
                           uint64_t arg4, uint64_t arg5) {
  (void)arg2;
  (void)arg3;
  (void)arg4;
  (void)arg5;
  char buffer[256];
  long n = strncpy_from_user(buffer, (const char *)arg1, sizeof(buffer));
  if (n < 0)
    return -EFAULT;
  serial_puts(buffer);
  return 0;
}

static int64_t sys_yield_k(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                           uint64_t a5) {
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  sched_yield();
  return 0;
}

static int64_t sys_exit_k(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                          uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  process_exit_current((int)a1);
  // no retorna
}

static int64_t sys_sbrk_k(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                          uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  process_t *proc = process_current();
  if (!proc)
    return -EFAULT;
  return (int64_t)process_sbrk(proc, (int64_t)a1);
}

static int64_t sys_open_k(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                          uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;
  process_t *proc = process_current();
  if (!proc)
    return -EFAULT;
  char path[VFS_PATH_MAX];
  int rc = resolve_user_path(proc, (const char *)a1, path, sizeof(path));

  // [DEBUG] Traza para diagnosticar paths relativos. Cambia a LOG_INFO
  // temporalmente si necesitas verlo, luego vuelve a LOG_TRACE.
  {
    char raw_dbg[VFS_PATH_MAX];
    long n_dbg = strncpy_from_user(raw_dbg, (const char *)a1, sizeof(raw_dbg));
    if (n_dbg < 0)
      raw_dbg[0] = '?', raw_dbg[1] = '\0';
    LOG_TRACE("[OPEN] raw='%s' cwd='%s' resolved='%s' rc=%d", raw_dbg,
              proc->cwd, rc == 0 ? path : "(err)", rc);
  }

  if (rc != 0)
    return rc;
  return vfs_open_for_proc(proc, path, (int)a2);
}

static int64_t sys_close_k(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                           uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  process_t *proc = process_current();
  if (!proc)
    return -EFAULT;
  return vfs_close_for_proc(proc, (int)a1);
}

static int64_t sys_read_k(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                          uint64_t a5) {
  (void)a4;
  (void)a5;
  process_t *proc = process_current();
  if (!proc)
    return -EFAULT;
  if (!access_ok((void *)a2, (size_t)a3))
    return -EFAULT;
  return vfs_read_for_proc(proc, (int)a1, (void *)a2, (size_t)a3);
}

static int64_t sys_write_k(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                           uint64_t a5) {
  (void)a4;
  (void)a5;
  process_t *proc = process_current();
  if (!proc)
    return -EFAULT;
  if (!access_ok((void *)a2, (size_t)a3))
    return -EFAULT;
  return vfs_write_for_proc(proc, (int)a1, (const void *)a2, (size_t)a3);
}

static int64_t sys_seek_k(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                          uint64_t a5) {
  (void)a4;
  (void)a5;
  process_t *proc = process_current();
  if (!proc)
    return -EFAULT;
  return vfs_seek_for_proc(proc, (int)a1, (int64_t)a2, (int)a3);
}

static int64_t sys_fstat_k(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                           uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;
  process_t *proc = process_current();
  if (!proc)
    return -EFAULT;
  if (!access_ok((void *)a2, sizeof(vfs_stat_t)))
    return -EFAULT;
  vfs_stat_t kst;
  int rc = vfs_fstat_for_proc(proc, (int)a1, &kst);
  if (rc != 0)
    return rc;
  if (copy_to_user((void *)a2, &kst, sizeof(kst)) < 0)
    return -EFAULT;
  return 0;
}

static int64_t sys_ipc_send_k(uint64_t a1, uint64_t a2, uint64_t a3,
                              uint64_t a4, uint64_t a5) {
  (void)a5;

  // Copiar el payload a un buffer de kernel ANTES de llamar a ipc_send.
  // ipc_send solo acepta punteros de kernel; copy_from_user tiene
  // fixups para el caso de punteros inválidos de userland.
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

static int64_t sys_ipc_recv_k(uint64_t a1, uint64_t a2, uint64_t a3,
                              uint64_t a4, uint64_t a5) {
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

static int64_t sys_get_task_id_k(uint64_t a1, uint64_t a2, uint64_t a3,
                                 uint64_t a4, uint64_t a5) {
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  task_t *cur = sched_current();
  return cur ? (int64_t)cur->id : -EFAULT;
}

static int64_t sys_spawn_k(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                           uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  process_t *proc = process_current();
  if (!proc)
    return -EFAULT;
  char path[VFS_PATH_MAX];
  int rc = resolve_user_path(proc, (const char *)a1, path, sizeof(path));
  if (rc != 0)
    return rc;
  process_t *child = process_spawn_child(proc, path);
  return child ? (int64_t)child->pid : -ENOENT;
}

static int64_t sys_waitpid_k(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                             uint64_t a5) {
  (void)a4;
  (void)a5;
  process_t *proc = process_current();
  if (!proc)
    return -EFAULT;
  int32_t kstatus = 0;
  int res = process_waitpid(proc, (int32_t)a1, &kstatus, (int)a3);
  if (res < 0)
    return res;
  if (a2 != 0) {
    if (!access_ok((void *)a2, sizeof(int32_t)))
      return -EFAULT;
    if (put_user_u32((uint32_t *)a2, (uint32_t)kstatus) < 0)
      return -EFAULT;
  }
  return res;
}

static int64_t sys_getpid_k(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                            uint64_t a5) {
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  process_t *proc = process_current();
  return proc ? (int64_t)proc->pid : -EFAULT;
}

static int64_t sys_mmap_k(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                          uint64_t a5) {
  process_t *proc = process_current();
  if (!proc)
    return -EFAULT;
  uint64_t flags = a4 & 0xFFFFFFFFULL;
  int fd = (int)(a4 >> 32);
  return sys_mmap(proc, a1, a2, a3, flags, fd, a5);
}

static int64_t sys_vm_debug_info_k(uint64_t a1, uint64_t a2, uint64_t a3,
                                   uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;

  process_t *proc = process_current();
  if (!proc || proc->pml4_phys == 0)
    return -EFAULT;

  if (a1 >= 0x0000800000000000ULL ||
      !access_ok((const void *)a2, sizeof(vm_debug_info_t)))
    return -EFAULT;

  uint64_t phys =
      paging_get_phys_in((uint64_t *)phys_to_virt(proc->pml4_phys), a1);
  if (phys == 0)
    return -EFAULT;

  vm_debug_info_t info = {
      .cr3_phys = proc->pml4_phys,
      .virt = a1,
      .phys = phys,
  };

  LOG_INFO("[VMTEST] PID=%u CR3=%p VA=%p -> PA=%p", proc->pid,
           (void *)info.cr3_phys, (void *)info.virt, (void *)info.phys);

  if (copy_to_user((void *)a2, &info, sizeof(info)) < 0)
    return -EFAULT;

  return 0;
}

static int64_t sys_munmap_k(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                            uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;
  process_t *proc = process_current();
  if (!proc)
    return -EFAULT;
  return sys_munmap(proc, a1, a2);
}

static int64_t sys_win_create_k(uint64_t a1, uint64_t a2, uint64_t a3,
                                uint64_t a4, uint64_t a5) {
  process_t *proc = process_current();
  if (!proc || !proc->task)
    return -EFAULT;
  return winsrv_create_window(proc->task, (int)a1, (int)a2, (int)a3, (int)a4,
                              (const char *)a5);
}

static int64_t sys_win_destroy_k(uint64_t a1, uint64_t a2, uint64_t a3,
                                 uint64_t a4, uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  process_t *proc = process_current();
  if (!proc || !proc->task)
    return -EFAULT;
  return winsrv_destroy_window(proc->task, (int)a1);
}

static int64_t sys_win_blit_k(uint64_t a1, uint64_t a2, uint64_t a3,
                              uint64_t a4, uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  process_t *proc = process_current();
  if (!proc || !proc->task)
    return -EFAULT;
  if (!access_ok((const void *)a1, sizeof(winsrv_blit_args_t)))
    return -EFAULT;

  winsrv_blit_args_t args;
  if (copy_from_user(&args, (const void *)a1, sizeof(args)) < 0)
    return -EFAULT;

  // Validar parámetros básicos.
  if (args.w <= 0 || args.h <= 0 || args.src_stride <= 0)
    return -EINVAL;
  if (args.src_x < 0 || args.src_y < 0)
    return -EINVAL;
  if (args.src_x + args.w > args.src_stride)
    return -EINVAL;

  // Calcular el tamaño del buffer fuente en bytes y validar overflow.
  //
  // El último píxel está en (src_y + h - 1) * src_stride + src_x + w - 1.
  // El tamaño total en bytes es ese offset + 1, multiplicado por 4.
  //
  // Para evitar overflow, comprobamos cada multiplicación con
  // __builtin_mul_overflow, que devuelve 1 si hay overflow.
  uint64_t rows = (uint64_t)args.src_y + (uint64_t)args.h; // src_y + h
  if (rows == 0) // underflow imposible: src_y >= 0, h > 0
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

  // last_pixel_offset es el offset en píxeles del último píxel + 1.
  // El tamaño en bytes es last_pixel_offset * sizeof(uint32_t).
  uint64_t size_bytes;
  if (__builtin_mul_overflow(last_pixel_offset, sizeof(uint32_t), &size_bytes))
    return -EINVAL;

  // access_ok valida que el rango [pixels, pixels + size_bytes) está
  // dentro del espacio de usuario. Si no, devuelve -EFAULT.
  if (!access_ok(args.pixels, (size_t)size_bytes))
    return -EFAULT;

  int rc = winsrv_blit(proc->task, args.win_id, args.x, args.y, args.w, args.h,
                       args.src_x, args.src_y, args.src_stride, args.pixels);
  return (uint64_t)rc;
}

static int64_t sys_win_poll_event_k(uint64_t a1, uint64_t a2, uint64_t a3,
                                    uint64_t a4, uint64_t a5) {
  (void)a4;
  (void)a5;
  process_t *proc = process_current();
  if (!proc || !proc->task)
    return -EFAULT;
  if (!access_ok((void *)a2, sizeof(winsrv_event_t)))
    return -EFAULT;
  winsrv_event_t ev;
  int rc = winsrv_poll_event(proc->task, (int)a1, &ev, (int)a3);
  if (rc <= 0)
    return rc;
  if (copy_to_user((void *)a2, &ev, sizeof(ev)) < 0)
    return -EFAULT;
  return 1;
}

static int64_t sys_win_register_console_k(uint64_t a1, uint64_t a2, uint64_t a3,
                                          uint64_t a4, uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  process_t *proc = process_current();
  if (!proc || !proc->task)
    return -EFAULT;
  return winsrv_register_console(proc->task, (int)a1);
}

// ---------------------------------------------------------------------------
// SYS_GET_SERVICE_ID
//
// Args:
//   a1 = puntero (userland) a una string NUL-terminada con el nombre del
//        servicio (p. ej. "echo").
//
// Retorna:
//   > 0  Task ID del servicio
//   -ENOENT si no existe
//   -EINVAL si el nombre es inválido
//   -EFAULT si el puntero de userland falla
// ---------------------------------------------------------------------------
static int64_t sys_get_service_id_k(uint64_t a1, uint64_t a2, uint64_t a3,
                                    uint64_t a4, uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  if (a1 == 0)
    return -EINVAL;

  char name[32];
  long n = strncpy_from_user(name, (const char *)a1, sizeof(name));
  if (n < 0)
    return -EFAULT;
  if (n == 0)
    return -EINVAL;

  uint32_t id = syscall_lookup_service(name);
  return id ? (int64_t)id : -ENOENT;
}

static int64_t sys_readdir_k(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                             uint64_t a5) {
  (void)a4;
  (void)a5;
  if (!a1 || !a3)
    return -EINVAL;
  process_t *proc = process_current();
  if (!proc)
    return -EFAULT;
  char path[VFS_PATH_MAX];
  int prc = resolve_user_path(proc, (const char *)a1, path, sizeof(path));
  if (prc != 0)
    return prc;

  if (!access_ok((void *)a3, sizeof(user_dirent_t)))
    return -EFAULT;

  vfs_dirent_t kout;
  int rc = vfs_readdir(path, a2, &kout);
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

  if (copy_to_user((void *)a3, &uout, sizeof(uout)) < 0)
    return -EFAULT;

  // Devolver 1 si hay entry, 0 si es el final.
  return kout.name[0] ? 1 : 0;
}

static int64_t sys_mkdir_k(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                           uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  process_t *proc = process_current();
  if (!proc)
    return -EFAULT;
  char path[VFS_PATH_MAX];
  int rc = resolve_user_path(proc, (const char *)a1, path, sizeof(path));
  if (rc != 0)
    return rc;
  return vfs_mkdir(path);
}

static int64_t sys_create_k(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                            uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  process_t *proc = process_current();
  if (!proc)
    return -EFAULT;
  char path[VFS_PATH_MAX];
  int rc = resolve_user_path(proc, (const char *)a1, path, sizeof(path));
  if (rc != 0)
    return rc;
  return vfs_create(path, 0);
}

static int64_t sys_unlink_k(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                            uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  process_t *proc = process_current();
  if (!proc)
    return -EFAULT;
  char path[VFS_PATH_MAX];
  int rc = resolve_user_path(proc, (const char *)a1, path, sizeof(path));
  if (rc != 0)
    return rc;
  return vfs_unlink(path);
}

static int64_t sys_win_set_icon_k(uint64_t a1, uint64_t a2, uint64_t a3,
                                  uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;
  process_t *proc = process_current();
  if (!proc || !proc->task)
    return -EFAULT;
  if (a2 == 0)
    return -EINVAL;
  char path[VFS_PATH_MAX];
  int rc = resolve_user_path(proc, (const char *)a2, path, sizeof(path));
  if (rc != 0)
    return rc;
  return winsrv_set_icon(proc->task, (int)a1, path);
}

static int64_t sys_spawn_args_k(uint64_t a1, uint64_t a2, uint64_t a3,
                                uint64_t a4, uint64_t a5) {
  (void)a4;
  (void)a5;
  process_t *proc = process_current();
  if (!proc || !a1)
    return -EFAULT;

  char path[VFS_PATH_MAX];
  int prc = resolve_user_path(proc, (const char *)a1, path, sizeof(path));
  if (prc != 0)
    return prc;

  int argc = (int)a3;
  if (argc < 0 || argc > SPAWN_ARGS_MAX)
    return -EINVAL;

  char storage[SPAWN_ARGS_MAX][SPAWN_ARG_STR_MAX];
  const char *kargv[SPAWN_ARGS_MAX];

  if (argc > 0) {
    if (!a2)
      return -EINVAL;
    if (!access_ok((void *)a2, (size_t)argc * sizeof(uint64_t)))
      return -EFAULT;
    uint64_t uargv[SPAWN_ARGS_MAX];
    if (copy_from_user(uargv, (void *)a2, (size_t)argc * sizeof(uint64_t)) < 0)
      return -EFAULT;

    for (int i = 0; i < argc; i++) {
      if (!uargv[i])
        return -EINVAL;
      long m = strncpy_from_user(storage[i], (const char *)uargv[i],
                                 SPAWN_ARG_STR_MAX);
      if (m < 0)
        return -EFAULT;
      kargv[i] = storage[i];
    }
  }

  process_t *child = process_spawn_child_args(proc, path, argc, kargv);
  return child ? (int64_t)child->pid : -ENOENT;
}

static int64_t sys_rename_k(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                            uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;
  process_t *proc = process_current();
  if (!proc)
    return -EFAULT;
  char oldpath[VFS_PATH_MAX];
  char newpath[VFS_PATH_MAX];
  int rc = resolve_user_path(proc, (const char *)a1, oldpath, sizeof(oldpath));
  if (rc != 0)
    return rc;
  rc = resolve_user_path(proc, (const char *)a2, newpath, sizeof(newpath));
  if (rc != 0)
    return rc;
  return vfs_rename(oldpath, newpath);
}

static int64_t sys_chdir_k(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                           uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  process_t *proc = process_current();
  if (!proc)
    return -EFAULT;
  char path[VFS_PATH_MAX];
  int rc = resolve_user_path(proc, (const char *)a1, path, sizeof(path));
  if (rc != 0)
    return rc;

  vfs_node_t *node = vfs_lookup(path);
  if (!node)
    return -ENOENT;
  if (!(node->flags & VFS_DIRECTORY)) {
    vfs_node_free(node);
    return -ENOTDIR;
  }
  vfs_node_free(node);

  size_t plen = strlen(path);
  if (plen >= sizeof(proc->cwd))
    return -ENAMETOOLONG;
  for (size_t i = 0; i < plen; i++)
    proc->cwd[i] = path[i];
  proc->cwd[plen] = '\0';
  return 0;
}

static int64_t sys_getcwd_k(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                            uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;
  process_t *proc = process_current();
  if (!proc)
    return -EFAULT;
  if (!a1 || a2 == 0)
    return -EINVAL;
  size_t len = strlen(proc->cwd) + 1;
  if (len > (size_t)a2)
    return -ERANGE;
  if (copy_to_user((void *)a1, proc->cwd, len) < 0)
    return -EFAULT;
  return (int64_t)len;
}

static int64_t sys_kill_k(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                          uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;
  int32_t pid = (int32_t)a1;
  int sig = (int)a2;
  if (sig < 1 || sig >= SIG_MAX)
    return -EINVAL;

  process_t *target = NULL;
  if (pid > 0) {
    target = process_find_by_pid((uint32_t)pid);
  } else if (pid == 0) {
    target = process_current();
  } else {
    return -EINVAL;
  }
  if (!target)
    return -ENOENT;

  __atomic_fetch_or(&target->pending_signals, 1ULL << sig, __ATOMIC_RELEASE);

  // Si la tarea destino está bloqueada en una wait queue, despertarla
  // para que retorne del syscall y vea la señal pendiente. En este MVP
  // solo las wait queues del proceso (child_wq) se despiertan; no hay
  // canales de bloqueo más complejos.
  if (target->task) {
    wake_up_all(&target->child_wq);
  }

  return 0;
}

// ===========================================================================
// Tabla de syscalls
// ===========================================================================
static const syscall_entry_t syscall_table[] = {
    [SYS_PRINT] = {sys_print_k, "print"},
    [SYS_YIELD] = {sys_yield_k, "yield"},
    [SYS_EXIT] = {sys_exit_k, "exit"},
    [SYS_SBRK] = {sys_sbrk_k, "sbrk"},
    [SYS_OPEN] = {sys_open_k, "open"},
    [SYS_CLOSE] = {sys_close_k, "close"},
    [SYS_READ] = {sys_read_k, "read"},
    [SYS_WRITE] = {sys_write_k, "write"},
    [SYS_SEEK] = {sys_seek_k, "seek"},
    [SYS_FSTAT] = {sys_fstat_k, "fstat"},
    [SYS_IPC_SEND] = {sys_ipc_send_k, "ipc_send"},
    [SYS_IPC_RECV] = {sys_ipc_recv_k, "ipc_recv"},
    [SYS_GET_TASK_ID] = {sys_get_task_id_k, "get_task_id"},
    [SYS_SPAWN] = {sys_spawn_k, "spawn"},
    [SYS_WAITPID] = {sys_waitpid_k, "waitpid"},
    [SYS_GETPID] = {sys_getpid_k, "getpid"},
    [SYS_MMAP] = {sys_mmap_k, "mmap"},
    [SYS_MUNMAP] = {sys_munmap_k, "munmap"},
    [SYS_VM_DEBUG_INFO] = {sys_vm_debug_info_k, "vm_debug_info"},
    [SYS_WIN_CREATE] = {sys_win_create_k, "win_create"},
    [SYS_WIN_DESTROY] = {sys_win_destroy_k, "win_destroy"},
    [SYS_WIN_BLIT] = {sys_win_blit_k, "win_blit"},
    [SYS_WIN_POLL_EVENT] = {sys_win_poll_event_k, "win_poll_event"},
    [SYS_WIN_REGISTER_CONSOLE] = {sys_win_register_console_k,
                                  "win_register_console"},
    [SYS_GET_SERVICE_ID] = {sys_get_service_id_k, "get_service_id"},
    [SYS_READDIR] = {sys_readdir_k, "readdir"},
    [SYS_MKDIR] = {sys_mkdir_k, "mkdir"},
    [SYS_UNLINK] = {sys_unlink_k, "unlink"},
    [SYS_WIN_SET_ICON] = {sys_win_set_icon_k, "win_set_icon"},
    [SYS_CREATE] = {sys_create_k, "create"},
    [SYS_SPAWN_ARGS] = {sys_spawn_args_k, "spawn_args"},
    [SYS_RENAME] = {sys_rename_k, "rename"},
    [SYS_CHDIR] = {sys_chdir_k, "chdir"},
    [SYS_GETCWD] = {sys_getcwd_k, "getcwd"},
    [SYS_KILL] = {sys_kill_k, "kill"},
};

// ===========================================================================
// Handler principal
// ===========================================================================
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

  if (num >= ARRAY_SIZE(syscall_table) || !syscall_table[num].fn) {
    LOG_WARN("[SYSCALL] num desconocido: %lu", (unsigned long)num);
    return err(ENOSYS);
  }

  // [SIG] Aplica señales pendientes antes de volver a userland.
  // Si la acción es fatal, no retorna.
  signal_check_pending();

  return (uint64_t)syscall_table[num].fn(arg1, arg2, arg3, arg4, arg5);
}
