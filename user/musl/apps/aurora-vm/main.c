// aurora-vm — VMM de la mini-VM de AuroraOS (Plan_MiniVM.md).
//
// Un PC "pc" de QEMU (i440FX + PIIX3) sobre /dev/aurora-vm (VT-x + EPT):
//   SeaBIOS + SeaVGABIOS, PIC 8259 x2 + ELCR, PIT 8254, RTC/CMOS, teclado y raton
//   PS/2 (i8042), IDE PIIX3 con DMA (disco y CD-ROM ATAPI), VGA estandar con VBE,
//   ACPI (PM PIIX4), LAPIC + IOAPIC y virtio-blk heredado. Los dispositivos copian
//   la semantica de QEMU (ver cada .h). Los accesos a memoria de dispositivo los
//   ejecuta insn.h; las interrupciones se inyectan con AVM_INJECT_IRQ.
//
// Uso:
//   aurora-vm --caps                 que ofrece la CPU (VMX, EPT, ...)
//   aurora-vm --autotest             tres guests incorporados (real, modo largo+MMIO, temporizador)
//   aurora-vm --bios-test[-acpi|-vga] SeaBIOS + virtio-blk + sector de arranque incorporado
//   aurora-vm --plano FICH [--en DIR] [--mem MiB]
//   aurora-vm --crear-disco IMG TAM  disco vacio (disperso) de TAM (20G, 512M...)
//   aurora-vm --bios [--ide IMG] [--cd ISO] [--disco IMG] [--mem MiB] [--arranque cd|disco]
//                    [--rom FICH] [--vgarom FICH] [--tiempo S] [--log-bios] [--sin-acpi]
//                    [--sin-vga] [--sin-ventana] [--depurar]
//   aurora-vm PROGRAMA.exe           (aurora-run lo llama asi; aun sin huesped)
// Puertos de depuracion: 0x3F8 serie -> stdout; 0xF4 termina con el codigo escrito
// (solo en las pruebas y en --plano).
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "../../../../kernel/aurora_vm.h"
#include "guests.h"
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define DEV "/dev/aurora-vm"
#define MMIO_TEST_BASE 0xE0000000ULL
#define VGA_HOLE_LO 0xA0000ULL
#define VGA_HOLE_HI 0xC0000ULL
#define BIOS_MAX (256u * 1024)

static int g_vm = -1;
static int g_mfd = -1;
static uint8_t *g_ram;      // RAM del huesped y, detras, la copia alta de la BIOS
static uint64_t g_ram_size; // bytes de RAM (sin la BIOS)
static uint8_t g_bios_img[BIOS_MAX]; // imagen original (para los reinicios)
static size_t g_bios_n;
static uint64_t g_bios_gpa; // 4 GiB - tamano
static char g_out[512];
static size_t g_out_n;
static int g_echo = 1;
static int g_debug = 1;     // intercepta excepciones del huesped y las registra (dmesg [VMX])
static int g_exit_port;     // 0xF4 termina la VM (pruebas)

static uint64_t coarse_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

// Reloj de los dispositivos (PIT, LAPIC, RTC, temporizador ACPI). CLOCK_MONOTONIC
// de AuroraOS avanza a saltos de 1 ms: con el el temporizador ACPI y el contador
// del PIT del huesped se quedaban quietos 1 ms y cualquier espera corta (udelay,
// calibraciones de Windows) duraba un tick entero. Se interpola con el TSC,
// calibrado al arrancar contra dos cambios de tick (AURORA_VM_RELOJ_TICK=1 lo
// desactiva).
static uint64_t g_clk_tsc0, g_clk_ns0, g_clk_mult, g_clk_last; // mult = ns por ciclo << 32
static uint64_t g_clk_hz;
static uint64_t g_fast_eoi_total, g_fast_rtc_total; // atajos del kernel (traza)
static inline uint64_t rdtsc_u(void) {
  uint32_t lo, hi;
  __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
  return ((uint64_t)hi << 32) | lo;
}
static uint64_t now_ns(void) {
  if (!g_clk_mult)
    return coarse_ns();
  uint64_t d = rdtsc_u() - g_clk_tsc0;
  uint64_t t = g_clk_ns0 + (uint64_t)(((__uint128_t)d * g_clk_mult) >> 32);
  if (t < g_clk_last) // nunca hacia atras (TSC de otra CPU un poco por detras)
    t = g_clk_last;
  g_clk_last = t;
  return t;
}
// Espera al siguiente cambio del reloj grueso y devuelve (ns, tsc) en ese instante.
static void clk_edge(uint64_t *ns, uint64_t *tsc) {
  uint64_t c0 = coarse_ns(), c;
  while ((c = coarse_ns()) == c0)
    ;
  *ns = c;
  *tsc = rdtsc_u();
}
static void clock_init(void) {
  const char *e = getenv("AURORA_VM_RELOJ_TICK");
  if (e && *e == '1')
    return;
  uint64_t n0, t0, n1, t1;
  clk_edge(&n0, &t0);
  struct timespec ts = {0, 100000000L}; // ~100 ms
  nanosleep(&ts, NULL);
  clk_edge(&n1, &t1);
  if (n1 <= n0 || t1 <= t0)
    return;
  uint64_t hz = (uint64_t)((__uint128_t)(t1 - t0) * 1000000000ULL / (n1 - n0));
  if (hz < 100000000ULL || hz > 20000000000ULL) { // TSC raro: reloj de ticks
    fprintf(stderr, "aurora-vm: TSC de %llu Hz no creible; se usa el reloj de 1 ms\n",
            (unsigned long long)hz);
    return;
  }
  g_clk_tsc0 = t1;
  g_clk_ns0 = n1;
  g_clk_mult = (uint64_t)(((__uint128_t)1000000000ULL << 32) / hz);
  g_clk_last = n1;
  g_clk_hz = hz;
}

static void irq_set(int irq, int level);
static int irq_edge_pending(int irq);
static uint8_t *dma_ptr(uint64_t gpa, uint64_t len);

#include "pic.h"
#include "pit.h"
#include "rtc.h"
#include "ps2.h"
#include "apic.h"
#include "ide.h"
#include "platform.h"
#include "vga.h"
#include "vgawin.h"
#include "insn.h"

// ---------------------------------------------------------------------------
// Lineas de interrupcion: ISA n -> PIC (n < 16) e IOAPIC pin n (la MADT de
// SeaBIOS no redirige el IRQ0; las PCI 5/9/10/11 son de nivel, activas en alto).
// ---------------------------------------------------------------------------
static void irq_set(int irq, int level) {
  if (irq < 0 || irq >= IOAPIC_PINS)
    return;
  if (irq < 16)
    pic_set_irq(irq, level);
  if (g_apic_on)
    ioapic_line(irq, level);
}

// Un flanco anterior de la linea aun no lo ha aceptado nadie (para no perder
// ticks del PIT: se reinyectan cuando el anterior ya se entrego).
static int irq_edge_pending(int irq) {
  if (irq < 16) {
    const struct i8259 *s = &g_pic[irq >> 3];
    int m = 1 << (irq & 7);
    if ((s->irr & m) && !(s->imr & m))
      return 1;
  }
  if (g_apic_on && irq < IOAPIC_PINS) {
    uint64_t e = g_io_red[irq];
    int v = (int)(e & 0xFF);
    if (!(e & 0x10000) && v >= 16 && ((g_irr[v >> 5] >> (v & 31)) & 1))
      return 1;
  }
  return 0;
}

// Memoria del huesped para DMA y colas: solo RAM (nunca el hueco VGA).
static uint8_t *dma_ptr(uint64_t gpa, uint64_t len) {
  if (len > g_ram_size || gpa > g_ram_size - len)
    return NULL;
  if (g_plat && gpa < VGA_HOLE_HI && gpa + len > VGA_HOLE_LO)
    return NULL;
  return g_ram + gpa;
}

// ---------------------------------------------------------------------------
// Memoria fisica del huesped (para insn.h): RAM, hueco VGA, BIOS, LFB/ROM de la
// VGA, LAPIC/IOAPIC y el dispositivo MMIO de prueba de --autotest.
// ---------------------------------------------------------------------------
static uint8_t g_mmio_test[4096];

static void apic_access(uint64_t gpa, uint8_t *buf, unsigned n, int write) {
  while (n) {
    unsigned c = n >= 4 && !(gpa & 3) ? 4 : n >= 2 && !(gpa & 1) ? 2 : 1;
    if (write) {
      uint64_t v = 0;
      memcpy(&v, buf, c);
      apic_mmio_write(gpa, (int)c, v);
    } else {
      uint64_t v = apic_mmio_read(gpa, (int)c);
      memcpy(buf, &v, c);
    }
    gpa += c;
    buf += c;
    n -= c;
  }
}

static int gmem_access(uint64_t gpa, uint8_t *buf, unsigned n, int write) {
  while (n) {
    unsigned c = 1;
    if (gpa < g_ram_size && !(g_plat && gpa >= VGA_HOLE_LO && gpa < VGA_HOLE_HI)) {
      uint64_t lim = (g_plat && gpa < VGA_HOLE_LO) ? VGA_HOLE_LO : g_ram_size;
      c = (unsigned)(lim - gpa < n ? lim - gpa : n);
      if (write)
        memcpy(g_ram + gpa, buf, c);
      else
        memcpy(buf, g_ram + gpa, c);
    } else if (g_plat && gpa >= VGA_HOLE_LO && gpa < VGA_HOLE_HI) {
      c = (unsigned)(VGA_HOLE_HI - gpa < n ? VGA_HOLE_HI - gpa : n);
      if (write)
        vga_mmio_write(gpa, buf, c);
      else
        vga_mmio_read(gpa, buf, c);
    } else if (g_plat && g_bios_n && gpa >= g_bios_gpa && gpa < 0x100000000ULL) {
      c = (unsigned)(0x100000000ULL - gpa < n ? 0x100000000ULL - gpa : n);
      uint8_t *p = g_ram + g_ram_size + (gpa - g_bios_gpa);
      if (write)
        memcpy(p, buf, c);
      else
        memcpy(buf, p, c);
    } else if (g_vga_on && g_lfb && g_vga_lfb_gpa && gpa >= g_vga_lfb_gpa &&
               gpa < g_vga_lfb_gpa + VGA_LFB_SIZE) {
      uint64_t left = g_vga_lfb_gpa + VGA_LFB_SIZE - gpa;
      c = (unsigned)(left < n ? left : n);
      if (write)
        memcpy(g_lfb + (gpa - g_vga_lfb_gpa), buf, c);
      else
        memcpy(buf, g_lfb + (gpa - g_vga_lfb_gpa), c);
    } else if (g_vga_on && g_vrom && g_vga_rom_gpa && gpa >= g_vga_rom_gpa &&
               gpa < g_vga_rom_gpa + VGA_ROM_SIZE) {
      uint64_t left = g_vga_rom_gpa + VGA_ROM_SIZE - gpa;
      c = (unsigned)(left < n ? left : n);
      if (!write)
        memcpy(buf, g_vrom + (gpa - g_vga_rom_gpa), c);
    } else if (apic_mmio_hit(gpa)) {
      uint64_t left = 0x1000 - (gpa & 0xFFF);
      c = (unsigned)(left < n ? left : n);
      apic_access(gpa, buf, c, write);
    } else if (!g_plat && gpa >= MMIO_TEST_BASE && gpa < MMIO_TEST_BASE + sizeof(g_mmio_test)) {
      uint64_t off = gpa - MMIO_TEST_BASE;
      if (write) {
        if (off >= 4)
          g_mmio_test[off] = *buf;
      } else {
        *buf = off < 4 ? 0xA0 : g_mmio_test[off];
      }
    } else if (!write) {
      *buf = 0xFF; // nada: bus flotante
    }
    gpa += c;
    buf += c;
    n -= c;
  }
  return 0;
}

