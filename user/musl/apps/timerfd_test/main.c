// timerfd_test: timerfd_create/settime/gettime con read, poll y epoll.
#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <poll.h>
#include <sys/epoll.h>
#include <sys/inotify.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

static int fails;
#define CHECK(c, name)                                                         \
  do {                                                                         \
    if (c)                                                                     \
      printf("[timerfd_test] PASS %s\n", name);                                \
    else {                                                                     \
      printf("[timerfd_test] FAIL %s (errno=%d)\n", name, errno);              \
      fails++;                                                                 \
    }                                                                          \
  } while (0)

static long ms_now(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

int main(void) {
  int fd = timerfd_create(CLOCK_MONOTONIC, 0);
  CHECK(fd >= 0, "timerfd_create");

  struct itimerspec its = {{0, 0}, {0, 60 * 1000000L}};
  long t0 = ms_now();
  CHECK(timerfd_settime(fd, 0, &its, NULL) == 0, "settime one-shot 60ms");
  uint64_t n = 0;
  CHECK(read(fd, &n, 8) == 8 && n == 1, "read bloqueante devuelve 1");
  long dt = ms_now() - t0;
  CHECK(dt >= 55 && dt < 400, "espera ~60ms");
  if (dt < 55 || dt >= 400)
    printf("[timerfd_test]   dt=%ld\n", dt);

  struct itimerspec cur;
  CHECK(timerfd_gettime(fd, &cur) == 0 && cur.it_value.tv_sec == 0 &&
            cur.it_value.tv_nsec == 0,
        "gettime tras one-shot = 0");

  // Periodico + epoll
  its.it_interval.tv_nsec = 20 * 1000000L;
  its.it_value.tv_nsec = 20 * 1000000L;
  CHECK(timerfd_settime(fd, 0, &its, NULL) == 0, "settime periodico 20ms");
  int ep = epoll_create1(0);
  struct epoll_event ev = {.events = EPOLLIN, .data.fd = fd}, out;
  CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, fd, &ev) == 0, "epoll_ctl");
  CHECK(epoll_wait(ep, &out, 1, 500) == 1 && out.data.fd == fd, "epoll_wait");
  usleep(100 * 1000);
  n = 0;
  CHECK(read(fd, &n, 8) == 8 && n >= 4, "expiraciones acumuladas >= 4");
  if (n < 4)
    printf("[timerfd_test]   n=%llu\n", (unsigned long long)n);

  // Desarmar
  struct itimerspec zero = {{0, 0}, {0, 0}};
  CHECK(timerfd_settime(fd, 0, &zero, NULL) == 0, "desarmar");
  close(ep);
  close(fd);

  // No bloqueante
  fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
  CHECK(read(fd, &n, 8) < 0 && errno == EAGAIN, "TFD_NONBLOCK -> EAGAIN");
  // Absoluto
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  struct itimerspec abs = {{0, 0}, {now.tv_sec, now.tv_nsec + 30 * 1000000L}};
  if (abs.it_value.tv_nsec >= 1000000000L) {
    abs.it_value.tv_sec++;
    abs.it_value.tv_nsec -= 1000000000L;
  }
  CHECK(timerfd_settime(fd, TFD_TIMER_ABSTIME, &abs, NULL) == 0, "settime abs");
  usleep(80 * 1000);
  CHECK(read(fd, &n, 8) == 8 && n == 1, "timer absoluto vencio");
  close(fd);

  // pidfd_open: legible cuando el hijo termina
  pid_t child = fork();
  if (child == 0) {
    usleep(60 * 1000);
    _exit(0);
  }
  int pfd = (int)syscall(434, child, 0);
  CHECK(pfd >= 0, "pidfd_open");
  struct pollfd pp = {pfd, POLLIN, 0};
  CHECK(poll(&pp, 1, 0) == 0, "pidfd no legible mientras vive");
  CHECK(poll(&pp, 1, 2000) == 1 && (pp.revents & POLLIN),
        "pidfd legible al terminar");
  waitpid(child, NULL, 0);
  close(pfd);
  CHECK(syscall(434, 99999, 0) < 0 && errno == ESRCH, "pidfd_open ESRCH");

  // inotify (stub): fd valido, watch aceptado, sin eventos
  int in = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
  CHECK(in >= 0, "inotify_init1");
  int wd = inotify_add_watch(in, "/tmp", IN_CREATE | IN_DELETE);
  CHECK(wd > 0, "inotify_add_watch");
  CHECK(inotify_add_watch(in, "/no/existe", IN_CREATE) < 0 && errno == ENOENT,
        "inotify_add_watch ENOENT");
  char ib[64];
  CHECK(read(in, ib, sizeof(ib)) < 0 && errno == EAGAIN, "inotify read EAGAIN");
  CHECK(inotify_rm_watch(in, wd) == 0, "inotify_rm_watch");
  close(in);

  printf("[timerfd_test] %s (%d fallos)\n", fails ? "FAILED" : "ALL PASS",
         fails);
  return fails ? 1 : 0;
}
