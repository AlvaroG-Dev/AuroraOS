// kernel/syscall.h
#ifndef SYSCALL_H
#define SYSCALL_H

#include "idt.h" // registers_t
#include <stdint.h>

// Números de syscall.
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
#define SYS_GET_SERVICE_ID 24
#define SYS_VM_DEBUG_INFO 25
#define SYS_READDIR 26
#define SYS_MKDIR 27
#define SYS_UNLINK 28
#define SYS_WIN_SET_ICON 29
#define SYS_CREATE 30
#define SYS_SPAWN_ARGS 31
#define SYS_RENAME 32
#define SYS_CHDIR 33
#define SYS_GETCWD 34

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