static int gmem_read(uint64_t gpa, void *dst, unsigned n) { return gmem_access(gpa, (uint8_t *)dst, n, 0); }
static int gmem_write(uint64_t gpa, const void *src, unsigned n) {
  return gmem_access(gpa, (uint8_t *)(uintptr_t)src, n, 1);
}
// Tablas de paginas: solo en RAM.
static int gpt_read(uint64_t gpa, void *dst, unsigned n) {
  uint8_t *p = dma_ptr(gpa, n);
  if (!p)
    return -1;
  memcpy(dst, p, n);
  return 0;
}

// ---------------------------------------------------------------------------
// Puertos de E/S
// ---------------------------------------------------------------------------
static int g_exit_code = -1;

static void serial_out(uint8_t c) {
  if (g_out_n < sizeof(g_out) - 1)
    g_out[g_out_n++] = (char)c;
  if (g_echo) {
    putchar(c);
    fflush(stdout);
  }
}

// Ultimos accesos de E/S del huesped (para el diagnostico de los cuelgues).
#define IOLOG_N 32
static struct { uint16_t port; uint8_t size, w; uint32_t v; } g_iolog[IOLOG_N];
static unsigned g_iolog_n;
static void iolog(uint16_t port, int size, int w, uint32_t v) {
  g_iolog[g_iolog_n % IOLOG_N].port = port;
  g_iolog[g_iolog_n % IOLOG_N].size = (uint8_t)size;
  g_iolog[g_iolog_n % IOLOG_N].w = (uint8_t)w;
  g_iolog[g_iolog_n % IOLOG_N].v = v;
  g_iolog_n++;
}

static uint32_t io_in(uint16_t port, int size) {
  uint32_t v = 0xFFFFFFFFu;
  if (plat_in(port, size, &v))
    ;
  else if (!g_plat && pic_port(port))
    v = pic_in(port);
  else if (!g_plat && port >= 0x40 && port <= 0x43)
    v = pit_read(port);
  else if (!g_plat && port == 0x61)
    v = port61_read();
  else if (port == 0x3FD)
    v = 0x60; // LSR: transmisor vacio
  else if (port >= 0x3F8 && port <= 0x3FF)
    v = 0;
  if (size == 1)
    v &= 0xFF;
  else if (size == 2)
    v &= 0xFFFF;
  if (port != 0x1F0 && port != 0x170) // los datos de IDE llenarian el registro
    iolog(port, size, 0, v);
  return v;
}

static void io_out(uint16_t port, uint32_t v, int size) {
  if (size == 1)
    v &= 0xFF;
  else if (size == 2)
    v &= 0xFFFF;
  if (port != 0x402 && port != 0xE9 && port != 0x1F0 && port != 0x170)
    iolog(port, size, 1, v);
  if (plat_out(port, v, size))
    return;
  if (!g_plat && pic_port(port))
    pic_out(port, (uint8_t)v);
  else if (!g_plat && port >= 0x40 && port <= 0x43)
    pit_write(port, (uint8_t)v);
  else if (!g_plat && port == 0x61)
    port61_write((uint8_t)v);
  else if (port == 0x3F8)
    serial_out((uint8_t)v);
  else if (port == 0xF4 && g_exit_port)
    g_exit_code = (int)(v & 0xFF);
}

// Decodifica los prefijos de la INS/OUTS (RIP - ilen): tamano de direccion
// (0=16, 1=32, 2=64) y segmento (0=ES 1=CS 2=SS 3=DS 4=FS 5=GS, codificacion de
// VT-x). -1 si no se puede leer o no es INS/OUTS. Hace falta porque con KVM
// anidado la informacion de instruccion y la direccion lineal de la salida
// llegan obsoletas: los datos de REP INSW acababan fuera del bufer.
// Registros para emular una instruccion (MMIO, INS/OUTS, HLT): sin leer todo el
// VMCS (ver AVM_GET_REGS_LITE). Con un kernel sin estas ordenes, las completas.
static int g_regs_lite = 1;
static int get_regs_lite(struct avm_regs *r) {
  if (g_regs_lite) {
    if (ioctl(g_vm, AVM_GET_REGS_LITE, r) == 0)
      return 0;
    if (errno != ENOTTY && errno != EINVAL)
      return -1;
    g_regs_lite = 0;
  }
  return ioctl(g_vm, AVM_GET_REGS, r);
}
// Solo cambia registros generales, RSP, RIP y RFLAGS de lo leido con get_regs_lite.
static int set_regs_lite(const struct avm_regs *r) {
  if (g_regs_lite)
    return ioctl(g_vm, AVM_SET_REGS_LITE, r);
  return ioctl(g_vm, AVM_SET_REGS, r);
}

static int string_io_decode(const struct avm_regs *r, unsigned ilen, int lm, int *asz, int *seg) {
  uint8_t b[15];
  if (ilen == 0 || ilen > sizeof(b))
    return -1;
  uint64_t lin = r->cs.base + r->rip - ilen;
  if (!lm)
    lin &= 0xFFFFFFFFULL;
  if (lin_read(r, lin, b, ilen) < 0)
    return -1;
  int def = lm ? 2 : (int)((r->cs.ar >> 14) & 1), a = def, sg = -1;
  for (unsigned i = 0; i < ilen; i++) {
    uint8_t c = b[i];
    if (c == 0x67)
      a = lm ? 1 : (def ^ 1);
    else if (c == 0x26)
      sg = 0;
    else if (c == 0x2E)
      sg = 1;
    else if (c == 0x36)
      sg = 2;
    else if (c == 0x3E)
      sg = 3;
    else if (c == 0x64)
      sg = 4;
    else if (c == 0x65)
      sg = 5;
    else if (c == 0xF0 || c == 0xF2 || c == 0xF3 || c == 0x66 || (lm && (c & 0xF0) == 0x40))
      ;
    else if (c >= 0x6C && c <= 0x6F && i == ilen - 1) {
      *asz = a;
      *seg = c <= 0x6D ? 0 : sg < 0 ? 3 : sg; // INS: siempre ES
      return 0;
    } else
      return -1;
  }
  return -1;
}

// INS/OUTS: el kernel solo describe la instruccion (RIP ya avanzo); aqui se mueven
// los datos y se ajustan RSI/RDI y RCX, con el ajuste de 16/32 bits del registro.
static int string_io(const struct avm_exit *x) {
  struct avm_regs r;
  if (get_regs_lite(&r) < 0)
    return -1;
  int dir = (r.rflags & (1u << 10)) ? -1 : 1;
  int lm = (r.efer & (1ULL << 10)) && (r.cs.ar & (1u << 13));
  uint32_t size = x->io.size, n = x->io.count, done = 0;
  int asz = x->io.asize, seg = -1;
  if (string_io_decode(&r, x->io.ilen, lm, &asz, &seg) < 0)
    seg = -1;
  uint64_t mask = asz == 0 ? 0xFFFFULL : asz == 1 ? 0xFFFFFFFFULL : ~0ULL;
  uint64_t *ireg = x->io.is_write ? &r.rsi : &r.rdi;
  uint64_t off0 = *ireg & mask;
  uint64_t base;
  if (seg >= 0) {
    const struct avm_seg *sr[6] = {&r.es, &r.cs, &r.ss, &r.ds, &r.fs, &r.gs};
    base = (lm && seg < 4) ? 0 : sr[seg]->base;
    if (x->io.rep) {
      uint64_t c = r.rcx & mask;
      n = c > 0xFFFFFFFFULL ? 0xFFFFFFFFu : (uint32_t)c;
    }
    static int warned;
    uint64_t want = lm ? base + off0 : ((base + off0) & 0xFFFFFFFFULL);
    if (!warned && (asz != x->io.asize || want != x->io.linear)) {
      warned = 1;
      fprintf(stderr, "aurora-vm: aviso: INS/OUTS del kernel con asize=%u lineal=0x%llx; "
                      "decodificado asize=%d lineal=0x%llx (se usa este)\n",
              x->io.asize, (unsigned long long)x->io.linear, asz, (unsigned long long)want);
    }
  } else {
    base = x->io.linear - off0; // base del segmento (lineal = base + desplazamiento)
  }
  // Camino rapido: REP INSW/INSD del puerto de datos IDE hacia RAM, de pagina en
  // pagina con memcpy (el CD/disco por PIO de la BIOS: millones de palabras).
  if (!x->io.is_write && dir == 1 && (size == 2 || size == 4) && g_ide_on &&
      (x->io.port == 0x1F0 || x->io.port == 0x170)) {
    while (done < n) {
      uint64_t off = (off0 + (uint64_t)size * done) & mask;
      uint64_t lin = base + off;
      if (!lm)
        lin &= 0xFFFFFFFFULL;
      uint64_t bytes = (uint64_t)(n - done) * size;
      uint64_t room = 4096 - (lin & 0xFFF);   // hasta el final de la pagina
      if (bytes > room)
        bytes = room;
      if (mask != ~0ULL && bytes > mask + 1 - off) // sin dar la vuelta al registro
        bytes = mask + 1 - off;
      bytes -= bytes % size;
      uint64_t gpa;
      uint8_t *hp;
      if (!bytes || gva_to_gpa(&r, lin, &gpa) < 0 || !(hp = dma_ptr(gpa, bytes)))
        break;
      unsigned got = ide_data_read_bulk(x->io.port, hp, (unsigned)bytes, (int)size);
      done += got / size;
      if (got < bytes)
        break;
    }
  }
  for (; done < n; done++) {
    uint64_t off = (off0 + (uint64_t)((int64_t)dir * (int64_t)size * (int64_t)done)) & mask;
    uint64_t lin = base + off;
    if (!lm)
      lin &= 0xFFFFFFFFULL;
    uint32_t v = 0;
    if (x->io.is_write) {
      if (lin_read(&r, lin, &v, size) < 0)
        break;
      io_out(x->io.port, v, (int)size);
      if (g_exit_code >= 0) {
        done++;
        break;
      }
    } else {
      v = io_in(x->io.port, (int)size);
      if (lin_write(&r, lin, &v, size) < 0)
        break;
    }
  }
  uint64_t noff = (off0 + (uint64_t)((int64_t)dir * (int64_t)size * (int64_t)done)) & mask;
  *ireg = (*ireg & ~mask) | noff;
  if (x->io.rep)
    r.rcx = (r.rcx & ~mask) | ((r.rcx - done) & mask);
  if (set_regs_lite(&r) < 0)
    return -1;
  if (done < n && g_exit_code < 0) {
    fprintf(stderr, "aurora-vm: INS/OUTS fallo en el elemento %u de %u (puerto 0x%x, lineal 0x%llx)\n",
            done, n, x->io.port, (unsigned long long)x->io.linear);
    return -1;
  }
  return 0;
}

