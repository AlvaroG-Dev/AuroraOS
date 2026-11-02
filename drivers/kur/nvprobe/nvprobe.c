// SPDX-License-Identifier: GPL-2.0-only
// drivers/kur/nvprobe/nvprobe.c — Plan_NVIDIA.md, N1 (hito 1): identidad de la
// GPU NVIDIA. SOLO LECTURAS: configuracion PCI y dos registros de BAR0
// (NV_PMC_BOOT_0 y NV_PMC_BOOT_42). No activa bus master, no reclama la GPU,
// no toca el framebuffer del firmware (GOP).
//
// Opt-in: no hace nada salvo que la linea del kernel lleve `nvprobe=1`.
// `nvprobe=size` ademas mide los BAR (escribe 0xFFFFFFFF con la decodificacion
// de memoria apagada unos microsegundos: si el GOP esta en esta GPU la imagen
// puede parpadear).  Resultado por klog (-> klog.file=/data/aurora.log).
#include "kur.h"
#include "paging.h"
#include "pci.h"
#include <stddef.h>
#include <stdint.h>

extern void klog_printf(int level, const char *fmt, ...);
extern int cmdline_get(const char *key, char *out, size_t outlen);
extern int pci_ecam_available(void);
extern uint32_t pci_read_config_ext_dword(uint8_t, uint8_t, uint8_t, uint16_t);
extern uint16_t pci_find_ext_capability(uint8_t, uint8_t, uint8_t, uint16_t);

const struct kur_manifest __kur_manifest = {
    .magic       = KUR_MAGIC,
    .abi_version = KUR_ABI_VERSION,
    .flags       = 0,
    .name_len    = 7,
    .reserved    = 0,
    .name        = "nvprobe",
    .hash        = {0},
};

#define NV_VENDOR 0x10de
#define PMC_BOOT_0  0x000
#define PMC_BOOT_42 0xA00
#define L(...) klog_printf(3, "[nvprobe] " __VA_ARGS__)

static const char *arch_name(uint32_t arch, uint32_t impl) {
  switch (arch) {
  case 0x16: switch (impl) { case 2: return "TU102"; case 4: return "TU104"; case 6: return "TU106";
                             case 7: return "TU117"; case 8: return "TU116"; } return "Turing (TU10x)";
  case 0x17: switch (impl) { case 0: return "GA100"; case 2: return "GA102"; case 3: return "GA103";
                             case 4: return "GA104"; case 6: return "GA106"; case 7: return "GA107"; }
             return "Ampere (GA10x)";
  case 0x18: return "Hopper (GH100)";
  case 0x19: switch (impl) { case 2: return "AD102"; case 3: return "AD103"; case 4: return "AD104";
                             case 6: return "AD106"; case 7: return "AD107"; } return "Ada (AD10x)";
  case 0x1a: return "Blackwell (GB10x)";
  case 0x1b: return "Blackwell (GB20x)";
  }
  return "desconocida";
}

static uint32_t cfg32(const pci_device_t *d, int off) {
  return pci_read_config_dword(d->bus, d->slot, d->func, off);
}

// BAR i (config 0x10+4i) como base fisica de 64 bits (pci_device_t.bar es de 32).
static uint64_t bar_base(const pci_device_t *d, int i, int *is64, int *pref, int *io) {
  uint32_t lo = cfg32(d, 0x10 + 4 * i);
  *io = lo & 1;
  *is64 = !*io && ((lo >> 1) & 3) == 2;
  *pref = !*io && ((lo >> 3) & 1);
  uint64_t base = *io ? (lo & ~3u) : (lo & ~0xFu);
  if (*is64 && i < 5) base |= (uint64_t)cfg32(d, 0x10 + 4 * (i + 1)) << 32;
  return base;
}

static uint64_t bar_size(const pci_device_t *d, int i, int is64, int io) {
  uint16_t cmd = pci_read_config_word(d->bus, d->slot, d->func, 4);
  uint32_t lo = cfg32(d, 0x10 + 4 * i), hi = is64 ? cfg32(d, 0x14 + 4 * i) : 0;
  pci_write_config_word(d->bus, d->slot, d->func, 4, cmd & ~(uint16_t)3); // sin IO/MEM
  pci_write_config_dword(d->bus, d->slot, d->func, 0x10 + 4 * i, 0xFFFFFFFFu);
  if (is64) pci_write_config_dword(d->bus, d->slot, d->func, 0x14 + 4 * i, 0xFFFFFFFFu);
  uint64_t m = cfg32(d, 0x10 + 4 * i);
  if (is64) m |= (uint64_t)cfg32(d, 0x14 + 4 * i) << 32;
  pci_write_config_dword(d->bus, d->slot, d->func, 0x10 + 4 * i, lo);
  if (is64) pci_write_config_dword(d->bus, d->slot, d->func, 0x14 + 4 * i, hi);
  pci_write_config_word(d->bus, d->slot, d->func, 4, cmd);
  m &= io ? ~3ULL : ~0xFULL;
  if (!is64) m |= 0xFFFFFFFF00000000ULL;
  return ~m + 1;
}

