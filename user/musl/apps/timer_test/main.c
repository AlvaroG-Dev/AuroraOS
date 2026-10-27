// timer_test: alarm, setitimer/getitimer (REAL, VIRTUAL, PROF) y los
// temporizadores POSIX (timer_create y compania, SIGEV_SIGNAL, SIGEV_NONE,
// SIGEV_THREAD de la libc, overrun, TIMER_ABSTIME), mas `timeout` de
// coreutils, que se apoya en ellos.
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int fails;
#define CHECK(c, name)                                                         \
  do {                                                                         \
    if (c)                                                                     \
      printf("[timer_test] PASS %s\n", name);                                  \
    else {                                                                     \
      printf("[timer_test] FAIL %s (errno=%d)\n", name, errno);                \
      fails++;                                                                 \
    }                                                                          \
  } while (0)

static long ms_now(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static volatile sig_atomic_t g_alrm, g_vt, g_prof, g_usr1;
static volatile int g_usr1_code, g_usr1_val;
static volatile int g_thread_calls, g_thread_val;

static void on_alrm(int s) {
  (void)s;
  g_alrm++;
}
static void on_vt(int s) {
  (void)s;
  g_vt++;
}
static void on_prof(int s) {
  (void)s;
  g_prof++;
}
static void on_usr1(int s, siginfo_t *si, void *uc) {
  (void)s;
  (void)uc;
  g_usr1_code = si->si_code;
  g_usr1_val = si->si_value.sival_int;
  g_usr1++;
}
static void on_thread(union sigval v) {
  g_thread_val = v.sival_int;
  __atomic_fetch_add(&g_thread_calls, 1, __ATOMIC_SEQ_CST);
}

static void handler(int sig, void (*fn)(int)) {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = fn;
  sigemptyset(&sa.sa_mask);
  sigaction(sig, &sa, NULL);
}

// Espera (sin pausar la CPU) a que `*flag` llegue a `n` o pasen `ms`.
static int wait_flag(volatile sig_atomic_t *flag, int n, long ms) {
  long t0 = ms_now();
  while (*flag < n && ms_now() - t0 < ms) {
    struct timespec ts = {0, 1000000L};
    nanosleep(&ts, NULL);
  }
  return *flag >= n;
}

// Gira en CPU hasta que `*flag` se ponga o pasen `ms` de reloj.
static int spin_flag(volatile sig_atomic_t *flag, long ms) {
  long t0 = ms_now();
  volatile unsigned long x = 0;
  while (!*flag && ms_now() - t0 < ms)
    x++;
  return *flag != 0;
}

static void test_alarm(void) {
  handler(SIGALRM, on_alrm);
  alarm(2);
  CHECK(alarm(0) == 2, "alarm devuelve los segundos que quedaban");
  CHECK(alarm(0) == 0, "alarm(0) sin alarma devuelve 0");

  g_alrm = 0;
  long t0 = ms_now();
  alarm(1);
  pause();
  long dt = ms_now() - t0;
  CHECK(g_alrm == 1, "alarm(1) entrega SIGALRM y saca de pause()");
  CHECK(dt >= 950 && dt < 2000, "alarm(1) tarda ~1 s");
  if (dt < 950 || dt >= 2000)
    printf("[timer_test]   dt=%ld\n", dt);
}

static void test_itimer_real(void) {
  handler(SIGALRM, on_alrm);
  g_alrm = 0;
  struct itimerval it = {{0, 20000}, {0, 20000}}, cur;
  long t0 = ms_now();
  CHECK(setitimer(ITIMER_REAL, &it, NULL) == 0, "setitimer REAL 20 ms");
  CHECK(getitimer(ITIMER_REAL, &cur) == 0 && cur.it_interval.tv_usec == 20000 &&
            cur.it_value.tv_sec == 0 && cur.it_value.tv_usec <= 20000,
        "getitimer REAL armado");
  int ok = wait_flag(&g_alrm, 5, 2000);
  long dt = ms_now() - t0;
  CHECK(ok, "ITIMER_REAL periodico: 5 SIGALRM");
  CHECK(dt >= 95 && dt < 1000, "5 periodos de 20 ms tardan ~100 ms");
  if (dt < 95 || dt >= 1000)
    printf("[timer_test]   dt=%ld\n", dt);

  struct itimerval zero = {{0, 0}, {0, 0}}, old;
  CHECK(setitimer(ITIMER_REAL, &zero, &old) == 0 &&
            old.it_interval.tv_usec == 20000,
        "desarmar devuelve el anterior");
  CHECK(getitimer(ITIMER_REAL, &cur) == 0 && cur.it_value.tv_sec == 0 &&
            cur.it_value.tv_usec == 0,
        "getitimer REAL desarmado = 0");
  int before = g_alrm;
  usleep(80 * 1000);
  CHECK(g_alrm == before, "desarmado no llegan mas SIGALRM");

  struct itimerval bad = {{0, 0}, {0, 1000000}};
  errno = 0;
  CHECK(setitimer(ITIMER_REAL, &bad, NULL) == -1 && errno == EINVAL,
        "tv_usec >= 1e6 da EINVAL");
  errno = 0;
  CHECK(setitimer(7, &it, NULL) == -1 && errno == EINVAL,
        "which invalido da EINVAL");
}

static void test_itimer_cpu(void) {
  handler(SIGVTALRM, on_vt);
  handler(SIGPROF, on_prof);
  g_vt = g_prof = 0;
  struct itimerval it = {{0, 0}, {0, 50000}};
  CHECK(setitimer(ITIMER_VIRTUAL, &it, NULL) == 0, "setitimer VIRTUAL 50 ms");
  CHECK(spin_flag(&g_vt, 5000), "ITIMER_VIRTUAL vence girando en CPU");
  CHECK(setitimer(ITIMER_PROF, &it, NULL) == 0, "setitimer PROF 50 ms");
  CHECK(spin_flag(&g_prof, 5000), "ITIMER_PROF vence girando en CPU");

  // Durmiendo no se consume CPU: VIRTUAL no vence.
  g_vt = 0;
  struct itimerval cur;
  CHECK(setitimer(ITIMER_VIRTUAL, &it, NULL) == 0, "rearmar VIRTUAL");
  usleep(200 * 1000);
  CHECK(g_vt == 0 && getitimer(ITIMER_VIRTUAL, &cur) == 0 &&
            cur.it_value.tv_usec > 0,
        "VIRTUAL no avanza mientras el proceso duerme");
  struct itimerval zero = {{0, 0}, {0, 0}};
  setitimer(ITIMER_VIRTUAL, &zero, NULL);
}

static void test_posix_signal(void) {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_sigaction = on_usr1;
  sa.sa_flags = SA_SIGINFO;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGUSR1, &sa, NULL);

  struct sigevent sev;
  memset(&sev, 0, sizeof(sev));
  sev.sigev_notify = SIGEV_SIGNAL;
  sev.sigev_signo = SIGUSR1;
  sev.sigev_value.sival_int = 0x1234;
  timer_t t;
  CHECK(timer_create(CLOCK_MONOTONIC, &sev, &t) == 0, "timer_create SIGEV_SIGNAL");

  g_usr1 = 0;
  struct itimerspec its = {{0, 0}, {0, 30 * 1000000L}}, cur;
  long t0 = ms_now();
  CHECK(timer_settime(t, 0, &its, NULL) == 0, "timer_settime 30 ms");
  CHECK(timer_gettime(t, &cur) == 0 && cur.it_value.tv_sec == 0 &&
            cur.it_value.tv_nsec > 0 && cur.it_value.tv_nsec <= 30 * 1000000L,
        "timer_gettime armado");
  CHECK(wait_flag(&g_usr1, 1, 2000), "SIGUSR1 del temporizador");
  long dt = ms_now() - t0;
  CHECK(dt >= 28 && dt < 600, "tarda ~30 ms");
  CHECK(g_usr1_code == SI_TIMER, "si_code = SI_TIMER");
  CHECK(g_usr1_val == 0x1234, "si_value = sigev_value");
  CHECK(timer_gettime(t, &cur) == 0 && cur.it_value.tv_sec == 0 &&
            cur.it_value.tv_nsec == 0,
        "one-shot vencido: gettime = 0");

  // TIMER_ABSTIME sobre CLOCK_MONOTONIC.
  g_usr1 = 0;
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  its.it_value = now;
  its.it_value.tv_nsec += 40 * 1000000L;
  if (its.it_value.tv_nsec >= 1000000000L) {
    its.it_value.tv_sec++;
    its.it_value.tv_nsec -= 1000000000L;
  }
  t0 = ms_now();
  CHECK(timer_settime(t, TIMER_ABSTIME, &its, NULL) == 0, "TIMER_ABSTIME +40 ms");
  CHECK(wait_flag(&g_usr1, 1, 2000) && ms_now() - t0 >= 35,
        "vence en el instante absoluto");
  CHECK(timer_delete(t) == 0, "timer_delete");
  errno = 0;
  CHECK(timer_gettime(t, &cur) == -1 && errno == EINVAL,
        "gettime de un temporizador borrado da EINVAL");

  errno = 0;
  CHECK(timer_create(CLOCK_PROCESS_CPUTIME_ID, &sev, &t) == -1,
        "reloj de CPU no soportado se rechaza");
}

