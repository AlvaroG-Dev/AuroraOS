// tests/compat/fd_prctl_test.c — capa Linux: pipe2, fcntl, O_APPEND, prctl
// y syscalls sueltas (preadv, close_range, renameat2, sched_*, mlock...).
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
#include <sched.h>
#include <sys/mman.h>
#include <sys/personality.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/uio.h>
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

  // preadv / pwritev con offset, sin mover el del fd.
  {
    const char *pf = "/tmp/compat-prwv.txt";
    int fd = open(pf, O_RDWR | O_CREAT | O_TRUNC, 0644);
    char a1[] = "hola ", a2[] = "mundo";
    struct iovec w[2] = {{a1, 5}, {a2, 5}};
    CHECK(pwritev(fd, w, 2, 3) == 10, "pwritev 10 bytes en el offset 3");
    char b1[4] = {0}, b2[8] = {0};
    struct iovec r[2] = {{b1, 3}, {b2, 7}};
    CHECK(preadv(fd, r, 2, 4) == 9 && !memcmp(b1, "ola", 3) && !memcmp(b2, " mundo", 6),
          "preadv lee '%s|%s'", b1, b2);
    CHECK(lseek(fd, 0, SEEK_CUR) == 0, "el offset del fd no se mueve");
    CHECK(fdatasync(fd) == 0, "fdatasync");
    CHECK(syncfs(fd) == 0, "syncfs");
    CHECK(readahead(fd, 0, 4096) == 0, "readahead");
    close(fd);
    // renameat2(RENAME_NOREPLACE)
    const char *pf2 = "/tmp/compat-prwv2.txt";
    close(open(pf2, O_WRONLY | O_CREAT, 0644));
    errno = 0;
    long rr = syscall(SYS_renameat2, AT_FDCWD, pf, AT_FDCWD, pf2, 1 /* RENAME_NOREPLACE */);
    CHECK(rr == -1 && errno == EEXIST, "renameat2 NOREPLACE con destino -> EEXIST (errno %d)",
          errno);
    unlink(pf2);
    CHECK(syscall(SYS_renameat2, AT_FDCWD, pf, AT_FDCWD, pf2, 1) == 0 && access(pf2, F_OK) == 0,
          "renameat2 NOREPLACE sin destino renombra");
    unlink(pf2);
  }

  // close_range
  {
    int base = open("/dev/null", O_RDONLY);
    int f1 = dup(base), f2 = dup(base);
    CHECK(syscall(SYS_close_range, f1, f2, 4 /* CLOEXEC */) == 0 &&
              fcntl(f1, F_GETFD) == FD_CLOEXEC && fcntl(f2, F_GETFD) == FD_CLOEXEC,
          "close_range(CLOSE_RANGE_CLOEXEC)");
    CHECK(syscall(SYS_close_range, f1, ~0U, 0) == 0 && fcntl(f1, F_GETFD) == -1 &&
              fcntl(f2, F_GETFD) == -1 && fcntl(base, F_GETFD) == 0,
          "close_range cierra solo su rango");
    close(base);
  }

  // planificador, mlock, personality, mincore, ioprio
  CHECK(sched_get_priority_max(SCHED_FIFO) == 99 && sched_get_priority_min(SCHED_RR) == 1 &&
            sched_get_priority_max(SCHED_OTHER) == 0,
        "sched_get_priority_max/min");
  struct sched_param sp = {0};
  CHECK(sched_setscheduler(0, SCHED_OTHER, &sp) == 0, "sched_setscheduler(SCHED_OTHER)");
  struct timespec q = {0};
  CHECK(sched_rr_get_interval(0, &q) == 0, "sched_rr_get_interval");
  static char lockbuf[8192];
  CHECK(mlock(lockbuf, sizeof(lockbuf)) == 0 && munlock(lockbuf, sizeof(lockbuf)) == 0,
        "mlock/munlock");
  CHECK(mlockall(MCL_CURRENT) == 0 && munlockall() == 0, "mlockall/munlockall");
  CHECK(personality(0xffffffff) == 0, "personality = PER_LINUX");
  void *mp = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  unsigned char mv[2] = {9, 9};
  CHECK(mp != MAP_FAILED && mincore(mp, 8192, mv) == 0 && mv[0] <= 1, "mincore");
  CHECK(syscall(SYS_ioprio_get, 1, 0) >= 0, "ioprio_get");

  // sigqueue
  {
    got_usr1 = 0;
    signal(SIGUSR1, on_usr1);
    union sigval v = {.sival_int = 7};
    CHECK(sigqueue(getpid(), SIGUSR1, v) == 0, "sigqueue");
    for (int i = 0; i < 100 && !got_usr1; i++)
      usleep(1000);
    CHECK(got_usr1, "sigqueue entrega la señal");
  }

  printf("COMPAT-TEST: %d/%d %s\n", pass, total, pass == total ? "OK" : "FALLOS");
  return pass == total ? 0 : 1;
}
