// user/musl/apps/symlink_test/main.c
//
// [3.1] Verifica readlink/lstat/stat con symlinks.

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
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

int main(void) {
  printf("=== symlink_test ===\n");

  // Buscar un symlink conocido en el tarfs.
  const char *candidates[] = {"/bin/sh",   "/bin/ls",   "/bin/cat",
                              "/bin/echo", "/bin/grep", NULL};
  const char *sl = NULL;
  char target[256] = {0};
  for (int i = 0; candidates[i]; i++) {
    ssize_t r = readlink(candidates[i], target, sizeof(target) - 1);
    if (r > 0) {
      target[r] = '\0';
      sl = candidates[i];
      break;
    }
  }
  if (!sl) {
    printf("[symlink] SKIP: no hay symlinks en /bin\n");
    return 0;
  }
  printf("[symlink] detectado %s -> %s\n", sl, target);

  // 1. readlink de un directorio devuelve EINVAL.
  errno = 0;
  ssize_t r = readlink("/bin", target, sizeof(target));
  CHECK(r < 0 && errno == EINVAL, "readlink(dir) -> EINVAL");

  // 2. readlink de un path que no existe -> ENOENT.
  errno = 0;
  r = readlink("/no_existe_xyz", target, sizeof(target));
  CHECK(r < 0 && errno == ENOENT, "readlink(nonexistent) -> ENOENT");

  // 3. lstat del symlink devuelve S_IFLNK.
  struct stat st;
  errno = 0;
  if (lstat(sl, &st) != 0) {
    CHECK(0, "lstat(symlink)");
  } else {
    CHECK(S_ISLNK(st.st_mode), "lstat(symlink) -> S_ISLNK");
  }

  // 4. stat del symlink NO es S_IFLNK (sigue el link).
  if (stat(sl, &st) != 0) {
    CHECK(0, "stat(symlink)");
  } else {
    CHECK(!S_ISLNK(st.st_mode), "stat(symlink) NO es S_IFLNK (sigue el link)");
  }

  // 5. symlink() debe fallar en tarfs RO.
  errno = 0;
  int rc = symlink("/etc", "/tmp/newlink");
  CHECK(rc < 0, "symlink() falla en tarfs RO");

  printf("=== %s (%d fallos) ===\n", g_fail ? "FAIL" : "PASS", g_fail);
  return g_fail ? 1 : 0;
}