// apps/segvtest_kernel/main.c
// Saltar a direccion del kernel desde Ring 3 → #PF → kill.
#include "../../syscall.h"

int main(void) {
  sys_print("[segv_kernel] Saltando a direccion del kernel...\n");

  void (*f)(void) = (void (*)(void))0xFFFFFFFF81000000ULL;
  f();

  sys_print("[segv_kernel] ERROR: el kernel NO me mato!\n");
  sys_exit(99);
  return 0;
}