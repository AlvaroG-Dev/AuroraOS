// user/syscall.h
#ifndef USER_SYSCALL_H
#define USER_SYSCALL_H

#include <stddef.h>
#include <stdint.h>

// ===========================================================================
// ABI doble (coherente con kernel/syscall.h).
//
//   - Rango 0..500       : Linux x86_64 ABI.
//   - Rango 0x1000..     : Aurora-only.
//
// Los wrappers de abajo mantienen la API histórica (sys_open, sys_read...)
// pero internamente usan ABI Linux. Así el userland existente sigue
// funcionando sin reescribir cada app.
// ===========================================================================

// ---- Linux ABI ----
#define SYS_READ 0
#define SYS_WRITE 1
#define SYS_OPEN 2
#define SYS_CLOSE 3
#define SYS_STAT 4
#define SYS_FSTAT 5
#define SYS_LSTAT 6
#define SYS_POLL 7
#define SYS_LSEEK 8
#define SYS_MMAP 9
#define SYS_MPROTECT 10
#define SYS_MUNMAP 11
#define SYS_BRK 12
#define SYS_RT_SIGACTION 13
#define SYS_RT_SIGPROCMASK 14
#define SYS_IOCTL 16
#define SYS_READV 19
#define SYS_WRITEV 20
#define SYS_ACCESS 21
#define SYS_PIPE 22
#define SYS_SELECT 23
#define SYS_SCHED_YIELD 24
#define SYS_DUP2 33
#define SYS_NANOSLEEP 35
#define SYS_GETPID 39
#define SYS_SENDFILE 40
#define SYS_CLONE 56
#define SYS_FORK 57
#define SYS_VFORK 58
#define SYS_EXECVE 59
#define SYS_WAIT4 61
#define SYS_KILL 62
#define SYS_UNAME 63
#define SYS_FCNTL 72
#define SYS_GETCWD 79
#define SYS_CHDIR 80
#define SYS_RENAME 82
#define SYS_MKDIR 83
#define SYS_UNLINK 87
#define SYS_READLINK 89
#define SYS_GETUID 102
#define SYS_GETGID 104
#define SYS_GETEUID 107
#define SYS_GETEGID 108
#define SYS_SETPGID 109
#define SYS_GETPPID 110
#define SYS_SETSID 112
#define SYS_GETGROUPS 115
#define SYS_GETPGID 121
#define SYS_GETSID 124
#define SYS_PRCTL 157
#define SYS_ARCH_PRCTL 158
#define SYS_GETTID 186
#define SYS_GETDENTS64 217
#define SYS_SET_TID_ADDRESS 218
#define SYS_CLOCK_GETTIME 228
#define SYS_EXIT_GROUP 231
#define SYS_OPENAT 257
#define SYS_MKDIRAT 258
#define SYS_FSTATAT 262
#define SYS_UNLINKAT 263
#define SYS_RENAMEAT 264
#define SYS_PSELECT6 270
#define SYS_PPOLL 271
#define SYS_SET_ROBUST_LIST 273
#define SYS_PRLIMIT64 302
#define SYS_GETRANDOM 318
#define SYS_RSEQ 334

// Alias de compatibilidad con el código viejo que decía SYS_EXIT.
// El kernel solo tiene exit_group; salir del proceso equivale.
#define SYS_EXIT SYS_EXIT_GROUP

// ---- Aurora-only ----
#define ASYS_BASE 0x1000
#define ASYS_SPAWN (ASYS_BASE + 0x00)
#define ASYS_WAITPID (ASYS_BASE + 0x01)
#define ASYS_SPAWN_ARGS (ASYS_BASE + 0x02)
#define ASYS_SPAWN_ARGS_FDS (ASYS_BASE + 0x03)
#define ASYS_GET_TASK_ID (ASYS_BASE + 0x04)
#define ASYS_READDIR_LEGACY (ASYS_BASE + 0x05)
#define ASYS_WIN_CREATE (ASYS_BASE + 0x10)
#define ASYS_WIN_DESTROY (ASYS_BASE + 0x11)
#define ASYS_WIN_BLIT (ASYS_BASE + 0x12)
#define ASYS_WIN_POLL_EVENT (ASYS_BASE + 0x13)
#define ASYS_WIN_REGISTER_CONSOLE (ASYS_BASE + 0x14)
#define ASYS_WIN_SET_ICON (ASYS_BASE + 0x15)
#define ASYS_IPC_SEND (ASYS_BASE + 0x20)
#define ASYS_IPC_RECV (ASYS_BASE + 0x21)
#define ASYS_GET_SERVICE_ID (ASYS_BASE + 0x22)
#define ASYS_VM_DEBUG_INFO (ASYS_BASE + 0x30)
#define ASYS_PRINT (ASYS_BASE + 0x40)

