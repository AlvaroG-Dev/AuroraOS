// apps/waitpid_stress/main.c
// Stress test for process/task lifetime:
// repeatedly spawn a short-lived child and immediately waitpid() it.
//
// This is intentionally sequential from userland. On a single CPU this still
// exercises the race because waitpid() blocks while the child runs, exits,
// becomes a zombie and can be observed by the reaper before the parent resumes.

#include "../../syscall.h"

#define ITERATIONS 100

static void print_int(const char *prefix, int value) {
  char buf[16];
  int i = 0;
  uint32_t v;

  sys_print(prefix);
  if (value < 0) {
    sys_print("-");
    v = (uint32_t)(-(int64_t)value);
  } else {
    v = (uint32_t)value;
  }

  if (v == 0)
    buf[i++] = '0';

  while (v) {
    buf[i++] = (char)('0' + (v % 10));
    v /= 10;
  }

  for (int a = 0, b = i - 1; a < b; ++a, --b) {
    char t = buf[a];
    buf[a] = buf[b];
    buf[b] = t;
  }

  buf[i] = 0;
  sys_print(buf);
  sys_print("\n");
}

int main(void) {
  sys_print("[waitpid_stress] === waitpid/task lifetime stress ===\n");
  print_int("[waitpid_stress] Iteraciones: ", ITERATIONS);
  sys_print("[waitpid_stress] spawn + waitpid inmediato en cada ciclo.\n");
  sys_print("[waitpid_stress] Diseñado para funcionar tambien con 1 CPU.\n");

  int passed = 0;

  for (int i = 0; i < ITERATIONS; ++i) {
    int pid = sys_spawn("apps/filetest");
    if (pid < 0) {
      sys_print("[waitpid_stress] FALLO: spawn en iteracion ");
      print_int("", i);
      sys_exit(1);
    }

    int status = -1;
    int wpid = sys_waitpid(pid, &status, 0);

    if (wpid != pid || status != 0) {
      sys_print("[waitpid_stress] FALLO en iteracion ");
      print_int("", i);
      print_int("[waitpid_stress]   pid esperado: ", pid);
      print_int("[waitpid_stress]   waitpid: ", wpid);
      print_int("[waitpid_stress]   exit code: ", status);
      sys_exit(2);
    }

    ++passed;
  }

  sys_print("[waitpid_stress] ====================================\n");
  print_int("[waitpid_stress] Spawn+waitpid correctos: ", passed);
  sys_print("[waitpid_stress] ====================================\n");

  sys_exit(0);
  return 0;
}