static void test_posix_overrun(void) {
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGUSR2);
  sigprocmask(SIG_BLOCK, &set, NULL);

  struct sigevent sev;
  memset(&sev, 0, sizeof(sev));
  sev.sigev_notify = SIGEV_SIGNAL;
  sev.sigev_signo = SIGUSR2;
  sev.sigev_value.sival_int = 77;
  timer_t t;
  CHECK(timer_create(CLOCK_MONOTONIC, &sev, &t) == 0, "timer_create periodico");
  struct itimerspec its = {{0, 5 * 1000000L}, {0, 5 * 1000000L}};
  CHECK(timer_settime(t, 0, &its, NULL) == 0, "settime 5 ms periodico");
  usleep(100 * 1000); // ~20 vencimientos con la senal bloqueada
  siginfo_t si;
  struct timespec to = {1, 0};
  int s = sigtimedwait(&set, &si, &to);
  CHECK(s == SIGUSR2, "sigtimedwait recoge SIGUSR2");
  CHECK(si.si_code == SI_TIMER && si.si_value.sival_int == 77,
        "sigtimedwait: SI_TIMER y sigev_value");
  CHECK(si.si_overrun >= 5, "overrun cuenta los vencimientos perdidos");
  int ov = timer_getoverrun(t);
  CHECK(ov == si.si_overrun, "timer_getoverrun = si_overrun");
  if (si.si_overrun < 5 || ov != si.si_overrun)
    printf("[timer_test]   si_overrun=%d getoverrun=%d\n", si.si_overrun, ov);
  timer_delete(t);
  sigprocmask(SIG_UNBLOCK, &set, NULL);

  // SIGEV_NONE: no manda nada pero gettime avanza.
  memset(&sev, 0, sizeof(sev));
  sev.sigev_notify = SIGEV_NONE;
  CHECK(timer_create(CLOCK_REALTIME, &sev, &t) == 0, "timer_create SIGEV_NONE");
  struct itimerspec one = {{0, 0}, {5, 0}}, cur;
  timer_settime(t, 0, &one, NULL);
  usleep(50 * 1000);
  CHECK(timer_gettime(t, &cur) == 0 && cur.it_value.tv_sec == 4,
        "SIGEV_NONE: gettime cuenta hacia atras");
  timer_delete(t);
}

