#include "syscall.h"
#include <stdint.h>

int main(void) {
  enum { WORKERS = 8 };
  int pids[WORKERS];
  int failed = 0;

  sys_print("[vma_isolation] === aislamiento de VA entre procesos ===\n");
  sys_print("[vma_isolation] Lanzando 8 procesos que usan la MISMA VA 0x70000000...\n");

  for (int i = 0; i < WORKERS; i++) {
    pids[i] = sys_spawn("apps/vma_isolation_worker");
    if (pids[i] < 0) {
      sys_print("[vma_isolation] ERROR: no se pudo lanzar worker\n");
      failed++;
    }
  }

  for (int i = 0; i < WORKERS; i++) {
    if (pids[i] < 0)
      continue;

    int status = -1;
    int rc = sys_waitpid(pids[i], &status, 0);
    if (rc != pids[i] || status != 0) {
      sys_print("[vma_isolation] ERROR: worker termino con fallo\n");
      failed++;
    }
  }

  if (failed) {
    sys_print("[vma_isolation] FALLO: aislamiento de direcciones virtuales\n");
    return 1;
  }

  sys_print("[vma_isolation] Todos los procesos usaron 0x70000000 sin interferirse.\n");
  sys_print("[vma_isolation] AISLAMIENTO ENTRE PML4: PASA\n");
  return 0;
}
