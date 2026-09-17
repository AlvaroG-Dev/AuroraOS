// apps/segvtest_nx/main.c
// Ejecutar codigo desde el stack (NX activo → #PF → kill).
#include "../../syscall.h"

int main(void) {
  sys_print("[segv_nx] Ejecutando codigo desde el stack (NX)...\n");

  char code[16];
  code[0] = (char)0xC3; // ret
  for (int i = 1; i < 16; i++)
    code[i] = (char)0x90; // nop

  void (*f)(void) = (void (*)(void))code;
  f();

  sys_print("[segv_nx] ERROR: el kernel NO me mato!\n");
  sys_exit(99);
  return 0;
}