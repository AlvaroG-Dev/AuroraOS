// sockfile_test: bind() de AF_UNIX crea un inodo S_IFSOCK en el VFS.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

static int g_fail;
#define CHECK(c, ...) do { if (c) { printf("[sockfile_test] PASS " __VA_ARGS__); printf("\n"); } \
  else { printf("[sockfile_test] FAIL " __VA_ARGS__); printf(" (errno=%d)\n", errno); g_fail++; } } while (0)

static int mk(const char *path, int do_listen) {
  int s = socket(AF_UNIX, SOCK_STREAM, 0);
  struct sockaddr_un a = {.sun_family = AF_UNIX};
  strncpy(a.sun_path, path, sizeof(a.sun_path) - 1);
  if (bind(s, (struct sockaddr *)&a, sizeof(a)) < 0) {
    int e = errno;
    close(s);
    errno = e;
    return -1;
  }
  if (do_listen)
    listen(s, 4);
  return s;
}

int main(void) {
  const char *P = "/tmp/sockfile_test.sock";
  unlink(P);
  int s = mk(P, 1);
  CHECK(s >= 0, "bind a %s", P);
  struct stat st;
  CHECK(stat(P, &st) == 0 && S_ISSOCK(st.st_mode), "stat: S_ISSOCK (mode=%o)", (unsigned)st.st_mode);
  CHECK(access(P, F_OK) == 0, "access(F_OK)");
  errno = 0;
  int s2 = mk(P, 0);
  CHECK(s2 < 0 && errno == EADDRINUSE, "segundo bind a la misma ruta: EADDRINUSE");

  int c = socket(AF_UNIX, SOCK_STREAM, 0);
  struct sockaddr_un a = {.sun_family = AF_UNIX};
  strncpy(a.sun_path, P, sizeof(a.sun_path) - 1);
  CHECK(connect(c, (struct sockaddr *)&a, sizeof(a)) == 0, "connect por la ruta");
  close(c);

  errno = 0;
  int fd = open(P, O_RDWR);
  CHECK(fd < 0, "open() de un socket falla (errno=%d)", errno);
  if (fd >= 0) close(fd);

  // unlink: el socket vive pero ya no se puede alcanzar; la ruta se puede reutilizar.
  CHECK(unlink(P) == 0, "unlink del fichero socket");
  c = socket(AF_UNIX, SOCK_STREAM, 0);
  errno = 0;
  CHECK(connect(c, (struct sockaddr *)&a, sizeof(a)) < 0 && (errno == ENOENT || errno == ECONNREFUSED),
        "connect tras unlink falla (errno=%d)", errno);
  close(c);
  int s3 = mk(P, 1);
  CHECK(s3 >= 0, "se puede volver a hacer bind a la ruta tras unlink");
  CHECK(stat(P, &st) == 0 && S_ISSOCK(st.st_mode), "el nuevo fichero es S_ISSOCK");
  close(s);
  // cerrar un socket NO borra el fichero (como Linux)
  close(s3);
  CHECK(stat(P, &st) == 0, "el fichero persiste al cerrar el socket");
  errno = 0;
  CHECK(mk(P, 0) < 0 && errno == EADDRINUSE, "ruta persistente: bind da EADDRINUSE");
  unlink(P);

  printf("[sockfile_test] %s\n", g_fail ? "FAILED" : "ALL PASS");
  return g_fail ? 1 : 0;
}
