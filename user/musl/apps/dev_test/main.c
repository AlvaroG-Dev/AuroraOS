// dev_test: mknod/mknodat de char devices bajo /dev, stat, readdir, unlink.
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <unistd.h>

static int fails;
#define CHECK(c, name)                                                         \
  do {                                                                         \
    if (c)                                                                     \
      printf("[dev_test] PASS %s\n", name);                                    \
    else {                                                                     \
      printf("[dev_test] FAIL %s (errno=%d)\n", name, errno);                  \
      fails++;                                                                 \
    }                                                                          \
  } while (0)

static int in_dir(const char *dir, const char *name) {
  DIR *d = opendir(dir);
  if (!d)
    return 0;
  int found = 0;
  struct dirent *e;
  while ((e = readdir(d)))
    if (!strcmp(e->d_name, name))
      found = 1;
  closedir(d);
  return found;
}

int main(void) {
  struct stat st;
  unlink("/dev/null2");
  CHECK(mknod("/dev/null2", S_IFCHR | 0666, makedev(1, 3)) == 0,
        "mknod /dev/null2 c 1 3");
  CHECK(stat("/dev/null2", &st) == 0 && S_ISCHR(st.st_mode) &&
            major(st.st_rdev) == 1 && minor(st.st_rdev) == 3,
        "stat: S_ISCHR y rdev 1:3");
  int fd = open("/dev/null2", O_WRONLY);
  CHECK(fd >= 0 && write(fd, "x", 1) == 1, "escribir en el alias de /dev/null");
  if (fd >= 0)
    close(fd);
  fd = open("/dev/null2", O_RDONLY);
  char c;
  CHECK(fd >= 0 && read(fd, &c, 1) == 0, "leer del alias da EOF");
  if (fd >= 0)
    close(fd);
  CHECK(in_dir("/dev", "null2"), "aparece en readdir(/dev)");
  CHECK(mknod("/dev/null2", S_IFCHR | 0666, makedev(1, 3)) < 0 &&
            errno == EEXIST,
        "mknod duplicado -> EEXIST");
  CHECK(mknod("/dev/nodrv", S_IFCHR | 0666, makedev(200, 9)) < 0 &&
            errno == ENODEV,
        "mknod sin driver -> ENODEV");
  CHECK(mknodat(AT_FDCWD, "/dev/zero2", S_IFCHR | 0666, makedev(1, 5)) == 0,
        "mknodat zero");
  fd = open("/dev/zero2", O_RDONLY);
  char z[4] = {1, 1, 1, 1};
  CHECK(fd >= 0 && read(fd, z, 4) == 4 && !z[0] && !z[3], "zero2 lee ceros");
  if (fd >= 0)
    close(fd);
  CHECK(unlink("/dev/null2") == 0 && unlink("/dev/zero2") == 0, "unlink");
  CHECK(stat("/dev/null2", &st) < 0 && errno == ENOENT, "ya no existe");
  CHECK(unlink("/dev/null") < 0, "no se puede borrar un nodo estatico");
  CHECK(mknod("/tmp/foo", S_IFCHR | 0666, makedev(1, 3)) < 0,
        "mknod char fuera de /dev falla");
  // Limite de fds por proceso (>32 para compositores Wayland).
  {
    int fds[200], n = 0, cx_ok = 1;
    for (; n < 200; n++) {
      fds[n] = open("/dev/null", O_RDONLY | O_CLOEXEC);
      if (fds[n] < 0)
        break;
      if (!(fcntl(fds[n], F_GETFD) & FD_CLOEXEC))
        cx_ok = 0;
    }
    CHECK(n == 200, "abrir 200 fds");
    CHECK(cx_ok, "FD_CLOEXEC en fds > 32");
    int d = fcntl(fds[0], F_DUPFD_CLOEXEC, 150);
    CHECK(d >= 150, "F_DUPFD_CLOEXEC desde 150");
    for (int k = 0; k < n; k++)
      close(fds[k]);
    if (d >= 0)
      close(d);
  }
  printf("[dev_test] %s (%d fallos)\n", fails ? "FAILED" : "ALL PASS", fails);
  return fails ? 1 : 0;
}
