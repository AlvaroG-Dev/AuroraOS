// shm_test: camino wl_shm. Un proceso crea un memfd, lo mapea MAP_SHARED y se
// lo pasa por SCM_RIGHTS a otro proceso (exec-less fork) que lo mapea y escribe;
// el primero ve los cambios. Tambien ftruncate (resize del pool) y sellos.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

static int fails;
#define CHECK(c, name)                                                         \
  do {                                                                         \
    if (c)                                                                     \
      printf("[shm_test] PASS %s\n", name);                                    \
    else {                                                                     \
      printf("[shm_test] FAIL %s (errno=%d)\n", name, errno);                  \
      fails++;                                                                 \
    }                                                                          \
  } while (0)

int main(void) {
  int sv[2];
  CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair");
  pid_t pid = fork();
  if (pid == 0) {
    close(sv[0]);
    char b;
    char cbuf[CMSG_SPACE(sizeof(int))];
    struct iovec iov = {&b, 1};
    struct msghdr mh = {.msg_iov = &iov, .msg_iovlen = 1, .msg_control = cbuf,
                        .msg_controllen = sizeof(cbuf)};
    if (recvmsg(sv[1], &mh, 0) != 1)
      _exit(10);
    struct cmsghdr *c = CMSG_FIRSTHDR(&mh);
    if (!c || c->cmsg_type != SCM_RIGHTS)
      _exit(11);
    int fd;
    memcpy(&fd, CMSG_DATA(c), sizeof(int));
    char *p = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED)
      _exit(12);
    if (memcmp(p, "from-compositor", 15) != 0)
      _exit(13);
    memcpy(p + 4096, "from-client", 12);
    write(sv[1], "k", 1);
    _exit(0);
  }
  close(sv[1]);
  int m = memfd_create("wl_shm", MFD_CLOEXEC | MFD_ALLOW_SEALING);
  CHECK(m >= 0, "memfd_create");
  CHECK(ftruncate(m, 8192) == 0, "ftruncate 8192");
  char *p = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_SHARED, m, 0);
  CHECK(p != MAP_FAILED, "mmap MAP_SHARED");
  memcpy(p, "from-compositor", 16);

  char b = 'x';
  char cbuf[CMSG_SPACE(sizeof(int))];
  memset(cbuf, 0, sizeof(cbuf));
  struct iovec iov = {&b, 1};
  struct msghdr mh = {.msg_iov = &iov, .msg_iovlen = 1, .msg_control = cbuf,
                      .msg_controllen = sizeof(cbuf)};
  struct cmsghdr *c = CMSG_FIRSTHDR(&mh);
  c->cmsg_level = SOL_SOCKET;
  c->cmsg_type = SCM_RIGHTS;
  c->cmsg_len = CMSG_LEN(sizeof(int));
  memcpy(CMSG_DATA(c), &m, sizeof(int));
  CHECK(sendmsg(sv[0], &mh, 0) == 1, "sendmsg memfd");
  char k = 0;
  CHECK(read(sv[0], &k, 1) == 1 && k == 'k', "el cliente confirma");
  int st = 0;
  waitpid(pid, &st, 0);
  CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "cliente sale 0");
  if (WIFEXITED(st) && WEXITSTATUS(st) != 0)
    printf("[shm_test]   cliente salio con %d\n", WEXITSTATUS(st));
  CHECK(memcmp(p + 4096, "from-client", 12) == 0,
        "el compositor ve la escritura del cliente (MAP_SHARED)");

  // Resize del pool + sellos
  CHECK(ftruncate(m, 16384) == 0, "ftruncate crece el pool");
  char *p2 = mmap(NULL, 16384, PROT_READ, MAP_SHARED, m, 0);
  CHECK(p2 != MAP_FAILED && memcmp(p2 + 4096, "from-client", 12) == 0,
        "remap mayor conserva datos");
  CHECK(fcntl(m, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_SEAL) == 0, "F_ADD_SEALS");
  CHECK(ftruncate(m, 4096) < 0 && errno == EPERM, "sello SHRINK respetado");

  printf("[shm_test] %s (%d fallos)\n", fails ? "FAILED" : "ALL PASS", fails);
  return fails ? 1 : 0;
}
