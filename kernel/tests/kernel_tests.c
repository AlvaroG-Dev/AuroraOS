// kernel/tests/kernel_tests.c
//
// Tests registrados en el framework .tests.
// Cada test corre con preempt_disable() activo y no debe bloquearse.
//
// Para añadir uno nuevo:
//   static void test_mi_cosa(void) {
//       TEST_ASSERT(cond, "mensaje");
//   }
//   REGISTER_TEST("mi_cosa", test_mi_cosa);

#include "../heap.h"
#include "../slab.h"
#include "../string.h"
#include "../test.h"


// ---------------------------------------------------------------------------
// Heap: kmalloc/kfree básicos
// ---------------------------------------------------------------------------
static void test_kmalloc_64(void) {
  uint8_t *p = (uint8_t *)kmalloc(64);
  TEST_ASSERT(p != NULL, "kmalloc(64) devolvió NULL");
  if (!p)
    return;

  for (int i = 0; i < 64; i++)
    p[i] = (uint8_t)i;
  int ok = 1;
  for (int i = 0; i < 64; i++)
    if (p[i] != (uint8_t)i) {
      ok = 0;
      break;
    }
  TEST_ASSERT(ok, "contenido corrupto tras escritura");
  kfree(p);
}
REGISTER_TEST("heap: kmalloc(64)", test_kmalloc_64);

static void test_kmalloc_8k(void) {
  uint8_t *p = (uint8_t *)kmalloc(8192);
  TEST_ASSERT(p != NULL, "kmalloc(8192) devolvió NULL");
  if (p)
    kfree(p);
}
REGISTER_TEST("heap: kmalloc(8192)", test_kmalloc_8k);

// ---------------------------------------------------------------------------
// Heap: kzalloc
// ---------------------------------------------------------------------------
static void test_kzalloc_zeroed(void) {
  uint8_t *p = (uint8_t *)kzalloc(200);
  TEST_ASSERT(p != NULL, "kzalloc(200) devolvió NULL");
  if (!p)
    return;
  int ok = 1;
  for (int i = 0; i < 200; i++)
    if (p[i] != 0) {
      ok = 0;
      break;
    }
  TEST_ASSERT(ok, "kzalloc no dejó todo a cero");
  kfree(p);
}
REGISTER_TEST("heap: kzalloc zeroed", test_kzalloc_zeroed);

// ---------------------------------------------------------------------------
// Heap: kcalloc + overflow
// ---------------------------------------------------------------------------
static void test_kcalloc(void) {
  int *arr = (int *)kcalloc(16, sizeof(int));
  TEST_ASSERT(arr != NULL, "kcalloc(16,4) devolvió NULL");
  if (!arr)
    return;
  int ok = 1;
  for (int i = 0; i < 16; i++)
    if (arr[i] != 0) {
      ok = 0;
      break;
    }
  TEST_ASSERT(ok, "kcalloc no zeroed");
  kfree(arr);
}
REGISTER_TEST("heap: kcalloc", test_kcalloc);

static void test_kcalloc_overflow(void) {
  void *ovf = kcalloc((size_t)-1, 2);
  TEST_ASSERT(ovf == NULL, "kcalloc no detectó overflow");
  if (ovf)
    kfree(ovf);
}
REGISTER_TEST("heap: kcalloc overflow", test_kcalloc_overflow);

// ---------------------------------------------------------------------------
// Heap: kstrdup / kstrndup
// ---------------------------------------------------------------------------
static void test_kstrdup(void) {
  char *s = kstrdup("Aurora OS");
  TEST_ASSERT(s != NULL, "kstrdup devolvió NULL");
  if (!s)
    return;
  TEST_ASSERT(strcmp(s, "Aurora OS") == 0, "contenido != original");
  kfree(s);
}
REGISTER_TEST("heap: kstrdup", test_kstrdup);

static void test_kstrndup_cut(void) {
  char *s = kstrndup("abcdefgh", 4);
  TEST_ASSERT(s != NULL, "kstrndup devolvió NULL");
  if (!s)
    return;
  TEST_ASSERT(strcmp(s, "abcd") == 0, "corte incorrecto: '%s'", s);
  kfree(s);
}
REGISTER_TEST("heap: kstrndup cut", test_kstrndup_cut);

