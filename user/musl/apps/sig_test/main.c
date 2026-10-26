// sig_test: rt_sigpending, rt_sigtimedwait, rt_sigsuspend, pause y SIGPIPE.
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int g_fail;
#define CHECK(c, ...) do { if (c) { printf("[sig_test] PASS " __VA_ARGS__); printf("\n"); } \
  else { printf("[sig_test] FAIL " __VA_ARGS__); printf(" (errno=%d)\n", errno); g_fail++; } } while (0)

static volatile sig_atomic_t g_got;
static volatile sig_atomic_t g_mask_in_handler_usr2;
static void on_usr1(int s) {
  (void)s;
  g_got++;
  sigset_t cur;
  sigprocmask(SIG_BLOCK, NULL, &cur);
  g_mask_in_handler_usr2 = sigismember(&cur, SIGUSR2);
}

static long ms_since(struct timespec *a) {
  struct timespec b;
  clock_gettime(CLOCK_MONOTONIC, &b);
  return (b.tv_sec - a->tv_sec) * 1000 + (b.tv_nsec - a->tv_nsec) / 1000000;
}

int main(void) {
  sigset_t blk, pend, old;
  sigemptyset(&blk);
  sigaddset(&blk, SIGUSR1);
  sigaddset(&blk, SIGUSR2);
  sigprocmask(SIG_BLOCK, &blk, &old);

  // sigpending
  kill(getpid(), SIGUSR1);
  sigemptyset(&pend);
  CHECK(sigpending(&pend) == 0 && sigismember(&pend, SIGUSR1) && !sigismember(&pend, SIGUSR2),
        "sigpending ve SIGUSR1 bloqueada y no SIGUSR2");

  // sigtimedwait consume la pendiente
  siginfo_t si;
  struct timespec zero = {0, 0};
  int r = sigtimedwait(&blk, &si, &zero);
  CHECK(r == SIGUSR1 && si.si_signo == SIGUSR1 && si.si_pid == getpid(),
        "sigtimedwait consume SIGUSR1 (r=%d pid=%d)", r, (int)si.si_pid);
  sigemptyset(&pend);
  sigpending(&pend);
  CHECK(!sigismember(&pend, SIGUSR1), "tras sigtimedwait ya no esta pendiente");

  // timeout sin senal
  struct timespec t0, to = {0, 200 * 1000000L};
  clock_gettime(CLOCK_MONOTONIC, &t0);
  errno = 0;
  r = sigtimedwait(&blk, &si, &to);
  long el = ms_since(&t0);
  CHECK(r < 0 && errno == EAGAIN && el >= 150 && el < 2000, "sigtimedwait vence con EAGAIN (%ld ms)", el);

  // sigtimedwait despertado por otro proceso
  pid_t child = fork();
  if (child == 0) {
    struct timespec d = {0, 300 * 1000000L};
    nanosleep(&d, NULL);
    kill(getppid(), SIGUSR2);
    _exit(0);
  }
  struct timespec five = {5, 0};
  clock_gettime(CLOCK_MONOTONIC, &t0);
  r = sigtimedwait(&blk, &si, &five);
  el = ms_since(&t0);
  CHECK(r == SIGUSR2 && el >= 200 && el < 4000, "sigtimedwait despertado por otro proceso (r=%d, %ld ms)", r, el);
  waitpid(child, NULL, 0);

  // sigsuspend: la senal llega con la mascara temporal, el handler ve la mascara
  // del proceso y al volver se restaura la original.
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = on_usr1;
  sigaction(SIGUSR1, &sa, NULL);
  sigset_t wait_mask;
  sigfillset(&wait_mask);
  sigdelset(&wait_mask, SIGUSR1);
  g_got = 0;
  child = fork();
  if (child == 0) {
    struct timespec d = {0, 300 * 1000000L};
    nanosleep(&d, NULL);
    kill(getppid(), SIGUSR1);
    _exit(0);
  }
  clock_gettime(CLOCK_MONOTONIC, &t0);
  errno = 0;
  r = sigsuspend(&wait_mask);
  el = ms_since(&t0);
  CHECK(r == -1 && errno == EINTR && g_got == 1 && el >= 200 && el < 4000,
        "sigsuspend despierta con SIGUSR1 y ejecuta el handler (%ld ms)", el);
  CHECK(g_mask_in_handler_usr2 == 1, "dentro del handler SIGUSR2 sigue bloqueada (mascara de sigsuspend)");
  waitpid(child, NULL, 0);
  sigset_t now;
  sigprocmask(SIG_BLOCK, NULL, &now);
  CHECK(sigismember(&now, SIGUSR1) && sigismember(&now, SIGUSR2),
        "tras sigsuspend se restaura la mascara original");

  // pause
  sigprocmask(SIG_SETMASK, &old, NULL);
  g_got = 0;
  child = fork();
  if (child == 0) {
    struct timespec d = {0, 300 * 1000000L};
    nanosleep(&d, NULL);
    kill(getppid(), SIGUSR1);
    _exit(0);
  }
  errno = 0;
  r = pause();
  CHECK(r == -1 && errno == EINTR && g_got == 1, "pause despierta con la senal");
  waitpid(child, NULL, 0);

  // SIGPIPE al escribir en una tuberia sin lector
  int fds[2];
  pipe(fds);
  close(fds[0]);
  struct sigaction ign;
  memset(&ign, 0, sizeof(ign));
  ign.sa_handler = SIG_IGN;
  sigaction(SIGPIPE, &ign, NULL);
  errno = 0;
  CHECK(write(fds[1], "x", 1) == -1 && errno == EPIPE, "pipe sin lector con SIGPIPE ignorada: EPIPE");
  child = fork();
  if (child == 0) {
    struct sigaction dfl;
    memset(&dfl, 0, sizeof(dfl));
    dfl.sa_handler = SIG_DFL;
    sigaction(SIGPIPE, &dfl, NULL);
    write(fds[1], "x", 1);
    _exit(0); // no deberia llegar: SIGPIPE mata al proceso
  }
  int st = 0;
  waitpid(child, &st, 0);
  CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGPIPE, "escribir en pipe roto mata con SIGPIPE (st=0x%x)", st);

  printf("[sig_test] %s\n", g_fail ? "FAILED" : "ALL PASS");
  return g_fail ? 1 : 0;
}
