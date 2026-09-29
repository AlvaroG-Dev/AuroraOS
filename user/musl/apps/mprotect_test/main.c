// user/musl/apps/mprotect_test/main.c
//
// [4.3] Verifica mprotect real: permisos cambian, y un acceso inválido
// entrega SIGSEGV que el handler puede capturar con sigsetjmp.

#define _GNU_SOURCE
#include <errno.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>


static sigjmp_buf g_jmp;
static volatile int g_sig = 0;

static void segv_handler(int sig) {
  g_sig = sig;
  siglongjmp(g_jmp, 1);
}

static int g_fail = 0;

#define CHECK(cond, msg)                                                       \
  do {                                                                         \
    if (cond)                                                                  \
      printf("  [PASS] %s\n", msg);                                            \
    else {                                                                     \
      printf("  [FAIL] %s (errno=%d)\n", msg, errno);                          \
      g_fail++;                                                                \
    }                                                                          \
  } while (0)

int main(void) {
  printf("=== mprotect_test ===\n");

  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = segv_handler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_NODEFER;
  sigaction(SIGSEGV, &sa, NULL);
  sigaction(SIGBUS, &sa, NULL);

  // 1. mmap RW.
  size_t pg = 4096;
  char *p = mmap(NULL, pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS,
                 -1, 0);
  CHECK(p != MAP_FAILED, "mmap RW");
  if (p == MAP_FAILED)
    return 1;

  p[0] = 'A';
  CHECK(p[0] == 'A', "escritura en RW OK");

  // 2. mprotect → R.
  int rc = mprotect(p, pg, PROT_READ);
  CHECK(rc == 0, "mprotect RW→R");

  // 3. Lectura sigue OK.
  g_sig = 0;
  volatile char c = p[0];
  CHECK(g_sig == 0 && c == 'A', "lectura en R OK");

  // 4. Escritura → SIGSEGV capturado.
  g_sig = 0;
  if (sigsetjmp(g_jmp, 1) == 0) {
    p[0] = 'B';
    CHECK(0, "escritura en R debería fallar");
  } else {
    CHECK(g_sig == SIGSEGV || g_sig == SIGBUS, "escritura en R → SIGSEGV");
  }

  // 5. Volver a RW → escritura OK.
  rc = mprotect(p, pg, PROT_READ | PROT_WRITE);
  CHECK(rc == 0, "mprotect R→RW");
  p[0] = 'C';
  CHECK(p[0] == 'C', "escritura tras R→RW OK");

  // 6. PROT_NONE → lectura falla.
  rc = mprotect(p, pg, PROT_NONE);
  CHECK(rc == 0, "mprotect → PROT_NONE");
  g_sig = 0;
  if (sigsetjmp(g_jmp, 1) == 0) {
    volatile char x = p[0];
    (void)x;
    CHECK(0, "lectura en PROT_NONE debería fallar");
  } else {
    CHECK(g_sig == SIGSEGV || g_sig == SIGBUS, "PROT_NONE → SIGSEGV en read");
  }

  // 7. Restaurar y verificar que sigue viva.
  rc = mprotect(p, pg, PROT_READ | PROT_WRITE);
  CHECK(rc == 0, "mprotect PROT_NONE → RW");
  p[0] = 'D';
  CHECK(p[0] == 'D', "escritura tras PROT_NONE → RW");

  // 8. mprotect con addr no alineado vía syscall cruda → EINVAL.
  // Nota: mprotect() de la libc redondea addr a página antes de llamar
  // al kernel, así que para probar la validación del kernel hay que
  // invocar la syscall directamente con syscall().
  errno = 0;
  rc = syscall(SYS_mprotect, (void *)((uintptr_t)p + 1), pg, PROT_READ);
  CHECK(rc < 0 && errno == EINVAL, "sys_mprotect addr no alineado → EINVAL");

  munmap(p, pg);

  printf("=== %s (%d fallos) ===\n", g_fail ? "FAIL" : "PASS", g_fail);
  return g_fail ? 1 : 0;
}