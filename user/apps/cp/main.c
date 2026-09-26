#include "../../lib/file.h"

int main(int argc, char **argv) {
  if (argc < 3) {
    puts("uso: cp <src> <dst>");
    return 1;
  }
  if (copy_file(argv[1], argv[2]) != 0) {
    printf("cp: fallo '%s' -> '%s'\n", argv[1], argv[2]);
    return 1;
  }
  printf("cp: '%s' -> '%s' OK\n", argv[1], argv[2]);
  return 0;
}