// ---------------------------------------------------------------------------
// Ventana de la VM y entrada del anfitrion
// ---------------------------------------------------------------------------
static int g_want_vga, g_want_win;
static const char *g_vga_rom_path;
static uint64_t g_vga_t_draw, g_vga_t_ev;

// AURORA_VM_CAPTURA=fichero.ppm: guarda la pantalla del huesped cada 2 s (tambien
// con --sin-ventana). Sirve para diagnosticar un arranque sin mirar la ventana.
static const char *g_capture;
static uint64_t g_capture_t;
static void vga_capture(void) {
  int w = 0, h = 0;
  uint32_t *px = vga_render(&w, &h);
  if (!px || w <= 0 || h <= 0)
    return;
  char tmp[512];
  snprintf(tmp, sizeof(tmp), "%s.tmp", g_capture);
  FILE *f = fopen(tmp, "wb");
  if (!f)
    return;
  fprintf(f, "P6\n%d %d\n255\n", w, h);
  for (int i = 0; i < w * h; i++) {
    uint8_t rgb[3] = {(uint8_t)(px[i] >> 16), (uint8_t)(px[i] >> 8), (uint8_t)px[i]};
    fwrite(rgb, 1, 3, f);
  }
  fclose(f);
  rename(tmp, g_capture);
}

static void vga_poll(uint64_t t) {
  if (g_vga_on && g_capture && t - g_capture_t >= 2000000000ULL) {
    g_capture_t = t;
    vga_capture();
  }
  if (!g_vga_on || !g_want_win)
    return;
  if (t - g_vga_t_ev >= 8000000ULL) {
    g_vga_t_ev = t;
    if (g_vw >= 0) {
      struct aw_event ev;
      int guard = 0;
      while (guard++ < 256 && aw_sys(AW_WIN_POLL, (uint64_t)g_vw, (uint64_t)&ev, 0, 0, 0) > 0) {
        switch (ev.type) {
        case AWEV_CLOSE: g_vw_closed = 1; break;
        case AWEV_RESIZE: g_vw_cw = ev.x; g_vw_ch = ev.y; break;
        case AWEV_FOCUS: host_focus(1); break;
        case AWEV_BLUR: host_focus(0); break;
        case AWEV_KEY: host_window_key((uint8_t)ev.x, 1); break;
        case AWEV_PTR: host_pointer(ev.x, ev.y, (int)(ev.data & 7), 0); break;
        case AWEV_PTR_LEAVE: host_pointer(0, 0, 0, 1); break;
        default: break;
        }
      }
    }
    host_input_poll();
  }
  if (t - g_vga_t_draw >= 33000000ULL) {
    g_vga_t_draw = t;
    int w = 0, h = 0;
    uint32_t *px = vga_render(&w, &h);
    if (px)
      vw_present(px, w, h);
  }
}

// ---------------------------------------------------------------------------
// Interrupciones hacia el vCPU
//
// Como cpu_get_pic_interrupt de QEMU: primero el LAPIC (si el TPR lo deja), luego
// el 8259 por LINT0 (ExtINT) y, si no, el vector del LAPIC bloqueado por el TPR
// (el kernel lo retiene hasta que CR8 baje). Lo que se pide al kernel puede no
// entregarse en esa vuelta: solo cuando x.irq_pending == -1 se hace el INTA
// (pic_intack_vector / lapic_accept). Si lo que toca cambia antes de entregarse,
// se retira y se pide lo nuevo.
// ---------------------------------------------------------------------------
static int g_inflight = -1; // valor pedido a AVM_INJECT_IRQ y aun sin entregar

static int irq_want(void) {
  int cand = g_apic_on ? lapic_candidate() : -1;
  if (cand >= 0 && ((uint32_t)cand & 0xF0) > (LREG(L_TPR) & 0xF0))
    return cand | (int)AVM_IRQ_TPR;
  if ((!g_apic_on || lapic_accept_pic()) && pic_output()) {
    int v = pic_peek_vector();
    if (v >= 0)
      return v;
  }
  if (cand >= 0)
    return cand | (int)AVM_IRQ_TPR;
  return -1;
}

static void irq_after_exit(const struct avm_exit *x) {
  if (g_apic_on)
    LREG(L_TPR) = x->tpr & 0xFF;
  if (g_inflight >= 0 && x->irq_pending < 0) { // entregada
    if (g_inflight & (int)AVM_IRQ_TPR) {
      lapic_accept(g_inflight & 0xFF);
    } else {
      int v = pic_intack_vector();
      if (v != (g_inflight & 0xFF) && g_debug > 1)
        fprintf(stderr, "aurora-vm: INTA del 8259 dio 0x%x (se entrego 0x%x)\n", v, g_inflight & 0xFF);
    }
    g_inflight = -1;
  }
}

static void irq_withdraw(void) {
  if (g_inflight >= 0) {
    ioctl(g_vm, AVM_WITHDRAW_IRQ, 0);
    g_inflight = -1;
  }
}

static void irq_before_run(void) {
  if (g_apic_on && g_tpr_dirty) {
    ioctl(g_vm, AVM_SET_TPR, (unsigned long)(LREG(L_TPR) & 0xFF));
    g_tpr_dirty = 0;
  }
  int want = irq_want();
  if (want == g_inflight)
    return;
  irq_withdraw();
  if (want >= 0) {
    if (ioctl(g_vm, AVM_INJECT_IRQ, (unsigned long)want) == 0)
      g_inflight = want;
    else if (g_debug > 1)
      fprintf(stderr, "aurora-vm: AVM_INJECT_IRQ 0x%x: %s\n", want, strerror(errno));
  }
}

// ¿La interrupcion pedida entraria ya (sin contar IF)?
static int irq_deliverable_now(void) {
  if (g_inflight < 0)
    return 0;
  if (!(g_inflight & (int)AVM_IRQ_TPR))
    return 1;
  return ((uint32_t)g_inflight & 0xF0) > (LREG(L_TPR) & 0xF0);
}

// Dispositivos con tiempo: genera lo que toque y devuelve el proximo instante.
static uint64_t devices_poll(uint64_t now) {
  uint64_t next = pit_poll(now), t;
  if (g_plat) {
    t = rtc_poll(now);
    if (t < next) next = t;
    t = pm_poll(now);
    if (t < next) next = t;
  }
  if (g_apic_on) {
    lapic_poll(now);
    if (g_lt_running && g_lt_deadline < next)
      next = g_lt_deadline;
  }
  vga_poll(now);
  if (g_vga_on && g_want_win) {
    uint64_t w = g_vga_t_ev + 8000000ULL;
    if (w < next) next = w;
  }
  return next;
}

static unsigned g_cur_timeout;
static void set_run_timeout(uint64_t now, uint64_t next) {
  uint64_t ms = next <= now ? 1 : (next - now + 999999ULL) / 1000000ULL;
  if (ms < 1) ms = 1;
  if (ms > 10) ms = 10;
  if (ms != g_cur_timeout) {
    ioctl(g_vm, AVM_SET_TIMEOUT, (unsigned long)ms);
    g_cur_timeout = (unsigned)ms;
  }
}

static void sleep_until(uint64_t t) {
  uint64_t n = now_ns();
  if (t <= n)
    return;
  uint64_t d = t - n;
  struct timespec ts = {(time_t)(d / 1000000000ULL), (long)(d % 1000000000ULL)};
  nanosleep(&ts, NULL);
}

// ---------------------------------------------------------------------------
// Diagnostico
// ---------------------------------------------------------------------------
// Lee memoria lineal del huesped (por bytes, cruzando paginas). Bytes leidos.
static int dbg_lin_read(const struct avm_regs *r, uint64_t lin, uint8_t *out, int n) {
  int k = 0;
  for (; k < n; k++) {
    uint64_t pa;
    if (gva_to_gpa(r, lin + (uint64_t)k, &pa) < 0 || gmem_read(pa, &out[k], 1) < 0)
      break;
  }
  return k;
}

