// apps/pftest/main.c
// Tests exhaustivos de demand paging, stack growth y mmap.
#include "../../lib/file.h"
#include "../../lib/malloc.h"
#include "../../lib/process.h"
#include "../../lib/string.h"
#include "../../syscall.h"

#define MMAP_PROT_READ 0x1
#define MMAP_PROT_WRITE 0x2
#define MMAP_PROT_EXEC 0x4

static int passed = 0;
static int failed = 0;

static void print_int(const char *prefix, int v) {
  char buf[16];
  int i = 0;
  int neg = 0;
  uint32_t uv;
  if (v < 0) {
    neg = 1;
    uv = (uint32_t)(-v);
  } else {
    uv = (uint32_t)v;
  }
  if (uv == 0)
    buf[i++] = '0';
  while (uv) {
    buf[i++] = '0' + (int)(uv % 10);
    uv /= 10;
  }
  if (neg)
    buf[i++] = '-';
  for (int a = 0, b = i - 1; a < b; a++, b--) {
    char t = buf[a];
    buf[a] = buf[b];
    buf[b] = t;
  }
  buf[i] = 0;
  char out[128];
  int oi = 0, pi = 0, bi = 0;
  while (prefix[pi])
    out[oi++] = prefix[pi++];
  while (buf[bi])
    out[oi++] = buf[bi++];
  out[oi++] = '\n';
  out[oi] = 0;
  sys_print(out);
}

// ---------------------------------------------------------------------------
// Test 1: Stack growth (recursión profunda)
// ---------------------------------------------------------------------------
static int recursive_growth(int depth) {
  volatile char buf[1024];
  for (int i = 0; i < 1024; i++) {
    buf[i] = (char)(depth + i);
  }
  int sum = 0;
  for (int i = 0; i < 1024; i += 64) {
    sum += buf[i];
  }
  if (depth > 0) {
    return recursive_growth(depth - 1) + (sum & 1);
  }
  return sum;
}

static void test_stack_growth(void) {
  sys_print("[pftest] Test 1: Stack growth (recursion de 80 KB)...\n");
  int r = recursive_growth(80);
  (void)r;
  sys_print("[pftest]   Stack growth: OK (no crasheo)\n");
  passed++;
}

// ---------------------------------------------------------------------------
// Test 2: mmap anónimo + demand paging
// ---------------------------------------------------------------------------
static void test_mmap_basic(void) {
  sys_print("[pftest] Test 2: mmap anonimo + demand paging...\n");

  void *p =
      (void *)sys_mmap(0, 16384, MMAP_PROT_READ | MMAP_PROT_WRITE, 0, -1, 0);
  if ((long)p < 0) {
    sys_print("[pftest]   mmap: FALLO (retorno negativo)\n");
    failed++;
    return;
  }

  volatile char *c = (volatile char *)p;
  c[0] = 'A';
  c[4095] = 'B';
  c[8192] = 'C';
  c[16383] = 'D';

  if (c[0] == 'A' && c[4095] == 'B' && c[8192] == 'C' && c[16383] == 'D') {
    sys_print("[pftest]   mmap read/write: OK\n");
    passed++;
  } else {
    sys_print("[pftest]   mmap read/write: FALLO (contenido incorrecto)\n");
    failed++;
  }

  int r = sys_munmap((uint64_t)p, 16384);
  if (r == 0) {
    sys_print("[pftest]   munmap: OK\n");
    passed++;
  } else {
    sys_print("[pftest]   munmap: FALLO\n");
    failed++;
  }
}

// ---------------------------------------------------------------------------
// Test 3: mmap de 64 KB, escribir página por página
// ---------------------------------------------------------------------------
static void test_mmap_many_pages(void) {
  sys_print(
      "[pftest] Test 3: mmap de 64 KB, escribir una pagina de cada vez...\n");

  void *p =
      (void *)sys_mmap(0, 65536, MMAP_PROT_READ | MMAP_PROT_WRITE, 0, -1, 0);
  if ((long)p < 0) {
    sys_print("[pftest]   mmap: FALLO\n");
    failed++;
    return;
  }

  volatile char *c = (volatile char *)p;
  int ok = 1;
  for (int i = 0; i < 65536; i += 4096) {
    c[i] = (char)(i / 4096);
  }
  for (int i = 0; i < 65536; i += 4096) {
    if (c[i] != (char)(i / 4096)) {
      ok = 0;
      break;
    }
  }

  if (ok) {
    sys_print("[pftest]   16 paginas mapeadas on-demand: OK\n");
    passed++;
  } else {
    sys_print("[pftest]   16 paginas mapeadas on-demand: FALLO\n");
    failed++;
  }

  sys_munmap((uint64_t)p, 65536);
}

