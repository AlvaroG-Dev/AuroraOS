#include "syscall.h"

void sys_exit(int code) {
  syscall(SYS_EXIT, (uint64_t)code, 0, 0, 0, 0);
  while (1)
    __asm__ volatile("hlt");
}