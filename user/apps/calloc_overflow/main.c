#include "../../lib/malloc.h"
#include "../../syscall.h"

int main(void) {
  sys_print("[calloc_overflow] === calloc overflow regression ===\n");

  void *p = calloc((size_t)-1, 2);
  if (p != NULL) {
    sys_print("[calloc_overflow] ERROR: overflow no fue rechazado\n");
    free(p);
    return 1;
  }

  sys_print("[calloc_overflow] OK: overflow rechazado\n");

  int *ok = (int *)calloc(16, sizeof(int));
  if (!ok) {
    sys_print("[calloc_overflow] ERROR: calloc normal fallo\n");
    return 1;
  }

  for (int i = 0; i < 16; i++) {
    if (ok[i] != 0) {
      sys_print("[calloc_overflow] ERROR: calloc normal no fue zeroed\n");
      free(ok);
      return 1;
    }
  }

  free(ok);
  sys_print("[calloc_overflow] calloc normal y overflow: PASA\n");
  return 0;
}
