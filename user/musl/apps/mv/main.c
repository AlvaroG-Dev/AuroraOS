// mv: renombra fichero.
// usage: mv source dest

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>


int main(int argc, char **argv) {
  if (argc != 3) {
    fprintf(stderr, "usage: mv source dest\n");
    return 1;
  }
  if (rename(argv[1], argv[2]) != 0) {
    // Fallback: copiar+unlink si rename falla por cross-device.
    // Por ahora solo reportamos el error.
    fprintf(stderr, "mv: %s -> %s: %s\n", argv[1], argv[2], strerror(errno));
    return 1;
  }
  return 0;
}