static void test_kstrndup_short(void) {
  char *s = kstrndup("ab", 8);
  TEST_ASSERT(s != NULL, "kstrndup devolvió NULL");
  if (!s)
    return;
  TEST_ASSERT(strcmp(s, "ab") == 0, "contenido incorrecto: '%s'", s);
  kfree(s);
}
REGISTER_TEST("heap: kstrndup short", test_kstrndup_short);

// ---------------------------------------------------------------------------
// Heap: krealloc
// ---------------------------------------------------------------------------
static void test_krealloc_grow(void) {
  char *r = (char *)kmalloc(16);
  TEST_ASSERT(r != NULL, "kmalloc(16) devolvió NULL");
  if (!r)
    return;
  for (int i = 0; i < 16; i++)
    r[i] = (char)('A' + i);
  char *r2 = (char *)krealloc(r, 128);
  TEST_ASSERT(r2 != NULL, "krealloc grow devolvió NULL");
  if (!r2) {
    kfree(r);
    return;
  }
  int ok = 1;
  for (int i = 0; i < 16; i++)
    if (r2[i] != (char)('A' + i)) {
      ok = 0;
      break;
    }
  TEST_ASSERT(ok, "contenido no preservado tras crecer");
  kfree(r2);
}
REGISTER_TEST("heap: krealloc grow", test_krealloc_grow);

static void test_krealloc_shrink_same(void) {
  void *same = kmalloc(64);
  TEST_ASSERT(same != NULL, "kmalloc(64) devolvió NULL");
  if (!same)
    return;
  void *same2 = krealloc(same, 32);
  TEST_ASSERT(same2 == same, "krealloc shrink devolvió distinto ptr");
  kfree(same);
}
REGISTER_TEST("heap: krealloc shrink same ptr", test_krealloc_shrink_same);

static void test_krealloc_null(void) {
  void *fresh = krealloc(NULL, 32);
  TEST_ASSERT(fresh != NULL, "krealloc(NULL, 32) devolvió NULL");
  if (fresh)
    kfree(fresh);
}
REGISTER_TEST("heap: krealloc(NULL)", test_krealloc_null);

static void test_krealloc_zero(void) {
  void *p = kmalloc(64);
  TEST_ASSERT(p != NULL, "kmalloc(64) devolvió NULL");
  if (!p)
    return;
  void *p2 = krealloc(p, 0);
  TEST_ASSERT(p2 == NULL, "krealloc(p, 0) debía devolver NULL");
}
REGISTER_TEST("heap: krealloc(p, 0)", test_krealloc_zero);

// ---------------------------------------------------------------------------
// SLAB tests
// ---------------------------------------------------------------------------

static void test_slab_basic_32(void) {
  void *p = kmalloc(32);
  TEST_ASSERT(p != NULL, "kmalloc(32) devolvió NULL");
  if (!p)
    return;

  // Comprobar que está en la región del SLAB.
  uint64_t addr = (uint64_t)p;
  TEST_ASSERT(addr >= SLAB_VMA, "kmalloc(32) no vino del SLAB: %p", p);

  // Escribir y verificar.
  memset(p, 0xAB, 32);
  uint8_t *b = (uint8_t *)p;
  int ok = 1;
  for (int i = 0; i < 32; i++)
    if (b[i] != 0xAB) {
      ok = 0;
      break;
    }
  TEST_ASSERT(ok, "contenido corrupto");

  kfree(p);
}
REGISTER_TEST("slab: basic 32", test_slab_basic_32);

static void test_slab_all_sizes(void) {
  static const size_t sizes[] = {16, 32, 64, 128, 256, 512, 1024, 2048};
  void *ptrs[8] = {0};

  for (int i = 0; i < 8; i++) {
    ptrs[i] = kmalloc(sizes[i]);
    TEST_ASSERT(ptrs[i] != NULL, "kmalloc(%lu) devolvió NULL",
                (unsigned long)sizes[i]);
    if (ptrs[i]) {
      uint64_t a = (uint64_t)ptrs[i];
      TEST_ASSERT(a >= SLAB_VMA, "kmalloc(%lu) no vino del SLAB",
                  (unsigned long)sizes[i]);
      memset(ptrs[i], (int)(i + 1), sizes[i]);
    }
  }

  for (int i = 0; i < 8; i++) {
    if (ptrs[i])
      kfree(ptrs[i]);
  }
}
REGISTER_TEST("slab: all sizes", test_slab_all_sizes);

