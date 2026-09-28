// user/musl/apps/jobctl_test/main.c
//
// [JOB CONTROL] Suite del bloque 1 del TODO.
//
// Verifica:
//   1.1  TASK_STOPPED no elegida por el scheduler (vía ru_utime)
//   1.2  SIGTSTP para el proceso
//   1.3  waitpid(WUNTRACED) / waitpid(WCONTINUED)
//   1.4  NOFLSH: Ctrl+C limpia canon_buf
//
// Sale con código != 0 si algún test falla.

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static int g_fail = 0;

#define CHECK(cond, msg)                                                       \
  do {                                                                         \
    if (cond) {                                                                \
      printf("  [PASS] %s\n", msg);                                            \
    } else {                                                                   \
      printf("  [FAIL] %s (errno=%d)\n", msg, errno);                          \
      g_fail++;                                                                \
    }                                                                          \
  } while (0)

static void sleep_ms(long ms) {
  struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
  nanosleep(&ts, NULL);
}

// ---------------------------------------------------------------------------
// [1.1] TASK_STOPPED no elegida por el scheduler.
//
// El hijo hace busy-loop puro. Le mandamos SIGSTOP durante 200 ms.
// Si el scheduler lo eligió durante ese tiempo, ru_utime (leído con
// wait4 al final) se acercará a 220 ms. Si NO lo eligió, se quedará
// cerca de 40 ms (los 20+20 ms en que sí corrió).
// ---------------------------------------------------------------------------
static void test_stopped_not_scheduled(void) {
  printf("[1.1] TASK_STOPPED no elegida por el scheduler\n");

  pid_t pid = fork();
  if (pid < 0) {
    CHECK(0, "fork");
    return;
  }
  if (pid == 0) {
    volatile unsigned long x = 0;
    for (;;)
      x++;
  }

  sleep_ms(20);
  kill(pid, SIGSTOP);

  int st = 0;
  pid_t r = waitpid(pid, &st, WUNTRACED);
  CHECK(r == pid && WIFSTOPPED(st) && WSTOPSIG(st) == SIGSTOP,
        "waitpid(WUNTRACED) detecta SIGSTOP");

  sleep_ms(200);
  kill(pid, SIGCONT);
  sleep_ms(20);
  kill(pid, SIGKILL);

  struct rusage ru;
  memset(&ru, 0, sizeof(ru));
  r = wait4(pid, &st, 0, &ru);
  CHECK(r == pid, "wait4 final recoge al hijo");

  long utime_ms = ru.ru_utime.tv_sec * 1000L + ru.ru_utime.tv_usec / 1000L;
  printf("         ru_utime del hijo = %ld ms (esperado < 150)\n", utime_ms);
  CHECK(utime_ms < 150, "ru_utime < 150 ms (hijo NO corrió durante el stop)");
}

// ---------------------------------------------------------------------------
// [1.2 + 1.3] SIGTSTP + SIGCONT + waitpid.
// ---------------------------------------------------------------------------
static void test_tstp_cont_wait(void) {
  printf("[1.2/1.3] SIGTSTP + SIGCONT + waitpid(WUNTRACED/WCONTINUED)\n");

  pid_t pid = fork();
  if (pid < 0) {
    CHECK(0, "fork");
    return;
  }
  if (pid == 0) {
    // Duerme en trozos largos. Cualquier syscall bloqueante vale; el
    // punto es que el hijo no consuma CPU y esté vivo al llegar la
    // señal.
    for (;;)
      sleep(1000);
  }

  sleep_ms(20);
  kill(pid, SIGTSTP);

  int st = 0;
  pid_t r = waitpid(pid, &st, WUNTRACED);
  CHECK(r == pid, "waitpid(WUNTRACED) devuelve el PID del hijo");
  CHECK(WIFSTOPPED(st), "WIFSTOPPED true tras SIGTSTP");
  CHECK(WSTOPSIG(st) == SIGTSTP, "WSTOPSIG == SIGTSTP");

  kill(pid, SIGCONT);
  r = waitpid(pid, &st, WCONTINUED);
  CHECK(r == pid, "waitpid(WCONTINUED) devuelve el PID del hijo");
  CHECK(WIFCONTINUED(st), "WIFCONTINUED true tras SIGCONT");

  kill(pid, SIGKILL);
  r = waitpid(pid, &st, 0);
  CHECK(r == pid, "waitpid final recoge al hijo muerto");
}

// ---------------------------------------------------------------------------
// [1.4] NOFLSH: Ctrl+C descarta el buffer canonical.
//
// Se abre un PTY, se configura sin echo para no contaminar la lectura.
// Se escribe "abc" + Ctrl+C (0x03) + "def\n". El slave debe devolver
// exactamente "def\n" (4 bytes): "abc" fue descartado por NOFLSH.
// ---------------------------------------------------------------------------
#ifndef TIOCGPTN
#define TIOCGPTN 0x80045430u
#endif
#ifndef TIOCSPTLCK
#define TIOCSPTLCK 0x40045431u
#endif

static void test_noflsh(void) {
  printf("[1.4] NOFLSH limpia canon_buf en Ctrl+C\n");

  int master = open("/dev/ptmx", O_RDWR | O_NOCTTY);
  if (master < 0) {
    CHECK(0, "open /dev/ptmx");
    return;
  }

  int ptn = -1;
  if (ioctl(master, TIOCGPTN, &ptn) < 0) {
    CHECK(0, "ioctl(TIOCGPTN)");
    close(master);
    return;
  }

  int zero = 0;
  ioctl(master, TIOCSPTLCK, &zero);

  char spath[32];
  snprintf(spath, sizeof(spath), "/dev/pts/%d", ptn);

  int slave = open(spath, O_RDWR | O_NOCTTY);
  if (slave < 0) {
    CHECK(0, "open slave");
    close(master);
    return;
  }

  struct termios tio;
  tcgetattr(slave, &tio);
  tio.c_lflag |= ICANON | ISIG;
  tio.c_lflag &= ~ECHO;
  tio.c_cc[VINTR] = 0x03;
  tcsetattr(slave, TCSANOW, &tio);

  const char *seq = "abc\x03"
                    "def\n";
  ssize_t w = write(master, seq, strlen(seq));
  CHECK(w == (ssize_t)strlen(seq), "write al master");

  char buf[32] = {0};
  ssize_t n = read(slave, buf, sizeof(buf));
  printf("         slave devolvió %ld bytes: '", (long)n);
  for (ssize_t i = 0; i < n; i++) {
    if (buf[i] == '\n')
      printf("\\n");
    else
      putchar(buf[i]);
  }
  printf("'\n");

  CHECK(n == 4, "read del slave devuelve 4 bytes");
  CHECK(n == 4 && memcmp(buf, "def\n", 4) == 0,
        "contenido == 'def\\n' (abc descartado por NOFLSH)");

  close(slave);
  close(master);
}

int main(void) {
  printf("=== jobctl_test (Bloque 1 del TODO) ===\n");
  test_stopped_not_scheduled();
  test_tstp_cont_wait();
  test_noflsh();
  printf("=== %s (%d fallos) ===\n", g_fail ? "FAIL" : "PASS", g_fail);
  return g_fail ? 1 : 0;
}