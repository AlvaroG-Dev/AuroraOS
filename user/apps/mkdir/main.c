#include "../../lib/file.h"

int main(int argc, char **argv) {
  if (argc < 2) {
    puts("uso: mkdir <path>...");
    return 1;
  }
  int rc = 0;
  for (int i = 1; i < argc; i++) {
    if (mkdir(argv[i]) != 0) {
      printf("mkdir: fallo '%s'\n", argv[i]);
      rc = 1;
    } else {
      printf("mkdir: '%s' OK\n", argv[i]);
    }
  }
  return rc;
}