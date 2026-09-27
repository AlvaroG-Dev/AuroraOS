#include "../../lib/file.h"
#include "../../lib/string.h"

int main(int argc, char **argv) {
  // [Unix] Sin args: leer de stdin (fd 0). Con args: uno o varios
  // ficheros. Combinación libre: `cat a b` lee a, luego b, luego nada.
  if (argc < 2) {
    char buf[512];
    int64_t n;
    int64_t total = 0;
    while ((n = read(0, buf, sizeof(buf))) > 0) {
      write(1, buf, (size_t)n);
      total += n;
    }
    // Solo imprime el resumen si ha leído algo. Así `cat | cat` no
    // ensucia la salida con un "[cat] N bytes leídos" por cada etapa.
    (void)total;
    return 0;
  }

  int rc = 0;
  for (int i = 1; i < argc; i++) {
    int fd = open(argv[i], O_RDONLY);
    if (fd < 0) {
      printf("[cat] no existe %s\n", argv[i]);
      rc = 1;
      continue;
    }
    char buf[512];
    int64_t n;
    int64_t total = 0;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
      write(1, buf, (size_t)n);
      total += n;
    }
    close(fd);
    printf("\n[cat] %u bytes leídos\n", (unsigned int)total);
  }
  return rc;
}