// ---------------------------------------------------------------------------
// Test 4: mmap devuelve páginas zeroed
// ---------------------------------------------------------------------------
static void test_mmap_zeroed(void) {
  sys_print("[pftest] Test 4: mmap devuelve paginas zeroed...\n");

  void *p =
      (void *)sys_mmap(0, 8192, MMAP_PROT_READ | MMAP_PROT_WRITE, 0, -1, 0);
  if ((long)p < 0) {
    sys_print("[pftest]   mmap: FALLO\n");
    failed++;
    return;
  }

  volatile unsigned char *c = (volatile unsigned char *)p;
  int all_zero = 1;
  for (int i = 0; i < 8192; i++) {
    if (c[i] != 0) {
      all_zero = 0;
      break;
    }
  }

  if (all_zero) {
    sys_print("[pftest]   Paginas zeroed: OK\n");
    passed++;
  } else {
    sys_print("[pftest]   Paginas zeroed: FALLO (no estan a cero)\n");
    failed++;
  }

  sys_munmap((uint64_t)p, 8192);
}

// ---------------------------------------------------------------------------
// Test 5: Múltiples mmaps simultáneos
// ---------------------------------------------------------------------------
static void test_mmap_multiple(void) {
  sys_print("[pftest] Test 5: Multiples mmaps simultaneos...\n");

  void *p1 =
      (void *)sys_mmap(0, 4096, MMAP_PROT_READ | MMAP_PROT_WRITE, 0, -1, 0);
  void *p2 =
      (void *)sys_mmap(0, 8192, MMAP_PROT_READ | MMAP_PROT_WRITE, 0, -1, 0);
  void *p3 =
      (void *)sys_mmap(0, 4096, MMAP_PROT_READ | MMAP_PROT_WRITE, 0, -1, 0);

  if ((long)p1 < 0 || (long)p2 < 0 || (long)p3 < 0) {
    sys_print("[pftest]   mmap multiple: FALLO (alguno retorno negativo)\n");
    failed++;
    return;
  }

  if (p1 == p2 || p2 == p3 || p1 == p3) {
    sys_print("[pftest]   mmap multiple: FALLO (direcciones solapadas)\n");
    failed++;
    return;
  }

  *(volatile char *)p1 = 1;
  *(volatile char *)p2 = 2;
  *(volatile char *)p3 = 3;

  if (*(volatile char *)p1 == 1 && *(volatile char *)p2 == 2 &&
      *(volatile char *)p3 == 3) {
    sys_print("[pftest]   3 mmaps independientes: OK\n");
    passed++;
  } else {
    sys_print("[pftest]   3 mmaps independientes: FALLO\n");
    failed++;
  }

  sys_munmap((uint64_t)p1, 4096);
  sys_munmap((uint64_t)p2, 8192);
  sys_munmap((uint64_t)p3, 4096);
}

// ---------------------------------------------------------------------------
// Test 6: mmap con dirección sugerida
// ---------------------------------------------------------------------------
static void test_mmap_fixed_addr(void) {
  sys_print("[pftest] Test 6: mmap con direccion sugerida...\n");

  uint64_t wanted = 0x0000700000000000ULL;
  void *p = (void *)sys_mmap(wanted, 4096, MMAP_PROT_READ | MMAP_PROT_WRITE, 0,
                             -1, 0);
  if ((long)p < 0) {
    sys_print("[pftest]   mmap fixed: FALLO\n");
    failed++;
    return;
  }

  if ((uint64_t)p != wanted) {
    sys_print("[pftest]   mmap fixed: FALLO (direccion distinta)\n");
    failed++;
    sys_munmap((uint64_t)p, 4096);
    return;
  }

  *(volatile char *)p = 42;
  if (*(volatile char *)p == 42) {
    sys_print("[pftest]   mmap fixed addr: OK\n");
    passed++;
  } else {
    sys_print("[pftest]   mmap fixed addr: FALLO\n");
    failed++;
  }

  sys_munmap((uint64_t)p, 4096);
}

