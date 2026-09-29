// user/musl/apps/pivot_test/main.c
//
// [2.2] Verifica vfs_pivot_root().
//
// Precondición: /data es un mount FAT32 escribible.
//
// El test crea /data/oldroot, hace pivot_root(/data, /data/oldroot),
// escribe un fichero en el nuevo / (que debe ir a FAT32), y comprueba
// que /oldroot sigue siendo el tarfs (el antiguo root).

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#define SYS_PIVOT_ROOT 155

int main(void) {
  printf("=== pivot_test ===\n");

  // 1. Crear el directorio put_old.
  if (mkdir("/data/oldroot", 0755) != 0 && errno != EEXIST) {
    printf("FAIL: mkdir /data/oldroot: errno=%d\n", errno);
    return 1;
  }
  printf("[pivot] /data/oldroot listo\n");

  // 2. pivot_root.
  long r = syscall(SYS_PIVOT_ROOT, "/data", "/data/oldroot");
  if (r != 0) {
    printf("FAIL: pivot_root -> %ld errno=%d\n", r, errno);
    return 1;
  }
  printf("[pivot] OK: pivot_root(/data, /data/oldroot)\n");

  // 3. / es ahora FAT32. Escribimos un fichero.
  FILE *f = fopen("/pivot_was_here.txt", "w");
  if (!f) {
    printf("FAIL: no puedo crear /pivot_was_here.txt errno=%d\n", errno);
    return 1;
  }
  fprintf(f, "hello from pivot_root\n");
  fclose(f);
  printf("[pivot] OK: escrito /pivot_was_here.txt (FAT32)\n");

  // 4. /oldroot debe ser el tarfs (el antiguo /).
  FILE *g = fopen("/oldroot/system/config.txt", "r");
  if (!g) {
    printf("FAIL: /oldroot/system/config.txt no existe errno=%d\n", errno);
    return 1;
  }
  char buf[128] = {0};
  fgets(buf, sizeof(buf), g);
  fclose(g);
  printf("[pivot] OK: /oldroot/system/config.txt = '%s'\n", buf);

  // 5. Verificar que NO podemos leer /system/config.txt en el nuevo /.
  FILE *h = fopen("/system/config.txt", "r");
  if (h) {
    printf("WARN: /system/config.txt sigue visible (¿FAT32 tiene system/?)\n");
    fclose(h);
  } else {
    printf(
        "[pivot] OK: /system/config.txt no existe (tarfs ya no está en /)\n");
  }

  printf("=== PASS ===\n");
  return 0;
}