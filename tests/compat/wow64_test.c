#define _GNU_SOURCE
// Lo que usa Wine en modo WoW64 (programas Windows de 32 bits): selectores
// estilo Linux (0x23 / 0x2B / 0x33), set_thread_area para el %fs de 32 bits,
// saltar a código de 32 bits y volver, señales que llegan en 32 bits,
// sigaltstack por hilo, excepciones de CPU como señales y FPU en señales.
// Compilar: gcc -static -O1 -fno-stack-protector -no-pie -pthread
#include <asm/ldt.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <ucontext.h>
#include <unistd.h>

static int pass, fail;
#define CHECK(c, m) do { if (c) pass++; else { fail++; printf("FALLO: %s\n", m); } } while (0)

uint32_t stack32[2048] __attribute__((used)) __attribute__((aligned(16)));
volatile uint64_t saved_rsp __attribute__((used));
volatile uint32_t r32_fs0, r32_cs, r32_left;
static volatile int alrm_hits, alrm_in32;
static uint16_t fs_sel;

// Wine (signal_x86_64.c) pide el selector con la syscall i386 por int 0x80:
// el puntero tiene que estar por debajo de 4 GiB.
static long ia32_tls(int nr, struct user_desc *d) {
  long r;
  __asm__ volatile("int $0x80" : "=a"(r) : "0"((long)nr), "b"(d) : "memory");
  return r;
}

// Salta a 32 bits, lee %fs:0 y %cs, da `loops` vueltas y vuelve a 64 bits.
static void run32(uint32_t loops) {
  __asm__ volatile(
      "mov %%rsp, saved_rsp(%%rip)\n"
      "lea stack32+8192(%%rip), %%rsp\n"
      "mov %%ss, %%ax\n mov %%ax, %%ds\n mov %%ax, %%es\n"
      "mov %0, %%ecx\n"
      "pushq $0x23\n"
      "lea 1f(%%rip), %%rax\n"
      "pushq %%rax\n"
      "lretq\n"
      ".code32\n"
      "1: movl %%fs:0, %%eax\n"
      "   movl %%eax, r32_fs0\n"
      "   movl %%cs, %%eax\n"
      "   movl %%eax, r32_cs\n"
      "3: decl %%ecx\n"
      "   jnz 3b\n"
      "   movl %%fs:4, %%eax\n"
      "   movl %%eax, r32_left\n"
      "   pushl $0x33\n"
      "   pushl $2f\n"
      "   lret\n"
      ".code64\n"
      "2: mov saved_rsp(%%rip), %%rsp\n"
      :
      : "r"(loops)
      : "rax", "rcx", "rdx", "memory", "cc");
}

static void on_alrm(int s, siginfo_t *si, void *uc_) {
  (void)s; (void)si;
  ucontext_t *uc = uc_;
  alrm_hits++;
  if ((uc->uc_mcontext.gregs[REG_CSGSFS] & 0xffff) == 0x23) alrm_in32++;
}

static sigjmp_buf jb;
static volatile int got_sig, got_trapno, got_err, got_code;
static volatile uint64_t got_addr, got_cr2;
static void on_fault(int s, siginfo_t *si, void *uc_) {
  ucontext_t *uc = uc_;
  got_sig = s; got_code = si->si_code; got_addr = (uint64_t)si->si_addr;
  got_trapno = uc->uc_mcontext.gregs[REG_TRAPNO];
  got_err = uc->uc_mcontext.gregs[REG_ERR];
  got_cr2 = uc->uc_mcontext.gregs[REG_CR2];
  siglongjmp(jb, 1);
}

static void on_usr1_xmm(int s) {
  (void)s;
  __asm__ volatile("pxor %%xmm0, %%xmm0\n movq $77, %%rax\n movq %%rax, %%xmm1" ::: "xmm0", "xmm1", "rax");
}

static char *thr_stack[2];
static volatile int thr_ok[2];
static void on_usr2(int s) {
  (void)s;
  char here;
  for (int i = 0; i < 2; i++)
    if (&here > thr_stack[i] && &here < thr_stack[i] + 65536) thr_ok[i]++;
}
static void *thr(void *a) {
  int i = (int)(intptr_t)a;
  stack_t ss = {.ss_sp = thr_stack[i], .ss_size = 65536, .ss_flags = 0};
  sigaltstack(&ss, 0);
  usleep(20000);  // el otro hilo pone la suya mientras tanto
  for (int k = 0; k < 5; k++) { syscall(SYS_tgkill, getpid(), syscall(SYS_gettid), SIGUSR2); usleep(1000); }
  return 0;
}

