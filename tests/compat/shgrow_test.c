// La memoria de sesión de wineserver: fichero temporal borrado que crece con
// pwrite(1 byte)+ftruncate y se mapea por trozos (offset != 0) en el
// servidor; otro proceso lo mapea entero o desde un offset y debe ver lo
// mismo, también lo escrito después de mapear.
#define _GNU_SOURCE
#include <fcntl.h>
#include <sys/stat.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

static int pass, fail;
#define CHECK(c, m) do { if (c) pass++; else { fail++; printf("FALLO: %s\n", m); } } while (0)
static void grow(int fd, off_t sz) { char z = 0; if (pwrite(fd, &z, 1, sz) == 1) (void)!ftruncate(fd, sz); }

int main(void) {
  mkdir("/tmp/shgrow", 0700);
  int fd = open("/tmp/shgrow/tmpmap-1", O_RDWR | O_CREAT | O_EXCL, 0600);
  unlink("/tmp/shgrow/tmpmap-1");
  const size_t B = 0x10000;
  grow(fd, B);
  volatile uint64_t *b0 = mmap(0, B, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  b0[1] = 0x1111;
  grow(fd, 2 * B + B / 2);  // 1.5x + 64K como grow_session_mapping
  volatile uint64_t *b1 = mmap(0, B + B / 2, PROT_READ | PROT_WRITE, MAP_SHARED, fd, B);
  CHECK(b0 != MAP_FAILED && b1 != MAP_FAILED, "mmap por trozos");
  b1[2] = 0x2222;            // offset B + 16
  b1[(B / 8) + 3] = 0x3333;  // offset 2B + 24
  int p[2]; (void)!pipe(p);
  pid_t c = fork();
  if (c == 0) {
    // proceso "cliente": vista entera desde 0 y vista desde B
    volatile uint64_t *v = mmap(0, 2 * B + B / 2, PROT_READ, MAP_SHARED, fd, 0);
    volatile uint64_t *w = mmap(0, B + B / 2, PROT_READ, MAP_SHARED, fd, B);
    int ok = v != MAP_FAILED && w != MAP_FAILED && v[1] == 0x1111 && v[B / 8 + 2] == 0x2222 &&
             v[2 * B / 8 + 3] == 0x3333 && w[2] == 0x2222 && w[B / 8 + 3] == 0x3333;
    char x; (void)!read(p[0], &x, 1);  // el servidor escribe después
    ok = ok && v[B / 8 + 5] == 0x5555 && w[5] == 0x5555 && v[7] == 0x7777;
    if (!ok) printf("  cliente: %lx %lx %lx | %lx %lx | %lx %lx\n", (long)v[1], (long)v[B/8+2], (long)v[2*B/8+3], (long)w[2], (long)w[B/8+3], (long)v[B/8+5], (long)v[7]);
    _exit(ok ? 0 : 1);
  }
  usleep(100000);
  b1[5] = 0x5555; b0[7] = 0x7777;
  (void)!write(p[1], "x", 1);
  int st; waitpid(c, &st, 0);
  CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "el otro proceso ve los trozos y lo escrito después");
  uint64_t r = 0; (void)!pread(fd, &r, 8, B + 16);
  CHECK(r == 0x2222, "pread ve lo escrito en el trozo 2");
  printf("SHGROW-TEST: %d ok, %d fallos %s\n", pass, fail, fail ? "FAIL" : "ALL PASS");
  return fail != 0;
}
