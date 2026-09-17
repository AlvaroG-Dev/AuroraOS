// kernel/test.h
#ifndef KERNEL_TEST_H
#define KERNEL_TEST_H

#include <stddef.h>
#include <stdint.h>

// Framework mínimo de tests en kernel.
//
// Uso:
//   static void test_foo(void) {
//       TEST_ASSERT(x == 3, "x esperado 3, obtenido %d", x);
//       TEST_ASSERT(y != NULL, "y es NULL");
//   }
//   REGISTER_TEST("foo", test_foo);
//
// El runner (run_all_tests) recorre la sección .tests y ejecuta
// cada test en orden de enlace. Cada test corre con preempt_disable
// activo (no puede ser desalojado por el scheduler). No debe bloquearse.

typedef void (*test_fn_t)(void);

typedef struct test_case {
  const char *name;
  test_fn_t fn;
} test_case_t;

// Pasa un test. Llamado por TEST_ASSERT cuando la condición es cierta.
void test_pass(void);

// Falla un test. Llamado por TEST_ASSERT cuando la condición es falsa.
// 'file' y 'line' los pasa la macro.
void test_fail(const char *file, int line, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

// Marca el test actual como "skipped".
void test_skip(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// Test activo (llamado por el runner antes de invocar el test).
void test_begin(const char *name);

// Ejecuta todos los tests registrados.
// Devuelve 0 si todos pasaron, != 0 si alguno falló.
int run_all_tests(void);

// Cuántos tests han pasado/fallado/se han saltado en la última ejecución.
// Útil si algún día quieres consultarlo desde userland.
int test_get_passed(void);
int test_get_failed(void);
int test_get_skipped(void);

// ---------------------------------------------------------------------------
// Macros
// ---------------------------------------------------------------------------

// Assert. Si cond es falsa, registra un fallo con mensaje formateado.
// No aborta el test: pueden fallar varios asserts en el mismo test.
#define TEST_ASSERT(cond, ...)                                                 \
  do {                                                                         \
    if (cond) {                                                                \
      test_pass();                                                             \
    } else {                                                                   \
      test_fail(__FILE__, __LINE__, __VA_ARGS__);                              \
    }                                                                          \
  } while (0)

// Atajo para comparaciones simples.
#define TEST_ASSERT_EQ(a, b)                                                   \
  do {                                                                         \
    unsigned long _a = (unsigned long)(a);                                     \
    unsigned long _b = (unsigned long)(b);                                     \
    if (_a == _b) {                                                            \
      test_pass();                                                             \
    } else {                                                                   \
      test_fail(__FILE__, __LINE__, "%s == %s: 0x%lx != 0x%lx", #a, #b, _a,    \
                _b);                                                           \
    }                                                                          \
  } while (0)

// Registra un test. El name es string literal (no se copia).
// El símbolo se coloca en la sección .tests, que el linker agrupa.
// El atributo 'used' evita que GCC elimine el símbolo por no usarse.
#define REGISTER_TEST(name, fn)                                                \
  __attribute__((used, section(".tests"),                                      \
                 aligned(8))) static const test_case_t _test_##fn = {(name),   \
                                                                     (fn)}

#endif // KERNEL_TEST_H