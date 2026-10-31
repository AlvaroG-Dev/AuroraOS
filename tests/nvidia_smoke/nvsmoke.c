// SPDX-License-Identifier: GPL-2.0-only
// tests/nvidia_smoke/nvsmoke.c — Plan_NVIDIA.md, T0.5. Modulo .kur de humo:
// enlaza el nv-kernel.o de NVIDIA (src/nvidia, sin modificar) con una capa de
// SO de mentira (nvsmoke_stubs.c, generada por scripts/nvidia-smoke.sh) y
// llama a una funcion pura del RM (tabla de PCI IDs). Valida cargador .kur,
// reubicaciones y ABI (NV_API_CALL = ABI SysV normal) sin hardware.
#include "kur.h"
#include <stdint.h>

extern void klog_printf(int level, const char *fmt, ...);
extern unsigned char rm_is_supported_pci_device(uint8_t pci_class, uint8_t pci_subclass,
        uint16_t vendor, uint16_t device, uint16_t subsys_vendor, uint16_t subsys_device,
        unsigned char print_legacy_warning);

const struct kur_manifest __kur_manifest = {
    .magic       = KUR_MAGIC,
    .abi_version = KUR_ABI_VERSION,
    .flags       = 0,
    .name_len    = 7,
    .reserved    = 0,
    .name        = "nvsmoke",
    .hash        = {0},
};

static const struct {
  uint8_t cls; uint16_t vendor, dev; unsigned char want; const char *what;
} cases[] = {
    { 0x03, 0x10de, 0x2882, 1, "RTX 4060 (AD107), VGA" },
    { 0x03, 0x10de, 0x2684, 1, "RTX 4090 (AD102), VGA" },
    { 0x03, 0x8086, 0x2882, 0, "otro fabricante (Intel)" },
    { 0x02, 0x10de, 0x2882, 0, "clase de red, no pantalla" },
};

int kur_init(void) {
  int fail = 0;
  for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
    unsigned char got = rm_is_supported_pci_device(cases[i].cls, 0x00, cases[i].vendor,
                                                   cases[i].dev, 0, 0, 0);
    int ok = (!!got == cases[i].want);
    fail += !ok;
    klog_printf(3, "[nvsmoke] %04x:%04x cls=%02x %s -> %d (esperado %d) %s", cases[i].vendor,
                cases[i].dev, cases[i].cls, cases[i].what, got, cases[i].want,
                ok ? "OK" : "FAIL");
  }
  klog_printf(3, "[nvsmoke] %s", fail ? "FAIL" : "ALL PASS");
  return 0;
}

void kur_exit(void) { klog_printf(3, "[nvsmoke] descargado"); }
