// Patrones de memoria de Wine: reservar con PROT_NONE, mapear encima con
// MAP_FIXED (anónimo y de fichero), mprotect de trozos, fork y escribir.
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

static int pass, fail;
#define CHECK(c, m) do { if (c) pass++; else { fail++; printf("FALLO: %s\n", m); } } while (0)

int main(void) {
  size_t R = 64 << 20;
  char *res = mmap((void *)0x40000000, R, PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
  CHECK(res == (void *)0x40000000, "reserva PROT_NONE en 0x40000000");
  char *a = mmap(res + 0x10000, 0x20000, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
  CHECK(a == res + 0x10000, "MAP_FIXED anónimo RW dentro");
  volatile char r = a[0x74c8]; (void)r;  // primero lectura (página cero)
  a[0x74c8] = 1;
  CHECK(a[0x74c8] == 1, "leer y luego escribir");
  CHECK(mprotect(a, 0x4000, PROT_READ) == 0, "mprotect RO trozo");
  CHECK(mprotect(a, 0x4000, PROT_READ | PROT_WRITE) == 0, "mprotect RW trozo");
  a[0] = 2; a[0x3000] = 3;
  CHECK(a[0] == 2, "escribir tras mprotect");
  // Fichero MAP_PRIVATE con MAP_FIXED sobre la reserva (como una imagen PE).
  int fd = open("/proc/self/exe", O_RDONLY);
  char *f = mmap(res + 0x100000, 0x3000, PROT_READ, MAP_PRIVATE | MAP_FIXED, fd, 0);
  CHECK(f == res + 0x100000, "fichero MAP_FIXED");
  CHECK(f[1] == 'E', "contenido del fichero");
  CHECK(mprotect(f, 0x3000, PROT_READ | PROT_WRITE) == 0, "mprotect RW fichero");
  f[0x1000] = 9;
  CHECK(f[0x1000] == 9, "escribir en fichero privado");
  // Anónimo compartido (como la memoria compartida con wineserver).
  char *s = mmap(res + 0x200000, 0x2000, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
  CHECK(s == res + 0x200000, "MAP_SHARED anónimo");
  s[5] = 5;
  a[0x8000] = 7;
  pid_t p = fork();
  if (p == 0) {
    a[0x74c8] = 11; a[0x8000] = 12; f[0x1000] = 13; s[5] = 6;
    _exit(a[0x74c8] == 11 && f[0x1000] == 13 ? 0 : 1);
  }
  a[0x74c8] = 21; a[0] = 22; f[0x1000] = 23; a[0x9000] = 1;
  CHECK(a[0x74c8] == 21 && f[0x1000] == 23, "padre escribe tras fork");
  int st; waitpid(p, &st, 0);
  CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "hijo escribe tras fork");
  CHECK(a[0x8000] == 7, "COW: el padre no ve lo del hijo");
  CHECK(s[5] == 6, "MAP_SHARED: el padre ve lo del hijo");
  // Mapear encima de algo ya mapeado y escrito (MAP_FIXED reemplaza).
  char *b = mmap(a, 0x10000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
  CHECK(b == a && b[0x74c8] == 0, "MAP_FIXED reemplaza (página a cero)");
  b[0x74c8] = 4;
  CHECK(b[0x74c8] == 4, "escribir tras reemplazo");
  // munmap de un trozo en medio y volver a reservar PROT_NONE.
  CHECK(munmap(a + 0x10000, 0x10000) == 0, "munmap trozo");
  char *c = mmap(a + 0x10000, 0x10000, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE, -1, 0);
  CHECK(c == a + 0x10000, "re-reserva PROT_NONE");
  CHECK(mprotect(c, 0x1000, PROT_READ | PROT_WRITE) == 0, "mprotect RW sobre reserva");
  c[10] = 1;
  CHECK(c[10] == 1, "escribir en reserva tras mprotect");
  // MAP_32BIT
  char *m32 = mmap(0, 0x1000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
  CHECK(m32 != MAP_FAILED && (uintptr_t)m32 < 0x80000000ULL, "MAP_32BIT bajo 2 GiB");
  printf("MMWINE-TEST: %d ok, %d fallos %s\n", pass, fail, fail ? "FAIL" : "ALL PASS");
  return fail != 0;
}
