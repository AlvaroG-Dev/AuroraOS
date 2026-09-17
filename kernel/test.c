// kernel/test.c
#include "test.h"
#include "klog.h"
#include "sched.h"
#include <stdarg.h>
#include <stddef.h>

// Símbolos definidos por el linker script.
extern const test_case_t __tests_start[];
extern const test_case_t __tests_end[];

static int g_passed = 0;
static int g_failed = 0;
static int g_skipped = 0;
static int g_current_failed = 0;

// Para los mensajes de test_pass/test_fail dentro del mismo test.
// No hace falta thread-safety: los tests corren con preempt_disable
// y secuencialmente.
static const char *g_current_name = NULL;

void test_begin(const char *name) {
  g_current_name = name;
  g_current_failed = 0;
  klog_printf(KLOG_INFO, "[TEST] RUN  %s", name);
}

void test_pass(void) {
  // No imprime nada por defecto para no inundar el log.
  // El resultado final del test se imprime al acabar.
}

void test_fail(const char *file, int line, const char *fmt, ...) {
  g_failed++;
  g_current_failed++;

  va_list ap;
  va_start(ap, fmt);
  klog_printf(KLOG_ERR,
              "[TEST] FAIL %s: %s:%d:", g_current_name ? g_current_name : "?",
              file, line);
  // Reusamos klog_vprintf para no repetir el formateo. Como queremos
  // el prefijo [TEST] otra vez, lo hacemos manual:
  klog_vprintf(KLOG_ERR, fmt, ap);
  va_end(ap);
}

void test_skip(const char *fmt, ...) {
  g_skipped++;
  va_list ap;
  va_start(ap, fmt);
  klog_printf(KLOG_WARN,
              "[TEST] SKIP %s:", g_current_name ? g_current_name : "?");
  klog_vprintf(KLOG_WARN, fmt, ap);
  va_end(ap);
}

int run_all_tests(void) {
  g_passed = 0;
  g_failed = 0;
  g_skipped = 0;

  ptrdiff_t total = __tests_end - __tests_start;
  if (total < 0)
    total = 0;

  klog_printf(KLOG_INFO, "[TEST] ============================================");
  klog_printf(KLOG_INFO, "[TEST] Aurora OS kernel test suite");
  klog_printf(KLOG_INFO, "[TEST] %ld tests registrados", (long)total);
  klog_printf(KLOG_INFO, "[TEST] ============================================");

  for (const test_case_t *t = __tests_start; t < __tests_end; t++) {
    if (!t->fn || !t->name) {
      klog_printf(KLOG_ERR, "[TEST] entrada inválida en .tests, saltando");
      g_skipped++;
      continue;
    }

    test_begin(t->name);

    // Un test puede fallar varios asserts. Marcamos el número
    // de fallos antes y después para saber si el test pasó.
    int failed_before = g_failed;

    // preempt_disable para que el scheduler no cambie de tarea a
    // mitad. Si un test hiciera wait_event() se produciría deadlock.
    // Ningún test debe bloquearse.
    preempt_disable();
    t->fn();
    preempt_enable();

    if (g_failed == failed_before) {
      g_passed++;
      klog_printf(KLOG_INFO, "[TEST] PASS %s", t->name);
    } else {
      // El fallo ya se imprimió desde test_fail().
      // Imprimimos solo el cierre.
      klog_printf(KLOG_ERR, "[TEST] FAIL %s (fin del test)", t->name);
    }
  }

  klog_printf(KLOG_INFO, "[TEST] ============================================");
  klog_printf(KLOG_INFO, "[TEST] %d tests: %d passed, %d failed, %d skipped",
              g_passed + g_failed, g_passed, g_failed, g_skipped);
  klog_printf(KLOG_INFO, "[TEST] ============================================");

  return g_failed;
}

int test_get_passed(void) { return g_passed; }
int test_get_failed(void) { return g_failed; }
int test_get_skipped(void) { return g_skipped; }