// Recorre la lista de capacidades PCI (no hay pci_find_capability exportado).
static uint8_t find_cap(const pci_device_t *d, uint8_t id) {
  if (!(pci_read_config_word(d->bus, d->slot, d->func, 6) & 0x10)) return 0; // sin lista
  uint8_t p = pci_read_config_byte(d->bus, d->slot, d->func, 0x34) & 0xFC;
  for (int n = 0; p >= 0x40 && n < 48; n++) {
    if (pci_read_config_byte(d->bus, d->slot, d->func, p) == id) return p;
    p = pci_read_config_byte(d->bus, d->slot, d->func, p + 1) & 0xFC;
  }
  return 0;
}

static void probe_one(const pci_device_t *d, int do_size) {
  L("== %02x:%02x.%x  %04x:%04x subsys %04x:%04x rev %02x clase %02x%02x", d->bus, d->slot,
    d->func, d->vendor_id, d->device_id, d->subsystem_vendor_id, d->subsystem_id,
    d->revision_id, d->class_id, d->subclass_id);
  uint16_t cmd = pci_read_config_word(d->bus, d->slot, d->func, 4);
  L("comando PCI=0x%04x (IO=%d MEM=%d BUSMASTER=%d)", cmd, cmd & 1, (cmd >> 1) & 1, (cmd >> 2) & 1);

  uint64_t bar0 = 0;
  for (int i = 0; i < 6; i++) {
    int is64, pref, io;
    uint64_t b = bar_base(d, i, &is64, &pref, &io);
    if (is64) {
      uint64_t sz = do_size ? bar_size(d, i, 1, 0) : 0;
      L("BAR%d%s base=0x%llx 64-bit%s tamano=%s%llu MiB", i, i == 0 ? " (registros)" : "",
        (unsigned long long)b, pref ? " prefetch" : "", do_size ? "" : "(no medido) ",
        (unsigned long long)(sz >> 20));
      if (i == 0) bar0 = b;
      i++; // ocupa dos
    } else if (b || io) {
      uint64_t sz = do_size ? bar_size(d, i, 0, io) : 0;
      L("BAR%d base=0x%llx %s%s tamano=%s%llu KiB", i, (unsigned long long)b, io ? "IO" : "32-bit",
        pref ? " prefetch" : "", do_size ? "" : "(no medido) ", (unsigned long long)(sz >> 10));
      if (i == 0) bar0 = b;
    }
  }

  // Capacidades PCI (la config extendida, ReBAR incluido, no se lee: sin ECAM).
  uint8_t pcie = find_cap(d, 0x10);
  if (pcie) {
    uint32_t lcap = cfg32(d, pcie + 0x0C);
    uint16_t lsta = pci_read_config_word(d->bus, d->slot, d->func, pcie + 0x12);
    L("PCIe: enlace maximo gen%u x%u, actual gen%u x%u", lcap & 0xF, (lcap >> 4) & 0x3F,
      lsta & 0xF, (lsta >> 4) & 0x3F);
  } else L("PCIe: sin capacidad PCIe");
  uint8_t msix = find_cap(d, 0x11);
  uint8_t msi = find_cap(d, 0x05);
  if (msix) L("MSI-X: %u entradas", (pci_read_config_word(d->bus, d->slot, d->func, msix + 2) & 0x7FF) + 1);
  L("MSI=%s MSI-X=%s", msi ? "si" : "no", msix ? "si" : "no");
  if (!pci_ecam_available()) {
    L("config extendida (ReBAR): no disponible (sin MCFG)");
  } else {
    uint16_t rb = pci_find_ext_capability(d->bus, d->slot, d->func, 0x15);
    if (!rb) {
      L("ReBAR: sin capacidad extendida 0x15 (config extendida por ECAM OK)");
    } else {
      uint32_t c0 = pci_read_config_ext_dword(d->bus, d->slot, d->func, rb + 8);
      unsigned nbars = (c0 >> 5) & 7;
      L("ReBAR en 0x%03x, %u BAR redimensionables", rb, nbars);
      for (unsigned i = 0; i < nbars && i < 6; i++) {
        uint32_t cap = pci_read_config_ext_dword(d->bus, d->slot, d->func, rb + 4 + 8 * i);
        uint32_t ctl = pci_read_config_ext_dword(d->bus, d->slot, d->func, rb + 8 + 8 * i);
        L("  ReBAR BAR%u: actual 2^%u MiB, soportados (mascara tamanos 1MiB..)=0x%08x", ctl & 7,
          (ctl >> 8) & 0x1F, cap >> 4);
      }
    }
  }

  if (!bar0 || bar0 >= (1ULL << 40)) { L("BAR0 no utilizable (0x%llx)", (unsigned long long)bar0); return; }
  if (!(cmd & 2)) { L("MEM desactivado en el comando PCI: no se lee BAR0 (el firmware no activo la GPU)"); return; }

  volatile uint32_t *r = mmio_map(bar0, 0x1000, PTE_WRITABLE | PTE_NOCACHE);
  if (!r) { L("mmio_map(BAR0) fallo"); return; }
  uint32_t b0 = r[PMC_BOOT_0 / 4], b42 = r[PMC_BOOT_42 / 4];
  mmio_unmap(bar0, 0x1000);
  L("NV_PMC_BOOT_0=0x%08x NV_PMC_BOOT_42=0x%08x", b0, b42);
  if (b0 == 0xFFFFFFFFu || b42 == 0xFFFFFFFFu) { L("la GPU no responde (lecturas 0xFFFFFFFF)"); return; }
  uint32_t arch = (b42 >> 24) & 0x3F, impl = (b42 >> 20) & 0xF;
  L("arquitectura=0x%02x implementacion=%u revision=%u.%u -> %s", arch, impl, (b42 >> 16) & 0xF,
    (b42 >> 12) & 0xF, arch_name(arch, impl));
}

