// user/musl/apps/proc_pid_test/main.c
//
// [3.3.d] Verifica /proc/<pid>/{stat,status,cmdline}.

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
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

int main(int argc, char **argv) {
  printf("=== proc_pid_test ===\n");
  char buf[2048];
  int pid = (int)getpid();

  // 1. /proc/self/stat
  int n = slurp("/proc/self/stat", buf, sizeof(buf));
  CHECK(n > 0, "/proc/self/stat lectura > 0");
  if (n > 0) {
    printf("         stat = %.80s...\n", buf);
    // Debe empezar por "<pid> ("
    char expected[32];
    snprintf(expected, sizeof(expected), "%d (", pid);
    CHECK(strncmp(buf, expected, strlen(expected)) == 0,
          "stat empieza por '<pid> ('");
  }

  // 2. /proc/self/status
  n = slurp("/proc/self/status", buf, sizeof(buf));
  CHECK(n > 0, "/proc/self/status lectura > 0");
  if (n > 0) {
    CHECK(strstr(buf, "Name:") != NULL, "status contiene Name:");
    CHECK(strstr(buf, "Pid:") != NULL, "status contiene Pid:");
    CHECK(strstr(buf, "PPid:") != NULL, "status contiene PPid:");
  }

  // 3. /proc/self/cmdline: argv[0]\0[argv[1]...]\0
  n = slurp("/proc/self/cmdline", buf, sizeof(buf));
  CHECK(n > 0, "/proc/self/cmdline lectura > 0");
  if (n > 0) {
    // Mostrar como 'arg1 arg2 ...'
    printf("         cmdline: '");
    for (int i = 0; i < n; i++) {
      if (buf[i] == '\0')
        putchar(' ');
      else
        putchar(buf[i]);
    }
    printf("'\n");
    // argv[0] debe ser el path del binario.
    CHECK(argv[0] && strcmp(buf, argv[0]) == 0, "cmdline empieza por argv[0]");
  }

  // 4. readdir de /proc debe listar pids.
  int found_pid_dir = 0;
  DIR *d = opendir("/proc");
  if (d) {
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
      if (atoi(e->d_name) == pid) {
        found_pid_dir = 1;
        break;
      }
    }
    closedir(d);
  }
  CHECK(found_pid_dir, "/proc lista el pid actual");

  // 5. /proc/self es symlink a /proc/<pid>.
  char target[64] = {0};
  ssize_t r = readlink("/proc/self", target, sizeof(target) - 1);
  if (r > 0) {
    target[r] = '\0';
    char expected[32];
    snprintf(expected, sizeof(expected), "/proc/%d", pid);
    CHECK(strcmp(target, expected) == 0, "/proc/self -> /proc/<pid>");
  }

  printf("=== %s (%d fallos) ===\n", g_fail ? "FAIL" : "PASS", g_fail);
  return g_fail ? 1 : 0;
}