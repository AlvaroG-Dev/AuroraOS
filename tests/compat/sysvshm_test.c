// tests/compat/sysvshm_test.c — memoria compartida System V (shmget/shmat/
// shmdt/shmctl) como la usa MIT-SHM de X11. Programa Linux normal (glibc
// estática). Escribe "SHM-TEST: N/M OK" al final.
#define _GNU_SOURCE
#pragma GCC diagnostic ignored "-Wunused-result"
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/shm.h>
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

int main(void) {
  const size_t SZ = 1 << 20;
  int id = shmget(IPC_PRIVATE, SZ, IPC_CREAT | 0600);
  CHECK(id >= 0, "shmget(IPC_PRIVATE, 1 MiB) = %d (errno %d)", id, id < 0 ? errno : 0);
  if (id < 0)
    goto out;
  char *p = shmat(id, NULL, 0);
  CHECK(p != (void *)-1, "shmat");
  if (p == (void *)-1)
    goto out;
  CHECK(p[0] == 0 && p[SZ - 1] == 0, "empieza a cero");
  strcpy(p, "hola desde el padre");
  struct shmid_ds ds;
  CHECK(shmctl(id, IPC_STAT, &ds) == 0 && ds.shm_segsz == SZ && ds.shm_nattch == 1 &&
            ds.shm_cpid == getpid() && (ds.shm_perm.mode & 0777) == 0600,
        "IPC_STAT: tamaño %zu, nattch %lu, modo %o", (size_t)ds.shm_segsz,
        (unsigned long)ds.shm_nattch, ds.shm_perm.mode & 0777);

  // Hijo: hereda el mapeo (fork) y además se engancha otra vez por id.
  pid_t c = fork();
  if (c == 0) {
    char *q = shmat(id, NULL, 0);
    int ok = q != (void *)-1 && !strcmp(q, "hola desde el padre") && !strcmp(p, q);
    struct shmid_ds d2;
    shmctl(id, IPC_STAT, &d2);
    ok = ok && d2.shm_nattch == 3; // padre + copia del fork + este shmat
    strcpy(q + 4096, "respuesta del hijo");
    shmdt(q);
    _exit(ok ? 0 : 1);
  }
  int st = 0;
  waitpid(c, &st, 0);
  CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "el hijo ve los datos y nattch = 3");
  CHECK(!strcmp(p + 4096, "respuesta del hijo"), "el padre ve lo que escribió el hijo");
  shmctl(id, IPC_STAT, &ds);
  CHECK(ds.shm_nattch == 1, "tras salir el hijo nattch = %lu", (unsigned long)ds.shm_nattch);

  // Solo lectura: escribir da SIGSEGV.
  c = fork();
  if (c == 0) {
    shmdt(p);
    volatile char *r = shmat(id, NULL, SHM_RDONLY);
    if (r == (void *)-1 || r[0] != 'h')
      _exit(2);
    r[0] = 'X';
    _exit(3);
  }
  waitpid(c, &st, 0);
  CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGSEGV, "SHM_RDONLY: escribir -> SIGSEGV");
  CHECK(p[0] == 'h', "el segmento no cambió");

  // Clave fija: IPC_EXCL, buscar por clave, tamaño mayor.
  key_t k = 0x41555230; // "AUR0"
  int kid = shmget(k, 8192, IPC_CREAT | IPC_EXCL | 0644);
  CHECK(kid >= 0, "shmget(clave, IPC_CREAT|IPC_EXCL)");
  errno = 0;
  CHECK(shmget(k, 8192, IPC_CREAT | IPC_EXCL | 0644) == -1 && errno == EEXIST,
        "otra vez IPC_EXCL -> EEXIST");
  CHECK(shmget(k, 4096, 0) == kid, "buscar por clave da el mismo id");
  errno = 0;
  CHECK(shmget(k, 1 << 20, 0) == -1 && errno == EINVAL, "pedir más tamaño -> EINVAL");
  errno = 0;
  CHECK(shmget(0x41555231, 4096, 0) == -1 && errno == ENOENT, "clave inexistente -> ENOENT");
  CHECK(shmctl(kid, IPC_RMID, NULL) == 0, "IPC_RMID de la clave");
  errno = 0;
  CHECK(shmat(kid, NULL, 0) == (void *)-1 && errno == EINVAL, "shmat tras IPC_RMID -> EINVAL");

  // IPC_RMID con el segmento enganchado: la memoria sigue hasta shmdt.
  CHECK(shmctl(id, IPC_RMID, NULL) == 0, "IPC_RMID con el segmento en uso");
  CHECK(!strcmp(p, "hola desde el padre"), "la memoria sigue viva");
  errno = 0;
  CHECK(shmdt(p + 4096) == -1 && errno == EINVAL, "shmdt en mitad del segmento -> EINVAL");
  CHECK(shmdt(p) == 0, "shmdt");
  errno = 0;
  CHECK(shmdt(p) == -1 && errno == EINVAL, "shmdt dos veces -> EINVAL");

  struct shminfo si;
  CHECK(shmctl(0, IPC_INFO, (struct shmid_ds *)&si) >= 0 && si.shmmax > 0, "IPC_INFO");
out:
  printf("SHM-TEST: %d/%d %s\n", pass, total, pass == total ? "OK" : "FALLOS");
  return pass == total ? 0 : 1;
}
