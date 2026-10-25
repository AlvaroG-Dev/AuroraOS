// unix_test: AF_UNIX (socketpair, bind/listen/accept/connect, SCM_RIGHTS,
// SO_PEERCRED, poll, EOF). Lo que usa libwayland, sin libwayland.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

static int fails;
#define CHECK(c, name)                                                         \
  do {                                                                         \
    if (c)                                                                     \
      printf("[unix_test] PASS %s\n", name);                                   \
    else {                                                                     \
      printf("[unix_test] FAIL %s (errno=%d)\n", name, errno);                 \
      fails++;                                                                 \
    }                                                                          \
  } while (0)

static int send_fd(int sock, int fd, const char *msg) {
  struct iovec iov = {(void *)msg, strlen(msg)};
  char cbuf[CMSG_SPACE(sizeof(int))];
  memset(cbuf, 0, sizeof(cbuf));
  struct msghdr mh = {.msg_iov = &iov, .msg_iovlen = 1, .msg_control = cbuf,
                      .msg_controllen = sizeof(cbuf)};
  struct cmsghdr *c = CMSG_FIRSTHDR(&mh);
  c->cmsg_level = SOL_SOCKET;
  c->cmsg_type = SCM_RIGHTS;
  c->cmsg_len = CMSG_LEN(sizeof(int));
  memcpy(CMSG_DATA(c), &fd, sizeof(int));
  return (int)sendmsg(sock, &mh, MSG_NOSIGNAL);
}

static int recv_fd(int sock, char *buf, size_t n, int *fd_out) {
  struct iovec iov = {buf, n};
  char cbuf[CMSG_SPACE(sizeof(int) * 4)];
  struct msghdr mh = {.msg_iov = &iov, .msg_iovlen = 1, .msg_control = cbuf,
                      .msg_controllen = sizeof(cbuf)};
  ssize_t r = recvmsg(sock, &mh, MSG_CMSG_CLOEXEC);
  *fd_out = -1;
  if (r < 0)
    return (int)r;
  for (struct cmsghdr *c = CMSG_FIRSTHDR(&mh); c; c = CMSG_NXTHDR(&mh, c))
    if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS)
      memcpy(fd_out, CMSG_DATA(c), sizeof(int));
  return (int)r;
}

int main(void) {
  int sv[2];
  char buf[64];

  // 1. socketpair STREAM
  CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair stream");
  CHECK(write(sv[0], "hola", 4) == 4, "write");
  CHECK(read(sv[1], buf, sizeof(buf)) == 4 && !memcmp(buf, "hola", 4), "read");
  struct pollfd p = {sv[1], POLLIN, 0};
  CHECK(poll(&p, 1, 0) == 0, "poll vacio");
  write(sv[0], "x", 1);
  CHECK(poll(&p, 1, 100) == 1 && (p.revents & POLLIN), "poll con datos");
  read(sv[1], buf, 1);

  // 2. SCM_RIGHTS con un memfd
  int m = memfd_create("t", 0);
  CHECK(m >= 0, "memfd_create");
  write(m, "SHM!", 4);
  CHECK(send_fd(sv[0], m, "F") == 1, "sendmsg SCM_RIGHTS");
  int got = -1;
  CHECK(recv_fd(sv[1], buf, sizeof(buf), &got) == 1 && got >= 0 && got != m,
        "recvmsg SCM_RIGHTS");
  char rb[8] = {0};
  CHECK(got >= 0 && pread(got, rb, 4, 0) == 4 && !memcmp(rb, "SHM!", 4),
        "fd recibido comparte el memfd");
  close(m);
  CHECK(got >= 0 && pread(got, rb, 4, 0) == 4, "fd sobrevive al close del emisor");
  close(got);

  // 3. EOF y EPIPE
  close(sv[0]);
  CHECK(read(sv[1], buf, sizeof(buf)) == 0, "EOF al cerrar el par");
  CHECK(send(sv[1], "x", 1, MSG_NOSIGNAL) < 0 && errno == EPIPE, "EPIPE");
  close(sv[1]);

  // 4. SOCK_DGRAM / SEQPACKET
  CHECK(socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) == 0, "socketpair dgram");
  send(sv[0], "aa", 2, 0);
  send(sv[0], "bbb", 3, 0);
  CHECK(recv(sv[1], buf, sizeof(buf), 0) == 2, "dgram limites 1");
  CHECK(recv(sv[1], buf, sizeof(buf), 0) == 3, "dgram limites 2");
  close(sv[0]);
  close(sv[1]);
  CHECK(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) == 0, "socketpair seqpacket");
  send(sv[0], "abc", 3, 0);
  CHECK(recv(sv[1], buf, 2, 0) == 2, "seqpacket trunca");
  close(sv[0]);
  close(sv[1]);

  // 5. bind / listen / accept / connect (ruta + abstracto) con fork
  const char *paths[2] = {"/tmp/unix_test.sock", "\0unix_test_abs"};
  for (int variant = 0; variant < 2; variant++) {
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    socklen_t sl;
    if (variant == 0) {
      strcpy(sa.sun_path, paths[0]);
      sl = sizeof(sa);
      unlink(paths[0]);
    } else {
      memcpy(sa.sun_path, "\0unix_test_abs", 14);
      sl = offsetof(struct sockaddr_un, sun_path) + 14;
    }
    const char *tag = variant ? "abstracto" : "ruta";
    char name[64];
    int ls = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    CHECK(ls >= 0, "socket");
    snprintf(name, sizeof(name), "bind %s", tag);
    CHECK(bind(ls, (struct sockaddr *)&sa, sl) == 0, name);
    int dup = socket(AF_UNIX, SOCK_STREAM, 0);
    CHECK(bind(dup, (struct sockaddr *)&sa, sl) < 0 && errno == EADDRINUSE,
          "bind duplicado -> EADDRINUSE");
    close(dup);
    CHECK(listen(ls, 4) == 0, "listen");
    int cs0 = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un bad = sa;
    bad.sun_path[variant ? 5 : 1] = 'Z';
    CHECK(connect(cs0, (struct sockaddr *)&bad, sl) < 0, "connect inexistente falla");
    close(cs0);

    pid_t pid = fork();
    if (pid == 0) {
      int c = socket(AF_UNIX, SOCK_STREAM, 0);
      if (connect(c, (struct sockaddr *)&sa, sl) != 0)
        _exit(10);
      struct ucred uc;
      socklen_t ul = sizeof(uc);
      if (getsockopt(c, SOL_SOCKET, SO_PEERCRED, &uc, &ul) != 0)
        _exit(11);
      write(c, "ping", 4);
      char r[8] = {0};
      if (read(c, r, 4) != 4 || memcmp(r, "pong", 4))
        _exit(12);
      _exit(0);
    }
    int a = accept4(ls, NULL, NULL, SOCK_CLOEXEC);
    CHECK(a >= 0, "accept4");
    struct ucred uc;
    socklen_t ul = sizeof(uc);
    CHECK(getsockopt(a, SOL_SOCKET, SO_PEERCRED, &uc, &ul) == 0 &&
              uc.pid == pid,
          "SO_PEERCRED = pid del cliente");
    char r[8] = {0};
    CHECK(read(a, r, 4) == 4 && !memcmp(r, "ping", 4), "ping");
    write(a, "pong", 4);
    int st = 0;
    waitpid(pid, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "cliente termina OK");
    close(a);
    close(ls);
    if (variant == 0)
      unlink(paths[0]);
  }

  printf("[unix_test] %s (%d fallos)\n", fails ? "FAILED" : "ALL PASS", fails);
  return fails ? 1 : 0;
}
