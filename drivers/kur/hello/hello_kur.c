#include "kur.h"
#include <stdint.h>

extern void klog_printf(int level, const char *fmt, ...);
extern uint64_t sched_get_ticks(void);

const struct kur_manifest __kur_manifest = {
    .magic       = KUR_MAGIC,
    .abi_version = KUR_ABI_VERSION,
    .flags       = 0,
    .name_len    = 5,
    .reserved    = 0,
    .name        = "hello",
    .hash        = {0},
};

int hello_add(int a, int b) { return a + b; }
KUR_EXPORT_SYMBOL(hello_add);

int kur_init(void) {
  klog_printf(3, "[hello.kur] cargado en tick=%lu",
              (unsigned long)sched_get_ticks());
  return 0;
}

void kur_exit(void) {
  klog_printf(3, "[hello.kur] descargado");
}
