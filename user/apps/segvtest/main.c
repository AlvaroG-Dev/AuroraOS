// apps/segvtest/main.c
// Test de muerte controlada por #PF no resoluble (acceso a dir invalida).
#include "../../lib/process.h"
#include "../../syscall.h"


int main(void) {
  sys_print("[segvtest] Accediendo a direccion invalida (0x12345000)...\n");
  sys_print("[segvtest] El kernel deberia matarme ahora.\n");

  volatile int *p = (volatile int *)0x12345000;
  *p = 42;

  sys_print("[segvtest] ERROR: el kernel NO me mato!\n");
  sys_exit(99);
  return 0;
}