// ---- Constantes de userland ----
#define AT_FDCWD (-100)

#define O_RDONLY 0x0000
#define O_WRONLY 0x0001
#define O_RDWR 0x0002
#define O_CREAT 0x0040
#define O_TRUNC 0x0200
#define O_APPEND 0x0400

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#define SIGHUP 1
#define SIGINT 2
#define SIGKILL 9
#define SIGTERM 15
#define SIGWINCH 28

#define WNOHANG 1

#define IPC_MAX_PAYLOAD 64
#define IPC_TYPE_RAW 0
#define IPC_TYPE_TEXT 1
#define IPC_TYPE_EVENT 2
#define IPC_TYPE_RESPONSE 3

// ---- Tipos ----
typedef struct ipc_msg {
  uint32_t sender;
  uint32_t receiver;
  uint32_t type;
  uint32_t size;
  uint8_t data[IPC_MAX_PAYLOAD];
} ipc_msg_t;

// struct stat Linux x86_64. Layout exacto.
typedef struct {
  int64_t st_dev;
  uint64_t st_ino;
  uint64_t st_nlink;
  uint32_t st_mode;
  uint32_t st_uid;
  uint32_t st_gid;
  uint32_t __pad0;
  int64_t st_rdev;
  int64_t st_size;
  int64_t st_blksize;
  int64_t st_blocks;
  int64_t st_atime_sec;
  int64_t st_atime_nsec;
  int64_t st_mtime_sec;
  int64_t st_mtime_nsec;
  int64_t st_ctime_sec;
  int64_t st_ctime_nsec;
  int64_t __unused[3];
} stat_t;

typedef struct {
  uint64_t cr3_phys;
  uint64_t virt;
  uint64_t phys;
} vm_debug_info_t;

typedef struct {
  char name[128];
  uint32_t type;
  uint32_t _pad;
  uint64_t size;
} dirent_t;

typedef struct {
  int fd_in;
  int fd_out;
  int fd_err;
} spawn_fds_t;

#define WINSRV_EV_NONE 0
#define WINSRV_EV_CLOSE 1
#define WINSRV_EV_FOCUS 2
#define WINSRV_EV_BLUR 3
#define WINSRV_EV_MOVE 4
#define WINSRV_EV_KEY 5
#define WINSRV_EV_MOUSE 6
#define WINSRV_EV_OUTPUT 7
#define WINSRV_EV_TTY_INPUT 8
#define WINSRV_EV_RESIZE 9

typedef struct winsrv_event {
  uint32_t type;
  int32_t x;
  int32_t y;
  uint32_t data;
} winsrv_event_t;

typedef struct {
  int32_t win_id;
  int32_t x, y, w, h;
  int32_t src_x, src_y, src_stride;
  uint32_t *pixels;
} winsrv_blit_args_t;

// ---- poll / ioctl / fcntl ----
struct pollfd {
  int fd;
  short events;
  short revents;
};

#define POLLIN 0x0001
#define POLLPRI 0x0002
#define POLLOUT 0x0004
#define POLLERR 0x0008
#define POLLHUP 0x0010
#define POLLNVAL 0x0020

// fcntl commands (Linux x86_64).
#define F_DUPFD 0
#define F_GETFD 1
#define F_SETFD 2
#define F_GETFL 3
#define F_SETFL 4
#define F_DUPFD_CLOEXEC 1030

// Extra open flag (Linux).
#define O_NONBLOCK 0x0800

// ---------------------------------------------------------------------------
// Primitiva de syscall
// ---------------------------------------------------------------------------
static inline long syscall(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3,
                           uint64_t a4, uint64_t a5) {
  long ret;
  register uint64_t r10 __asm__("r10") = a4;
  register uint64_t r8 __asm__("r8") = a5;
  register uint64_t r9 __asm__("r9") = 0;
  __asm__ volatile("syscall"
                   : "=a"(ret), "+D"(a1), "+S"(a2), "+d"(a3), "+r"(r10),
                     "+r"(r8), "+r"(r9)
                   : "a"(num)
                   : "rcx", "r11", "memory");
  return ret;
}

// ===========================================================================
// Wrappers Linux-style (mantienen la API vieja).
// ===========================================================================

static inline long sys_print(const char *msg) {
  return syscall(ASYS_PRINT, (uint64_t)msg, 0, 0, 0, 0);
}
static inline void sys_yield(void) { syscall(SYS_SCHED_YIELD, 0, 0, 0, 0, 0); }

