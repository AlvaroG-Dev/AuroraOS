// apps/segvtest_mmap_ro/main.c
// Escribir en un mmap read-only → #PF de proteccion → kill.
#include "../../syscall.h"

#define MMAP_PROT_READ 0x1

int main(void) {
  sys_print("[segv_ro] Escribiendo en mmap read-only...\n");

  void *p = (void *)sys_mmap(0, 4096, MMAP_PROT_READ, 0, -1, 0);
  if ((long)p < 0) {
    sys_print("[segv_ro] ERROR: mmap fallo\n");
    sys_exit(1);
  }

  *(volatile char *)p = 'X';

  sys_print("[segv_ro] ERROR: el kernel NO me mato!\n");
  sys_exit(99);
  return 0;
}