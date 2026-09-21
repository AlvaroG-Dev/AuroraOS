#include "../../syscall.h"

int main(void) {
  sys_print("[sbrk_shrink] === sbrk shrink regression ===\n");

  int pid = sys_spawn("apps/sbrk_shrink_test");
  if (pid < 0) {
    sys_print("[sbrk_shrink] ERROR: no se pudo lanzar el test\n");
    return 1;
  }

  int status = 0;
  if (sys_waitpid(pid, &status, 0) != pid) {
    sys_print("[sbrk_shrink] ERROR: waitpid fallo\n");
    return 2;
  }

  if (status == 0) {
    sys_print("[sbrk_shrink] ERROR: la pagina liberada seguia accesible\n");
    return 3;
  }

  sys_print("[sbrk_shrink] OK: sbrk(-) desmapeo la pagina liberada\n");
  sys_print("[sbrk_shrink] Liberacion de heap: PASA\n");
  return 0;
}
