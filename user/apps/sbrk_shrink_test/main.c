#include "../../syscall.h"

int main(void) {
  sys_print("[sbrk_shrink_test] Reservando 2 paginas...\n");

  unsigned char *base = (unsigned char *)sys_sbrk(8192);
  if (base == (void *)-1) {
    sys_print("[sbrk_shrink_test] ERROR: sbrk(+8192) fallo\n");
    return 1;
  }

  volatile unsigned char *released_page = base + 4096;
  *released_page = 0xA5;

  if (sys_sbrk(-4096) == (void *)-1) {
    sys_print("[sbrk_shrink_test] ERROR: sbrk(-4096) fallo\n");
    return 2;
  }

  sys_print("[sbrk_shrink_test] Accediendo a la pagina liberada (debe provocar #PF)...\n");

  // This page is completely above the new program break. If sbrk() really
  // released it, the access must fault and this process must be killed.
  volatile unsigned char value = *released_page;
  (void)value;

  sys_print("[sbrk_shrink_test] ERROR: la pagina seguia mapeada\n");
  return 3;
}
