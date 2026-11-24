// tests/fpu/fpu_test.c
//
// El kernel (compilado con SSE) no debe pisar XMM/YMM/MXCSR de userland en
// syscalls ni en interrupciones. Freestanding, corre como /usr/bin/init (ver
// tests/fpu/run.sh). Varios procesos (fork) con patrones distintos fuerzan
// ademas cambios de contexto con estado FPU vivo.
// Salida: "FPU-TEST: PASS ..." / "FPU-TEST: FAIL ..." y "FPU-TEST: DONE".
typedef unsigned long u64;
typedef long i64;

static i64 sc(i64 n, i64 a, i64 b, i64 c) {
  i64 r;
  __asm__ volatile("syscall"
                   : "=a"(r)
                   : "a"(n), "D"(a), "S"(b), "d"(c)
                   : "rcx", "r11", "memory");
  return r;
}
static void out(const char *s) {
  u64 n = 0;
  while (s[n]) n++;
  sc(1, 1, (i64)s, n);
}
static void outnum(const char *p, u64 v) {
  char b[48];
  int n = 0;
  while (p[n]) { b[n] = p[n]; n++; }
  char t[24];
  int k = 0;
  if (!v) t[k++] = '0';
  while (v) { t[k++] = '0' + v % 10; v /= 10; }
  while (k) b[n++] = t[--k];
  b[n++] = '\n';
  b[n] = 0;
  out(b);
}

static unsigned char pat[512] __attribute__((aligned(64)));
static unsigned char res[512] __attribute__((aligned(64)));

// Un lote: carga xmm0-15 (o ymm0-15), hace `nsys` syscalls y espera `spin`
// vueltas (para que caigan IRQ de timer), y vuelca los registros.
static void batch_sse(int nsys, int spin) {
  __asm__ volatile(
      "movdqa 0(%[p]), %%xmm0\n movdqa 16(%[p]), %%xmm1\n"
      "movdqa 32(%[p]), %%xmm2\n movdqa 48(%[p]), %%xmm3\n"
      "movdqa 64(%[p]), %%xmm4\n movdqa 80(%[p]), %%xmm5\n"
      "movdqa 96(%[p]), %%xmm6\n movdqa 112(%[p]), %%xmm7\n"
      "movdqa 128(%[p]), %%xmm8\n movdqa 144(%[p]), %%xmm9\n"
      "movdqa 160(%[p]), %%xmm10\n movdqa 176(%[p]), %%xmm11\n"
      "movdqa 192(%[p]), %%xmm12\n movdqa 208(%[p]), %%xmm13\n"
      "movdqa 224(%[p]), %%xmm14\n movdqa 240(%[p]), %%xmm15\n"
      "1: test %[ns], %[ns]\n jz 2f\n"
      "mov $39, %%eax\n syscall\n dec %[ns]\n jmp 1b\n"
      "2: test %[sp], %[sp]\n jz 3f\n pause\n dec %[sp]\n jmp 2b\n"
      "3:\n"
      "movdqa %%xmm0, 0(%[r])\n movdqa %%xmm1, 16(%[r])\n"
      "movdqa %%xmm2, 32(%[r])\n movdqa %%xmm3, 48(%[r])\n"
      "movdqa %%xmm4, 64(%[r])\n movdqa %%xmm5, 80(%[r])\n"
      "movdqa %%xmm6, 96(%[r])\n movdqa %%xmm7, 112(%[r])\n"
      "movdqa %%xmm8, 128(%[r])\n movdqa %%xmm9, 144(%[r])\n"
      "movdqa %%xmm10, 160(%[r])\n movdqa %%xmm11, 176(%[r])\n"
      "movdqa %%xmm12, 192(%[r])\n movdqa %%xmm13, 208(%[r])\n"
      "movdqa %%xmm14, 224(%[r])\n movdqa %%xmm15, 240(%[r])\n"
      : [ns] "+r"(nsys), [sp] "+r"(spin)
      : [p] "r"(pat), [r] "r"(res)
      : "rax", "rcx", "r11", "memory", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4",
        "xmm5", "xmm6", "xmm7", "xmm8", "xmm9", "xmm10", "xmm11", "xmm12",
        "xmm13", "xmm14", "xmm15");
}

