#include "../../lib/file.h"

int main(int argc, char **argv) {
  if (argc < 2) {
    puts("uso: rm <path>...");
    return 1;
  }
  int rc = 0;
  for (int i = 1; i < argc; i++) {
    if (unlink(argv[i]) != 0) {
      printf("rm: fallo '%s'\n", argv[i]);
      rc = 1;
    } else {
      printf("rm: '%s' OK\n", argv[i]);
    }
  }
  return rc;
}