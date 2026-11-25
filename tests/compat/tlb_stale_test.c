// munmap con varios hilos: otro hilo que esta escribiendo en la pagina desde
// otra CPU tiene que fallar (SIGSEGV) en cuanto munmap vuelve. Si el kernel
// liberara la fisica sin invalidar el TLB de esa CPU, el hilo seguiria
// escribiendo en un frame ya entregado a otro: aqui, a las paginas de un memfd
// recien creado (como la RAM de la mini-VM). Se buscan sus marcas ahi.
#define _GNU_SOURCE
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define MARK 0x5AFEDEADBEEF0000ULL
#define QPAGES 64

static volatile uint64_t *volatile g_p;
static volatile int g_stop, g_gen;
static volatile int g_faults;
static volatile unsigned long g_writes;
static __thread sigjmp_buf t_jb;

static void on_segv(int s) {
  (void)s;
  siglongjmp(t_jb, 1);
}

// Cada hilo coge la pagina de la ronda y escribe sin parar hasta que falle
// (munmap). Despues espera la ronda siguiente.
static void *writer(void *a) {
  (void)a;
  int seen = 0;
  unsigned long it = 0;
  while (!g_stop) {
    if (sigsetjmp(t_jb, 1)) {
      __atomic_add_fetch(&g_faults, 1, __ATOMIC_SEQ_CST);
      continue;
    }
    while (g_gen == seen && !g_stop)
      ;
    seen = g_gen;
    volatile uint64_t *p = g_p;
    if (!p)
      continue;
    for (;;) {
      for (int i = 0; i < 512; i++)
        p[i] = MARK | (it & 0xFFFF);
      it++;
      __atomic_add_fetch(&g_writes, 1, __ATOMIC_RELAXED);
    }
  }
  return NULL;
}

int main(void) {
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_handler = on_segv;
  sigaction(SIGSEGV, &sa, NULL);
  pthread_t th[3];
  for (int i = 0; i < 3; i++)
    pthread_create(&th[i], NULL, writer, NULL);
  int bad = 0, rounds = 300, nofault = 0, stale = 0;
  for (int r = 0; r < rounds; r++) {
    uint64_t *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
      printf("mmap fallo\n");
      return 1;
    }
    p[0] = 1;
    int f0 = g_faults;
    unsigned long w0 = g_writes;
    g_p = p;
    g_gen = r + 1;
    while (g_writes < w0 + 200)
      ;
    munmap(p, 4096);
    // La direccion vieja queda reservada sin acceso: q no puede caer ahi (los
    // hilos escribirian en q de forma legitima).
    void *guard = mmap(p, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    // Paginas nuevas de un memfd: el asignador suele dar primero el frame que
    // acaba de quedar libre.
    int fd = memfd_create("tlbq", 0);
    if (ftruncate(fd, QPAGES * 4096) < 0)
      return 2;
    uint64_t *q = mmap(NULL, QPAGES * 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    for (int k = 0; k < QPAGES; k++)
      q[k * 512] = 0;
    usleep(2000);
    for (int k = 0; k < QPAGES * 512; k++)
      if ((q[k] & ~0xFFFFULL) == MARK) {
        bad++;
        printf("FALLO ronda %d: marca de un hilo en la pagina %d del memfd\n", r, k / 512);
        break;
      }
    // Tras munmap ningun hilo puede seguir escribiendo: si el contador sube,
    // alguno escribe por una traduccion vieja del TLB en un frame liberado.
    unsigned long wa = g_writes;
    usleep(5000);
    if (g_writes != wa) {
      stale++;
      if (stale <= 5)
        printf("FALLO ronda %d: %lu escrituras despues de munmap\n", r, g_writes - wa);
    }
    if (g_faults - f0 < 3)
      nofault++; // algun hilo no llego a escribir en esta ronda
    munmap(q, QPAGES * 4096);
    close(fd);
    if (guard != MAP_FAILED)
      munmap(guard, 4096);
  }
  g_stop = 1;
  g_gen = -1;
  for (int i = 0; i < 3; i++)
    pthread_join(th[i], NULL);
  printf("TLB-STALE-TEST: %d rondas, %d con marcas ajenas, %d con escrituras tras munmap, %d con "
         "algun hilo parado %s\n", rounds, bad, stale, nofault, bad || stale ? "FALLOS" : "OK");
  return bad || stale;
}