// Para diagnosticar un huesped parado (p. ej. Windows en KeBugCheckEx): codigo
// antes de rip, la pila y las bases de segmento. Con esto se saca el codigo de
// la pantalla azul aunque no llegue a dibujarse.
static void dump_guest_mem(const struct avm_regs *r) {
  uint64_t rip_lin = r->cs.base + r->rip;
  uint8_t c[64];
  int n = dbg_lin_read(r, rip_lin - 48, c, 48);
  fprintf(stderr, "  codigo antes de rip (%d):", n);
  for (int i = 0; i < n; i++)
    fprintf(stderr, " %02x", c[i]);
  fprintf(stderr, "\n  fs=%llx gs=%llx tr=%x(base %llx) ss=%x(ar %x)\n",
          (unsigned long long)r->fs.base, (unsigned long long)r->gs.base, r->tr.selector,
          (unsigned long long)r->tr.base, r->ss.selector, r->ss.ar);
  int l64 = (r->efer & (1ULL << 10)) && (r->cs.ar & 0x2000);
  uint64_t sp = r->ss.base + (l64 ? r->rsp : (r->rsp & 0xFFFFFFFFULL));
  int w = l64 ? 8 : 4;
  static uint8_t st[0x2000];
  int got = dbg_lin_read(r, sp, st, (int)sizeof(st));
  fprintf(stderr, "  pila en %llx (%d bytes, sin las filas a cero):", (unsigned long long)sp, got);
  for (int i = 0; i + w * 4 <= got; i += w * 4) {
    static const uint8_t z[32];
    if (!memcmp(st + i, z, (size_t)w * 4))
      continue;
    fprintf(stderr, "\n    +%03x:", i);
    for (int k = 0; k < 4; k++) {
      uint64_t v = 0;
      memcpy(&v, st + i + k * w, (size_t)w);
      fprintf(stderr, w == 8 ? " %016llx" : " %08llx", (unsigned long long)v);
    }
  }
  fprintf(stderr, "\n");
  if (!l64)
    return;
  // Pantalla azul de Windows x64: KeBugCheckEx guarda un CONTEXT (ContextFlags
  // 0x10000F en +0x30) con el codigo en RCX y los parametros en RDX, R8 y R9.
  for (int i = 0; i + 0x100 <= got; i += 8) {
    uint32_t cf;
    memcpy(&cf, st + i + 0x30, 4);
    if (cf != 0x0010000F)
      continue;
    uint64_t rc, rd, r8v, r9v, rp;
    memcpy(&rc, st + i + 0x80, 8);
    memcpy(&rd, st + i + 0x88, 8);
    memcpy(&r8v, st + i + 0xB8, 8);
    memcpy(&r9v, st + i + 0xC0, 8);
    memcpy(&rp, st + i + 0xF8, 8);
    if (rc && rc < 0x400)
      fprintf(stderr, "  PANTALLA AZUL de Windows: 0x%llX (%llx, %llx, %llx) [rip del CONTEXT %llx]\n",
              (unsigned long long)rc, (unsigned long long)rd, (unsigned long long)r8v,
              (unsigned long long)r9v, (unsigned long long)rp);
    if (!(rc && rc < 0x400))
      continue;
    // Cuarto parametro (en la pila de la llamada) y, si es la direccion de una
    // instruccion (0xA, 0xD1, 0x50...), el KTRAP_FRAME de KiPageFault que la
    // contiene (Rip en +0x168): registros y la cadena de llamadas de esa funcion.
    uint64_t crsp, a4 = 0;
    memcpy(&crsp, st + i + 0x98, 8);
    if (dbg_lin_read(r, crsp + 0x28, (uint8_t *)&a4, 8) == 8)
      fprintf(stderr, "    parametro 4: %llx\n", (unsigned long long)a4);
    if ((a4 >> 47) != 0x1FFFF)
      continue;
    uint8_t cb[96];
    int m = dbg_lin_read(r, a4 - 64, cb, 96);
    fprintf(stderr, "    [%llx-40]:", (unsigned long long)a4);
    for (int k = 0; k < m; k++)
      fprintf(stderr, k == 64 ? " |%02x" : " %02x", cb[k]);
    fprintf(stderr, "\n");
    for (int j = i + 0x100; j + 8 <= got; j += 8) {
      uint64_t v;
      memcpy(&v, st + j, 8);
      if (v != a4 || j < 0x168)
        continue;
      const uint8_t *tf = st + j - 0x168;
      static const char *nm[] = {"rax", "rcx", "rdx", "r8", "r9", "r10", "r11"};
      fprintf(stderr, "    KTRAP_FRAME:");
      for (int k = 0; k < 7; k++) {
        uint64_t q;
        memcpy(&q, tf + 0x30 + 8 * k, 8);
        fprintf(stderr, " %s=%llx", nm[k], (unsigned long long)q);
      }
      uint64_t trsp, tfl;
      memcpy(&tfl, tf + 0x178, 8);
      memcpy(&trsp, tf + 0x180, 8);
      fprintf(stderr, " rflags=%llx rsp=%llx\n", (unsigned long long)tfl, (unsigned long long)trsp);
      uint64_t q[32];
      int qn = dbg_lin_read(r, trsp, (uint8_t *)q, (int)sizeof(q)) / 8, shown = 0;
      for (int k = 0; k < qn && shown < 8; k++) {
        if ((q[k] >> 47) != 0x1FFFF || (q[k] >= sp - 0x20000 && q[k] < sp + 0x20000))
          continue;
        shown++;
        int mm = dbg_lin_read(r, q[k] - 48, cb, 64);
        fprintf(stderr, "    llamada [rsp+%x] %llx:", k * 8, (unsigned long long)q[k]);
        for (int t = 0; t < mm; t++)
          fprintf(stderr, t == 48 ? " |%02x" : " %02x", cb[t]);
        fprintf(stderr, "\n");
      }
      break;
    }
  }
  // Codigo alrededor de las direcciones del nucleo que hay en la pila (retornos,
  // la direccion de la excepcion en un CONTEXT...): con eso se reconoce la funcion.
  uint64_t seen[24];
  int ns = 0;
  for (int i = 0; i + 8 <= got && ns < 24; i += 8) {
    uint64_t v;
    memcpy(&v, st + i, 8);
    if ((v >> 47) != 0x1FFFF || (v >= sp - 0x20000 && v < sp + 0x20000))
      continue;
    int dup = 0;
    for (int k = 0; k < ns; k++)
      dup |= seen[k] == v;
    if (dup)
      continue;
    seen[ns++] = v;
    uint8_t cb[64];
    int m = dbg_lin_read(r, v - 32, cb, 64);
    if (m < 64)
      continue;
    fprintf(stderr, "  [%llx-20]:", (unsigned long long)v);
    for (int k = 0; k < 64; k++)
      fprintf(stderr, k == 32 ? " |%02x" : " %02x", cb[k]);
    fprintf(stderr, "\n");
  }
  // KUSER_SHARED_DATA (Windows x64): NtBuildNumber (+0x260) y KdDebuggerEnabled
  // (+0x2D4), para saber si el nucleo cree tener depurador.
  uint8_t ku[0x2D8];
  if (dbg_lin_read(r, 0xFFFFF78000000000ULL, ku, (int)sizeof(ku)) == (int)sizeof(ku)) {
    uint32_t build;
    memcpy(&build, ku + 0x260, 4);
    fprintf(stderr, "  windows: compilacion %u, KdDebuggerEnabled=%02x\n", build & 0xFFFF,
            ku[0x2D4]);
  }
}

static void dump_regs(const char *why) {
  struct avm_regs r;
  if (ioctl(g_vm, AVM_GET_REGS, &r) < 0)
    return;
  fprintf(stderr,
          "aurora-vm: %s\n"
          "  rip=%llx rsp=%llx rflags=%llx cs=%x(base %llx ar %x) ss=%x\n"
          "  rax=%llx rbx=%llx rcx=%llx rdx=%llx rsi=%llx rdi=%llx rbp=%llx\n"
          "  cr0=%llx cr2=%llx cr3=%llx cr4=%llx efer=%llx gdt=%llx/%x idt=%llx/%x tpr=%x\n",
          why, (unsigned long long)r.rip, (unsigned long long)r.rsp,
          (unsigned long long)r.rflags, r.cs.selector, (unsigned long long)r.cs.base, r.cs.ar,
          r.ss.selector, (unsigned long long)r.rax, (unsigned long long)r.rbx,
          (unsigned long long)r.rcx, (unsigned long long)r.rdx, (unsigned long long)r.rsi,
          (unsigned long long)r.rdi, (unsigned long long)r.rbp, (unsigned long long)r.cr0,
          (unsigned long long)r.cr2, (unsigned long long)r.cr3, (unsigned long long)r.cr4,
          (unsigned long long)r.efer, (unsigned long long)r.gdt_base, r.gdt_limit,
          (unsigned long long)r.idt_base, r.idt_limit, LREG(L_TPR));
  uint8_t b[16];
  int n = insn_bytes(&r, b, 16);
  fprintf(stderr, "  bytes en rip:");
  for (int i = 0; i < n; i++)
    fprintf(stderr, " %02x", b[i]);
  fprintf(stderr, "\n");
  dump_guest_mem(&r);
}

static void dump_io(void) {
  unsigned n = g_iolog_n < IOLOG_N ? g_iolog_n : IOLOG_N;
  fprintf(stderr, "  ultimos accesos de E/S (%u en total):\n", g_iolog_n);
  for (unsigned i = 0; i < n; i++) {
    unsigned k = (g_iolog_n - n + i) % IOLOG_N;
    fprintf(stderr, "    %s 0x%04x/%u = 0x%x\n", g_iolog[k].w ? "OUT" : "IN ", g_iolog[k].port,
            g_iolog[k].size, g_iolog[k].v);
  }
  fprintf(stderr, "  PIC: irr %02x/%02x imr %02x/%02x isr %02x/%02x  IRQ pedida: %x\n",
          g_pic[0].irr, g_pic[1].irr, g_pic[0].imr, g_pic[1].imr, g_pic[0].isr, g_pic[1].isr,
          (unsigned)g_inflight);
  if (g_dbgbuf_n) {
    size_t from = g_dbgbuf_n > 5000 ? g_dbgbuf_n - 5000 : 0;
    fprintf(stderr, "--- log de SeaBIOS (final) ---\n%.*s\n", (int)(g_dbgbuf_n - from), g_dbgbuf + from);
  }
}

// ---------------------------------------------------------------------------
// Encendido / reinicio de la plataforma
// ---------------------------------------------------------------------------
static struct avm_regs g_reset_regs; // CPU recien encendida (modo real, F000:FFF0)
static int g_boot_cd;

