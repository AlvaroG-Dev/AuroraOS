// user/syscall.c
//
// Implementación mínima de sys_exit fuera de línea para que crt0.asm
// pueda llamarla desde asm con `call sys_exit`. El resto de wrappers
// están en syscall.h como static inline.

#include "syscall.h"

void sys_exit(int code) {
  syscall(SYS_EXIT_GROUP, (uint64_t)(int64_t)code, 0, 0, 0, 0);
  while (1)
    __asm__ volatile("hlt");
}