static void test_posix_thread(void) {
  struct sigevent sev;
  memset(&sev, 0, sizeof(sev));
  sev.sigev_notify = SIGEV_THREAD;
  sev.sigev_notify_function = on_thread;
  sev.sigev_value.sival_int = 99;
  timer_t t;
  CHECK(timer_create(CLOCK_MONOTONIC, &sev, &t) == 0, "timer_create SIGEV_THREAD");
  struct itimerspec its = {{0, 0}, {0, 20 * 1000000L}};
  CHECK(timer_settime(t, 0, &its, NULL) == 0, "settime SIGEV_THREAD 20 ms");
  long t0 = ms_now();
  while (__atomic_load_n(&g_thread_calls, __ATOMIC_SEQ_CST) == 0 &&
         ms_now() - t0 < 2000)
    usleep(1000);
  CHECK(g_thread_calls == 1 && g_thread_val == 99,
        "SIGEV_THREAD llama a la funcion con sigev_value");
  timer_delete(t);
}

static void test_fork_and_timeout(void) {
  struct itimerval it = {{0, 0}, {10, 0}};
  setitimer(ITIMER_REAL, &it, NULL);
  pid_t pid = fork();
  if (pid == 0) {
    struct itimerval cur;
    getitimer(ITIMER_REAL, &cur);
    _exit(cur.it_value.tv_sec == 0 && cur.it_value.tv_usec == 0 ? 0 : 1);
  }
  int st = 0;
  waitpid(pid, &st, 0);
  CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "fork no hereda ITIMER_REAL");
  struct itimerval zero = {{0, 0}, {0, 0}};
  setitimer(ITIMER_REAL, &zero, NULL);

  // coreutils `timeout` usa timer_create/alarm: antes nunca vencia.
  long t0 = ms_now();
  pid = fork();
  if (pid == 0) {
    char *argv[] = {"timeout", "1", "sleep", "10", NULL};
    execv("/usr/bin/timeout", argv);
    _exit(127);
  }
  waitpid(pid, &st, 0);
  long dt = ms_now() - t0;
  CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 124, "timeout 1 sleep 10 sale con 124");
  CHECK(dt >= 900 && dt < 4000, "timeout 1 corta a ~1 s");
  if (dt < 900 || dt >= 4000 || !WIFEXITED(st))
    printf("[timer_test]   dt=%ld st=0x%x\n", dt, st);
}

int main(void) {
  test_alarm();
  test_itimer_real();
  test_itimer_cpu();
  test_posix_signal();
  test_posix_overrun();
  test_posix_thread();
  test_fork_and_timeout();
  printf("[timer_test] %s (%d fallos)\n", fails ? "FAILED" : "ALL PASS",
         fails);
  return fails ? 1 : 0;
}
