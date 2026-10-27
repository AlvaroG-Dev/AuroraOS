// thread_sig_test: senales con hilos (mascara por hilo, senales dirigidas a
// un hilo, eleccion del hilo que atiende una senal de proceso, sigwait en un
// hilo dedicado), tid de los hilos, salida del grupo (exit y senal mortal
// desde un hilo secundario), waitpid desde un hilo secundario y sigsuspend
// con una senal ignorada.
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int fails;
#define CHECK(c, name)                                                         \
  do {                                                                         \
    if (c)                                                                     \
      printf("[thread_sig_test] PASS %s\n", name);                             \
    else {                                                                     \
      printf("[thread_sig_test] FAIL %s (errno=%d)\n", name, errno);           \
      fails++;                                                                 \
    }                                                                          \
  } while (0)

static long ms_now(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static pid_t my_gettid(void) { return (pid_t)syscall(SYS_gettid); }

static volatile pid_t g_usr1_tid, g_usr2_tid;
static volatile int g_usr1, g_usr2;

static void on_usr1(int s) {
  (void)s;
  g_usr1_tid = my_gettid();
  g_usr1++;
}
static void on_usr2(int s) {
  (void)s;
  g_usr2_tid = my_gettid();
  g_usr2++;
}

static void handler(int sig, void (*fn)(int)) {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = fn;
  sigemptyset(&sa.sa_mask);
  sigaction(sig, &sa, NULL);
}

static void wait_until(volatile int *v, int n, long ms) {
  long t0 = ms_now();
  while (*v < n && ms_now() - t0 < ms) {
    struct timespec ts = {0, 1000000L};
    nanosleep(&ts, NULL);
  }
}

// --- hilo auxiliar: bloquea SIGUSR1 y duerme hasta que le digan ---------
static volatile int g_stop;
static volatile pid_t g_worker_tid;

static void *worker_block_usr1(void *arg) {
  (void)arg;
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGUSR1);
  pthread_sigmask(SIG_BLOCK, &set, NULL);
  g_worker_tid = my_gettid();
  while (!g_stop) {
    struct timespec ts = {0, 2000000L};
    nanosleep(&ts, NULL);
  }
  return NULL;
}

static void test_tids_and_directed(void) {
  pid_t pid = getpid();
  CHECK(my_gettid() == pid, "gettid() del hilo principal = getpid()");

  handler(SIGUSR1, on_usr1);
  handler(SIGUSR2, on_usr2);
  g_usr1 = 0;
  CHECK(raise(SIGUSR1) == 0 && g_usr1 == 1 && g_usr1_tid == pid,
        "raise() entrega al propio hilo");

  g_stop = 0;
  g_worker_tid = 0;
  pthread_t th;
  pthread_create(&th, NULL, worker_block_usr1, NULL);
  long t0 = ms_now();
  while (!g_worker_tid && ms_now() - t0 < 2000)
    usleep(1000);
  CHECK(g_worker_tid > 0 && g_worker_tid != pid,
        "un hilo secundario tiene su propio tid");

  // pthread_kill(SIGUSR2) al hilo: lo atiende el propio hilo.
  g_usr2 = 0;
  CHECK(pthread_kill(th, SIGUSR2) == 0, "pthread_kill a un hilo");
  wait_until(&g_usr2, 1, 2000);
  CHECK(g_usr2 == 1 && g_usr2_tid == g_worker_tid,
        "pthread_kill: el handler corre en el hilo destino");

  // tgkill con un tgid equivocado: ESRCH.
  errno = 0;
  CHECK(syscall(SYS_tgkill, pid + 100000, g_worker_tid, SIGUSR2) == -1 &&
            errno == ESRCH,
        "tgkill con tgid que no es el del hilo da ESRCH");

  // El principal bloquea SIGUSR1 un momento: el hilo tambien la bloquea, asi
  // que kill() la deja pendiente; al desbloquearla el principal, la atiende.
  sigset_t set, old;
  sigemptyset(&set);
  sigaddset(&set, SIGUSR1);
  pthread_sigmask(SIG_BLOCK, &set, &old);
  g_usr1 = 0;
  kill(pid, SIGUSR1);
  usleep(30 * 1000);
  CHECK(g_usr1 == 0, "bloqueada en todos los hilos: queda pendiente");
  sigset_t pend;
  sigpending(&pend);
  CHECK(sigismember(&pend, SIGUSR1), "sigpending la ve");
  pthread_sigmask(SIG_SETMASK, &old, NULL);
  CHECK(g_usr1 == 1 && g_usr1_tid == pid,
        "al desbloquearla la atiende el hilo principal");

  // La mascara es por hilo: la del principal no ha cambiado por el hilo.
  sigset_t cur;
  pthread_sigmask(SIG_SETMASK, NULL, &cur);
  CHECK(!sigismember(&cur, SIGUSR1), "la mascara del principal es suya");

  g_stop = 1;
  pthread_join(th, NULL);
}