// ---------------------------------------------------------------------------
// Test 8: Verificar que las syscalls siguen funcionando después de mmap
// ---------------------------------------------------------------------------
static void test_mmap_kernel_boundary(void) {
  sys_print("[pftest] Test 8: mmap fuera de USER_LIMIT (debe fallar)...\\n");

  const uint64_t kernel_addr = 0xFFFFFFFF80000000ULL;
  const uint64_t boundary_addr = 0x0000800000000000ULL;

  void *p1 = (void *)sys_mmap(kernel_addr, 4096,
                              MMAP_PROT_READ | MMAP_PROT_WRITE, 0, -1, 0);
  void *p2 = (void *)sys_mmap(boundary_addr, 4096,
                              MMAP_PROT_READ | MMAP_PROT_WRITE, 0, -1, 0);

  if ((long)p1 >= 0 || (long)p2 >= 0) {
    sys_print("[pftest]   mmap kernel/boundary: FALLO (aceptado)\\n");
    failed++;
  } else {
    sys_print("[pftest]   mmap kernel/boundary: OK (rechazado)\\n");
    passed++;
  }
}

// ---------------------------------------------------------------------------
// Test 8: Verificar que las syscalls siguen funcionando después de mmap
// ---------------------------------------------------------------------------
static void test_syscalls_after_mmap(void) {
  sys_print("[pftest] Test 8: Syscalls tras mmap...\n");

  void *p =
      (void *)sys_mmap(0, 32768, MMAP_PROT_READ | MMAP_PROT_WRITE, 0, -1, 0);
  if ((long)p < 0) {
    sys_print("[pftest]   mmap: FALLO\n");
    failed++;
    return;
  }
  *(volatile char *)p = 1;
  sys_munmap((uint64_t)p, 32768);

  int fd = open("system/config.txt", O_RDONLY);
  if (fd < 0) {
    sys_print("[pftest]   open tras mmap: FALLO\n");
    failed++;
    return;
  }
  char buf[64];
  int64_t n = read(fd, buf, sizeof(buf) - 1);
  if (n > 0)
    buf[n] = '\0';
  close(fd);

  if (n > 0 && buf[0] == 'A') {
    sys_print("[pftest]   open/read tras mmap: OK\n");
    passed++;
  } else {
    sys_print("[pftest]   open/read tras mmap: FALLO\n");
    failed++;
  }
}

// ---------------------------------------------------------------------------
// Test 9: mmap con length=0 (debe fallar)
// ---------------------------------------------------------------------------
static void test_mmap_zero_length(void) {
  sys_print("[pftest] Test 9: mmap con length=0 (debe fallar)...\n");
  void *p = (void *)sys_mmap(0, 0, MMAP_PROT_READ | MMAP_PROT_WRITE, 0, -1, 0);
  if ((long)p < 0) {
    sys_print("[pftest]   mmap(0): falla correctamente: OK\n");
    passed++;
  } else {
    sys_print("[pftest]   mmap(0): FALLO (deberia fallar)\n");
    failed++;
    sys_munmap((uint64_t)p, 4096);
  }
}

// ---------------------------------------------------------------------------
// Test 10: mmap excesivo (>256 MB, debe fallar)
// ---------------------------------------------------------------------------
static void test_mmap_too_large(void) {
  sys_print("[pftest] Test 10: mmap >256 MB (debe fallar)...\n");
  void *p = (void *)sys_mmap(0, 512ULL * 1024 * 1024,
                             MMAP_PROT_READ | MMAP_PROT_WRITE, 0, -1, 0);
  if ((long)p < 0) {
    sys_print("[pftest]   mmap excesivo: falla correctamente: OK\n");
    passed++;
  } else {
    sys_print("[pftest]   mmap excesivo: FALLO (deberia fallar)\n");
    failed++;
    sys_munmap((uint64_t)p, 512ULL * 1024 * 1024);
  }
}