static void bios_load_copies(void) {
  size_t low = g_bios_n < BIOS_SIZE ? g_bios_n : BIOS_SIZE;
  memcpy(g_ram + 0x100000 - low, g_bios_img + (g_bios_n - low), low);
  memcpy(g_ram + g_ram_size, g_bios_img, g_bios_n);
}

static void platform_reset_devices(void) {
  g_poweroff = 0;
  memset(&g_pm, 0, sizeof(g_pm));
  g_port92 = 0x02;
  g_reset_req = 0;
  pic_reset();
  pit_reset();
  cmos_init(g_ram_size, g_boot_cd);
  i8042_reset();
  lapic_reset();
  ioapic_reset();
  pci_init();
  ide_reset_all();
  if (g_vga_on) {
    // LFB y ROM vuelven a estar sin mapear hasta que la BIOS programe los BAR
    struct avm_memory m = {.fd = -1, .slot = 2, .gpa = 0, .size = 0, .offset = 0};
    ioctl(g_vm, AVM_SET_MEMORY, &m);
    m.slot = 3;
    ioctl(g_vm, AVM_SET_MEMORY, &m);
    vga_reset();
  }
}

static int g_resets;
static uint64_t g_reset_t0;

// AURORA_VM_TRAZA=1: cada 2 s una linea con donde esta el huesped y que salidas
// produjo (para ver si un cuelgue es un bucle de E/S, un HLT esperando una IRQ...).
static int g_traza;
static void traza(uint64_t tnow, const struct avm_exit *x) {
  static unsigned long nr[16], nport[8];
  static uint16_t port[8];
  static uint64_t t0, mmio_last;
  if (x) {
    nr[x->reason & 15]++;
    if (x->reason == AVM_EXIT_IO) {
      int i;
      for (i = 0; i < 8 && nport[i] && port[i] != x->io.port; i++)
        ;
      if (i < 8) {
        port[i] = x->io.port;
        nport[i] += x->io.string ? x->io.count : 1;
      }
    } else if (x->reason == AVM_EXIT_MMIO) {
      mmio_last = x->mmio.gpa;
    }
    return;
  }
  if (!t0)
    t0 = tnow;
  if (tnow - t0 < 2000000000ULL)
    return;
  t0 = tnow;
  struct avm_regs r;
  if (ioctl(g_vm, AVM_GET_REGS, &r) < 0)
    return;
  fprintf(stderr, "[traza] cs=%x:%llx cr0=%llx cr3=%llx if=%d | io=%lu mmio=%lu(%llx) hlt=%lu sig=%lu tpr=%lu win=%lu | puertos:",
          r.cs.selector, (unsigned long long)r.rip, (unsigned long long)r.cr0, (unsigned long long)r.cr3,
          (int)((r.rflags >> 9) & 1), nr[AVM_EXIT_IO], nr[AVM_EXIT_MMIO], (unsigned long long)mmio_last,
          nr[AVM_EXIT_HLT], nr[AVM_EXIT_SIGNAL], nr[AVM_EXIT_TPR], nr[AVM_EXIT_INTR_WINDOW]);
  for (int i = 0; i < 8 && nport[i]; i++)
    fprintf(stderr, " %x:%lu", port[i], nport[i]);
  fprintf(stderr, " | atajos rtc=%llu eoi=%llu\n", (unsigned long long)g_fast_rtc_total,
          (unsigned long long)g_fast_eoi_total);
  // Huesped muerto en "jmp $" con IF=0 (Windows tras KeBugCheckEx, sin pantalla
  // azul): volcado una sola vez, sin esperar a que se cierre la ventana.
  static int parado_dump;
  if (!parado_dump && !(r.rflags & (1ULL << 9))) {
    uint8_t b[2];
    if (insn_bytes(&r, b, 2) == 2 && b[0] == 0xEB && b[1] == 0xFE) {
      parado_dump = 1;
      dump_regs("huesped parado en jmp $ con interrupciones cortadas");
    }
  }
  // Rendimiento: donde se va el tiempo (kernel vs VMM, preparar ventana vs huesped)
  static struct avm_stats ps;
  static uint64_t ptsc, pns;
  struct avm_stats st;
  uint32_t lo, hi;
  __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
  uint64_t tsc = ((uint64_t)hi << 32) | lo;
  if (ioctl(g_vm, AVM_GET_STATS, &st) == 0) {
    if (pns && tsc > ptsc) {
      double hz = (double)(tsc - ptsc) * 1e9 / (double)(tnow - pns); // ciclos por segundo
      double us = 1e6 / hz;
      uint64_t runs = st.runs - ps.runs, win = st.windows - ps.windows, ex = st.exits - ps.exits;
      double trun = (double)(st.tsc_run - ps.tsc_run), tall = (double)(tsc - ptsc);
      fprintf(stderr, "[rend] RUN=%llu (%.0f us c/u) ventanas=%llu (preparar %.0f us, salir %.0f us) "
                      "salidas=%llu ext=%llu ept=%llu | huesped %.0f%% kernel %.0f%% VMM %.0f%%\n",
              (unsigned long long)runs, runs ? trun * us / (double)runs : 0.0, (unsigned long long)win,
              win ? (double)(st.tsc_setup - ps.tsc_setup) * us / (double)win : 0.0,
              win ? (double)(st.tsc_leave - ps.tsc_leave) * us / (double)win : 0.0, (unsigned long long)ex,
              (unsigned long long)(st.ext_exits - ps.ext_exits), (unsigned long long)(st.ept_maps - ps.ept_maps),
              100.0 * (double)(st.tsc_guest - ps.tsc_guest) / tall,
              100.0 * (trun - (double)(st.tsc_guest - ps.tsc_guest)) / tall, 100.0 * (tall - trun) / tall);
    }
    if (st.msr_unk != ps.msr_unk || st.msr_gp != ps.msr_gp) {
      fprintf(stderr, "[traza] MSR sin emular: %llu (", (unsigned long long)st.msr_unk);
      for (int k = 0; k < 8 && st.msr_unk_list[k]; k++)
        fprintf(stderr, "%s%s%x", k ? " " : "", (st.msr_unk_list[k] >> 31) ? "w" : "r",
                st.msr_unk_list[k] & 0x7FFFFFFFu);
      fprintf(stderr, ") | con #GP: %llu (ultimo %s%x)\n", (unsigned long long)st.msr_gp,
              (st.msr_gp_last >> 31) ? "w" : "r", st.msr_gp_last & 0x7FFFFFFFu);
    }
    if (st.bp_skipped != ps.bp_skipped) {
      // INT3 de anillo 0 que el kernel salto (Windows sin depurador): de donde
      // vienen, para reconocer la funcion.
      uint8_t cb[64];
      int m = dbg_lin_read(&r, st.bp_rip - 32, cb, 64);
      fprintf(stderr, "[traza] INT3 del huesped saltados: %llu, ultimo en %llx:",
              (unsigned long long)st.bp_skipped, (unsigned long long)st.bp_rip);
      for (int k = 0; k < m; k++)
        fprintf(stderr, k == 32 ? " |%02x" : " %02x", cb[k]);
      fprintf(stderr, "\n");
      // Miles por intervalo = Windows en KiBugCheckDebugBreak (pantalla azul sin
      // depurador): volcado completo una vez.
      static int bsod_dump;
      if (!bsod_dump && st.bp_skipped - ps.bp_skipped > 1000) {
        bsod_dump = 1;
        dump_regs("el huesped repite INT3 sin parar (pantalla azul de Windows?)");
      }
    }
    ps = st;
    ptsc = tsc;
    pns = tnow;
  }
  memset(nr, 0, sizeof(nr));
  memset(nport, 0, sizeof(nport));
}

// Reinicio del huesped (0xCF9, 0x64 FE, 0x92, triple fallo): todo como al encender
// (la BIOS se recarga entera; SeaBIOS hace un arranque en frio).
static int guest_reset(const char *why) {
  uint64_t t = now_ns();
  if (t - g_reset_t0 > 30000000000ULL) {
    g_reset_t0 = t;
    g_resets = 0;
  }
  if (++g_resets > 5) {
    fprintf(stderr, "aurora-vm: el huesped se reinicia en bucle (%s); se para\n", why);
    return -1;
  }
  fprintf(stderr, "aurora-vm: reinicio del huesped (%s)\n", why);
  // Windows se reinicia solo tras una pantalla azul (sin depurador): el volcado
  // busca en la pila el CONTEXT de KeBugCheckEx y dice el codigo.
  static int dumps;
  if (g_plat && dumps < 3) {
    dumps++;
    dump_regs("estado antes del reinicio");
  }
  irq_withdraw();
  bios_load_copies();
  platform_reset_devices();
  g_dbgbuf_n = 0;
  if (ioctl(g_vm, AVM_SET_REGS, &g_reset_regs) < 0) {
    perror("AVM_SET_REGS");
    return -1;
  }
  ioctl(g_vm, AVM_SET_TPR, 0UL);
  g_tpr_dirty = 0;
  return 0;
}

