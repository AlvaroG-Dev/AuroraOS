// user/musl/apps/mmap_file_test/main.c
//
// [3.5] Verifica mmap file-backed.

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define TEST_FILE "/data/mmap_file_test"

static int g_fail = 0;
#define CHECK(cond, msg)                                                       \
  do {                                                                         \
    if (cond)                                                                  \
      printf("  [PASS] %s\n", msg);                                            \
    else {                                                                     \
      printf("  [FAIL] %s (errno=%d)\n", msg, errno);                          \
      g_fail++;                                                                \
    }                                                                          \
  } while (0)

int main(void) {
  const char content[] = "Hello from mmap file-backed!\n";
  size_t content_len = sizeof(content) - 1;

  // 1. Escribir el fichero.
  int fd = open(TEST_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  CHECK(fd >= 0, "open O_CREAT");
  if (fd < 0)
    return 1;
  ssize_t w = write(fd, content, content_len);
  CHECK(w == (ssize_t)content_len, "write content");
  close(fd);

  // 2. Abrir para mmap.
  fd = open(TEST_FILE, O_RDONLY);
  CHECK(fd >= 0, "open O_RDONLY");
  if (fd < 0)
    return 1;

  // 3. mmap file-backed.
  void *p = mmap(NULL, 4096, PROT_READ, MAP_PRIVATE, fd, 0);
  CHECK(p != MAP_FAILED, "mmap file-backed");
  if (p == MAP_FAILED) {
    close(fd);
    return 1;
  }

  // 4. Leer desde la memoria mapeada. La primera lectura dispara un
  //    #PF; el demand pager lee del fichero.
  char buf[64];
  memcpy(buf, p, content_len);
  buf[content_len] = '\0';
  CHECK(memcmp(buf, content, content_len) == 0, "contenido mapeado OK");

  // 5. Bytes más allá del EOF deben ser 0.
  CHECK(((char *)p)[content_len] == 0, "resto de la página a cero");

  // 6. munmap y close.
  CHECK(munmap(p, 4096) == 0, "munmap");
  close(fd);

  // 7. close(fd) ANTES de munmap (POSIX: mapping sigue válido).
  fd = open(TEST_FILE, O_RDONLY);
  CHECK(fd >= 0, "open #2");
  void *p2 = mmap(NULL, 4096, PROT_READ, MAP_PRIVATE, fd, 0);
  CHECK(p2 != MAP_FAILED, "mmap #2");
  close(fd);
  char buf2[64];
  memcpy(buf2, p2, content_len);
  buf2[content_len] = '\0';
  CHECK(memcmp(buf2, content, content_len) == 0, "contenido tras close(fd)");
  munmap(p2, 4096);

  // 8. Limpiar.
  unlink(TEST_FILE);

  printf("=== %s (%d fallos) ===\n", g_fail ? "FAIL" : "PASS", g_fail);
  return g_fail ? 1 : 0;
}