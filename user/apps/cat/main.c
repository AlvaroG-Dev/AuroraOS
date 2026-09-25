// apps/cat/main.c
#include "../../lib/file.h"
#include "../../lib/string.h"
#include "../../syscall.h"


int main(void) {
  // Sin args → lee /boot/EFI/BOOT/BOOTX64.EFI no, mejor un archivo
  // pequeño. Como aún no hay argumentos en sys_spawn, hardcodeamos.
  const char *path = "/boot/EFI/BOOT/BOOTX64.EFI";

  int fd = open(path, O_RDONLY);
  if (fd < 0) {
    sys_print("[cat] no se pudo abrir: ");
    sys_print(path);
    sys_print("\n");
    sys_exit(1);
  }

  char buf[512];
  int64_t total = 0;
  while (1) {
    int64_t n = read(fd, buf, sizeof(buf));
    if (n < 0) {
      sys_print("[cat] error de lectura\n");
      close(fd);
      sys_exit(2);
    }
    if (n == 0)
      break;
    // Imprimir como texto imprimible; no-op para binarios pero útil
    // para verificar que lee.
    for (int64_t i = 0; i < n; i++) {
      char c = buf[i];
      if (c >= 32 && c < 127)
        write(1, &c, 1);
    }
    total += n;
  }
  close(fd);

  char b[32];
  sys_print("\n[cat] ");
  itoa(total, b, 10);
  sys_print(b);
  sys_print(" bytes leídos\n");
  sys_exit(0);
  return 0;
}