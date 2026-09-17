#ifndef USER_SYSCALL_H
#define USER_SYSCALL_H

#include <stddef.h>
#include <stdint.h>

#define SYS_PRINT 1
#define SYS_YIELD 2
#define SYS_EXIT 3
#define SYS_SBRK 4
#define SYS_OPEN 5
#define SYS_CLOSE 6
#define SYS_READ 7
#define SYS_WRITE 8
#define SYS_SEEK 9
#define SYS_FSTAT 10
#define SYS_IPC_SEND 11
#define SYS_IPC_RECV 12
#define SYS_GET_TASK_ID 13
#define SYS_SPAWN 14
#define SYS_WAITPID 15
#define SYS_GETPID 16
#define SYS_MMAP 17
#define SYS_MUNMAP 18
#define SYS_WIN_CREATE 19
#define SYS_WIN_DESTROY 20
#define SYS_WIN_BLIT 21
#define SYS_WIN_POLL_EVENT 22
#define SYS_WIN_REGISTER_CONSOLE 23

#define WNOHANG 1

#define O_RDONLY 0x0000
#define O_WRONLY 0x0001
#define O_RDWR 0x0002
#define O_CREAT 0x0040

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#define IPC_MAX_PAYLOAD 64

#define IPC_TYPE_RAW 0
#define IPC_TYPE_TEXT 1
#define IPC_TYPE_EVENT 2
#define IPC_TYPE_RESPONSE 3

typedef struct ipc_msg {
  uint32_t sender;
  uint32_t receiver;
  uint32_t type;
  uint32_t size;
  uint8_t data[IPC_MAX_PAYLOAD];
} ipc_msg_t;

typedef struct {
  uint32_t flags;
  size_t size;
  uint32_t inode;
} stat_t;

// --- Winsrv (window server) ---

#define WINSRV_EV_NONE      0
#define WINSRV_EV_CLOSE     1
#define WINSRV_EV_FOCUS     2
#define WINSRV_EV_BLUR      3
#define WINSRV_EV_MOVE      4
#define WINSRV_EV_KEY       5
#define WINSRV_EV_MOUSE     6
#define WINSRV_EV_OUTPUT    7
#define WINSRV_EV_TTY_INPUT 8

typedef struct winsrv_event {
  uint32_t type;
  int32_t  x;
  int32_t  y;
  uint32_t data;
} winsrv_event_t;

typedef struct {
  int32_t  win_id;
  int32_t  x, y, w, h;
  uint32_t *pixels;
} winsrv_blit_args_t;

static inline long syscall(uint64_t num, uint64_t arg1, uint64_t arg2,
                           uint64_t arg3, uint64_t arg4, uint64_t arg5) {
  register uint64_t rax __asm__("rax") = num;
  register uint64_t rdi __asm__("rdi") = arg1;
  register uint64_t rsi __asm__("rsi") = arg2;
  register uint64_t rdx __asm__("rdx") = arg3;
  register uint64_t r10 __asm__("r10") = arg4;
  register uint64_t r8 __asm__("r8") = arg5;
  __asm__ volatile("syscall"
                   : "+r"(rax)
                   : "r"(rdi), "r"(rsi), "r"(rdx), "r"(r10), "r"(r8)
                   : "rcx", "r11", "memory");
  return rax;
}

static inline long sys_print(const char *msg) {
  return syscall(SYS_PRINT, (uint64_t)msg, 0, 0, 0, 0);
}
static inline void sys_yield(void) { syscall(SYS_YIELD, 0, 0, 0, 0, 0); }
static inline void *sys_sbrk(intptr_t increment) {
  return (void *)syscall(SYS_SBRK, (uint64_t)increment, 0, 0, 0, 0);
}
static inline int sys_open(const char *path, int flags) {
  return (int)syscall(SYS_OPEN, (uint64_t)path, (uint64_t)flags, 0, 0, 0);
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
  return (int64_t)syscall(SYS_SEEK, (uint64_t)fd, (uint64_t)offset,
                          (uint64_t)whence, 0, 0);
}
static inline int sys_fstat(int fd, stat_t *st) {
  return (int)syscall(SYS_FSTAT, (uint64_t)fd, (uint64_t)st, 0, 0, 0);
}
static inline int sys_ipc_send(uint32_t target_id, uint32_t type,
                               const void *data, size_t size) {
  return (int)syscall(SYS_IPC_SEND, (uint64_t)target_id, (uint64_t)type,
                      (uint64_t)data, (uint64_t)size, 0);
}
static inline int sys_ipc_recv(ipc_msg_t *msg, int non_blocking) {
  return (int)syscall(SYS_IPC_RECV, (uint64_t)msg, (uint64_t)non_blocking, 0, 0,
                      0);
}
static inline uint32_t sys_get_task_id(void) {
  return (uint32_t)syscall(SYS_GET_TASK_ID, 0, 0, 0, 0, 0);
}
static inline int sys_spawn(const char *path) {
  return (int)syscall(SYS_SPAWN, (uint64_t)path, 0, 0, 0, 0);
}
static inline int sys_waitpid(int pid, int *status, int options) {
  return (int)syscall(SYS_WAITPID, (uint64_t)pid, (uint64_t)status,
                      (uint64_t)options, 0, 0);
}
static inline int sys_getpid(void) {
  return (int)syscall(SYS_GETPID, 0, 0, 0, 0, 0);
}

static inline void *sys_mmap(uint64_t addr, uint64_t length, uint64_t prot,
                             uint64_t flags, int fd, uint64_t offset) {
  return (void *)syscall(SYS_MMAP, addr, length, prot,
                         (flags & 0xFFFFFFFF) | ((uint64_t)fd << 32), offset);
}

static inline int sys_munmap(uint64_t addr, uint64_t length) {
  return (int)syscall(SYS_MUNMAP, addr, length, 0, 0, 0);
}

// --- Winsrv wrappers ---

static inline int sys_win_create(int x, int y, int w, int h, const char *title) {
  return (int)syscall(SYS_WIN_CREATE, (uint64_t)x, (uint64_t)y,
                      (uint64_t)w, (uint64_t)h, (uint64_t)title);
}
static inline int sys_win_destroy(int win_id) {
  return (int)syscall(SYS_WIN_DESTROY, (uint64_t)win_id, 0, 0, 0, 0);
}
static inline int sys_win_blit(int win_id, int x, int y, int w, int h,
                               const uint32_t *pixels) {
  winsrv_blit_args_t args;
  args.win_id = win_id;
  args.x = x; args.y = y; args.w = w; args.h = h;
  args.pixels = (uint32_t *)pixels;
  return (int)syscall(SYS_WIN_BLIT, (uint64_t)&args, 0, 0, 0, 0);
}
static inline int sys_win_poll_event(int win_id, winsrv_event_t *ev, int blocking) {
  return (int)syscall(SYS_WIN_POLL_EVENT, (uint64_t)win_id, (uint64_t)ev,
                      (uint64_t)blocking, 0, 0);
}
static inline int sys_win_register_console(int win_id) {
  return (int)syscall(SYS_WIN_REGISTER_CONSOLE, (uint64_t)win_id, 0, 0, 0, 0);
}

void sys_exit(int code);

#endif
