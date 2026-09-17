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
#include "string.h"
#include "vfs.h"
#include <stddef.h>
#include <stdint.h>

extern void syscall_entry(void);

void syscall_init(void) {
  uint64_t efer = rdmsr(MSR_EFER);
  efer |= EFER_SCE;
  wrmsr(MSR_EFER, efer);

  uint64_t star = ((uint64_t)KERNEL_CS << 32) | ((uint64_t)(USER_DS - 8) << 48);
  wrmsr(MSR_STAR, star);

  wrmsr(MSR_LSTAR, (uint64_t)syscall_entry);

  // Ver el comentario en cpu.h sobre MSR_SFMASK_BITS.
  wrmsr(MSR_SFMASK, MSR_SFMASK_BITS);

  LOG_INFO("[SYSCALL] Syscalls inicializados (SCE, STAR, LSTAR).");
}

static size_t copy_string_from_user(char *dst, size_t dst_size,
                                    const char *user_src) {
  if (!dst || dst_size == 0)
    return 0;

  size_t i = 0;
  stac();
  while (i < dst_size - 1) {
    char c;
    __asm__ volatile("movb (%1), %0" : "=r"(c) : "r"(user_src + i) : "memory");
    if (c == '\0')
      break;
    dst[i++] = c;
  }
  clac();
  dst[i] = '\0';
  return i;
}

