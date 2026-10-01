// kernel/syscall.h
#ifndef SYSCALL_H
#define SYSCALL_H

#include "idt.h"
#include <stdint.h>

// ===========================================================================
// ABI doble.
//
//  - Rango 0..500       : números exactos de Linux x86_64. Toda syscall
//                         que musl/los programas de Linux esperan va aquí.
//  - Rango 0x1000..     : Aurora-only. Window server, IPC, service registry
//                         y spawn (no hay clone todavía).
//
// El dispatcher en syscall.c decide por rango.
// ===========================================================================

// ---- Linux ABI (no tocar los números) ----
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
#define SYS_RT_SIGRETURN 15
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
#define SYS_FSYNC 74
#define SYS_GETCWD 79
#define SYS_CHDIR 80
#define SYS_RENAME 82
#define SYS_MKDIR 83
#define SYS_LINK 86
#define SYS_UNLINK 87
#define SYS_SYMLINK 88
#define SYS_READLINK 89
#define SYS_CHMOD 90
#define SYS_FCHMOD 91
#define SYS_CHOWN 92
#define SYS_FCHOWN 93
#define SYS_LCHOWN 94
#define SYS_UMASK 95
#define SYS_GETRLIMIT 97
#define SYS_GETRUSAGE 98
#define SYS_SYSINFO 99
#define SYS_TIMES 100
#define SYS_GETUID 102
#define SYS_GETGID 104
#define SYS_SETUID 105
#define SYS_SETGID 106
#define SYS_GETEUID 107
#define SYS_GETEGID 108
#define SYS_SETPGID 109
#define SYS_GETPPID 110
#define SYS_SETSID 112
#define SYS_SETREUID 113
#define SYS_SETREGID 114
#define SYS_GETGROUPS 115
#define SYS_SETGROUPS 116
#define SYS_SETRESUID 117
#define SYS_GETRESUID 118
#define SYS_SETRESGID 119
#define SYS_GETRESGID 120
#define SYS_GETPGID 121
#define SYS_SETFSUID 122
#define SYS_SETFSGID 123
#define SYS_GETSID 124
#define SYS_MKNOD 133
#define SYS_STATFS 137
#define SYS_FSTATFS 138
#define SYS_PIVOT_ROOT 155
#define SYS_PRCTL 157
#define SYS_ARCH_PRCTL 158
#define SYS_SETRLIMIT 160
#define SYS_SETHOSTNAME 170
#define SYS_GETTID 186
#define SYS_TKILL 200
#define SYS_GETDENTS64 217
#define SYS_SET_TID_ADDRESS 218
#define SYS_CLOCK_GETTIME 228
#define SYS_EXIT_GROUP 231
#define SYS_OPENAT 257
#define SYS_MKDIRAT 258
#define SYS_FSTATAT 262
#define SYS_UNLINKAT 263
#define SYS_RENAMEAT 264
#define SYS_LINKAT 265
#define SYS_READLINKAT 267
#define SYS_FCHMODAT 268
#define SYS_FACCESSAT 269
#define SYS_PSELECT6 270
#define SYS_PPOLL 271
#define SYS_SET_ROBUST_LIST 273
#define SYS_UTIMENSAT 280
#define SYS_PIPE2 293
#define SYS_PRLIMIT64 302
#define SYS_GETRANDOM 318
#define SYS_RSEQ 334

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
#define ASYS_FS_CHECK (ASYS_BASE + 0x50)
#define ASYS_MAX (ASYS_BASE + 0x100)

// ---------------------------------------------------------------------------
// Constantes Linux que el kernel necesita.
// ---------------------------------------------------------------------------
#define AT_FDCWD (-100)
#define AT_SYMLINK_NOFOLLOW 0x100
#define AT_REMOVEDIR 0x200

#define ARCH_SET_GS 0x1001
#define ARCH_SET_FS 0x1002
#define ARCH_GET_FS 0x1003
#define ARCH_GET_GS 0x1004

#define LINUX_O_ACCMODE 0x0003
#define LINUX_O_RDONLY 0x0000
#define LINUX_O_WRONLY 0x0001
#define LINUX_O_RDWR 0x0002
#define LINUX_O_CREAT 0x0040
#define LINUX_O_TRUNC 0x0200
#define LINUX_O_APPEND 0x0400
#define LINUX_O_EXCL 0x0080

// ---------------------------------------------------------------------------
// struct stat de Linux x86_64, layout byte a byte.
// ---------------------------------------------------------------------------
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
} linux_stat_t;

typedef struct {
  uint64_t d_ino;
  int64_t d_off;
  uint16_t d_reclen;
  uint8_t d_type;
  char d_name[];
} linux_dirent64_t;

#define DT_UNKNOWN 0
#define DT_FIFO 1
#define DT_CHR 2
#define DT_DIR 4
#define DT_BLK 6
#define DT_REG 8
#define DT_LNK 10
#define DT_SOCK 12

// ---------------------------------------------------------------------------
// Tipos auxiliares varios.
// ---------------------------------------------------------------------------
typedef struct {
  uint64_t cr3_phys;
  uint64_t virt;
  uint64_t phys;
} vm_debug_info_t;

#define SYSCALL_INT_NUM 0xFFFFFFFFFFFFFFFFULL

void syscall_init(void);
uint64_t syscall_handler_c(registers_t *regs);
void syscall_init_ap(void);

#define KERNEL_SERVICES_MAX 8
void syscall_register_service(const char *name, uint32_t task_id);

#endif