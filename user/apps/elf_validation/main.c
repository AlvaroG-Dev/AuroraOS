#include "syscall.h"

int main(void) {
  sys_print("[elf_validation] === ELF malformed regression test ===\n");
  sys_print("[elf_validation] Intentando cargar apps/elf_malformed...\n");

  int pid = sys_spawn("apps/elf_malformed");
  if (pid >= 0) {
    sys_print("[elf_validation] ERROR: ELF malformado fue aceptado\n");
    int status = -1;
    sys_waitpid(pid, &status, 0);
    return 1;
  }

  sys_print("[elf_validation] OK: ELF malformado rechazado\n");
  sys_print("[elf_validation] Validacion de limites PT_LOAD: PASA\n");
  return 0;
}