// ---------------------------------------------------------------------------
// Bucle principal
// ---------------------------------------------------------------------------
// Atajos en el kernel (struct avm_fast): el reloj de Windows (IRQ8 del RTC a
// 1024 Hz) lee el registro C y manda EOI en cada tick; en KVM anidado cada
// vuelta al VMM cuesta ~170 us, asi que eso se resuelve dentro de AVM_RUN y aqui
// solo se sincroniza el estado antes y despues. Con el backend de pruebas
// (Unicorn) los campos vuelven tal cual y no cambia nada.
static uint64_t ns_to_tsc(uint64_t ns) {
  if (ns <= g_clk_ns0)
    return g_clk_tsc0;
  return g_clk_tsc0 + (uint64_t)((__uint128_t)(ns - g_clk_ns0) * g_clk_hz / 1000000000ULL);
}
static uint64_t tsc_to_ns(uint64_t t) {
  if (t <= g_clk_tsc0)
    return g_clk_ns0;
  return g_clk_ns0 + (uint64_t)(((__uint128_t)(t - g_clk_tsc0) * g_clk_mult) >> 32);
}
static int g_fast_off = -1;
static void fast_before_run(struct avm_exit *x) {
  if (g_fast_off < 0) {
    const char *e = getenv("AURORA_VM_SIN_ATAJOS");
    g_fast_off = e && *e == '1';
  }
  struct avm_fast *f = &x->fast;
  memset(f, 0, sizeof(*f));
  if (g_fast_off)
    return;
  if (g_plat && g_clk_mult && g_clk_hz) {
    f->flags |= AVM_FAST_RTC;
    f->rtc_index = g_rtc.index;
    f->rtc_c = g_rtc.cmos[REG_C];
    f->rtc_pie = (g_rtc.cmos[REG_B] & 0x40) != 0;
    if (g_rtc.period) {
      f->rtc_period_tsc = ns_to_tsc(g_clk_ns0 + rtc_ticks_to_ns(g_rtc.period)) - g_clk_tsc0;
      if (!f->rtc_period_tsc)
        f->rtc_period_tsc = 1;
      f->rtc_next_tsc = ns_to_tsc(g_rtc.next_periodic);
    }
  }
  // EOI: solo si se sabe a que vector ira (el inyectado, una vez entregado, o
  // el mas alto del ISR) y ese vector es de flanco (sin EOI al IOAPIC), y no hay
  // nada mas esperando en el IRR (tras el EOI tendria que entrar sin esperar).
  if (g_apic_on) {
    int v = -1;
    if (g_inflight >= 0) {
      if (g_inflight & (int)AVM_IRQ_TPR)
        v = g_inflight & 0xFF;
    } else {
      v = bm_hi(g_isr);
    }
    int otra = 0;
    for (int i = 0; i < 8; i++) {
      uint32_t w = g_irr[i];
      if (v >= 0 && (v >> 5) == i)
        w &= ~(1u << (v & 31));
      if (w)
        otra = 1;
    }
    if (v >= 0 && !otra && !((g_tmr[v >> 5] >> (v & 31)) & 1) && !pic_output())
      f->flags |= AVM_FAST_EOI;
  }
}
static void fast_after_exit(struct avm_exit *x) {
  struct avm_fast *f = &x->fast;
  if (!f->flags)
    return;
  if (f->rtc_dirty & 1)
    g_rtc.index = f->rtc_index & 0x7f;
  if (f->rtc_dirty & 2) {
    g_fast_rtc_total++;
    g_rtc.cmos[REG_C] = f->rtc_c;
    if (g_rtc.period && f->rtc_next_tsc) {
      uint64_t nn = tsc_to_ns(f->rtc_next_tsc);
      if (nn > g_rtc.next_periodic)
        g_rtc.next_periodic = nn;
    }
    rtc_irq_update();
  }
}
// Despues de irq_after_exit (que pasa al ISR la interrupcion entregada).
static void fast_eoi_apply(const struct avm_exit *x) {
  for (uint32_t i = 0; i < x->fast.eoi_count; i++)
    lapic_write(0xB0, 0);
  g_fast_eoi_total += x->fast.eoi_count;
}

static int run_loop(uint64_t deadline_ns) {
  ioctl(g_vm, AVM_SET_DEBUG, (unsigned long)(g_debug ? 1 : 0));
  g_cur_timeout = 0;
  g_inflight = -1;
  struct avm_exit x;
  memset(&x, 0, sizeof(x));
  for (unsigned long n = 0;; n++) {
    uint64_t tnow = now_ns();
    if (deadline_ns && tnow > deadline_ns) {
      fprintf(stderr, "aurora-vm: tiempo agotado\n");
      dump_regs("estado al agotarse el tiempo");
      dump_io();
      return 5;
    }
    if (g_reset_req) {
      g_reset_req = 0;
      if (!g_plat) {
        fprintf(stderr, "aurora-vm: el huesped pidio reiniciar\n");
        return 0;
      }
      if (guest_reset("peticion del huesped") < 0)
        return 2;
      memset(&x, 0, sizeof(x));
      continue;
    }
    if (g_poweroff) {
      fprintf(stderr, "aurora-vm: el huesped se apago (ACPI)\n");
      return 0;
    }
    if (g_vw_closed) {
      fprintf(stderr, "aurora-vm: ventana cerrada\n");
      if (g_plat) { // si el huesped estaba colgado, esto dice donde
        dump_regs("estado al cerrar la ventana");
        dump_io();
      }
      return 0;
    }
    if (g_traza)
      traza(tnow, NULL);
    uint64_t next = devices_poll(tnow);
    irq_before_run();
    set_run_timeout(tnow, next);
    fast_before_run(&x);
    if (ioctl(g_vm, AVM_RUN, &x) < 0) {
      perror("AVM_RUN");
      return 1;
    }
    fast_after_exit(&x);
    irq_after_exit(&x);
    fast_eoi_apply(&x);
    if (g_traza)
      traza(0, &x);
    switch (x.reason) {
    case AVM_EXIT_IO:
      if (x.io.string) {
        if (string_io(&x) < 0) {
          dump_regs("fallo en INS/OUTS");
          return 2;
        }
      } else if (x.io.is_write) {
        io_out(x.io.port, x.io.data, x.io.size);
      } else {
        x.io.data = io_in(x.io.port, x.io.size);
      }
      if (g_exit_code >= 0)
        return g_exit_code;
      break;
    case AVM_EXIT_MMIO: {
      struct avm_regs r;
      if (get_regs_lite(&r) < 0) {
        perror("AVM_GET_REGS");
        return 1;
      }
      if (insn_emulate(&r, x.mmio.gpa) < 0) {
        char why[160];
        snprintf(why, sizeof(why), "acceso a memoria de dispositivo sin emular en 0x%llx: %s",
                 (unsigned long long)x.mmio.gpa, g_insn_err ? g_insn_err : "?");
        dump_regs(why);
        dump_io();
        return 2;
      }
      if (set_regs_lite(&r) < 0) {
        perror("AVM_SET_REGS");
        return 1;
      }
      break;
    }
    case AVM_EXIT_HLT: {
      struct avm_regs r;
      if (get_regs_lite(&r) < 0)
        return 1;
      if (!(r.rflags & (1u << 9))) { // HLT con IF=0: solo un NMI lo despertaria
        if (!g_plat)
          return 0;
        dump_regs("el huesped se detuvo (HLT con interrupciones desactivadas)");
        if (g_vga_on && g_want_win) { // dejar la pantalla (p. ej. pantalla azul) hasta cerrar
          while (!g_vw_closed) {
            vga_poll(now_ns());
            struct timespec ts = {0, 20000000};
            nanosleep(&ts, NULL);
          }
          return 0;
        }
        if (g_vga_on)
          vga_dump_text(stderr);
        return 3;
      }
      // Dormir hasta el proximo evento de un dispositivo, salvo que ya haya algo
      // que entregar.
      uint64_t t = now_ns();
      uint64_t nx = devices_poll(t);
      irq_before_run();
      if (irq_deliverable_now())
        break;
      if (!g_plat && nx == ~0ULL && g_inflight < 0)
        return 0; // nada puede despertarlo
      if (nx > t + 10000000ULL)
        nx = t + 10000000ULL;
      sleep_until(nx);
      break;
    }
    case AVM_EXIT_SIGNAL:
    case AVM_EXIT_TPR:
    case AVM_EXIT_INTR_WINDOW:
      break;
    case AVM_EXIT_SHUTDOWN:
      dump_regs("triple fallo del huesped");
      if (!g_plat)
        return 2;
      if (guest_reset("triple fallo") < 0) {
        dump_io();
        return 2;
      }
      memset(&x, 0, sizeof(x));
      break;
    default:
      fprintf(stderr,
              "aurora-vm: fallo de VMX (salida %u, motivo %u, codigo %llu, qual 0x%llx); "
              "mira dmesg: lineas [VMX]\n",
              x.reason, x.hw_reason, (unsigned long long)x.fail.code,
              (unsigned long long)x.fail.qual);
      dump_regs("estado del huesped");
      return 3;
    }
  }
}

// ---------------------------------------------------------------------------
// Creacion de la VM
// ---------------------------------------------------------------------------
static int open_vm(void) {
  int fd = open(DEV, O_RDWR | O_CLOEXEC);
  if (fd < 0) {
    fprintf(stderr, "aurora-vm: no puedo abrir %s: %s\n", DEV, strerror(errno));
    fprintf(stderr, "  (sin VT-x/EPT en esta CPU, o virtualizacion apagada en la BIOS)\n");
  }
  return fd;
}

static int cmd_caps(void) {
  int fd = open_vm();
  if (fd < 0)
    return 1;
  struct avm_caps c;
  if (ioctl(fd, AVM_GET_CAPS, &c) < 0) {
    perror("AVM_GET_CAPS");
    return 1;
  }
  printf("VMX: %s\nEPT: %s\nunrestricted guest: %s\nVPID: %s\nusable: %s\n"
         "revision VMCS: %u\nEPT/VPID cap: 0x%llx\n",
         c.flags & AVM_CAP_VMX ? "si" : "no", c.flags & AVM_CAP_EPT ? "si" : "no",
         c.flags & AVM_CAP_UNRESTRICTED ? "si" : "no",
         c.flags & AVM_CAP_VPID ? "si" : "no", c.flags & AVM_CAP_USABLE ? "SI" : "no",
         c.vmcs_revision, (unsigned long long)c.ept_vpid_cap);
  return (c.flags & AVM_CAP_USABLE) ? 0 : 1;
}

static int set_slot(uint32_t slot, uint64_t gpa, uint64_t size, uint64_t offset) {
  struct avm_memory m = {.fd = g_mfd, .slot = slot, .gpa = gpa, .size = size, .offset = offset};
  if (ioctl(g_vm, AVM_SET_MEMORY, &m) < 0) {
    fprintf(stderr, "aurora-vm: AVM_SET_MEMORY (slot %u, 0x%llx+0x%llx): %s\n", slot,
            (unsigned long long)gpa, (unsigned long long)size, strerror(errno));
    return -1;
  }
  return 0;
}

static void vm_close(void) {
  if (g_vw >= 0) {
    aw_sys(AW_WIN_DESTROY, (uint64_t)g_vw, 0, 0, 0, 0);
    g_vw = -1;
  }
  if (g_vm >= 0)
    close(g_vm);
  g_vm = -1;
  if (g_ram && g_ram != MAP_FAILED)
    munmap(g_ram, g_ram_size + (g_plat ? g_bios_n : 0));
  g_ram = NULL;
  if (g_mfd >= 0)
    close(g_mfd);
  g_mfd = -1;
}