uint64_t syscall_handler_c(uint64_t num, uint64_t arg1, uint64_t arg2,
                           uint64_t arg3, uint64_t arg4, uint64_t arg5) {
  switch (num) {
  case SYS_PRINT: {
    if (arg1 == 0 || (arg1 >= 0xFFFFFFFF80000000ULL)) {
      return (uint64_t)-1;
    }
    char buffer[256];
    copy_string_from_user(buffer, sizeof(buffer), (const char *)arg1);
    serial_puts(buffer);
    return 0;
  }

  case SYS_YIELD:
    sched_yield();
    return 0;

  case SYS_EXIT: {
    process_t *proc = process_current();
    if (proc) {
      process_exit(proc, (int)arg1);
    }
    task_t *cur = sched_current();
    if (cur) {
      cur->state = TASK_DEAD;
    }
    sched_yield();
    while (1)
      __asm__ volatile("hlt");
  }

  case SYS_SBRK: {
    process_t *proc = process_current();
    if (!proc) {
      return (uint64_t)-1;
    }
    return (uint64_t)process_sbrk(proc, (int64_t)arg1);
  }

  case SYS_OPEN: {
    process_t *proc = process_current();
    if (!proc || arg1 == 0 || arg1 >= 0xFFFFFFFF80000000ULL) {
      return (uint64_t)-1;
    }
    char path[256];
    copy_string_from_user(path, sizeof(path), (const char *)arg1);
    int fd = vfs_open_for_proc(proc, path, (int)arg2);
    return (uint64_t)fd;
  }

  case SYS_CLOSE: {
    process_t *proc = process_current();
    if (!proc)
      return (uint64_t)-1;
    return (uint64_t)vfs_close_for_proc(proc, (int)arg1);
  }

  case SYS_READ: {
    process_t *proc = process_current();
    if (!proc || arg2 == 0 || arg2 >= 0xFFFFFFFF80000000ULL) {
      return (uint64_t)-1;
    }
    return (uint64_t)vfs_read_for_proc(proc, (int)arg1, (void *)arg2,
                                       (size_t)arg3);
  }

  case SYS_WRITE: {
    process_t *proc = process_current();
    if (!proc || arg2 == 0 || arg2 >= 0xFFFFFFFF80000000ULL) {
      return (uint64_t)-1;
    }
    return (uint64_t)vfs_write_for_proc(proc, (int)arg1, (const void *)arg2,
                                        (size_t)arg3);
  }

  case SYS_SEEK: {
    process_t *proc = process_current();
    if (!proc)
      return (uint64_t)-1;
    return (uint64_t)vfs_seek_for_proc(proc, (int)arg1, (int64_t)arg2,
                                       (int)arg3);
  }

  case SYS_FSTAT: {
    process_t *proc = process_current();
    if (!proc || arg2 == 0 || arg2 >= 0xFFFFFFFF80000000ULL) {
      return (uint64_t)-1;
    }
    return (uint64_t)vfs_fstat_for_proc(proc, (int)arg1, (vfs_stat_t *)arg2);
  }

  case SYS_IPC_SEND: {
    if (arg3 != 0 && arg3 >= 0xFFFFFFFF80000000ULL) {
      return (uint64_t)-1;
    }
    return (uint64_t)ipc_send((uint32_t)arg1, (uint32_t)arg2,
                              (const void *)arg3, (size_t)arg4);
  }

  case SYS_IPC_RECV: {
    if (arg1 == 0 || arg1 >= 0xFFFFFFFF80000000ULL) {
      return (uint64_t)-1;
    }
    return (uint64_t)ipc_recv((ipc_msg_t *)arg1, (int)arg2);
  }

  case SYS_GET_TASK_ID: {
    task_t *cur = sched_current();
    return cur ? (uint64_t)cur->id : (uint64_t)-1;
  }

  case SYS_SPAWN: {
    process_t *proc = process_current();
    if (arg1 == 0 || arg1 >= 0xFFFFFFFF80000000ULL) {
      return (uint64_t)-1;
    }
    char path[256];
    copy_string_from_user(path, sizeof(path), (const char *)arg1);
    process_t *child = process_spawn_child(proc, path);
    return child ? (uint64_t)child->pid : (uint64_t)-1;
  }

  case SYS_WAITPID: {
    process_t *proc = process_current();
    if (!proc)
      return (uint64_t)-1;
    int *status_ptr = NULL;
    if (arg2 != 0 && arg2 < 0xFFFFFFFF80000000ULL) {
      status_ptr = (int *)arg2;
    }
    int res = process_waitpid(proc, (int32_t)arg1, status_ptr, (int)arg3);
    return (uint64_t)res;
  }

  case SYS_GETPID: {
    process_t *proc = process_current();
    return proc ? (uint64_t)proc->pid : (uint64_t)-1;
  }

  case SYS_MMAP: {
    process_t *proc = process_current();
    if (!proc)
      return (uint64_t)-1;

    uint64_t flags = arg4 & 0xFFFFFFFFULL;
    int fd = (int)(arg4 >> 32);
    int64_t ret = sys_mmap(proc, arg1, arg2, arg3, flags, fd, arg5);
    return (uint64_t)ret;
  }

  case SYS_MUNMAP: {
    process_t *proc = process_current();
    if (!proc)
      return (uint64_t)-1;
    int64_t ret = sys_munmap(proc, arg1, arg2);
    return (uint64_t)ret;
  }

  // -------------------------------------------------------------------------
  // Window server
  // -------------------------------------------------------------------------
  case SYS_WIN_CREATE: {
    process_t *proc = process_current();
    if (!proc || !proc->task)
      return (uint64_t)-1;
    // arg1=x, arg2=y, arg3=w, arg4=h, arg5=title
    int win_id = winsrv_create_window(proc->task,
                                      (int)arg1, (int)arg2,
                                      (int)arg3, (int)arg4,
                                      (const char *)arg5);
    return (uint64_t)win_id;
  }

  case SYS_WIN_DESTROY: {
    process_t *proc = process_current();
    if (!proc || !proc->task)
      return (uint64_t)-1;
    return (uint64_t)winsrv_destroy_window(proc->task, (int)arg1);
  }

  case SYS_WIN_BLIT: {
    process_t *proc = process_current();
    if (!proc || !proc->task)
      return (uint64_t)-1;

    winsrv_blit_args_t args;
    stac();
    const winsrv_blit_args_t *u = (const winsrv_blit_args_t *)arg1;
    if (!u) { clac(); return (uint64_t)-1; }
    args = *u;
    clac();

    int rc = winsrv_blit(proc->task, args.win_id, args.x, args.y,
                         args.w, args.h, args.pixels);
    return (uint64_t)rc;
  }

  case SYS_WIN_POLL_EVENT: {
    process_t *proc = process_current();
    if (!proc || !proc->task)
      return (uint64_t)-1;
    return (uint64_t)winsrv_poll_event(proc->task, (int)arg1,
                                       (winsrv_event_t *)arg2,
                                       (int)arg3);
  }

  case SYS_WIN_REGISTER_CONSOLE: {
    process_t *proc = process_current();
    if (!proc || !proc->task)
      return (uint64_t)-1;
    return (uint64_t)winsrv_register_console(proc->task, (int)arg1);
  }

  default:
    LOG_WARN("[SYSCALL] Syscall desconocido: %lu", (unsigned long)num);
    return (uint64_t)-1;
  }
}