// ---------------------------------------------------------------------------
// Test 11: munmap sin mmap previo (debe fallar)
// ---------------------------------------------------------------------------
static void test_munmap_invalid(void) {
  sys_print("[pftest] Test 11: munmap sin mmap previo...\n");
  int r = sys_munmap(0x50000000, 4096);
  if (r < 0) {
    sys_print("[pftest]   munmap sin VMA: falla correctamente: OK\n");
    passed++;
  } else {
    sys_print("[pftest]   munmap sin VMA: FALLO (deberia fallar)\n");
    failed++;
  }
}

// ---------------------------------------------------------------------------
// Test 12: mmap RW, sólo escribir
// ---------------------------------------------------------------------------
static void test_mmap_write_only(void) {
  sys_print("[pftest] Test 12: mmap RW, solo escribir...\n");
  void *p =
      (void *)sys_mmap(0, 4096, MMAP_PROT_READ | MMAP_PROT_WRITE, 0, -1, 0);
  if ((long)p < 0) {
    sys_print("[pftest]   mmap: FALLO\n");
    failed++;
    return;
  }
  volatile unsigned char *c = (volatile unsigned char *)p;
  for (int i = 0; i < 4096; i += 64) {
    c[i] = (unsigned char)i;
  }
  sys_print("[pftest]   Escritura en mmap: OK\n");
  passed++;
  sys_munmap((uint64_t)p, 4096);
}

// ---------------------------------------------------------------------------
// Test 14: mmap con dirección sugerida no alineada
// ---------------------------------------------------------------------------
static void test_mmap_unaligned_addr(void) {
  sys_print("[pftest] Test 14: mmap con direccion no alineada...\n");
  void *p = (void *)sys_mmap(0x0000700000001234ULL, 4096,
                             MMAP_PROT_READ | MMAP_PROT_WRITE, 0, -1, 0);
  if ((long)p < 0) {
    sys_print("[pftest]   mmap unaligned: FALLO\n");
    failed++;
    return;
  }
  if (((uint64_t)p & 0xFFF) != 0) {
    sys_print("[pftest]   mmap unaligned: FALLO (no alineada)\n");
    failed++;
    sys_munmap((uint64_t)p, 4096);
    return;
  }
  sys_print("[pftest]   mmap unaligned: OK\n");
  passed++;
  sys_munmap((uint64_t)p, 4096);
}

// ---------------------------------------------------------------------------
// Test 15: Escribir/leer patrón de bytes en mmap
// ---------------------------------------------------------------------------
static void test_mmap_pattern(void) {
  sys_print("[pftest] Test 15: mmap con patron de bytes...\n");
  void *p =
      (void *)sys_mmap(0, 16384, MMAP_PROT_READ | MMAP_PROT_WRITE, 0, -1, 0);
  if ((long)p < 0) {
    sys_print("[pftest]   mmap: FALLO\n");
    failed++;
    return;
  }
  volatile unsigned char *c = (volatile unsigned char *)p;
  for (int i = 0; i < 16384; i++) {
    c[i] = (unsigned char)(i * 31);
  }
  int ok = 1;
  for (int i = 0; i < 16384; i++) {
    if (c[i] != (unsigned char)(i * 31)) {
      ok = 0;
      break;
    }
  }
  if (ok) {
    sys_print("[pftest]   Patron 16 KB: OK\n");
    passed++;
  } else {
    sys_print("[pftest]   Patron 16 KB: FALLO\n");
    failed++;
  }
  sys_munmap((uint64_t)p, 16384);
}

// ---------------------------------------------------------------------------
// Test 16: mmap + munmap + mmap en el mismo rango
// ---------------------------------------------------------------------------
static void test_mmap_reuse(void) {
  sys_print("[pftest] Test 16: mmap/munmap/mmap en mismo rango...\n");
  uint64_t wanted = 0x0000750000000000ULL;

  void *p1 = (void *)sys_mmap(wanted, 8192, MMAP_PROT_READ | MMAP_PROT_WRITE, 0,
                              -1, 0);
  if ((long)p1 < 0) {
    sys_print("[pftest]   mmap 1: FALLO\n");
    failed++;
    return;
  }
  *(volatile char *)p1 = 'A';
  sys_munmap((uint64_t)p1, 8192);

  void *p2 = (void *)sys_mmap(wanted, 8192, MMAP_PROT_READ | MMAP_PROT_WRITE, 0,
                              -1, 0);
  if ((long)p2 < 0 || (uint64_t)p2 != wanted) {
    sys_print("[pftest]   mmap 2 en mismo rango: FALLO\n");
    failed++;
    return;
  }
  if (*(volatile char *)p2 == 'A') {
    sys_print("[pftest]   mmap 2 en mismo rango: FALLO (contenido antiguo)\n");
    failed++;
  } else {
    sys_print("[pftest]   mmap reuse: OK\n");
    passed++;
  }
  sys_munmap((uint64_t)p2, 8192);
}

