// kernel/test.c
#include "test.h"
#include "klog.h"
#include "sched.h"
#include "smp_boot.h"
#include <stdarg.h>
#include <stddef.h>

// Símbolos definidos por el linker script.
extern const test_case_t __tests_start[];
extern const test_case_t __tests_end[];

static int g_passed = 0;
static int g_failed = 0;
static int g_skipped = 0;
static int g_current_failed = 0;

static const char *g_current_name = NULL;

void test_begin(const char *name) {
  g_current_name = name;
  g_current_failed = 0;
  klog_printf(KLOG_INFO, "[TEST] RUN  %s", name);
}

void test_pass(void) {}

void test_fail(const char *file, int line, const char *fmt, ...) {
  g_failed++;
  g_current_failed++;

  va_list ap;
  va_start(ap, fmt);
  klog_printf(KLOG_ERR,
              "[TEST] FAIL %s: %s:%d:", g_current_name ? g_current_name : "?",
              file, line);
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

    if ((t->flags & TEST_FLAG_NEEDS_SMP) && smp_aps_ready() == 0) {
      test_begin(t->name);
      test_skip("SMP no disponible (aps_ready=0)");
      continue;
    }

    test_begin(t->name);

    int failed_before = g_failed;

    // [FIX] El framework ya NO llama a preempt_disable() automáticamente.
    //
    // El comportamiento anterior envolvía cada test no-BLOCKING en
    // preempt_disable(). Eso es peligroso: si el test llama a cualquier
    // función que haga sched_yield() (por ejemplo ata_chan_acquire
    // cuando el canal está ocupado, o wait_event_interruptible_timeout),
    // el scheduler no puede cambiar de tarea y el kernel se cuelga en
    // un bucle infinito silencioso.
    //
    // Los tests que realmente necesitan atomicidad deben llamar a
    // preempt_disable()/preempt_enable() ellos mismos.
    t->fn();

    if (g_failed == failed_before) {
      g_passed++;
      klog_printf(KLOG_INFO, "[TEST] PASS %s", t->name);
    } else {
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