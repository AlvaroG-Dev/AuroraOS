// apps/mmap_race/main.c
#include "../../syscall.h"

#define WORKERS 32

static void print_u32(const char *prefix, uint32_t value) {
  char buf[16];
  int i = 0;
  if (value == 0) buf[i++] = '0';
  while (value) {
    buf[i++] = '0' + (int)(value % 10);
    value /= 10;
  }
  for (int a = 0, b = i - 1; a < b; a++, b--) {
    char t = buf[a]; buf[a] = buf[b]; buf[b] = t;
  }
  buf[i] = 0;

  char out[96];
  int oi = 0, pi = 0, bi = 0;
  while (prefix[pi]) out[oi++] = prefix[pi++];
  while (buf[bi]) out[oi++] = buf[bi++];
  out[oi++] = '\n'; out[oi] = 0;
  sys_print(out);
}

int main(void) {
  sys_print("[mmap_race] === mmap automatic address SMP race test ===\n");
  sys_print("[mmap_race] Lanzando 32 workers...\n");

  int pids[WORKERS];
  int spawned = 0;

  for (int i = 0; i < WORKERS; i++) {
    int pid = sys_spawn("apps/mmap_race_worker");
    if (pid < 0) {
      sys_print("[mmap_race] ERROR: no se pudo crear worker\n");
      continue;
    }
    pids[spawned++] = pid;
  }

  int failures = 0;
  for (int i = 0; i < spawned; i++) {
    int status = -1;
    int r = sys_waitpid(pids[i], &status, 0);
    if (r < 0 || status != 0)
      failures++;
  }

  print_u32("[mmap_race] Workers lanzados: ", (uint32_t)spawned);
  print_u32("[mmap_race] Workers con fallo: ", (uint32_t)failures);

  if (failures != 0) {
    sys_print("[mmap_race] FALLO\n");
    sys_exit(1);
  }

  sys_print("[mmap_race] Todos los workers terminaron.\n");
  sys_print("[mmap_race] COMPROBACION: las direcciones mapping=0x... deben ser todas distintas.\n");
  sys_exit(0);
  return 0;
}
