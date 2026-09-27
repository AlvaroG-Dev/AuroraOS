// user/apps/syscall_linux_test/main.c
//
// PR 3a — Test del ABI Linux del kernel sin libc ni crt0.
//
// Compilado con -nostdlib y sin linkear crt0.o. Define su propio _start.
// Hace syscalls con los NÚMEROS de Linux x86_64, para verificar que el
// dispatcher del kernel las enruta correctamente. Si esta app imprime
// "linux abi ok" y sale con código 0, el ABI Linux básico funciona.

#include <stdint.h>

// ---------------------------------------------------------------------------
// Primitivas de syscall. Convención Linux x86_64:
//   rax = número, rdi/rsi/rdx/r10/r8/r9 = args, retorno en rax.
//   "syscall" clobbea rcx (RIP de retorno) y r11 (RFLAGS).
// ---------------------------------------------------------------------------
static inline long sc0(long n) {
  long ret;
  __asm__ volatile("syscall" : "=a"(ret) : "a"(n) : "rcx", "r11", "memory");
  return ret;
}

static inline long sc1(long n, long a) {
  long ret;
  __asm__ volatile("syscall"
                   : "=a"(ret)
                   : "a"(n), "D"(a)
                   : "rcx", "r11", "memory");
  return ret;
}

static inline long sc3(long n, long a, long b, long c) {
  long ret;
  __asm__ volatile("syscall"
                   : "=a"(ret)
                   : "a"(n), "D"(a), "S"(b), "d"(c)
                   : "rcx", "r11", "memory");
  return ret;
}

// ---------------------------------------------------------------------------
// Helpers de salida (usan syscall write directamente).
// ---------------------------------------------------------------------------
static unsigned long my_strlen(const char *s) {
  const char *p = s;
  while (*p)
    p++;
  return (unsigned long)(p - s);
}

static long my_write(int fd, const char *buf, unsigned long len) {
  return sc3(1 /* SYS_WRITE Linux */, fd, (long)buf, (long)len);
}

static void my_puts(const char *s) { my_write(1, s, my_strlen(s)); }

static void my_putn(long v) {
  char buf[32];
  int i = 0;
  if (v == 0) {
    buf[i++] = '0';
  } else {
    long n = v;
    int neg = 0;
    if (n < 0) {
      neg = 1;
      n = -n;
    }
    char tmp[32];
    int t = 0;
    while (n > 0) {
      tmp[t++] = '0' + (char)(n % 10);
      n /= 10;
    }
    if (neg)
      buf[i++] = '-';
    while (t > 0)
      buf[i++] = tmp[--t];
  }
  buf[i++] = '\n';
  my_write(1, buf, (unsigned long)i);
}

// ---------------------------------------------------------------------------
// Entry point.
// ---------------------------------------------------------------------------
__attribute__((noreturn)) void _start(void) {
  my_puts("[linux-abi] begin\n");

  // Test 1: write a stdout (nº Linux 1).
  long ret_write = my_write(1, "linux abi ok\n", 13);
  my_puts("[linux-abi] write ret=");
  my_putn(ret_write);

  // Test 2: getpid (nº Linux 39).
  long pid = sc0(39);
  my_puts("[linux-abi] getpid=");
  my_putn(pid);

  // Test 3: exit_group (nº Linux 231) con código 0.
  my_puts("[linux-abi] exiting\n");
  sc1(231, 0);

  // No alcanzable. Si llegamos aquí, exit_group falló.
  my_puts("[linux-abi] ERROR: exit_group returned\n");
  for (;;)
    __asm__ volatile("hlt");
}