// ---------------------------------------------------------------------------
// Test 17: Escribir en todas las páginas de un mmap grande
// ---------------------------------------------------------------------------
static void test_mmap_all_pages(void) {
  sys_print("[pftest] Test 17: mmap 128 KB, tocar todas las paginas...\n");
  void *p = (void *)sys_mmap(0, 128 * 1024, MMAP_PROT_READ | MMAP_PROT_WRITE, 0,
                             -1, 0);
  if ((long)p < 0) {
    sys_print("[pftest]   mmap: FALLO\n");
    failed++;
    return;
  }
  volatile unsigned char *c = (volatile unsigned char *)p;
  // 128 KB = 32 páginas de 4 KB
  for (int i = 0; i < 32; i++) {
    c[i * 4096] = (unsigned char)i;
  }
  int ok = 1;
  for (int i = 0; i < 32; i++) {
    if (c[i * 4096] != (unsigned char)i) {
      ok = 0;
      break;
    }
  }
  if (ok) {
    sys_print("[pftest]   32 paginas (128 KB): OK\n");
    passed++;
  } else {
    sys_print("[pftest]   32 paginas (128 KB): FALLO\n");
    failed++;
  }
  sys_munmap((uint64_t)p, 128 * 1024);
}

// ---------------------------------------------------------------------------
// Test 18: malloc después de muchos mmaps
// ---------------------------------------------------------------------------
static void test_malloc_after_mmap(void) {
  sys_print("[pftest] Test 18: malloc tras muchos mmaps...\n");

  void *ptrs[20];
  for (int i = 0; i < 20; i++) {
    ptrs[i] =
        (void *)sys_mmap(0, 4096, MMAP_PROT_READ | MMAP_PROT_WRITE, 0, -1, 0);
    if ((long)ptrs[i] < 0) {
      sys_print("[pftest]   mmap en loop: FALLO\n");
      failed++;
      return;
    }
    *(volatile char *)ptrs[i] = (char)i;
  }

  void *m = malloc(1024);
  if (!m) {
    sys_print("[pftest]   malloc tras mmaps: FALLO\n");
    failed++;
  } else {
    memset(m, 0xAA, 1024);
    sys_print("[pftest]   malloc tras 20 mmaps: OK\n");
    passed++;
    free(m);
  }

  for (int i = 0; i < 20; i++) {
    sys_munmap((uint64_t)ptrs[i], 4096);
  }
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
int main(void) {
  sys_print("[pftest] === Aurora OS Page Fault Test App ===\n");
  sys_print("[pftest] Demand paging + stack growth + mmap\n\n");

  test_stack_growth();
  test_mmap_basic();
  test_mmap_many_pages();
  test_mmap_zeroed();
  test_mmap_multiple();
  test_mmap_fixed_addr();
  test_mmap_kernel_boundary();
  test_syscalls_after_mmap();
  test_mmap_zero_length();
  test_mmap_too_large();
  test_munmap_invalid();
  test_mmap_write_only();
  test_mmap_unaligned_addr();
  test_mmap_pattern();
  test_mmap_reuse();
  test_mmap_all_pages();
  test_malloc_after_mmap();

  sys_print("\n[pftest] =============================\n");
  print_int("[pftest] Pruebas PASADAS: ", passed);
  print_int("[pftest] Pruebas FALLIDAS: ", failed);
  if (failed == 0) {
    sys_print("[pftest] *** TODOS LOS TESTS DE PF/MMAP PASARON ***\n");
    sys_exit(0);
  } else {
    sys_print("[pftest] *** ALGUNOS TESTS FALLARON ***\n");
    sys_exit(1);
  }
  return 0;
}