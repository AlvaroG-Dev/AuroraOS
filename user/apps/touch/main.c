#include "../../lib/file.h"

// Sin timestamps reales todavía: si el fichero no existe, lo crea; si
// existe, no hace nada (equivale a "tocar" el mtime a nivel usuario).
// EEXIST = -17 en este kernel.
int main(int argc, char **argv) {
  if (argc < 2) {
    puts("uso: touch <file>...");
    return 1;
  }
  int rc = 0;
  for (int i = 1; i < argc; i++) {
    int r = create(argv[i]);
    if (r != 0 && r != -17) {
      printf("touch: fallo %s (%d)\n", argv[i], r);
      rc = 1;
    }
  }
  return rc;
}