static int run_flat(const uint8_t *code, size_t len, uint64_t at, unsigned mem_mb) {
  g_plat = 0;
  g_apic_on = 0;
  g_vga_on = 0;
  g_exit_port = 1;
  g_bios_n = 0;
  pic_reset();
  pit_reset();
  // Como una BIOS: 8259 en 0x08 / 0x70, todo enmascarado.
  static const uint8_t init[][2] = {{0x20, 0x11}, {0x21, 0x08}, {0x21, 0x04}, {0x21, 0x01},
                                    {0xA0, 0x11}, {0xA1, 0x70}, {0xA1, 0x02}, {0xA1, 0x01},
                                    {0xA1, 0xFF}, {0x21, 0xFF}};
  for (size_t i = 0; i < sizeof(init) / sizeof(init[0]); i++)
    pic_out(init[i][0], init[i][1]);
  memset(g_mmio_test, 0, sizeof(g_mmio_test));
  g_exit_code = -1;
  g_out_n = 0;
  g_vm = open_vm();
  if (g_vm < 0)
    return 1;
  g_ram_size = (uint64_t)mem_mb << 20;
  g_mfd = memfd_create("aurora-vm-ram", MFD_CLOEXEC);
  if (g_mfd < 0 || ftruncate(g_mfd, (off_t)g_ram_size) < 0) {
    perror("memfd");
    return 1;
  }
  g_ram = mmap(NULL, g_ram_size, PROT_READ | PROT_WRITE, MAP_SHARED, g_mfd, 0);
  if (g_ram == MAP_FAILED) {
    g_ram = NULL;
    perror("mmap");
    return 1;
  }
  if (at + len > g_ram_size) {
    fprintf(stderr, "aurora-vm: el binario no cabe en la RAM del huesped\n");
    return 1;
  }
  memcpy(g_ram + at, code, len);
  if (set_slot(0, 0, g_ram_size, 0) < 0)
    return 1;
  struct avm_regs r;
  if (ioctl(g_vm, AVM_GET_REGS, &r) < 0) {
    perror("AVM_GET_REGS");
    return 1;
  }
  struct avm_seg cs = {.base = 0, .limit = 0xFFFF, .selector = 0, .ar = 0x9B};
  struct avm_seg ds = {.base = 0, .limit = 0xFFFF, .selector = 0, .ar = 0x93};
  r.cs = cs;
  r.ds = r.es = r.fs = r.gs = r.ss = ds;
  r.rip = at;
  r.rsp = 0x8000;
  r.rflags = 2;
  if (ioctl(g_vm, AVM_SET_REGS, &r) < 0) {
    perror("AVM_SET_REGS");
    return 1;
  }
  return run_loop(now_ns() + 30000000000ULL);
}

static int autotest(void) {
  struct {
    const char *name;
    const unsigned char *code;
    size_t len;
    const char *expect;
  } t[] = {
      {"modo real + serie", guest_hola, sizeof(guest_hola), "Hola desde la VM\n"},
      {"modo largo + MMIO", guest_largo, sizeof(guest_largo), "Modo largo OK\nMMIO OK\n"},
      {"PIT + PIC + HLT + IRQ", guest_tick, sizeof(guest_tick), "Timer OK\n"},
  };
  int bad = 0;
  g_echo = 0;
  for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
    int rc = run_flat(t[i].code, t[i].len, 0x1000, 4);
    g_out[g_out_n] = 0;
    int ok = rc == 0 && strcmp(g_out, t[i].expect) == 0;
    printf("[%s] %s%s\n", ok ? "OK" : "FALLA", t[i].name, ok ? "" : " (salida distinta)");
    if (!ok) {
      printf("   rc=%d salida=\"%s\"\n", rc, g_out);
      bad++;
    }
    vm_close();
  }
  return bad ? 1 : 0;
}

// ---------------------------------------------------------------------------
// PC completo con SeaBIOS
// ---------------------------------------------------------------------------
#define DEFAULT_BIOS "/usr/share/aurora/vm/bios.bin"
#define DEFAULT_VGABIOS "/usr/share/aurora/vm/vgabios.bin"

struct bios_opts {
  const char *bios, *disk, *ide, *cd;
  unsigned mem_mb, timeout_s;
  int acpi, boot_cd;
};

static int open_virtio_disk(const char *path) {
  g_disk_ro = 0;
  g_disk_fd = open(path, O_RDWR | O_CLOEXEC);
  if (g_disk_fd < 0) {
    g_disk_ro = 1;
    g_disk_fd = open(path, O_RDONLY | O_CLOEXEC);
  }
  if (g_disk_fd < 0) {
    fprintf(stderr, "aurora-vm: no puedo abrir el disco %s: %s\n", path, strerror(errno));
    return -1;
  }
  struct stat st;
  uint64_t bytes = 0;
  if (fstat(g_disk_fd, &st) == 0) {
    bytes = (uint64_t)st.st_size;
    if (S_ISBLK(st.st_mode)) {
      off_t e = lseek(g_disk_fd, 0, SEEK_END);
      bytes = e > 0 ? (uint64_t)e : 0;
    }
  }
  if (bytes < 512) {
    fprintf(stderr, "aurora-vm: el disco %s esta vacio\n", path);
    return -1;
  }
  g_disk_sectors = bytes / 512;
  if (g_disk_ro)
    fprintf(stderr, "aurora-vm: aviso: disco abierto solo lectura\n");
  return 0;
}

static int run_bios(const struct bios_opts *o) {
  if (o->mem_mb < 16 || o->mem_mb > 3072) {
    fprintf(stderr, "aurora-vm: --mem debe estar entre 16 y 3072 MiB\n");
    return 1;
  }
  FILE *f = fopen(o->bios, "rb");
  if (!f) {
    fprintf(stderr, "aurora-vm: no encuentro la BIOS %s: %s\n", o->bios, strerror(errno));
    return 1;
  }
  size_t bn = fread(g_bios_img, 1, sizeof(g_bios_img), f);
  int more = fgetc(f) != EOF;
  fclose(f);
  if (more || bn < 64 * 1024 || (bn & 0xFFFF)) {
    fprintf(stderr, "aurora-vm: %s: tamano de BIOS no valido (se esperan 64..256 KiB, multiplo de 64 KiB)\n",
            o->bios);
    return 1;
  }
  g_bios_n = bn;
  g_bios_gpa = 0x100000000ULL - bn;

  // Discos
  g_disk_fd = -1;
  g_disk_sectors = 0;
  if (o->disk && open_virtio_disk(o->disk) < 0)
    return 1;
  memset(g_ide, 0, sizeof(g_ide));
  g_ide_on = 0;
  if (o->ide && ide_attach(0, 0, IDE_HD, o->ide) < 0)
    return 1;
  if (o->cd && ide_attach(1, 0, IDE_CD, o->cd) < 0)
    return 1;
  ide_init();
  g_boot_cd = o->boot_cd;

  g_acpi_on = g_apic_on = o->acpi;
  g_plat = 1;
  g_exit_code = -1;
  g_out_n = 0;
  g_dbgbuf_n = 0;
  g_iolog_n = 0;
  g_resets = 0;
  g_ram_size = (uint64_t)o->mem_mb << 20;

  g_vm = open_vm();
  if (g_vm < 0)
    return 1;
  g_mfd = memfd_create("aurora-vm-ram", MFD_CLOEXEC);
  if (g_mfd < 0 || ftruncate(g_mfd, (off_t)(g_ram_size + bn)) < 0) {
    perror("memfd");
    return 1;
  }
  g_ram = mmap(NULL, g_ram_size + bn, PROT_READ | PROT_WRITE, MAP_SHARED, g_mfd, 0);
  if (g_ram == MAP_FAILED) {
    g_ram = NULL;
    perror("mmap");
    return 1;
  }
  bios_load_copies();
  // RAM en dos trozos alrededor del hueco VGA (0xA0000-0xBFFFF, siempre MMIO) y la
  // BIOS en lo alto de los 4 GiB (vector de reinicio). SeaBIOS se copia a si misma
  // a 0xE0000-0xFFFFF, que aqui es RAM (y ya lleva la copia).
  if (set_slot(0, 0, VGA_HOLE_LO, 0) < 0 ||
      set_slot(4, VGA_HOLE_HI, g_ram_size - VGA_HOLE_HI, VGA_HOLE_HI) < 0 ||
      set_slot(1, g_bios_gpa, bn, g_ram_size) < 0)
    return 1;

  g_vga_on = 0;
  if (g_want_vga) {
    static uint8_t vrom[VGA_ROM_SIZE];
    size_t vn = 0;
    const char *vp = g_vga_rom_path ? g_vga_rom_path : DEFAULT_VGABIOS;
    FILE *vf = fopen(vp, "rb");
    if (vf) {
      vn = fread(vrom, 1, sizeof(vrom), vf);
      fclose(vf);
    }
    if (vn < 512) {
      fprintf(stderr, "aurora-vm: no encuentro la ROM de VGA %s (se arranca sin VGA)\n", vp);
    } else if (vga_setup(vrom, vn) < 0) {
      perror("aurora-vm: VGA");
      return 1;
    }
  }
  platform_reset_devices();
  if (g_want_win && g_vga_on)
    host_input_open();

  // CPU recien encendida
  if (ioctl(g_vm, AVM_GET_REGS, &g_reset_regs) < 0) {
    perror("AVM_GET_REGS");
    return 1;
  }
  struct avm_regs *r = &g_reset_regs;
  struct avm_seg ds = {.base = 0, .limit = 0xFFFF, .selector = 0, .ar = 0x93};
  struct avm_seg cs = {.base = 0xFFFF0000ULL, .limit = 0xFFFF, .selector = 0xF000, .ar = 0x9B};
  r->cs = cs;
  r->ds = r->es = r->fs = r->gs = r->ss = ds;
  r->rip = 0xFFF0;
  r->rsp = 0;
  r->rflags = 2;
  r->rax = r->rbx = r->rcx = r->rsi = r->rdi = r->rbp = 0;
  r->rdx = 0x600; // firma de la CPU (familia 6)
  r->r8 = r->r9 = r->r10 = r->r11 = r->r12 = r->r13 = r->r14 = r->r15 = 0;
  r->cr0 = 0x60000010;
  r->cr2 = r->cr3 = r->cr4 = r->efer = 0;
  r->gdt_base = r->idt_base = 0;
  r->gdt_limit = r->idt_limit = 0xFFFF;
  if (ioctl(g_vm, AVM_SET_REGS, r) < 0) {
    perror("AVM_SET_REGS");
    return 1;
  }
  if (o->acpi && ioctl(g_vm, AVM_SET_FLAGS, (unsigned long)AVM_VMF_APIC) < 0)
    perror("AVM_SET_FLAGS (kernel antiguo sin soporte de APIC?)");
  uint64_t deadline = o->timeout_s ? now_ns() + (uint64_t)o->timeout_s * 1000000000ULL : 0;
  return run_loop(deadline);
}