// --- senal de proceso: la atiende un hilo que no la bloquea ------------
static void *worker_unblock_and_wait(void *arg) {
  (void)arg;
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGUSR1);
  pthread_sigmask(SIG_UNBLOCK, &set, NULL);
  g_worker_tid = my_gettid();
  while (!g_stop) {
    struct timespec ts = {0, 2000000L};
    nanosleep(&ts, NULL);
  }
  return NULL;
}

static void test_routing(void) {
  sigset_t set, old;
  sigemptyset(&set);
  sigaddset(&set, SIGUSR1);
  pthread_sigmask(SIG_BLOCK, &set, &old);
  g_stop = 0;
  g_worker_tid = 0;
  pthread_t th;
  pthread_create(&th, NULL, worker_unblock_and_wait, NULL);
  long t0 = ms_now();
  while (!g_worker_tid && ms_now() - t0 < 2000)
    usleep(1000);
  g_usr1 = 0;
  g_usr1_tid = 0;
  kill(getpid(), SIGUSR1);
  wait_until(&g_usr1, 1, 2000);
  CHECK(g_usr1 == 1 && g_usr1_tid == g_worker_tid,
        "kill() al proceso: la atiende el hilo que no la bloquea");
  g_stop = 1;
  pthread_join(th, NULL);
  pthread_sigmask(SIG_SETMASK, &old, NULL);
}

// --- sigwait en un hilo dedicado ----------------------------------------
static volatile int g_waited_sig;
static void *sigwait_thread(void *arg) {
  sigset_t *set = arg;
  int sig = 0;
  if (sigwait(set, &sig) == 0)
    g_waited_sig = sig;
  return NULL;
}

static void test_sigwait_thread(void) {
  sigset_t set, old;
  sigemptyset(&set);
  sigaddset(&set, SIGUSR2);
  pthread_sigmask(SIG_BLOCK, &set, &old); // bloqueada en todos los hilos
  g_waited_sig = 0;
  g_usr2 = 0;
  pthread_t th;
  pthread_create(&th, NULL, sigwait_thread, &set);
  usleep(30 * 1000);
  kill(getpid(), SIGUSR2);
  pthread_join(th, NULL);
  CHECK(g_waited_sig == SIGUSR2 && g_usr2 == 0,
        "sigwait en un hilo recoge la senal del proceso sin handler");
  sigset_t cur;
  pthread_sigmask(SIG_SETMASK, NULL, &cur);
  CHECK(sigismember(&cur, SIGUSR2),
        "sigwait de otro hilo no cambia la mascara del principal");
  pthread_sigmask(SIG_SETMASK, &old, NULL);
}

// --- salida del grupo ----------------------------------------------------
static void *sleeper(void *arg) {
  (void)arg;
  for (;;)
    pause();
  return NULL;
}
static void *exiter(void *arg) {
  (void)arg;
  usleep(20 * 1000);
  exit(42); // exit_group desde un hilo secundario
  return NULL;
}
static void *crasher(void *arg) {
  (void)arg;
  usleep(20 * 1000);
  raise(SIGTERM); // senal mortal en un hilo secundario
  return NULL;
}