static inline int sys_open(const char *path, int flags) {
  return (int)syscall(SYS_OPENAT, (uint64_t)(int64_t)AT_FDCWD, (uint64_t)path,
                      (uint64_t)flags, 0644, 0);
}
static inline int sys_close(int fd) {
  return (int)syscall(SYS_CLOSE, (uint64_t)fd, 0, 0, 0, 0);
}
static inline int64_t sys_read(int fd, void *buf, size_t count) {
  return (int64_t)syscall(SYS_READ, (uint64_t)fd, (uint64_t)buf,
                          (uint64_t)count, 0, 0);
}
static inline int64_t sys_write(int fd, const void *buf, size_t count) {
  return (int64_t)syscall(SYS_WRITE, (uint64_t)fd, (uint64_t)buf,
                          (uint64_t)count, 0, 0);
}
static inline int64_t sys_seek(int fd, int64_t offset, int whence) {
  return (int64_t)syscall(SYS_LSEEK, (uint64_t)fd, (uint64_t)offset,
                          (uint64_t)whence, 0, 0);
}
static inline int sys_fstat(int fd, stat_t *st) {
  return (int)syscall(SYS_FSTAT, (uint64_t)fd, (uint64_t)st, 0, 0, 0);
}
static inline int sys_create(const char *path) {
  return (int)syscall(SYS_OPENAT, (uint64_t)(int64_t)AT_FDCWD, (uint64_t)path,
                      (uint64_t)(O_CREAT | O_WRONLY), 0644, 0);
}
static inline int sys_mkdir(const char *path) {
  return (int)syscall(SYS_MKDIRAT, (uint64_t)(int64_t)AT_FDCWD, (uint64_t)path,
                      0755, 0, 0);
}
static inline int sys_unlink(const char *path) {
  return (int)syscall(SYS_UNLINKAT, (uint64_t)(int64_t)AT_FDCWD, (uint64_t)path,
                      0, 0, 0);
}
static inline int sys_rename(const char *oldp, const char *newp) {
  return (int)syscall(SYS_RENAMEAT, (uint64_t)(int64_t)AT_FDCWD, (uint64_t)oldp,
                      (uint64_t)(int64_t)AT_FDCWD, (uint64_t)newp, 0);
}
static inline int sys_chdir(const char *path) {
  return (int)syscall(SYS_CHDIR, (uint64_t)path, 0, 0, 0, 0);
}
static inline int sys_getcwd(char *buf, size_t size) {
  return (int)syscall(SYS_GETCWD, (uint64_t)buf, (uint64_t)size, 0, 0, 0);
}
static inline int sys_kill(int pid, int sig) {
  return (int)syscall(SYS_KILL, (uint64_t)(int64_t)pid, (uint64_t)sig, 0, 0, 0);
}
static inline int sys_pipe(int fds[2]) {
  return (int)syscall(SYS_PIPE, (uint64_t)fds, 0, 0, 0, 0);
}
static inline int sys_dup2(int oldfd, int newfd) {
  return (int)syscall(SYS_DUP2, (uint64_t)oldfd, (uint64_t)newfd, 0, 0, 0);
}
static inline int sys_getpid(void) {
  return (int)syscall(SYS_GETPID, 0, 0, 0, 0, 0);
}

static inline void *sys_mmap(uint64_t addr, uint64_t length, uint64_t prot,
                             uint64_t flags, int fd, uint64_t offset) {
  (void)fd;
  (void)offset;
  return (void *)syscall(SYS_MMAP, addr, length, prot, flags, 0);
}
static inline int sys_munmap(uint64_t addr, uint64_t length) {
  return (int)syscall(SYS_MUNMAP, addr, length, 0, 0, 0);
}

// [LEGACY] readdir con API Aurora. Se enruta por ASYS_READDIR_LEGACY.
static inline int sys_readdir(const char *path, uint64_t index, dirent_t *out) {
  return (int)syscall(ASYS_READDIR_LEGACY, (uint64_t)path, index, (uint64_t)out,
                      0, 0);
}

static inline int sys_vm_debug_info(uint64_t virt, vm_debug_info_t *info) {
  return (int)syscall(ASYS_VM_DEBUG_INFO, virt, (uint64_t)info, 0, 0, 0);
}

