#include "syscall.h"

int main(void) {
  sys_print("[elf_rotest] Intentando escribir en .text (debe provocar #PF)\n");

  volatile unsigned char *text = (volatile unsigned char *)0x400000ULL;
  unsigned char original = *text;
  *text = (unsigned char)(original ^ 0xFF);

  // Si llegamos aquí, el segmento R-X fue mapeado como escribible.
  sys_print("[elf_rotest] ERROR: escritura en .text fue permitida\n");
  return 1;
}
