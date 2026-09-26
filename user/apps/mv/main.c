#include "../../lib/file.h"

int main(int argc, char **argv) {
  if (argc != 3) {
    puts("uso: mv <src> <dst>");
    return 1;
  }
  int rc = rename_path(argv[1], argv[2]);
  if (rc != 0) {
    printf("mv: fallo '%s' -> '%s' (rc=%d)\n", argv[1], argv[2], rc);
    return 1;
  }
  printf("mv: '%s' -> '%s' OK\n", argv[1], argv[2]);
  return 0;
}