// nvprobe=ecam: autoprueba de ECAM (compara con el acceso por puertos).
static void ecam_selftest(void) {
  int same = 0, diff = 0, ext = 0;
  L("ECAM %s", pci_ecam_available() ? "disponible (MCFG)" : "NO disponible");
  for (int bus = 0; bus < 4; bus++)
    for (int slot = 0; slot < 32; slot++)
      for (int fn = 0; fn < 8; fn++) {
        uint32_t a = pci_read_config_dword(bus, slot, fn, 0);
        if ((a & 0xFFFF) == 0xFFFF) continue;
        uint32_t b = pci_read_config_ext_dword(bus, slot, fn, 0);
        if (a == b) same++; else diff++;
        uint16_t c = 0;
        for (int id = 1; id < 0x30 && !c; id++) c = pci_find_ext_capability(bus, slot, fn, id);
        if (c) { ext++; L("  %02x:%02x.%x id=%08x con capacidad extendida en 0x%03x", bus, slot, fn, a, c); }
      }
  L("ECAM vs puertos: %d iguales, %d distintos; %d funciones con cap. extendida", same, diff, ext);
}

int kur_init(void) {
  char opt[16];
  if (!cmdline_get("nvprobe", opt, sizeof opt) || !opt[0] || opt[0] == '0') return 0; // opt-in
  if (opt[0] == 'e') { ecam_selftest(); return 0; }
  int do_size = opt[0] == 's';
  int found = 0, scanned = 0;
  L("N1: buscando GPU NVIDIA en el bus PCI (solo lectura%s)", do_size ? ", midiendo BAR" : "");
  // Escaneo propio: los .kur de /lib/modules cargan antes de que el kernel
  // enumere PCI, asi que pci_get_devices() puede estar vacio.
  for (int bus = 0; bus < 256; bus++)
    for (int slot = 0; slot < 32; slot++)
      for (int fn = 0; fn < 8; fn++) {
        uint32_t id = pci_read_config_dword(bus, slot, fn, 0);
        if ((id & 0xFFFF) == 0xFFFF) { if (fn == 0) break; continue; }
        scanned++;
        if ((id & 0xFFFF) != NV_VENDOR) {
          if (fn == 0 && !(pci_read_config_byte(bus, slot, 0, 0x0E) & 0x80)) break;
          continue;
        }
        uint32_t cls = pci_read_config_dword(bus, slot, fn, 8);
        if ((cls >> 24) != 0x03) continue;
        pci_device_t d = {0};
        d.bus = bus; d.slot = slot; d.func = fn;
        d.vendor_id = id & 0xFFFF; d.device_id = id >> 16;
        d.class_id = cls >> 24; d.subclass_id = cls >> 16; d.revision_id = cls;
        uint32_t ss = pci_read_config_dword(bus, slot, fn, 0x2C);
        d.subsystem_vendor_id = ss & 0xFFFF; d.subsystem_id = ss >> 16;
        probe_one(&d, do_size);
        found++;
      }
  L("%d funciones PCI vistas", scanned);
  klog_printf(3, found ? "[nvprobe] fin: %d GPU(s)" : "[nvprobe] no hay GPU NVIDIA", found);
  return 0;
}

void kur_exit(void) {}