static int bios_test(int acpi, int vga) {
  char path[] = "/tmp/aurora-vm-f3-XXXXXX";
  int fd = mkstemp(path);
  if (fd < 0) {
    perror("mkstemp");
    return 1;
  }
  uint8_t img[4096];
  memset(img, 0, sizeof(img));
  memcpy(img, guest_mbr, sizeof(guest_mbr) < 512 ? sizeof(guest_mbr) : 512);
  memcpy(img + 512, "AURORA-F3", 9);
  if (write(fd, img, sizeof(img)) != (ssize_t)sizeof(img)) {
    perror("write");
    close(fd);
    unlink(path);
    return 1;
  }
  // SeaBIOS traduce la geometria en LBA (sectores / (16*63)): con menos de 1008
  // sectores saldrian 0 cilindros y rechazaria toda lectura CHS.
  if (ftruncate(fd, 1 << 20) < 0)
    perror("ftruncate");
  close(fd);
  g_echo = 0;
  g_want_vga = vga;
  g_want_win = 0;
  if (vga && getenv("AURORA_VM_VGABIOS"))
    g_vga_rom_path = getenv("AURORA_VM_VGABIOS");
  struct bios_opts o = {.bios = getenv("AURORA_VM_BIOS") ? getenv("AURORA_VM_BIOS") : DEFAULT_BIOS,
                        .disk = path, .mem_mb = 32, .timeout_s = 60, .acpi = acpi};
  g_exit_port = 1; // el sector de arranque de prueba termina por el puerto 0xF4
  int rc = run_bios(&o);
  vm_close();
  unlink(path);
  g_out[g_out_n] = 0;
  const char *expect = "MBR arrancado desde SeaBIOS\nDisco OK\n";
  int ok = rc == 0 && strcmp(g_out, expect) == 0;
  int vok = !vga || (g_vga_on && vga_text_contains("SeaBIOS"));
  printf("[%s] SeaBIOS + virtio-blk + sector de arranque%s%s\n", ok && vok ? "OK" : "FALLA",
         acpi ? " + ACPI/LAPIC/IOAPIC" : "", vga ? " + VGA (texto)" : "");
  if (!ok || !vok)
    printf("   rc=%d salida=\"%s\"\n", rc, g_out);
  if (vga && g_vga_on && (!vok || getenv("AURORA_VM_DUMP"))) {
    printf("--- pantalla de texto del huesped ---\n");
    vga_dump_text(stdout);
  }
  return ok && vok ? 0 : 1;
}

// Disco vacio (fichero disperso) para instalar un sistema.
static int create_disk(const char *path, const char *size) {
  char *end;
  unsigned long long n = strtoull(size, &end, 10);
  if (*end == 'G' || *end == 'g')
    n <<= 30;
  else if (*end == 'M' || *end == 'm')
    n <<= 20;
  else if (*end) {
    fprintf(stderr, "aurora-vm: tamano no valido: %s (p. ej. 20G o 512M)\n", size);
    return 2;
  }
  if (n < (1ULL << 20)) {
    fprintf(stderr, "aurora-vm: el disco debe tener al menos 1M\n");
    return 2;
  }
  int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
  if (fd < 0) {
    fprintf(stderr, "aurora-vm: no puedo crear %s: %s\n", path, strerror(errno));
    return 1;
  }
  if (ftruncate(fd, (off_t)(n & ~511ULL)) < 0) {
    fprintf(stderr, "aurora-vm: %s: %s\n", path, strerror(errno));
    close(fd);
    unlink(path);
    return 1;
  }
  close(fd);
  printf("%s: %llu MiB\n", path, n >> 20);
  return 0;
}

static void usage(void) {
  fprintf(stderr,
          "Uso: aurora-vm --caps | --autotest | --bios-test[-acpi|-vga] | --crear-disco IMG 20G |\n"
          "       --plano FICH [--en DIR] [--mem MiB] |\n"
          "       --bios [--ide IMG] [--cd ISO] [--disco IMG] [--mem MiB] [--arranque cd|disco]\n"
          "              [--rom FICH] [--vgarom FICH] [--tiempo S] [--log-bios] [--sin-acpi]\n"
          "              [--sin-vga] [--sin-ventana] [--depurar] | PROGRAMA.exe\n"
          "  Ejemplo (instalar Windows):\n"
          "    aurora-vm --crear-disco win.img 20G\n"
          "    aurora-vm --bios --ide win.img --cd windows.iso --mem 2048\n");
}

int main(int argc, char **argv) {
  if (argc < 2) {
    usage();
    return 2;
  }
  if (!strcmp(argv[1], "--caps"))
    return cmd_caps();
  if (!strcmp(argv[1], "--autotest"))
    return autotest();
  if (!strcmp(argv[1], "--crear-disco") && argc == 4)
    return create_disk(argv[2], argv[3]);
  if (!strcmp(argv[1], "--bios-test") || !strcmp(argv[1], "--bios-test-acpi") ||
      !strcmp(argv[1], "--bios-test-vga")) {
    int acpi = strcmp(argv[1], "--bios-test") != 0, vga = !strcmp(argv[1], "--bios-test-vga");
    return bios_test(acpi, vga);
  }
  g_capture = getenv("AURORA_VM_CAPTURA");
  g_traza = getenv("AURORA_VM_TRAZA") != NULL;
  clock_init();
  if (!strcmp(argv[1], "--bios")) {
    struct bios_opts o = {.bios = getenv("AURORA_VM_BIOS") ? getenv("AURORA_VM_BIOS") : DEFAULT_BIOS,
                          .mem_mb = 1024, .acpi = 1, .boot_cd = -1};
    int vga = 1, win = 1;
    g_debug = 0; // interceptar #PF/#GP cuesta mucho con un Windows: solo si se pide
    for (int i = 2; i < argc; i++) {
      if (!strcmp(argv[i], "--depurar"))
        g_debug = 1;
      else if (!strcmp(argv[i], "--log-bios"))
        g_bios_log = 1;
      else if (!strcmp(argv[i], "--sin-acpi"))
        o.acpi = 0;
      else if (!strcmp(argv[i], "--sin-vga"))
        vga = 0;
      else if (!strcmp(argv[i], "--sin-ventana"))
        win = 0;
      else if (i + 1 < argc && !strcmp(argv[i], "--vgarom"))
        g_vga_rom_path = argv[++i];
      else if (i + 1 < argc && !strcmp(argv[i], "--rom"))
        o.bios = argv[++i];
      else if (i + 1 < argc && !strcmp(argv[i], "--disco"))
        o.disk = argv[++i];
      else if (i + 1 < argc && !strcmp(argv[i], "--ide"))
        o.ide = argv[++i];
      else if (i + 1 < argc && !strcmp(argv[i], "--cd"))
        o.cd = argv[++i];
      else if (i + 1 < argc && !strcmp(argv[i], "--mem"))
        o.mem_mb = (unsigned)strtoul(argv[++i], NULL, 0);
      else if (i + 1 < argc && !strcmp(argv[i], "--tiempo"))
        o.timeout_s = (unsigned)strtoul(argv[++i], NULL, 0);
      else if (i + 1 < argc && !strcmp(argv[i], "--arranque")) {
        const char *a = argv[++i];
        if (!strcmp(a, "cd"))
          o.boot_cd = 1;
        else if (!strcmp(a, "disco"))
          o.boot_cd = 0;
        else {
          fprintf(stderr, "aurora-vm: --arranque cd|disco\n");
          return 2;
        }
      } else {
        fprintf(stderr, "aurora-vm: opcion desconocida: %s\n", argv[i]);
        usage();
        return 2;
      }
    }
    if (o.boot_cd < 0)
      o.boot_cd = o.cd != NULL; // con CD se arranca de el (y si no arranca, del disco)
    g_want_vga = vga;
    g_want_win = vga && win;
    if (getenv("AURORA_VM_VGABIOS") && !g_vga_rom_path)
      g_vga_rom_path = getenv("AURORA_VM_VGABIOS");
    g_exit_port = 0;
    if (g_want_win && access("/dev/input/event0", R_OK) != 0)
      fprintf(stderr, "aurora-vm: aviso: sin acceso a /dev/input/event0 (grupo input): "
                      "el teclado solo enviara pulsaciones sueltas\n");
    int rc = run_bios(&o);
    if (g_vga_on && !g_want_win && rc != 0)
      vga_dump_text(stderr);
    vm_close();
    return rc;
  }
  if (!strcmp(argv[1], "--plano") && argc >= 3) {
    uint64_t at = 0x1000;
    unsigned mem = 16;
    for (int i = 3; i + 1 < argc; i += 2) {
      if (!strcmp(argv[i], "--en"))
        at = strtoull(argv[i + 1], NULL, 0);
      else if (!strcmp(argv[i], "--mem"))
        mem = (unsigned)strtoul(argv[i + 1], NULL, 0);
    }
    FILE *f = fopen(argv[2], "rb");
    if (!f) {
      perror(argv[2]);
      return 1;
    }
    uint8_t *buf = malloc(1 << 20);
    size_t n = buf ? fread(buf, 1, 1 << 20, f) : 0;
    fclose(f);
    if (!n) {
      fprintf(stderr, "aurora-vm: %s vacio o ilegible\n", argv[2]);
      return 1;
    }
    int rc = run_flat(buf, n, at, mem);
    vm_close();
    return rc;
  }
  fprintf(stderr,
          "aurora-vm: %s: para programas Windows hace falta un Windows instalado en una imagen.\n"
          "  Arranca uno con: aurora-vm --bios --ide win.img [--cd instalador.iso]\n",
          argv[1]);
  return 126;
}
