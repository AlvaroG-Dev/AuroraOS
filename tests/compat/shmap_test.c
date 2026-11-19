// Memoria compartida como la usa wineserver: fichero en /tmp o memfd, mmap
// MAP_SHARED en dos procesos (fd pasado por SCM_RIGHTS), escrituras por mmap
// y por pwrite/ftruncate visibles en el otro lado.
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

static int pass, fail;
#define CHECK(c, m) do { if (c) pass++; else { fail++; printf("FALLO: %s\n", m); } } while (0)

static int send_fd(int s, int fd) {
  char b = 'x'; struct iovec io = {&b, 1};
  char cb[CMSG_SPACE(sizeof(int))]; memset(cb, 0, sizeof cb);
  struct msghdr m = {0}; m.msg_iov = &io; m.msg_iovlen = 1; m.msg_control = cb; m.msg_controllen = sizeof cb;
  struct cmsghdr *c = CMSG_FIRSTHDR(&m); c->cmsg_level = SOL_SOCKET; c->cmsg_type = SCM_RIGHTS; c->cmsg_len = CMSG_LEN(sizeof(int));
  memcpy(CMSG_DATA(c), &fd, sizeof(int));
  return sendmsg(s, &m, 0);
}
static int recv_fd(int s) {
  char b; struct iovec io = {&b, 1};
  char cb[CMSG_SPACE(sizeof(int))];
  struct msghdr m = {0}; m.msg_iov = &io; m.msg_iovlen = 1; m.msg_control = cb; m.msg_controllen = sizeof cb;
  if (recvmsg(s, &m, 0) <= 0) return -1;
  struct cmsghdr *c = CMSG_FIRSTHDR(&m); int fd; memcpy(&fd, CMSG_DATA(c), sizeof(int)); return fd;
}

static void run(const char *what, int fd) {
  int sv[2]; socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
  CHECK(ftruncate(fd, 65536) == 0, "ftruncate");
  char *p = mmap(0, 65536, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  CHECK(p != MAP_FAILED, "mmap servidor");
  pid_t c = fork();
  if (c == 0) {
    close(sv[0]);
    int f = recv_fd(sv[1]);
    char *q = mmap(0, 65536, PROT_READ, MAP_SHARED, f, 0);
    char ok = 0;
    if (q != MAP_FAILED) {
      for (int i = 0; i < 400 && q[40000] != 'S'; i++) usleep(5000);
      ok = q[40000] == 'S' && q[100] == 'P';
      char b = 1; write(sv[1], &b, 1);  // esperar segunda escritura
      read(sv[1], &b, 1);
      ok = ok && q[50000] == 'T' && q[60000] == 'W';
    }
    _exit(ok ? 0 : 1);
  }
  close(sv[1]);
  int f2 = open("/dev/null", O_RDONLY);  // otro número de fd
  close(f2);
  pwrite(fd, "P", 1, 100);   // antes de que el hijo mapee
  send_fd(sv[0], fd);
  p[40000] = 'S';
  char b; read(sv[0], &b, 1);
  p[50000] = 'T';
  pwrite(fd, "W", 1, 60000);
  write(sv[0], &b, 1);
  int st; waitpid(c, &st, 0);
  char msg[80]; snprintf(msg, sizeof msg, "%s: el otro proceso ve mmap y pwrite", what);
  CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, msg);
  char r = 0; pread(fd, &r, 1, 40000);
  snprintf(msg, sizeof msg, "%s: pread ve lo escrito por mmap", what);
  CHECK(r == 'S', msg);
}

int main(void) {
  int fd = open("/tmp/shmap-test", O_RDWR | O_CREAT | O_TRUNC, 0600);
  unlink("/tmp/shmap-test");
  run("fichero /tmp", fd);
  int m = memfd_create("wine-mapping", MFD_ALLOW_SEALING);
  run("memfd", m);
  printf("SHMAP-TEST: %d ok, %d fallos %s\n", pass, fail, fail ? "FAIL" : "ALL PASS");
  return fail != 0;
}
