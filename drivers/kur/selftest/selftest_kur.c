// tools/selftest_kur.c
//
// .kur de smoke-test de Fase 3.x.
//
// Ejercita:
//   - manifest + SHA-256 (vía kurbuild -H)
//   - imports del kernel: klog_printf, sched_get_ticks, kmalloc, kfree
//   - exports: selftest_add (para que otro .kur lo pueda importar)
//   - alloc/free de heap con patrón de verificación
//   - monotonicidad de sched_get_ticks
//
// En kur_init, si algo falla, devuelve -1 para que el loader rechace
// el módulo (ver kur_loader.c: rc < 0 → goto fail → vmm_free_pages).

#include "kur.h"
#include <stddef.h>
#include <stdint.h>

extern void klog_printf(int level, const char *fmt, ...);
extern uint64_t sched_get_ticks(void);
extern void *kmalloc(size_t size);
extern void kfree(void *ptr);

const struct kur_manifest __kur_manifest = {
    .magic = KUR_MAGIC,
    .abi_version = KUR_ABI_VERSION,
    .flags = 0,
    .name_len = 9,
    .reserved = 0,
    .name = "selftest",
    .hash = {0},
};

// Símbolo exportado. hello2.kur podría importarlo:
//   extern int selftest_add(int, int);
//   int r = selftest_add(5, 7);   // r == 57
int selftest_add(int a, int b) { return a * 10 + b; }
KUR_EXPORT_SYMBOL(selftest_add);

int kur_init(void) {
  int failures = 0;
  uint64_t t0 = sched_get_ticks();
  klog_printf(3, "[selftest.kur] init tick=%lu", (unsigned long)t0);

  // -- Test 1: kmalloc/kfree con verificación de patrón ---------------
  uint8_t *buf = (uint8_t *)kmalloc(256);
  if (!buf) {
    klog_printf(1, "[selftest.kur] FAIL kmalloc(256)");
    failures++;
  } else {
    for (int i = 0; i < 256; i++)
      buf[i] = (uint8_t)(i ^ 0xA5);
    int ok = 1;
    for (int i = 0; i < 256; i++) {
      if (buf[i] != (uint8_t)(i ^ 0xA5)) {
        ok = 0;
        break;
      }
    }
    kfree(buf);
    if (ok) {
      klog_printf(3, "[selftest.kur] TEST kmalloc/kfree OK");
    } else {
      klog_printf(1, "[selftest.kur] FAIL kmalloc contenido");
      failures++;
    }
  }

  // -- Test 2: llamada local al export propio -------------------------
  int r = selftest_add(3, 4);
  if (r == 34) {
    klog_printf(3, "[selftest.kur] TEST selftest_add(3,4)=34 OK");
  } else {
    klog_printf(1, "[selftest.kur] FAIL selftest_add(3,4)=%d", r);
    failures++;
  }

  // -- Test 3: sched_get_ticks es monótono ----------------------------
  uint64_t t1 = sched_get_ticks();
  if (t1 >= t0) {
    klog_printf(3, "[selftest.kur] TEST ticks monótonos OK");
  } else {
    klog_printf(1, "[selftest.kur] FAIL ticks retroceden");
    failures++;
  }

  if (failures == 0) {
    klog_printf(3, "[selftest.kur] PASS 3/3");
    return 0;
  }
  klog_printf(1, "[selftest.kur] FAILED %d/3", failures);
  return -1;
}

void kur_exit(void) { klog_printf(3, "[selftest.kur] exit"); }