static int run_child(void *(*fn)(void *), int *st) {
  pid_t pid = fork();
  if (pid == 0) {
    pthread_t a, b;
    pthread_create(&a, NULL, sleeper, NULL);
    pthread_create(&b, NULL, fn, NULL);
    for (;;)
      pause(); // el principal no sale por si mismo
  }
  long t0 = ms_now();
  int r = -1;
  while (ms_now() - t0 < 5000) {
    r = waitpid(pid, st, WNOHANG);
    if (r == pid)
      break;
    usleep(5000);
  }
  if (r != pid) {
    kill(pid, SIGKILL);
    waitpid(pid, st, 0);
    return 0;
  }
  return 1;
}

static void test_group_exit(void) {
  int st = 0;
  int ok = run_child(exiter, &st);
  CHECK(ok && WIFEXITED(st) && WEXITSTATUS(st) == 42,
        "exit() en un hilo secundario termina el proceso entero (42)");
  st = 0;
  ok = run_child(crasher, &st);
  CHECK(ok && WIFSIGNALED(st) && WTERMSIG(st) == SIGTERM,
        "senal mortal en un hilo termina el proceso entero");
}

// --- waitpid desde un hilo secundario -------------------------------------
static volatile int g_reaped;
static void *reaper(void *arg) {
  pid_t pid = *(pid_t *)arg;
  int st = 0;
  if (waitpid(pid, &st, 0) == pid && WIFEXITED(st) && WEXITSTATUS(st) == 7)
    g_reaped = 1;
  return NULL;
}

static void test_waitpid_thread(void) {
  pid_t pid = fork();
  if (pid == 0) {
    usleep(50 * 1000);
    _exit(7);
  }
  g_reaped = 0;
  pthread_t th;
  pthread_create(&th, NULL, reaper, &pid);
  long t0 = ms_now();
  while (!g_reaped && ms_now() - t0 < 3000)
    usleep(2000);
  CHECK(g_reaped, "waitpid() en un hilo secundario se despierta");
  if (!g_reaped) {
    kill(pid, SIGKILL);
    pthread_cancel(th);
  }
  pthread_join(th, NULL);
}

// --- sigsuspend con una senal ignorada ------------------------------------
static void test_sigsuspend_ignored(void) {
  handler(SIGCHLD, SIG_DFL);
  handler(SIGUSR1, on_usr1);
  sigset_t set, old;
  sigemptyset(&set);
  sigaddset(&set, SIGUSR1);
  sigaddset(&set, SIGCHLD);
  pthread_sigmask(SIG_BLOCK, &set, &old);

  pid_t me = getpid();
  pid_t kid = fork();
  if (kid == 0) {
    _exit(0); // SIGCHLD (SIG_DFL: ignorada) enseguida
  }
  pid_t kid2 = fork();
  if (kid2 == 0) {
    usleep(150 * 1000);
    kill(me, SIGUSR1);
    _exit(0);
  }
  g_usr1 = 0;
  long t0 = ms_now();
  sigset_t wait_mask;
  sigemptyset(&wait_mask); // todo desbloqueado durante la espera
  sigsuspend(&wait_mask);
  long dt = ms_now() - t0;
  CHECK(g_usr1 == 1, "sigsuspend vuelve por SIGUSR1 (con handler)");
  CHECK(dt >= 100, "SIGCHLD con SIG_DFL no saca de sigsuspend");
  sigset_t cur;
  pthread_sigmask(SIG_SETMASK, NULL, &cur);
  CHECK(sigismember(&cur, SIGUSR1) && sigismember(&cur, SIGCHLD),
        "tras sigsuspend vuelve la mascara anterior");
  waitpid(kid, NULL, 0);
  waitpid(kid2, NULL, 0);
  pthread_sigmask(SIG_SETMASK, &old, NULL);
}

int main(void) {
  test_tids_and_directed();
  test_routing();
  test_sigwait_thread();
  test_group_exit();
  test_waitpid_thread();
  test_sigsuspend_ignored();
  printf("[thread_sig_test] %s (%d fallos)\n", fails ? "FAILED" : "ALL PASS",
         fails);
  return fails ? 1 : 0;
}
