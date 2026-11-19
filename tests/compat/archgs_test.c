// ARCH_SET_GS / ARCH_GET_GS: Wine x86_64 guarda el TEB en %gs.
// Comprueba que %gs sobrevive a syscalls, señales, cambios de contexto,
// fork e hilos, y que cada hilo tiene la suya.
#include <asm/prctl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int pass, fail;
#define CHECK(c, m) do { if (c) pass++; else { fail++; printf("FALLO: %s\n", m); } } while (0)

static uint64_t buf_main[4] = {0x1111111111111111ULL, 0x2222, 0, 0};
static uint64_t buf_thr[4] = {0x3333333333333333ULL, 0x4444, 0, 0};

static uint64_t rd_gs0(void) { uint64_t v; __asm__ volatile("movq %%gs:0, %0" : "=r"(v)); return v; }
static long set_gs(void *p) { return syscall(SYS_arch_prctl, ARCH_SET_GS, (unsigned long)p); }
static uint64_t get_gs(void) { unsigned long v = 0; syscall(SYS_arch_prctl, ARCH_GET_GS, &v); return v; }

static volatile uint64_t sig_seen;
static void on_sig(int s) { (void)s; sig_seen = rd_gs0(); }

static void *thr(void *a) {
  (void)a;
  CHECK(set_gs(buf_thr) == 0, "hilo: ARCH_SET_GS");
  for (int i = 0; i < 20; i++) {
    struct timespec ts = {0, 2000000};
    nanosleep(&ts, 0);
    if (rd_gs0() != buf_thr[0]) { CHECK(0, "hilo: %gs cambió tras dormir"); return 0; }
  }
  CHECK(get_gs() == (uint64_t)buf_thr, "hilo: ARCH_GET_GS");
  return 0;
}

int main(int argc, char **argv) {
  if (argc > 1 && !strcmp(argv[1], "--exec")) {
    return get_gs() == 0 ? 0 : 1;
  }
  CHECK(get_gs() == 0, "GS inicial 0");
  CHECK(set_gs(buf_main) == 0, "ARCH_SET_GS");
  CHECK(rd_gs0() == buf_main[0], "lee %gs:0");
  CHECK(get_gs() == (uint64_t)buf_main, "ARCH_GET_GS");
  CHECK(set_gs((void *)0xffff800000000000ULL) != 0, "rechaza dirección de kernel");
  CHECK(rd_gs0() == buf_main[0], "sigue tras rechazo");
  for (int i = 0; i < 1000; i++) getppid();
  CHECK(rd_gs0() == buf_main[0], "tras 1000 syscalls");
  signal(SIGUSR1, on_sig);
  raise(SIGUSR1);
  CHECK(sig_seen == buf_main[0], "dentro del manejador de señal");
  CHECK(rd_gs0() == buf_main[0], "tras volver de la señal");
  pthread_t t;
  pthread_create(&t, 0, thr, 0);
  for (int i = 0; i < 20; i++) {
    struct timespec ts = {0, 2000000};
    nanosleep(&ts, 0);
    if (rd_gs0() != buf_main[0]) { CHECK(0, "principal: %gs cambió con el hilo"); break; }
  }
  pthread_join(t, 0);
  CHECK(rd_gs0() == buf_main[0], "principal: %gs propia tras hilo");
  volatile uint64_t spin = 0;
  for (uint64_t i = 0; i < 50000000ULL; i++) spin += i;  // deja que entren IRQ de reloj
  CHECK(rd_gs0() == buf_main[0], "tras bucle largo (IRQ)");
  pid_t p = fork();
  if (p == 0) _exit(rd_gs0() == buf_main[0] && get_gs() == (uint64_t)buf_main ? 0 : 1);
  int st = 0;
  waitpid(p, &st, 0);
  CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "fork hereda %gs");
  p = fork();
  if (p == 0) {
    execl("/proc/self/exe", "archgs_test", "--exec", (char *)0);
    execl(argv[0], "archgs_test", "--exec", (char *)0);
    _exit(3);
  }
  waitpid(p, &st, 0);
  CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "execve deja %gs a 0");
  if (WIFEXITED(st) && WEXITSTATUS(st) == 3) printf("  (execve falló)\n");
  printf("ARCHGS-TEST: %d ok, %d fallos %s\n", pass, fail, fail ? "FAIL" : "ALL PASS");
  return fail != 0;
}
