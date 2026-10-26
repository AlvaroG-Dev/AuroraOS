// rseq_test: registro de rseq(2), cpu_id, aborto de la seccion critica por senal.
#define _GNU_SOURCE
#include <errno.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#define RSEQ_SIG 0x53053053u
struct rseq_area {
  uint32_t cpu_id_start, cpu_id;
  uint64_t rseq_cs;
  uint32_t flags, node_id, mm_cid, pad;
} __attribute__((aligned(32)));
struct rseq_cs {
  uint32_t version, flags;
  uint64_t start_ip, post_commit_offset, abort_ip;
} __attribute__((aligned(32)));

static struct rseq_area g_rseq;
volatile int g_aborted;
volatile int g_stop;
extern struct rseq_cs g_cs;

static int g_fail;
#define CHECK(c, ...) do { if (c) { printf("[rseq_test] PASS " __VA_ARGS__); printf("\n"); } \
  else { printf("[rseq_test] FAIL " __VA_ARGS__); printf(" (errno=%d)\n", errno); g_fail++; } } while (0)

// Seccion critica: bucle infinito entre `start` y `end`; al abortar el kernel
// salta a `abort` (precedido de la firma). g_cs apunta a esos simbolos.
__asm__(
    ".text\n"
    ".globl rseq_spin\n"
    "rseq_spin:\n"
    "  leaq g_cs(%rip), %rax\n"
    "  movq %rax, g_rseq+8(%rip)\n"   // rseq_cs = &g_cs
    "rseq_start:\n"
    "  cmpl $0, g_stop(%rip)\n"
    "  je rseq_start\n"
    "rseq_end:\n"
    "  movq $0, g_rseq+8(%rip)\n"
    "  xorl %eax, %eax\n"
    "  ret\n"
    "  .long 0x53053053\n"            // firma justo antes de abort_ip
    "rseq_abort:\n"
    "  movl $1, g_aborted(%rip)\n"
    "  movq $0, g_rseq+8(%rip)\n"
    "  movl $1, %eax\n"
    "  ret\n");
extern int rseq_spin(void);
extern char rseq_start[], rseq_end[], rseq_abort[];
struct rseq_cs g_cs;

static void on_alrm(int s) { (void)s; }

int main(void) {
  long rc = syscall(SYS_rseq, &g_rseq, sizeof(g_rseq), 0, RSEQ_SIG);
  CHECK(rc == 0, "rseq registro (rc=%ld)", rc);
  int cpu = sched_getcpu();
  CHECK(g_rseq.cpu_id < 64 && (int)g_rseq.cpu_id >= 0, "cpu_id=%u valido (getcpu=%d)", g_rseq.cpu_id, cpu);
  errno = 0;
  rc = syscall(SYS_rseq, &g_rseq, sizeof(g_rseq), 0, RSEQ_SIG);
  CHECK(rc == -1 && errno == EBUSY, "doble registro: EBUSY");
  errno = 0;
  rc = syscall(SYS_rseq, &g_rseq, sizeof(g_rseq), 0, RSEQ_SIG + 1);
  CHECK(rc == -1 && errno == EINVAL, "registro con otra firma: EINVAL");
  errno = 0;
  rc = syscall(SYS_rseq, (char *)&g_rseq + 1, sizeof(g_rseq), 1, RSEQ_SIG);
  CHECK(rc == -1 && errno == EINVAL, "desregistro con otra direccion: EINVAL");

  // Seccion critica interrumpida por una senal -> salta a abort_ip.
  g_cs.version = 0;
  g_cs.flags = 0;
  g_cs.start_ip = (uint64_t)rseq_start;
  g_cs.post_commit_offset = (uint64_t)(rseq_end - rseq_start);
  g_cs.abort_ip = (uint64_t)rseq_abort;
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = on_alrm;
  sigaction(SIGALRM, &sa, NULL);
  alarm(1);
  int r = rseq_spin();
  CHECK(r == 1 && g_aborted == 1, "la senal aborta la seccion critica (salto a abort_ip)");
  CHECK(g_rseq.rseq_cs == 0, "rseq_cs queda a 0 tras abortar");

  // Sin interrupcion: la seccion termina por su camino normal.
  g_aborted = 0;
  g_stop = 1;
  r = rseq_spin();
  CHECK(r == 0 && g_aborted == 0, "sin interrupcion no hay aborto");

  rc = syscall(SYS_rseq, &g_rseq, sizeof(g_rseq), 1, RSEQ_SIG);
  CHECK(rc == 0, "desregistro");
  errno = 0;
  rc = syscall(SYS_rseq, &g_rseq, sizeof(g_rseq), 1, RSEQ_SIG);
  CHECK(rc == -1 && errno == EINVAL, "segundo desregistro: EINVAL");

  printf("[rseq_test] %s\n", g_fail ? "FAILED" : "ALL PASS");
  return g_fail ? 1 : 0;
}
