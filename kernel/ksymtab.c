// kernel/ksymtab.c
#include "ksymtab.h"
#include "klog.h"
#include "string.h"

static int g_initialized = 0;
static size_t g_count = 0;

void ksymtab_init(void) {
  if (g_initialized)
    return;
  g_count = (size_t)(__ksymtab_end - __ksymtab_start);
  LOG_INFO("[KSYMTAB] %zu símbolos del kernel exportados", g_count);
  g_initialized = 1;
}

size_t ksymtab_count(void) { return g_count; }

uint64_t ksymtab_lookup(const char *name) {
  if (!name)
    return 0;
  for (const struct ksymtab_entry *e = __ksymtab_start; e < __ksymtab_end; e++) {
    if (strcmp(e->name, name) == 0)
      return e->addr;
  }
  return 0;
}