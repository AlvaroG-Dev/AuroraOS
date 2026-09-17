// kernel/syscall.h
#ifndef SYSCALL_H
#define SYSCALL_H

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
#define SYS_WIN_CREATE           19
#define SYS_WIN_DESTROY          20
#define SYS_WIN_BLIT             21
#define SYS_WIN_POLL_EVENT       22
#define SYS_WIN_REGISTER_CONSOLE 23

void syscall_init(void);
uint64_t syscall_handler_c(uint64_t num, uint64_t arg1, uint64_t arg2,
                           uint64_t arg3, uint64_t arg4, uint64_t arg5);

#endif
