// tests/compat/fd_prctl_test.c — capa Linux: pipe2, fcntl, O_APPEND, prctl.
// Programa Linux normal (glibc estática): en AuroraOS como boot-test, o en
// Linux para comparar. Escribe "COMPAT-TEST: N/M OK" al final.
#define _GNU_SOURCE
#pragma GCC diagnostic ignored "-Wunused-result"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int pass, total;
#define CHECK(c, ...)                                                          \
  do {                                                                         \
    total++;                                                                   \
    if (c) {                                                                   \
      pass++;                                                                  \
      printf("ok   ");                                                         \
    } else {                                                                   \
      printf("FALLO ");                                                        \
    }                                                                          \
    printf(__VA_ARGS__);                                                       \
    printf("\n");                                                              \
    fflush(stdout);                                                            \
  } while (0)

static volatile sig_atomic_t got_usr1;
static void on_usr1(int s) { (void)s; got_usr1 = 1; }

int main(void) {
  int p[2];
  // pipe2(0): ningún extremo con FD_CLOEXEC.
  CHECK(pipe2(p, 0) == 0, "pipe2(0)");
  CHECK(fcntl(p[0], F_GETFD) == 0 && fcntl(p[1], F_GETFD) == 0,
        "pipe2(0) sin FD_CLOEXEC (lectura %d, escritura %d)", fcntl(p[0], F_GETFD),
        fcntl(p[1], F_GETFD));
  close(p[0]);
  close(p[1]);
  CHECK(pipe2(p, O_CLOEXEC) == 0 && fcntl(p[0], F_GETFD) == FD_CLOEXEC &&
            fcntl(p[1], F_GETFD) == FD_CLOEXEC,
        "pipe2(O_CLOEXEC) en los dos extremos");
  close(p[0]);
  close(p[1]);

  // pipe2(O_NONBLOCK): lectura vacía EAGAIN, llenar hasta EAGAIN.
  int r = pipe2(p, O_NONBLOCK | O_CLOEXEC);
  CHECK(r == 0, "pipe2(O_NONBLOCK|O_CLOEXEC) = %d (errno %d)", r, r ? errno : 0);
  if (r == 0) {
    char c;
    errno = 0;
    CHECK(read(p[0], &c, 1) == -1 && errno == EAGAIN, "read vacío -> EAGAIN (errno %d)",
          errno);
    CHECK(fcntl(p[0], F_GETFL) & O_NONBLOCK, "F_GETFL tiene O_NONBLOCK");
    static char buf[4096];
    long sum = 0, w;
    while ((w = write(p[1], buf, sizeof(buf))) > 0)
      sum += w;
    CHECK(w == -1 && errno == EAGAIN && sum > 0,
          "write en pipe lleno -> EAGAIN tras %ld bytes (errno %d)", sum, errno);
    int sz = fcntl(p[1], F_GETPIPE_SZ);
    CHECK(sz > 0 && sz == sum, "F_GETPIPE_SZ = %d (cabe %ld)", sz, sum);
    close(p[0]);
    close(p[1]);
  }

  // O_APPEND en /tmp (tmpfs): dos fds, y F_SETFL.
  const char *f = "/tmp/compat-append.txt";
  unlink(f);
  int a = open(f, O_WRONLY | O_CREAT | O_APPEND, 0644);
  int b = open(f, O_WRONLY | O_APPEND);
  CHECK(a >= 0 && b >= 0, "open O_APPEND x2");
  write(a, "uno\n", 4);
  write(b, "dos\n", 4);
  write(a, "tres\n", 5);
  struct stat st;
  fstat(b, &st);
  CHECK(st.st_size == 13, "fstat ve lo que escribió otro fd (tamaño %ld)", (long)st.st_size);
  CHECK(lseek(b, 0, SEEK_END) == 13, "lseek SEEK_END en el final real");
  stat(f, &st);
  CHECK(st.st_size == 13, "dos fds O_APPEND no se pisan (tamaño %ld, esperado 13)",
        (long)st.st_size);
  int fl = fcntl(a, F_GETFL);
  CHECK((fl & O_APPEND) && !(fl & 0x20000000), "F_GETFL = 0x%x (O_APPEND, sin bits internos)",
        fl);
  close(a);
  close(b);
  int c = open(f, O_WRONLY);
  CHECK(fcntl(c, F_SETFL, O_APPEND) == 0 && (fcntl(c, F_GETFL) & O_APPEND),
        "F_SETFL O_APPEND");
  write(c, "cuatro\n", 7);
  stat(f, &st);
  CHECK(st.st_size == 20, "tras F_SETFL O_APPEND escribe al final (tamaño %ld, esperado 20)",
        (long)st.st_size);
  close(c);
  unlink(f);

  // prctl(PR_SET_NAME) y /proc/self/comm.
  char nm[17] = {0};
  CHECK(prctl(PR_SET_NAME, "nombre-prueba-largo") == 0, "PR_SET_NAME");
  prctl(PR_GET_NAME, nm);
  CHECK(!strcmp(nm, "nombre-prueba-l"), "PR_GET_NAME = '%s' (15 caracteres)", nm);
  char comm[64] = {0};
  FILE *fc = fopen("/proc/self/comm", "r");
  if (fc) {
    fgets(comm, sizeof(comm), fc);
    fclose(fc);
  }
  CHECK(!strcmp(comm, "nombre-prueba-l\n"), "/proc/self/comm = '%.*s'", (int)strcspn(comm, "\n"),
        comm);

  // PR_SET_PDEATHSIG: el nieto recibe SIGUSR1 cuando muere su padre.
  int sync[2];
  pipe(sync);
  pid_t mid = fork();
  if (mid == 0) {
    pid_t g = fork();
    if (g == 0) {
      signal(SIGUSR1, on_usr1);
      prctl(PR_SET_PDEATHSIG, SIGUSR1);
      int v = 0;
      prctl(PR_GET_PDEATHSIG, &v);
      write(sync[1], v == SIGUSR1 ? "g" : "x", 1); // listo
      for (int i = 0; i < 300 && !got_usr1; i++)
        usleep(10000);
      write(sync[1], got_usr1 ? "S" : "N", 1);
      _exit(0);
    }
    char ch;
    read(sync[0], &ch, 1); // el nieto ya pidió la señal
    _exit(ch == 'g' ? 0 : 3);
  }
  int stv = 0;
  waitpid(mid, &stv, 0);
  char res = 0;
  read(sync[0], &res, 1);
  CHECK(WIFEXITED(stv) && WEXITSTATUS(stv) == 0 && res == 'S',
        "PR_SET_PDEATHSIG: SIGUSR1 al morir el padre (%c)", res ? res : '?');

  printf("COMPAT-TEST: %d/%d %s\n", pass, total, pass == total ? "OK" : "FALLOS");
  return pass == total ? 0 : 1;
}