static void batch_avx(int nsys, int spin) {
  __asm__ volatile(
      "vmovdqa 0(%[p]), %%ymm0\n vmovdqa 32(%[p]), %%ymm1\n"
      "vmovdqa 64(%[p]), %%ymm2\n vmovdqa 96(%[p]), %%ymm3\n"
      "vmovdqa 128(%[p]), %%ymm4\n vmovdqa 160(%[p]), %%ymm5\n"
      "vmovdqa 192(%[p]), %%ymm6\n vmovdqa 224(%[p]), %%ymm7\n"
      "vmovdqa 256(%[p]), %%ymm8\n vmovdqa 288(%[p]), %%ymm9\n"
      "vmovdqa 320(%[p]), %%ymm10\n vmovdqa 352(%[p]), %%ymm11\n"
      "vmovdqa 384(%[p]), %%ymm12\n vmovdqa 416(%[p]), %%ymm13\n"
      "vmovdqa 448(%[p]), %%ymm14\n vmovdqa 480(%[p]), %%ymm15\n"
      "1: test %[ns], %[ns]\n jz 2f\n"
      "mov $39, %%eax\n syscall\n dec %[ns]\n jmp 1b\n"
      "2: test %[sp], %[sp]\n jz 3f\n pause\n dec %[sp]\n jmp 2b\n"
      "3:\n"
      "vmovdqa %%ymm0, 0(%[r])\n vmovdqa %%ymm1, 32(%[r])\n"
      "vmovdqa %%ymm2, 64(%[r])\n vmovdqa %%ymm3, 96(%[r])\n"
      "vmovdqa %%ymm4, 128(%[r])\n vmovdqa %%ymm5, 160(%[r])\n"
      "vmovdqa %%ymm6, 192(%[r])\n vmovdqa %%ymm7, 224(%[r])\n"
      "vmovdqa %%ymm8, 256(%[r])\n vmovdqa %%ymm9, 288(%[r])\n"
      "vmovdqa %%ymm10, 320(%[r])\n vmovdqa %%ymm11, 352(%[r])\n"
      "vmovdqa %%ymm12, 384(%[r])\n vmovdqa %%ymm13, 416(%[r])\n"
      "vmovdqa %%ymm14, 448(%[r])\n vmovdqa %%ymm15, 480(%[r])\n"
      "vzeroupper\n"
      : [ns] "+r"(nsys), [sp] "+r"(spin)
      : [p] "r"(pat), [r] "r"(res)
      : "rax", "rcx", "r11", "memory", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4",
        "xmm5", "xmm6", "xmm7", "xmm8", "xmm9", "xmm10", "xmm11", "xmm12",
        "xmm13", "xmm14", "xmm15");
}

static int has_avx(void) {
  unsigned a, b, c, d;
  __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
  if (!(c & (1u << 27)) || !(c & (1u << 28))) return 0;
  unsigned lo, hi;
  __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
  return (lo & 6) == 6;
}

// Devuelve nº de lotes erroneos.
static u64 run(unsigned seed, int avx, int rounds) {
  u64 bad = 0;
  int sz = avx ? 512 : 256;
  for (int r = 0; r < rounds; r++) {
    for (int i = 0; i < sz; i++)
      pat[i] = (unsigned char)(seed * 31 + i * 7 + r);
    if (avx) batch_avx(300, 200000); else batch_sse(300, 200000);
    for (int i = 0; i < sz; i++)
      if (res[i] != pat[i]) { bad++; break; }
  }
  return bad;
}

void _start(void) {
  int avx = has_avx();
  outnum("FPU-TEST: avx=", avx);
  i64 pid = sc(57, 0, 0, 0); // fork
  unsigned seed = pid == 0 ? 7 : 3;
  u64 bad = run(seed, 0, 40);
  if (avx) bad += run(seed + 100, 1, 40);
  if (pid == 0) {
    if (bad) outnum("FPU-TEST: FAIL hijo lotes=", bad);
    else out("FPU-TEST: hijo ok\n");
    sc(60, bad ? 1 : 0, 0, 0);
  }
  i64 st = 0;
  sc(61, -1, (i64)&st, 0); // wait4
  if (bad) outnum("FPU-TEST: FAIL padre lotes=", bad);
  if (!bad && st == 0) out("FPU-TEST: PASS todo\n");
  else out("FPU-TEST: FAIL\n");
  out("FPU-TEST: DONE\n");
  sc(60, 0, 0, 0);
  for (;;) {}
}
