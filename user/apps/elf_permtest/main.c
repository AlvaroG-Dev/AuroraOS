#include "syscall.h"

int main(void) {
  sys_print("[elf_permtest] === ELF PT_LOAD permissions regression ===\n");
  sys_print("[elf_permtest] Lanzando ELF con .text R-X y .data RW...\n");

  int pid = sys_spawn("apps/elf_rotest");
  if (pid < 0) {
    sys_print("[elf_permtest] ERROR: no se pudo lanzar elf_rotest\n");
    return 1;
  }

  int status = -1;
  int rc = sys_waitpid(pid, &status, 0);
  if (rc != pid) {
    sys_print("[elf_permtest] ERROR: waitpid fallo\n");
    return 2;
  }

  if (status == 0) {
    sys_print("[elf_permtest] ERROR: escritura en .text fue permitida\n");
    return 3;
  }

  sys_print("[elf_permtest] OK: .text R-X rechazo escritura\n");
  sys_print("[elf_permtest] Permisos PT_LOAD: PASA\n");
  return 0;
}