int main(void) {
  uint16_t cs, ss;
  __asm__("mov %%cs, %0\n mov %%ss, %1" : "=r"(cs), "=r"(ss));
  CHECK(cs == 0x33, "%cs de 64 bits = 0x33");
  CHECK(ss == 0x2b, "%ss = 0x2b");

  // TLS de 32 bits en memoria baja.
  uint32_t *teb = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
  teb[0] = 0x7eb7eb; teb[1] = 0x1e57;
  struct user_desc *dp = (struct user_desc *)(teb + 256);
  struct user_desc d = {.entry_number = -1, .base_addr = (uint32_t)(uintptr_t)teb, .limit = 0xfff,
                        .seg_32bit = 1, .contents = 0, .read_exec_only = 0, .limit_in_pages = 0,
                        .seg_not_present = 0, .useable = 1};
  *dp = d;
  long r = ia32_tls(243, dp);
  d = *dp;
  CHECK(r == 0 && d.entry_number >= 12 && d.entry_number <= 14, "set_thread_area(-1) da 12..14");
  struct user_desc g = {.entry_number = d.entry_number};
  *dp = g;
  long rg = ia32_tls(244, dp);
  g = *dp;
  CHECK(rg == 0 && g.base_addr == d.base_addr && g.limit == 0xfff &&
            g.seg_32bit == 1 && g.useable == 1, "get_thread_area devuelve lo mismo");
  fs_sel = (uint16_t)((d.entry_number << 3) | 3);

  // Saltar a 32 bits con %fs = TLS. Guardar el %fs de 64 bits (glibc).
  unsigned long fs64 = 0;
  syscall(SYS_arch_prctl, 0x1003 /* ARCH_GET_FS */, &fs64);
  __asm__ volatile("mov %0, %%fs" ::"r"(fs_sel));
  run32(1000);
  uint16_t fs_now;
  __asm__ volatile("mov %%fs, %0" : "=r"(fs_now));
  syscall(SYS_arch_prctl, 0x1002 /* ARCH_SET_FS */, fs64);
  CHECK(r32_cs == 0x23, "en 32 bits %cs = 0x23");
  CHECK(r32_fs0 == 0x7eb7eb, "en 32 bits %fs:0 = TEB");
  CHECK(r32_left == 0x1e57, "vuelta de 32 bits a 64");
  CHECK(fs_now == fs_sel, "%fs conserva el selector TLS");

  // Señales mientras se ejecuta código de 32 bits; %fs sobrevive a
  // interrupciones y cambios de contexto.
  struct sigaction sa = {0};
  sa.sa_sigaction = on_alrm; sa.sa_flags = SA_SIGINFO | SA_RESTART;
  sigaction(SIGALRM, &sa, 0);
  struct itimerval it = {{0, 2000}, {0, 2000}};
  setitimer(ITIMER_REAL, &it, 0);
  int rounds = 0;
  for (; rounds < 400 && alrm_in32 < 3; rounds++) {
    __asm__ volatile("mov %0, %%fs" ::"r"(fs_sel));
    r32_left = 0;
    run32(3000000);
    syscall(SYS_arch_prctl, 0x1002, fs64);
    if (r32_left != 0x1e57) break;
  }
  struct itimerval off = {{0, 0}, {0, 0}};
  setitimer(ITIMER_REAL, &off, 0);
  CHECK(r32_left == 0x1e57, "%fs:4 correcto tras señales/IRQ en 32 bits");
  CHECK(alrm_in32 >= 1, "señal recibida con %cs = 0x23 en sigcontext");
  printf("  (señales: %d, en 32 bits: %d, vueltas: %d)\n", alrm_hits, alrm_in32, rounds);

  // Borrar la entrada TLS.
  struct user_desc e = {.entry_number = d.entry_number, .read_exec_only = 1, .seg_not_present = 1};
  *dp = e;
  CHECK(ia32_tls(243, dp) == 0, "borrar entrada TLS");

  // Excepciones de CPU como señales.
  sa.sa_sigaction = on_fault; sa.sa_flags = SA_SIGINFO | SA_NODEFER;
  sigaction(SIGSEGV, &sa, 0); sigaction(SIGILL, &sa, 0); sigaction(SIGTRAP, &sa, 0);
  sigaction(SIGFPE, &sa, 0); sigaction(SIGBUS, &sa, 0);
  if (!sigsetjmp(jb, 1)) { *(volatile int *)0x1234 = 1; }
  CHECK(got_sig == SIGSEGV && got_addr == 0x1234 && got_trapno == 14 && (got_err & 2) &&
            got_cr2 == 0x1234 && got_code == SEGV_MAPERR, "escritura en 0x1234: SIGSEGV con trapno 14, err, cr2");
  char *ro = mmap(0, 4096, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  got_sig = 0;
  if (!sigsetjmp(jb, 1)) { volatile char c = ro[0]; (void)c; ro[1] = 1; }
  CHECK(got_sig == SIGSEGV && got_code == SEGV_ACCERR && got_addr == (uint64_t)(ro + 1), "escritura en página RO: SEGV_ACCERR");
  got_sig = 0;
  if (!sigsetjmp(jb, 1)) { __asm__ volatile("ud2"); }
  CHECK(got_sig == SIGILL && got_trapno == 6, "ud2: SIGILL trapno 6");
  got_sig = 0;
  if (!sigsetjmp(jb, 1)) { __asm__ volatile("int3"); }
  CHECK(got_sig == SIGTRAP && got_trapno == 3, "int3: SIGTRAP trapno 3");
  got_sig = 0;
  if (!sigsetjmp(jb, 1)) { volatile int z = 0; volatile int q = 5 / z; (void)q; }
  CHECK(got_sig == SIGFPE && got_trapno == 0 && got_code == FPE_INTDIV, "división por cero: SIGFPE");
  got_sig = 0;
  if (!sigsetjmp(jb, 1)) { __asm__ volatile("mov $0x10, %%ax\n mov %%ax, %%ds" ::: "ax"); }
  CHECK(got_sig == SIGSEGV && got_trapno == 13, "cargar selector del kernel: SIGSEGV trapno 13 (#GP)");
  __asm__ volatile("mov %%ss, %%ax\n mov %%ax, %%ds" ::: "ax");

  // FPU/SSE del código interrumpido se conserva tras el manejador.
  signal(SIGUSR1, on_usr1_xmm);
  uint64_t x0 = 0, x1 = 0;
  __asm__ volatile("movq $0x1234567, %%rax\n movq %%rax, %%xmm0\n movq $5, %%rax\n movq %%rax, %%xmm1\n"
                   "mov $10, %%eax\n mov %1, %%edi\n mov %2, %%esi\n mov $200, %%eax\n syscall\n"
                   "movq %%xmm0, %0\n"
                   : "=r"(x0) : "r"((int)syscall(SYS_gettid)), "r"(SIGUSR1) : "rax", "rdi", "rsi", "rcx", "r11", "xmm0", "xmm1", "memory");
  __asm__ volatile("movq %%xmm1, %0" : "=r"(x1));
  CHECK(x0 == 0x1234567 && x1 == 5, "xmm0/xmm1 intactos tras la señal");

  // sigaltstack por hilo.
  thr_stack[0] = mmap(0, 65536, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  thr_stack[1] = mmap(0, 65536, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  struct sigaction s2 = {0};
  s2.sa_handler = on_usr2; s2.sa_flags = SA_ONSTACK;
  sigaction(SIGUSR2, &s2, 0);
  pthread_t t0, t1;
  pthread_create(&t0, 0, thr, (void *)0);
  usleep(5000);
  pthread_create(&t1, 0, thr, (void *)1);
  pthread_join(t0, 0); pthread_join(t1, 0);
  CHECK(thr_ok[0] == 5 && thr_ok[1] == 5, "cada hilo usa su altstack");

  printf("WOW64-TEST: %d ok, %d fallos %s\n", pass, fail, fail ? "FAIL" : "ALL PASS");
  return fail != 0;
}
