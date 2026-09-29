// user/musl/apps/proc_test/main.c
//
// [3.3.a/b] Verifica /proc básico.

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

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

static int slurp(const char *path, char *out, size_t cap) {
  int fd = open(path, O_RDONLY);
  if (fd < 0)
    return -1;
  ssize_t n = read(fd, out, cap - 1);
  close(fd);
  if (n < 0)
    return -1;
  out[n] = '\0';
  return (int)n;
}

int main(void) {
  printf("=== proc_test ===\n");
  char buf[4096];

  // 1. /proc/version.
  int n = slurp("/proc/version", buf, sizeof(buf));
  CHECK(n > 0, "/proc/version lectura > 0");
  if (n > 0) {
    printf("         /proc/version = %.60s...\n", buf);
    CHECK(strstr(buf, "Aurora") != NULL, "/proc/version contiene 'Aurora'");
  }

  // 2. /proc/uptime.
  n = slurp("/proc/uptime", buf, sizeof(buf));
  CHECK(n > 0, "/proc/uptime lectura > 0");
  if (n > 0) {
    printf("         /proc/uptime = %s", buf);
    CHECK(strchr(buf, '.') != NULL, "/proc/uptime tiene formato float");
  }

  // 3. /proc/meminfo.
  n = slurp("/proc/meminfo", buf, sizeof(buf));
  CHECK(n > 0, "/proc/meminfo lectura > 0");
  if (n > 0) {
    CHECK(strstr(buf, "MemTotal:") != NULL, "meminfo contiene MemTotal:");
    CHECK(strstr(buf, "MemFree:") != NULL, "meminfo contiene MemFree:");
    // Imprimir solo las dos primeras líneas.
    int lines = 0;
    for (char *p = buf; *p && lines < 3; p++) {
      if (*p == '\n')
        lines++;
    }
  }

  // 4. /proc/stat.
  n = slurp("/proc/stat", buf, sizeof(buf));
  CHECK(n > 0, "/proc/stat lectura > 0");
  if (n > 0)
    CHECK(strstr(buf, "cpu") != NULL, "/proc/stat contiene 'cpu'");

  // 5. /proc/self es un symlink → /proc/<pid>. Como /proc/<pid> aún
  //    no existe, readlink debe devolver el target correcto pero
  //    stat seguirá el link y fallará con ENOENT. Verificamos readlink.
  char target[64] = {0};
  ssize_t r = readlink("/proc/self", target, sizeof(target) - 1);
  if (r > 0) {
    target[r] = '\0';
    printf("         /proc/self -> %s\n", target);
    char expected[32];
    snprintf(expected, sizeof(expected), "/proc/%d", (int)getpid());
    CHECK(strcmp(target, expected) == 0, "/proc/self apunta al pid actual");
  } else {
    printf("  [SKIP] /proc/self symlink\n");
  }

  // 6. Fichero inexistente.
  errno = 0;
  int fd = open("/proc/no_existe", O_RDONLY);
  CHECK(fd < 0 && errno == ENOENT, "/proc/no_existe → ENOENT");
  if (fd >= 0)
    close(fd);

  printf("=== %s (%d fallos) ===\n", g_fail ? "FAIL" : "PASS", g_fail);
  return g_fail ? 1 : 0;
}