// ---- winsrv (Aurora-only) ----
static inline int sys_win_create(int x, int y, int w, int h,
                                 const char *title) {
  return (int)syscall(ASYS_WIN_CREATE, (uint64_t)x, (uint64_t)y, (uint64_t)w,
                      (uint64_t)h, (uint64_t)title);
}
static inline int sys_win_destroy(int win_id) {
  return (int)syscall(ASYS_WIN_DESTROY, (uint64_t)win_id, 0, 0, 0, 0);
}
static inline int sys_win_blit(int win_id, int x, int y, int w, int h,
                               int src_x, int src_y, int src_stride,
                               const uint32_t *pixels) {
  winsrv_blit_args_t args;
  args.win_id = win_id;
  args.x = x;
  args.y = y;
  args.w = w;
  args.h = h;
  args.src_x = src_x;
  args.src_y = src_y;
  args.src_stride = src_stride;
  args.pixels = (uint32_t *)pixels;
  return (int)syscall(ASYS_WIN_BLIT, (uint64_t)&args, 0, 0, 0, 0);
}
static inline int sys_win_poll_event(int win_id, winsrv_event_t *ev,
                                     int blocking) {
  return (int)syscall(ASYS_WIN_POLL_EVENT, (uint64_t)win_id, (uint64_t)ev,
                      (uint64_t)blocking, 0, 0);
}
static inline int sys_win_register_console(int win_id) {
  return (int)syscall(ASYS_WIN_REGISTER_CONSOLE, (uint64_t)win_id, 0, 0, 0, 0);
}
static inline int sys_win_set_icon(int win_id, const char *path) {
  return (int)syscall(ASYS_WIN_SET_ICON, (uint64_t)win_id, (uint64_t)path, 0, 0,
                      0);
}
static inline int sys_get_service_id(const char *name) {
  return (int)syscall(ASYS_GET_SERVICE_ID, (uint64_t)name, 0, 0, 0, 0);
}

// ---- spawn / wait (Aurora-only) ----
static inline int sys_spawn(const char *path) {
  return (int)syscall(ASYS_SPAWN, (uint64_t)path, 0, 0, 0, 0);
}

// [ENV] 4º argumento (a4 → r10) es el envp del hijo. Pasar NULL es válido.
static inline int sys_spawn_args(const char *path, char *const argv[], int argc,
                                 char *const envp[]) {
  return (int)syscall(ASYS_SPAWN_ARGS, (uint64_t)path, (uint64_t)argv,
                      (uint64_t)argc, (uint64_t)envp, 0);
}

// [ENV] 5º argumento (a5 → r8) es el envp del hijo. Pasar NULL es válido.
static inline int sys_spawn_args_fds(const char *path, char *const argv[],
                                     int argc, const spawn_fds_t *fds,
                                     char *const envp[]) {
  return (int)syscall(ASYS_SPAWN_ARGS_FDS, (uint64_t)path, (uint64_t)argv,
                      (uint64_t)argc, (uint64_t)fds, (uint64_t)envp);
}

static inline int sys_waitpid(int pid, int *status, int options) {
  return (int)syscall(ASYS_WAITPID, (uint64_t)pid, (uint64_t)status,
                      (uint64_t)options, 0, 0);
}

static inline uint32_t sys_get_task_id(void) {
  return (uint32_t)syscall(ASYS_GET_TASK_ID, 0, 0, 0, 0, 0);
}
static inline void *sys_sbrk(intptr_t increment) {
  // Emulado sobre brk: lee la actual, suma, y llama brk.
  long cur = syscall(SYS_BRK, 0, 0, 0, 0, 0);
  if (cur < 0)
    return (void *)-1;
  long next = syscall(SYS_BRK, (uint64_t)(cur + increment), 0, 0, 0, 0);
  if (next < 0)
    return (void *)-1;
  return (void *)cur;
}

// ---- IPC (Aurora-only) ----
static inline int sys_ipc_send(uint32_t target_id, uint32_t type,
                               const void *data, size_t size) {
  return (int)syscall(ASYS_IPC_SEND, (uint64_t)target_id, (uint64_t)type,
                      (uint64_t)data, (uint64_t)size, 0);
}
static inline int sys_ipc_recv(ipc_msg_t *msg, int non_blocking) {
  return (int)syscall(ASYS_IPC_RECV, (uint64_t)msg, (uint64_t)non_blocking, 0,
                      0, 0);
}

static inline int sys_poll(struct pollfd *fds, unsigned int nfds,
                           int timeout_ms) {
  return (int)syscall(SYS_POLL, (uint64_t)fds, (uint64_t)nfds,
                      (uint64_t)(int64_t)timeout_ms, 0, 0);
}
static inline int sys_ioctl(int fd, unsigned long req, void *arg) {
  return (int)syscall(SYS_IOCTL, (uint64_t)fd, (uint64_t)req, (uint64_t)arg, 0,
                      0);
}
static inline int sys_fcntl(int fd, int cmd, int arg) {
  return (int)syscall(SYS_FCNTL, (uint64_t)fd, (uint64_t)cmd, (uint64_t)arg, 0,
                      0);
}

void sys_exit(int code);

#endif