static void test_slab_many_32(void) {
  // Asignar muchos objetos de 32 bytes para forzar varios slabs.
  // 1 slab con header de 64 bytes: (4096-64)/32 = 126 objetos.
  // Pedimos 500 para forzar ~4 slabs.
  enum { N = 500 };
  static void *ptrs[N];

  for (int i = 0; i < N; i++) {
    ptrs[i] = kmalloc(32);
    if (!ptrs[i]) {
      TEST_ASSERT(0, "kmalloc(32) falló en iteración %d", i);
      // Liberar lo asignado y salir.
      for (int j = 0; j < i; j++)
        kfree(ptrs[j]);
      return;
    }
    // Escribir un patrón único para detectar solapamientos.
    ((uint32_t *)ptrs[i])[0] = (uint32_t)i;
    ((uint32_t *)ptrs[i])[1] = (uint32_t)~i;
  }

  // Verificar que cada objeto conserva su patrón.
  int ok = 1;
  for (int i = 0; i < N; i++) {
    uint32_t a = ((uint32_t *)ptrs[i])[0];
    uint32_t b = ((uint32_t *)ptrs[i])[1];
    if (a != (uint32_t)i || b != (uint32_t)~i) {
      ok = 0;
      break;
    }
  }
  TEST_ASSERT(ok, "objetos se solapan o se corrompen");

  for (int i = 0; i < N; i++)
    kfree(ptrs[i]);
}
REGISTER_TEST("slab: 500 x 32 bytes", test_slab_many_32);

static void test_slab_free_reuse(void) {
  // Asignar, liberar, y volver a asignar. Debe reutilizar el mismo
  // slab y probablemente la misma dirección.
  void *a = kmalloc(64);
  void *b = kmalloc(64);
  TEST_ASSERT(a && b, "kmalloc falló");
  if (!a || !b) {
    if (a)
      kfree(a);
    if (b)
      kfree(b);
    return;
  }

  TEST_ASSERT(a != b, "kmalloc devolvió la misma dirección dos veces");

  kfree(a);
  void *c = kmalloc(64);
  TEST_ASSERT(c != NULL, "kmalloc tras kfree falló");
  // Lo más probable es que c == a (LIFO freelist), pero no lo exigimos.
  if (c)
    kfree(c);
  kfree(b);
}
REGISTER_TEST("slab: free + reuse", test_slab_free_reuse);

static void test_slab_krealloc_inplace(void) {
  // kmalloc(32) → krealloc(64). Ambos caben... no, 64 > 32, hay que
  // mover a un cache mayor. Pero kmalloc(64) → krealloc(48) debería
  // ser in-place (48 <= 64, mismo cache).
  void *p = kmalloc(64);
  TEST_ASSERT(p != NULL, "kmalloc(64) falló");
  if (!p)
    return;

  // Escribir algo.
  memset(p, 0x42, 64);

  void *p2 = krealloc(p, 48);
  TEST_ASSERT(p2 == p, "krealloc(64→48) debía ser in-place");
  // Verificar contenido.
  uint8_t *b = (uint8_t *)p2;
  int ok = 1;
  for (int i = 0; i < 48; i++)
    if (b[i] != 0x42) {
      ok = 0;
      break;
    }
  TEST_ASSERT(ok, "contenido no preservado");
  kfree(p2);
}
REGISTER_TEST("slab: krealloc in-place", test_slab_krealloc_inplace);

static void test_slab_usable_size(void) {
  void *p = kmalloc(20); // cae en cache de 32
  TEST_ASSERT(p != NULL, "kmalloc(20) falló");
  if (!p)
    return;
  size_t usable = slab_usable_size(p);
  TEST_ASSERT(usable >= 20, "usable_size < solicitado: %lu",
              (unsigned long)usable);
  kfree(p);
}
REGISTER_TEST("slab: usable size", test_slab_usable_size);