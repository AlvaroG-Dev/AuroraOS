// tools/hello2_kur.c
#include "kur.h"
#include <stdint.h>

extern void klog_printf(int level, const char *fmt, ...);
extern int hello_add(int a, int b);

const struct kur_manifest __kur_manifest = {
    .magic       = KUR_MAGIC,
    .abi_version = KUR_ABI_VERSION,
    .flags       = 0,
    .name_len    = 6,
    .reserved    = 0,
    .name        = "hello2",
    .hash        = {0},
};

int kur_init(void) {
  int r = hello_add(3, 4);
  klog_printf(3, "[hello2.kur] cargado, hello_add(3,4)=%d", r);
  return 0;
}

void kur_exit(void) {
  klog_printf(3, "[hello2.kur] descargado");
}