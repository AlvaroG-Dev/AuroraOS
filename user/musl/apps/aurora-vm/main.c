// aurora-vm — VMM de la mini-VM de AuroraOS (Plan_MiniVM.md).
//
// F1: ejecuta un binario plano en modo real sobre /dev/aurora-vm.
// F2: dispositivos (serie 16550, PIC 8259 x2, PIT 8254, CMOS, dispositivo MMIO
//     de prueba), decodificador MMIO x86 (MOV/MOVZX/MOVSX, 16/32/64 bits con
//     paginacion) e inyeccion de IRQ con temporizador. Los .exe necesitan un
//     huesped completo (F3 en adelante): por ahora se avisa.
//
// Uso:
//   aurora-vm --caps                 que ofrece la CPU (VMX, EPT, ...)
//   aurora-vm --autotest             tres guests incorporados (real, modo largo+MMIO, temporizador)
//   aurora-vm --plano FICH [--en DIR] [--mem MiB]
//   aurora-vm --bios-test-acpi       igual con ACPI + LAPIC/IOAPIC (F3c)
//   aurora-vm --bios-test            SeaBIOS + virtio-blk + sector de arranque incorporado (F3a)
//   aurora-vm --bios [--rom FICH] [--disco IMG] [--ide IMG] [--mem MiB] [--tiempo S] [--log-bios] [--sin-acpi]
//   aurora-vm PROGRAMA.exe           (aurora-run lo llama asi; aun sin huesped)
// Puertos: 0x3F8 serie -> stdout, 0xF4 termina con el codigo escrito.
#define _GNU_SOURCE
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
#include <time.h>
#include <unistd.h>

#define DEV "/dev/aurora-vm"
#define PIT_HZ 1193182ULL
#define MMIO_TEST_BASE 0xE0000000ULL

static int g_vm = -1;
static int g_mfd = -1;
static uint8_t *g_ram;
static uint64_t g_ram_size;
static char g_out[512];
static size_t g_out_n;
static int g_echo = 1;
static int g_debug = 1; // intercepta excepciones del huesped y las registra (dmesg [VMX])

#include "platform.h"
#include "apic.h"

static uint64_t now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

// ---------------------------------------------------------------------------
// PIC 8259 (maestro + esclavo)
// ---------------------------------------------------------------------------
struct pic {
  uint8_t irr, isr, imr, base, step, icw4, read_isr;
};
static struct pic g_pic[2];

static void pic_reset(void) {
  memset(g_pic, 0, sizeof(g_pic));
  g_pic[0].base = 0x08;
  g_pic[1].base = 0x70;
  g_pic[0].imr = g_pic[1].imr = 0xFF;
}

static int pic_pending(const struct pic *p) {
  for (int i = 0; i < 8; i++) {
    if (p->isr & (1u << i))
      break;
    if ((p->irr & ~p->imr) & (1u << i))
      return i;
  }
  return -1;
}

static void pic_set_irr(int irq) {
  if (irq < 8)
    g_pic[0].irr |= (uint8_t)(1u << irq);
  else
    g_pic[1].irr |= (uint8_t)(1u << (irq - 8));
}

// Linea ISA que se activa (nivel alto) y se mantiene hasta pic_lower.
static void pic_raise(int irq) {
  pic_set_irr(irq);
  if (g_apic_on)
    ioapic_line(irq, 1);
}

// Pulso (fuentes por flanco, como el PIT).
static void pic_pulse(int irq) {
  pic_set_irr(irq);
  if (g_apic_on) {
    ioapic_line(irq, 1);
    ioapic_line(irq, 0);
  }
}

static void pic_lower(int irq) {
  if (irq < 8)
    g_pic[0].irr &= (uint8_t)~(1u << irq);
  else
    g_pic[1].irr &= (uint8_t)~(1u << (irq - 8));
  if (g_apic_on)
    ioapic_line(irq, 0);
}

// Vector de la IRQ pendiente (sin aceptarla) o -1.
static int pic_peek(int *which, int *line) {
  if (pic_pending(&g_pic[1]) >= 0)
    g_pic[0].irr |= 4;
  else
    g_pic[0].irr &= (uint8_t)~4;
  int m = pic_pending(&g_pic[0]);
  if (m < 0)
    return -1;
  if (m == 2) {
    int s = pic_pending(&g_pic[1]);
    if (s < 0)
      return -1;
    *which = 1;
    *line = s;
    return g_pic[1].base + s;
  }
  *which = 0;
  *line = m;
  return g_pic[0].base + m;
}

static void pic_accept(int which, int line) {
  g_pic[which].irr &= (uint8_t)~(1u << line);
  g_pic[which].isr |= (uint8_t)(1u << line);
  if (which == 1) {
    g_pic[0].irr &= (uint8_t)~4;
    g_pic[0].isr |= 4;
  }
}

static void pic_write(int n, int port1, uint8_t v) {
  struct pic *p = &g_pic[n];
  if (!port1) {
    if (v & 0x10) { // ICW1
      p->imr = 0;
      p->isr = p->irr = 0;
      p->icw4 = v & 1;
      p->step = 1;
    } else if ((v & 0x18) == 0x08) { // OCW3
      if ((v & 3) == 3)
        p->read_isr = 1;
      else if ((v & 3) == 2)
        p->read_isr = 0;
    } else if (v & 0x20) { // EOI
      if ((v & 0xE0) == 0x60) {
        p->isr &= (uint8_t)~(1u << (v & 7));
      } else {
        for (int i = 0; i < 8; i++)
          if (p->isr & (1u << i)) {
            p->isr &= (uint8_t)~(1u << i);
            break;
          }
      }
    }
    return;
  }
  switch (p->step) {
  case 1: p->base = v & 0xF8; p->step = 2; break;
  case 2: p->step = p->icw4 ? 3 : 0; break;
  case 3: p->step = 0; break;
  default: p->imr = v; break;
  }
}

static uint8_t pic_read(int n, int port1) {
  struct pic *p = &g_pic[n];
  if (port1)
    return p->imr;
  return p->read_isr ? p->isr : p->irr;
}

// ---------------------------------------------------------------------------
// PIT 8254 (canal 0 con IRQ0; canal 2 y puerto 0x61 solo lo justo)
// ---------------------------------------------------------------------------
static struct {
  uint32_t reload;
  uint8_t access, mode, wr_hi, rd_hi, running;
  uint8_t lo_latch;
  uint64_t period_ns, next_ns, start_ns;
  uint8_t port61;
  // canal 2 (calibracion del TSC de SeaBIOS: modo 0, puerta por 0x61 bit 0)
  uint8_t c2_access, c2_mode, c2_hi, c2_lo, c2_armed;
  uint32_t c2_reload;
  uint64_t c2_start_ns;
} g_pit;

static void pit_reset(void) { memset(&g_pit, 0, sizeof(g_pit)); }

static void pit_write(uint16_t port, uint8_t v) {
  if (port == 0x43 && (v >> 6) == 2) {
    g_pit.c2_access = (v >> 4) & 3;
    g_pit.c2_mode = (v >> 1) & 7;
    g_pit.c2_hi = 0;
    g_pit.c2_armed = 0;
  } else if (port == 0x42) {
    if (g_pit.c2_access == 3 && !g_pit.c2_hi) {
      g_pit.c2_lo = v;
      g_pit.c2_hi = 1;
      return;
    }
    uint32_t cnt = g_pit.c2_access == 2 ? ((uint32_t)v << 8)
                   : g_pit.c2_access == 1 ? v
                                          : ((uint32_t)v << 8) | g_pit.c2_lo;
    g_pit.c2_hi = 0;
    g_pit.c2_reload = cnt ? cnt : 65536;
    g_pit.c2_start_ns = now_ns();
    g_pit.c2_armed = 1;
  } else if (port == 0x43) {
    if ((v >> 6) == 0) {
      g_pit.access = (v >> 4) & 3;
      g_pit.mode = (v >> 1) & 7;
      g_pit.wr_hi = g_pit.rd_hi = 0;
    }
  } else if (port == 0x40) {
    if (g_pit.access == 3 && !g_pit.wr_hi) {
      g_pit.lo_latch = v;
      g_pit.wr_hi = 1;
      return;
    }
    uint32_t cnt = g_pit.access == 2 ? ((uint32_t)v << 8)
                   : g_pit.access == 1 ? v
                                       : ((uint32_t)v << 8) | g_pit.lo_latch;
    g_pit.wr_hi = 0;
    g_pit.reload = cnt ? cnt : 65536;
    g_pit.period_ns = (uint64_t)g_pit.reload * 1000000000ULL / PIT_HZ;
    g_pit.start_ns = now_ns();
    g_pit.next_ns = g_pit.start_ns + g_pit.period_ns;
    g_pit.running = 1;
  } else if (port == 0x61) {
    g_pit.port61 = v;
  }
}

static uint8_t pit_read(uint16_t port) {
  if (port == 0x61)
  {
    uint8_t out2 = 0;
    if (g_pit.c2_armed && (g_pit.port61 & 1) &&
        (now_ns() - g_pit.c2_start_ns) * PIT_HZ / 1000000000ULL >= g_pit.c2_reload)
      out2 = 1;
    return (uint8_t)((g_pit.port61 & 0x0F) | (((now_ns() / 15000) & 1) << 4) | (out2 << 5));
  }
  if (port == 0x40 && g_pit.running) {
    uint64_t t = (now_ns() - g_pit.start_ns) * PIT_HZ / 1000000000ULL;
    uint32_t cnt = g_pit.reload - (uint32_t)(t % g_pit.reload);
    uint8_t b = g_pit.rd_hi ? (uint8_t)(cnt >> 8) : (uint8_t)cnt;
    g_pit.rd_hi ^= 1;
    return b;
  }
  return 0;
}

static void pit_poll(uint64_t now) {
  if (!g_pit.running || now < g_pit.next_ns)
    return;
  pic_pulse(0);
  g_pit.next_ns += g_pit.period_ns * (1 + (now - g_pit.next_ns) / g_pit.period_ns);
}

// ---------------------------------------------------------------------------
// E/S y MMIO
// ---------------------------------------------------------------------------
static uint8_t g_mmio_test[4096];
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
static struct { uint16_t port; uint8_t size, w; uint32_t v; } g_iolog[24];
static unsigned g_iolog_n;
static void iolog(uint16_t port, int size, int w, uint32_t v) {
  g_iolog[g_iolog_n % 24] = (typeof(g_iolog[0])){port, (uint8_t)size, (uint8_t)w, v};
  g_iolog_n++;
}

static uint32_t io_in(uint16_t port, int size) {
  uint32_t v = 0xFFFFFFFFu;
  if (plat_in(port, size, &v))
    ;
  else if (port == 0x3FD)
    v = 0x60;
  else if (port >= 0x3F8 && port <= 0x3FF)
    v = 0;
  else if (port == 0x20 || port == 0x21)
    v = pic_read(0, port & 1);
  else if (port == 0xA0 || port == 0xA1)
    v = pic_read(1, port & 1);
  else if (port == 0x40 || port == 0x42 || port == 0x61)
    v = pit_read(port);
  else if (port == 0x71)
    v = 0;
  if (size == 1)
    v &= 0xFF;
  else if (size == 2)
    v &= 0xFFFF;
  iolog(port, size, 0, v);
  return v;
}

static void io_out(uint16_t port, uint32_t v, int size) {
  if (port != 0x402 && port != 0xE9)
    iolog(port, size, 1, v);
  if (plat_out(port, v, size))
    return;
  if (port == 0x3F8)
    serial_out((uint8_t)v);
  else if (port == 0xF4)
    g_exit_code = (int)(v & 0xFF);
  else if (port == 0x20 || port == 0x21)
    pic_write(0, port & 1, (uint8_t)v);
  else if (port == 0xA0 || port == 0xA1)
    pic_write(1, port & 1, (uint8_t)v);
  else if (port == 0x40 || port == 0x42 || port == 0x43 || port == 0x61)
    pit_write(port, (uint8_t)v);
  else if (port == 0x70)
    g_cmos_idx = (uint8_t)v;
}

static int mmio_known(uint64_t gpa) {
  if (g_plat) // BIOS: lo que no es RAM ni LAPIC/IOAPIC se lee como 1s y se ignora
    return gpa >= g_ram_size;
  return gpa >= MMIO_TEST_BASE && gpa < MMIO_TEST_BASE + sizeof(g_mmio_test);
}
static uint64_t mmio_read(uint64_t gpa, int size) {
  if (apic_mmio_hit(gpa))
    return apic_mmio_read(gpa, size);
  if (g_plat)
    return size == 1 ? 0xFF : size == 2 ? 0xFFFF : size == 4 ? 0xFFFFFFFFULL : ~0ULL;
  uint64_t off = gpa - MMIO_TEST_BASE, v = 0;
  if (off == 0)
    return 0xA0A0A0A0ULL;
  memcpy(&v, g_mmio_test + off, (size_t)size);
  return v;
}
static void mmio_write(uint64_t gpa, int size, uint64_t v) {
  if (apic_mmio_hit(gpa)) {
    apic_mmio_write(gpa, size, v);
    return;
  }
  if (g_plat)
    return;
  uint64_t off = gpa - MMIO_TEST_BASE;
  if (off + (uint64_t)size <= sizeof(g_mmio_test) && off >= 4)
    memcpy(g_mmio_test + off, &v, (size_t)size);
}

// ---------------------------------------------------------------------------
// Memoria del huesped: lectura virtual (recorrido de tablas) para decodificar
// ---------------------------------------------------------------------------
static int gpa_read(uint64_t gpa, void *dst, size_t n) {
  if (gpa + n > g_ram_size)
    return -1;
  memcpy(dst, g_ram + gpa, n);
  return 0;
}

static int gva_to_gpa(const struct avm_regs *r, uint64_t gva, uint64_t *gpa) {
  if (!(r->cr0 & (1ULL << 31))) {
    *gpa = gva;
    return 0;
  }
  uint64_t e;
  if (r->efer & (1ULL << 10)) { // 4 niveles
    uint64_t t = r->cr3 & ~0xFFFULL;
    for (int lvl = 3; lvl >= 0; lvl--) {
      uint64_t idx = (gva >> (12 + 9 * lvl)) & 0x1FF;
      if (gpa_read(t + idx * 8, &e, 8) < 0 || !(e & 1))
        return -1;
      if (lvl > 0 && lvl < 3 && (e & 0x80)) {
        uint64_t sz = 1ULL << (12 + 9 * lvl);
        *gpa = (e & 0x000FFFFFFFFFF000ULL & ~(sz - 1)) | (gva & (sz - 1));
        return 0;
      }
      if (lvl == 0) {
        *gpa = (e & 0x000FFFFFFFFFF000ULL) | (gva & 0xFFF);
        return 0;
      }
      t = e & 0x000FFFFFFFFFF000ULL;
    }
    return -1;
  }
  if (r->cr4 & 0x20)
    return -1; // PAE de 32 bits: no soportado en F2
  uint32_t d;
  if (gpa_read((r->cr3 & ~0xFFFULL) + ((gva >> 22) & 0x3FF) * 4, &d, 4) < 0 || !(d & 1))
    return -1;
  if ((d & 0x80) && (r->cr4 & 0x10)) {
    *gpa = ((uint64_t)(d & 0xFFC00000u)) | (gva & 0x3FFFFF);
    return 0;
  }
  uint32_t t;
  if (gpa_read((d & ~0xFFFu) + ((gva >> 12) & 0x3FF) * 4, &t, 4) < 0 || !(t & 1))
    return -1;
  *gpa = (t & ~0xFFFu) | (gva & 0xFFF);
  return 0;
}

// ---------------------------------------------------------------------------
// Decodificador MMIO: MOV 88/89/8A/8B/C6/C7, MOVZX 0F B6/B7, MOVSX 0F BE/BF
// ---------------------------------------------------------------------------
static uint64_t *reg_ptr(struct avm_regs *r, int n) {
  uint64_t *tab[16] = {&r->rax, &r->rcx, &r->rdx, &r->rbx, &r->rsp, &r->rbp,
                       &r->rsi, &r->rdi, &r->r8,  &r->r9,  &r->r10, &r->r11,
                       &r->r12, &r->r13, &r->r14, &r->r15};
  return tab[n & 15];
}

static uint64_t reg_get(struct avm_regs *r, int n, int size, int rex) {
  if (size == 1 && !rex && n >= 4 && n < 8)
    return (*reg_ptr(r, n - 4) >> 8) & 0xFF;
  uint64_t v = *reg_ptr(r, n);
  return size == 1 ? v & 0xFF : size == 2 ? v & 0xFFFF : size == 4 ? v & 0xFFFFFFFFULL : v;
}
static void reg_set(struct avm_regs *r, int n, int size, int rex, uint64_t v) {
  if (size == 1 && !rex && n >= 4 && n < 8) {
    uint64_t *p = reg_ptr(r, n - 4);
    *p = (*p & ~0xFF00ULL) | ((v & 0xFF) << 8);
    return;
  }
  uint64_t *p = reg_ptr(r, n);
  switch (size) {
  case 1: *p = (*p & ~0xFFULL) | (v & 0xFF); break;
  case 2: *p = (*p & ~0xFFFFULL) | (v & 0xFFFF); break;
  case 4: *p = v & 0xFFFFFFFFULL; break; // 32 bits ponen a cero la mitad alta
  default: *p = v;
  }
}

// 0 = emulado (regs actualizados, rip avanzado); <0 = no se pudo
static int emulate_mmio(struct avm_regs *r, uint64_t gpa, int is_write) {
  uint64_t rip_lin = r->cs.base + r->rip, rip_gpa;
  uint8_t b[16];
  if (gva_to_gpa(r, rip_lin, &rip_gpa) < 0 || gpa_read(rip_gpa, b, sizeof(b)) < 0)
    return -1;
  bool lm = (r->efer & (1ULL << 10)) && (r->cs.ar & (1u << 13));
  bool d32 = (r->cs.ar & (1u << 14)) != 0;
  int opsz = lm || d32 ? 4 : 2;
  bool addr16 = !lm && !d32;
  int i = 0, rex = 0;
  bool p66 = false, p67 = false;
  for (;; i++) {
    if (b[i] == 0x66) p66 = true;
    else if (b[i] == 0x67) p67 = true;
    else if (b[i] == 0x2E || b[i] == 0x36 || b[i] == 0x3E || b[i] == 0x26 ||
             b[i] == 0x64 || b[i] == 0x65 || b[i] == 0xF3 || b[i] == 0xF2)
      ;
    else break;
  }
  if (p66) opsz = opsz == 4 ? 2 : 4;
  if (p67) addr16 = !addr16;
  if (lm && (b[i] & 0xF0) == 0x40) {
    rex = b[i++];
    if (rex & 8) opsz = 8;
  }
  uint8_t op = b[i++];
  if (op >= 0xA0 && op <= 0xA3) { // MOV AL/AX/EAX <-> [moffs]
    int sz = (op & 1) ? opsz : 1;
    int al = lm && !p67 ? 8 : addr16 ? 2 : 4;
    if (lm && p67) al = 4;
    i += al;
    if (is_write != (op >= 0xA2))
      return -1;
    if (is_write)
      mmio_write(gpa, sz, reg_get(r, 0, sz, rex));
    else
      reg_set(r, 0, sz, rex, mmio_read(gpa, sz));
    r->rip += (uint64_t)i;
    return 0;
  }
  int kind; // 0 mov rm<-reg, 1 mov reg<-rm, 2 mov rm<-imm, 3 movzx, 4 movsx
  int size = opsz, srcsz = 0;
  if (op == 0x88 || op == 0x89) { kind = 0; if (op == 0x88) size = 1; }
  else if (op == 0x8A || op == 0x8B) { kind = 1; if (op == 0x8A) size = 1; }
  else if (op == 0xC6 || op == 0xC7) { kind = 2; if (op == 0xC6) size = 1; }
  else if (op == 0x0F && (b[i] == 0xB6 || b[i] == 0xB7 || b[i] == 0xBE || b[i] == 0xBF)) {
    uint8_t o2 = b[i++];
    kind = (o2 < 0xB8) ? 3 : 4;
    srcsz = (o2 & 1) ? 2 : 1;
  } else {
    fprintf(stderr, "aurora-vm: instruccion MMIO no soportada: %02x %02x %02x %02x\n",
            b[0], b[1], b[2], b[3]);
    return -1;
  }
  uint8_t modrm = b[i++];
  int mod = modrm >> 6, reg = ((modrm >> 3) & 7) | ((rex & 4) ? 8 : 0), rm = modrm & 7;
  if (mod == 3)
    return -1;
  // longitud de SIB/desplazamiento
  if (addr16) {
    if (mod == 0 && rm == 6) i += 2;
    else if (mod == 1) i += 1;
    else if (mod == 2) i += 2;
  } else {
    if (rm == 4) {
      uint8_t sib = b[i++];
      if (mod == 0 && (sib & 7) == 5) i += 4;
    } else if (mod == 0 && rm == 5) {
      i += 4;
    }
    if (mod == 1) i += 1;
    else if (mod == 2) i += 4;
  }
  uint64_t val = 0;
  int msize = (kind == 3 || kind == 4) ? srcsz : size;
  if (kind == 2) {
    int isz = size == 1 ? 1 : size == 2 ? 2 : 4;
    memcpy(&val, b + i, (size_t)isz);
    if (size == 8 && (val & 0x80000000ULL)) val |= 0xFFFFFFFF00000000ULL;
    i += isz;
  }
  if (is_write != (kind == 0 || kind == 2)) // el tipo de acceso no encaja con la instruccion
    return -1;
  if (kind == 0) {
    val = reg_get(r, reg, size, rex);
    mmio_write(gpa, msize, val);
  } else if (kind == 2) {
    mmio_write(gpa, msize, val);
  } else {
    uint64_t v = mmio_read(gpa, msize);
    if (kind == 4) {
      if (msize == 1) v = (uint64_t)(int64_t)(int8_t)v;
      else v = (uint64_t)(int64_t)(int16_t)v;
    }
    reg_set(r, reg, size, rex, v);
  }
  r->rip += (uint64_t)i;
  return 0;
}

// ---------------------------------------------------------------------------
// Ejecucion
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

// INS/OUTS: el kernel solo describe la instruccion; aqui se mueven los datos
// y se ajustan RSI/RDI y RCX (RIP ya avanzo).
static int string_io(const struct avm_exit *x) {
  struct avm_regs r;
  if (ioctl(g_vm, AVM_GET_REGS, &r) < 0)
    return -1;
  int dir = (r.rflags & (1u << 10)) ? -1 : 1;
  uint32_t size = x->io.size, n = x->io.count, done = 0;
  uint64_t lin = x->io.linear;
  for (; done < n; done++) {
    uint64_t gpa;
    if (gva_to_gpa(&r, lin, &gpa) < 0 || gpa + size > g_ram_size)
      break;
    if (x->io.is_write) {
      uint32_t v = 0;
      memcpy(&v, g_ram + gpa, size);
      io_out(x->io.port, v, (int)size);
    } else {
      uint32_t v = io_in(x->io.port, (int)size);
      memcpy(g_ram + gpa, &v, size);
    }
    lin += (uint64_t)(int64_t)dir * size;
  }
  uint64_t mask = x->io.asize == 0 ? 0xFFFFULL : x->io.asize == 1 ? 0xFFFFFFFFULL : ~0ULL;
  uint64_t *reg = x->io.is_write ? &r.rsi : &r.rdi;
  int64_t delta = (int64_t)dir * (int64_t)size * (int64_t)done;
  *reg = x->io.asize == 0 ? ((*reg & ~mask) | ((uint64_t)((int64_t)*reg + delta) & mask))
                          : (uint64_t)((int64_t)*reg + delta) & mask;
  if (x->io.rep)
    r.rcx = x->io.asize == 0 ? ((r.rcx & ~mask) | ((r.rcx - done) & mask)) : (r.rcx - done) & mask;
  if (ioctl(g_vm, AVM_SET_REGS, &r) < 0)
    return -1;
  if (done < n) {
    fprintf(stderr, "aurora-vm: INS/OUTS fallo en la direccion 0x%llx (puerto 0x%x)\n",
            (unsigned long long)lin, x->io.port);
    return -1;
  }
  return 0;
}

static int run_loop(unsigned max_iter, uint64_t deadline_ns) {
  ioctl(g_vm, AVM_SET_TIMEOUT, 2);
  ioctl(g_vm, AVM_SET_DEBUG, g_debug);
  struct avm_exit x;
  memset(&x, 0, sizeof(x));
  for (unsigned n = 0; n < max_iter; n++) {
    if (deadline_ns && (n & 63) == 0 && now_ns() > deadline_ns) {
      fprintf(stderr, "aurora-vm: tiempo agotado\n");
      {
        struct avm_regs tr;
        if (ioctl(g_vm, AVM_GET_REGS, &tr) == 0)
          fprintf(stderr, "  cs=%x:%llx (base %llx) rflags=%llx cr0=%llx ax=%llx bx=%llx cx=%llx dx=%llx\n",
                  tr.cs.selector, (unsigned long long)tr.rip, (unsigned long long)tr.cs.base,
                  (unsigned long long)tr.rflags, (unsigned long long)tr.cr0,
                  (unsigned long long)tr.rax, (unsigned long long)tr.rbx,
                  (unsigned long long)tr.rcx, (unsigned long long)tr.rdx);
        fprintf(stderr, "  ultimos accesos de E/S (%u en total):\n", g_iolog_n);
        unsigned n = g_iolog_n < 24 ? g_iolog_n : 24;
        for (unsigned i = 0; i < n; i++) {
          unsigned k = (g_iolog_n - n + i) % 24;
          fprintf(stderr, "    %s 0x%04x/%u = 0x%x\n", g_iolog[k].w ? "OUT" : "IN ", g_iolog[k].port,
                  g_iolog[k].size, g_iolog[k].v);
        }
        if (g_dbgbuf_n) {
          size_t from = g_dbgbuf_n > 5000 ? g_dbgbuf_n - 5000 : 0;
          fprintf(stderr, "--- log de SeaBIOS (final) ---\n%.*s\n", (int)(g_dbgbuf_n - from),
                  g_dbgbuf + from);
        }
      }
      return 5;
    }
    if (g_reset_req) {
      fprintf(stderr, "aurora-vm: el huesped pidio reiniciar\n");
      return 0;
    }
    {
      uint64_t tnow = now_ns();
      pit_poll(tnow);
      if (g_apic_on)
        lapic_poll(tnow);
    }
    if (g_poweroff) {
      fprintf(stderr, "aurora-vm: el huesped se apago (ACPI)\n");
      return 0;
    }
    int which = 0, line = 0, vec = -1;
    int lv = g_apic_on ? lapic_deliverable() : -1;
    if (lv >= 0) {
      if (ioctl(g_vm, AVM_INJECT_IRQ, lv) == 0)
        lapic_accept(lv);
    } else {
      vec = pic_peek(&which, &line);
      if (vec >= 0 && ioctl(g_vm, AVM_INJECT_IRQ, vec) == 0)
        pic_accept(which, line);
    }
    if (ioctl(g_vm, AVM_RUN, &x) < 0) {
      perror("AVM_RUN");
      return 1;
    }
    switch (x.reason) {
    case AVM_EXIT_IO:
      if (x.io.string) {
        if (string_io(&x) < 0)
          return 2;
        if (g_exit_code >= 0)
          return g_exit_code;
        break;
      }
      if (x.io.is_write) {
        io_out(x.io.port, x.io.data, x.io.size);
        if (g_exit_code >= 0)
          return g_exit_code;
      } else {
        x.io.data = io_in(x.io.port, x.io.size);
      }
      break;
    case AVM_EXIT_HLT: {
      struct avm_regs hr;
      ioctl(g_vm, AVM_GET_REGS, &hr);
      if (!(hr.rflags & (1u << 9)))
        return 0; // HLT con IF=0: el huesped termino
      uint64_t wake = ~0ULL;
      if (g_pit.running)
        wake = g_pit.next_ns;
      if (g_apic_on && g_lt_running && g_lt_deadline < wake)
        wake = g_lt_deadline;
      if (wake == ~0ULL) {
        if (g_plat) { // BIOS: puede estar esperando una E/S del VMM; no terminar
          struct timespec ts = {0, 1000000};
          nanosleep(&ts, NULL);
          break;
        }
        return 0; // nada puede despertarlo
      }
      uint64_t t = now_ns();
      if (wake > t) {
        struct timespec ts = {0, (long)(wake - t)};
        nanosleep(&ts, NULL);
      }
      break;
    }
    case AVM_EXIT_SIGNAL:
      break;
    case AVM_EXIT_SHUTDOWN:
    {
      struct avm_regs sr;
      ioctl(g_vm, AVM_GET_REGS, &sr);
      fprintf(stderr,
              "aurora-vm: el huesped se apago (triple fallo)\n"
              "  rip=%llx rsp=%llx rflags=%llx cs=%x(base %llx ar %x) cr0=%llx cr3=%llx cr4=%llx efer=%llx\n"
              "  gdt=%llx/%x idt=%llx/%x\n",
              (unsigned long long)sr.rip, (unsigned long long)sr.rsp,
              (unsigned long long)sr.rflags, sr.cs.selector,
              (unsigned long long)sr.cs.base, sr.cs.ar, (unsigned long long)sr.cr0,
              (unsigned long long)sr.cr3, (unsigned long long)sr.cr4,
              (unsigned long long)sr.efer, (unsigned long long)sr.gdt_base,
              sr.gdt_limit, (unsigned long long)sr.idt_base, sr.idt_limit);
    }
      return 2;
    case AVM_EXIT_MMIO: {
      struct avm_regs mr;
      if (!mmio_known(x.mmio.gpa) || ioctl(g_vm, AVM_GET_REGS, &mr) < 0 ||
          emulate_mmio(&mr, x.mmio.gpa, x.mmio.is_write) < 0 ||
          ioctl(g_vm, AVM_SET_REGS, &mr) < 0) {
        fprintf(stderr, "aurora-vm: acceso MMIO sin resolver en 0x%llx\n",
                (unsigned long long)x.mmio.gpa);
        return 2;
      }
      break;
    }
    default:
      fprintf(stderr,
              "aurora-vm: fallo de VMX (salida %u, motivo %u, codigo %llu, qual 0x%llx); "
              "mira dmesg: lineas [VMX]\n",
              x.reason, x.hw_reason, (unsigned long long)x.fail.code,
              (unsigned long long)x.fail.qual);
      return 3;
    }
  }
  fprintf(stderr, "aurora-vm: demasiadas salidas sin terminar\n");
  return 4;
}

static int run_flat(const uint8_t *code, size_t len, uint64_t at, unsigned mem_mb) {
  pic_reset();
  pit_reset();
  memset(g_mmio_test, 0, sizeof(g_mmio_test));
  g_exit_code = -1;
  g_out_n = 0;
  g_vm = open_vm();
  if (g_vm < 0)
    return 1;
  g_ram_size = (uint64_t)mem_mb << 20;
  int mfd = g_mfd = memfd_create("aurora-vm-ram", MFD_CLOEXEC);
  if (mfd < 0 || ftruncate(mfd, (off_t)g_ram_size) < 0) {
    perror("memfd");
    return 1;
  }
  g_ram = mmap(NULL, g_ram_size, PROT_READ | PROT_WRITE, MAP_SHARED, mfd, 0);
  if (g_ram == MAP_FAILED) {
    perror("mmap");
    return 1;
  }
  if (at + len > g_ram_size) {
    fprintf(stderr, "aurora-vm: el binario no cabe en la RAM del huesped\n");
    return 1;
  }
  memcpy(g_ram + at, code, len);
  struct avm_memory m = {.fd = mfd, .slot = 0, .gpa = 0, .size = g_ram_size, .offset = 0};
  if (ioctl(g_vm, AVM_SET_MEMORY, &m) < 0) {
    perror("AVM_SET_MEMORY");
    return 1;
  }
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
  return run_loop(20000000, 0);
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
    close(g_vm);
    munmap(g_ram, g_ram_size);
    close(g_mfd);
  }
  return bad ? 1 : 0;
}

// ---------------------------------------------------------------------------
// F3a: arranque con SeaBIOS y un disco virtio-blk
// ---------------------------------------------------------------------------
#define DEFAULT_BIOS "/usr/share/aurora/vm/bios.bin"

static int run_bios(const char *bios_path, const char *disk_path, const char *ide_path,
                    unsigned mem_mb, unsigned timeout_s, int acpi) {
  if (mem_mb < 16 || mem_mb > 3072) {
    fprintf(stderr, "aurora-vm: --mem debe estar entre 16 y 3072 MiB\n");
    return 1;
  }
  FILE *f = fopen(bios_path, "rb");
  if (!f) {
    fprintf(stderr, "aurora-vm: no encuentro la BIOS %s: %s\n", bios_path, strerror(errno));
    return 1;
  }
  static uint8_t bios[256 * 1024];
  size_t bn = fread(bios, 1, sizeof(bios), f);
  fclose(f);
  if (bn < 64 * 1024 || bn > sizeof(bios) || (bn & 0xFFFF)) {
    fprintf(stderr, "aurora-vm: %s: tamano de BIOS no valido (%zu bytes; se esperan 64..256 KiB multiplo de 64 KiB)\n",
            bios_path, bn);
    return 1;
  }
  if (disk_path) {
    g_disk_ro = 0;
    g_disk_fd = open(disk_path, O_RDWR | O_CLOEXEC);
    if (g_disk_fd < 0) {
      g_disk_ro = 1;
      g_disk_fd = open(disk_path, O_RDONLY | O_CLOEXEC);
    }
    if (g_disk_fd < 0) {
      fprintf(stderr, "aurora-vm: no puedo abrir el disco %s: %s\n", disk_path, strerror(errno));
      return 1;
    }
    struct stat st;
    if (fstat(g_disk_fd, &st) < 0 || st.st_size < 512) {
      fprintf(stderr, "aurora-vm: el disco %s esta vacio\n", disk_path);
      return 1;
    }
    g_disk_sectors = (uint64_t)st.st_size / 512;
    if (g_disk_ro)
      fprintf(stderr, "aurora-vm: aviso: disco abierto solo lectura\n");
  }
  g_ide_on = 0;
  if (ide_path) {
    g_ide_ro = 0;
    g_ide_fd = open(ide_path, O_RDWR | O_CLOEXEC);
    if (g_ide_fd < 0) {
      g_ide_ro = 1;
      g_ide_fd = open(ide_path, O_RDONLY | O_CLOEXEC);
    }
    struct stat ist;
    if (g_ide_fd < 0 || fstat(g_ide_fd, &ist) < 0 || ist.st_size < 512) {
      fprintf(stderr, "aurora-vm: no puedo usar el disco IDE %s: %s\n", ide_path,
              g_ide_fd < 0 ? strerror(errno) : "vacio");
      return 1;
    }
    g_ide_sectors = (uint64_t)ist.st_size / 512;
    g_ide_on = 1;
    if (g_ide_ro)
      fprintf(stderr, "aurora-vm: aviso: disco IDE abierto solo lectura\n");
  }
  g_acpi_on = g_apic_on = acpi;
  g_poweroff = 0;
  memset(&g_pm, 0, sizeof(g_pm));
  lapic_reset();
  ioapic_reset();
  pic_reset();
  pit_reset();
  // El PIT cuenta solo desde el encendido (65536 ciclos): SeaBIOS lo lee como
  // temporizador antes de programarlo; si no avanza, sus esperas no terminan.
  g_pit.reload = 65536;
  g_pit.period_ns = 65536ULL * 1000000000ULL / PIT_HZ;
  g_pit.start_ns = now_ns();
  g_pit.next_ns = g_pit.start_ns + g_pit.period_ns;
  g_pit.running = 1;
  kbc_reset();
  pci_init();
  ide_init();
  g_exit_code = -1;
  g_reset_req = 0;
  g_out_n = 0;
  g_dbgbuf_n = 0;
  g_iolog_n = 0;
  g_ram_size = (uint64_t)mem_mb << 20;
  cmos_init(g_ram_size);
  g_plat = 1;
  g_vm = open_vm();
  if (g_vm < 0)
    return 1;
  int mfd = g_mfd = memfd_create("aurora-vm-ram", MFD_CLOEXEC);
  if (mfd < 0 || ftruncate(mfd, (off_t)(g_ram_size + bn)) < 0) {
    perror("memfd");
    return 1;
  }
  g_ram = mmap(NULL, g_ram_size + bn, PROT_READ | PROT_WRITE, MAP_SHARED, mfd, 0);
  if (g_ram == MAP_FAILED) {
    perror("mmap");
    return 1;
  }
  // La BIOS aparece arriba de los 4 GiB (vector de reinicio) y en 0xE0000-0xFFFFF
  // (SeaBIOS se copia a si misma a esa zona). Aqui ambas son RAM del huesped.
  size_t low = bn < BIOS_SIZE ? bn : BIOS_SIZE;
  memcpy(g_ram + 0x100000 - low, bios + (bn - low), low);
  memcpy(g_ram + g_ram_size, bios, bn);
  struct avm_memory m0 = {.fd = mfd, .slot = 0, .gpa = 0, .size = g_ram_size, .offset = 0};
  struct avm_memory m1 = {.fd = mfd, .slot = 1, .gpa = 0x100000000ULL - bn, .size = bn,
                          .offset = g_ram_size};
  if (ioctl(g_vm, AVM_SET_MEMORY, &m0) < 0 || ioctl(g_vm, AVM_SET_MEMORY, &m1) < 0) {
    perror("AVM_SET_MEMORY");
    return 1;
  }
  struct avm_regs r;
  if (ioctl(g_vm, AVM_GET_REGS, &r) < 0) {
    perror("AVM_GET_REGS");
    return 1;
  }
  struct avm_seg ds = {.base = 0, .limit = 0xFFFF, .selector = 0, .ar = 0x93};
  struct avm_seg cs = {.base = 0xFFFF0000ULL, .limit = 0xFFFF, .selector = 0xF000, .ar = 0x9B};
  r.cs = cs;
  r.ds = r.es = r.fs = r.gs = r.ss = ds;
  r.rip = 0xFFF0;
  r.rsp = 0;
  r.rflags = 2;
  if (ioctl(g_vm, AVM_SET_REGS, &r) < 0) {
    perror("AVM_SET_REGS");
    return 1;
  }
  if (acpi && ioctl(g_vm, AVM_SET_FLAGS, AVM_VMF_APIC) < 0)
    perror("AVM_SET_FLAGS (kernel antiguo sin soporte de APIC?)");
  uint64_t deadline = timeout_s ? now_ns() + (uint64_t)timeout_s * 1000000000ULL : 0;
  return run_loop(0xFFFFFFFFu, deadline);
}

static int bios_test(int acpi) {
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
  int rc = run_bios(getenv("AURORA_VM_BIOS") ? getenv("AURORA_VM_BIOS") : DEFAULT_BIOS, path, NULL, 32, 60, acpi);
  unlink(path);
  g_out[g_out_n] = 0;
  const char *expect = "MBR arrancado desde SeaBIOS\nDisco OK\n";
  int ok = rc == 0 && strcmp(g_out, expect) == 0;
  printf("[%s] SeaBIOS + virtio-blk + sector de arranque%s\n", ok ? "OK" : "FALLA", acpi ? " + ACPI/LAPIC/IOAPIC" : "");
  if (!ok)
    printf("   rc=%d salida=\"%s\"\n", rc, g_out);
  return ok ? 0 : 1;
}

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "Uso: aurora-vm --caps | --autotest | --bios-test | --plano FICH [--en DIR] [--mem MiB] |\n"
          "       --bios [--rom FICH] [--disco IMG] [--ide IMG] [--mem MiB] [--tiempo S] [--log-bios] [--sin-acpi] | PROGRAMA.exe\n");
    return 2;
  }
  if (!strcmp(argv[1], "--caps"))
    return cmd_caps();
  if (!strcmp(argv[1], "--autotest"))
    return autotest();
  if (!strcmp(argv[1], "--bios-test"))
    return bios_test(0);
  if (!strcmp(argv[1], "--bios-test-acpi"))
    return bios_test(1);
  if (!strcmp(argv[1], "--bios")) {
    const char *rom = getenv("AURORA_VM_BIOS") ? getenv("AURORA_VM_BIOS") : DEFAULT_BIOS;
    const char *disk = NULL, *ide = NULL;
    unsigned mem = 128, tmo = 0;
    int acpi = 1;
    for (int i = 2; i < argc; i++) {
      if (!strcmp(argv[i], "--log-bios"))
        g_bios_log = 1;
      else if (!strcmp(argv[i], "--sin-acpi"))
        acpi = 0;
      else if (i + 1 < argc && !strcmp(argv[i], "--rom"))
        rom = argv[++i];
      else if (i + 1 < argc && !strcmp(argv[i], "--disco"))
        disk = argv[++i];
      else if (i + 1 < argc && !strcmp(argv[i], "--ide"))
        ide = argv[++i];
      else if (i + 1 < argc && !strcmp(argv[i], "--mem"))
        mem = (unsigned)strtoul(argv[++i], NULL, 0);
      else if (i + 1 < argc && !strcmp(argv[i], "--tiempo"))
        tmo = (unsigned)strtoul(argv[++i], NULL, 0);
      else {
        fprintf(stderr, "aurora-vm: opcion desconocida: %s\n", argv[i]);
        return 2;
      }
    }
    return run_bios(rom, disk, ide, mem, tmo, acpi);
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
    return run_flat(buf, n, at, mem);
  }
  fprintf(stderr,
          "aurora-vm: %s: ejecutar programas Windows necesita un huesped completo "
          "(ReactOS/Windows en una imagen), que llega en las fases F3-F5 de "
          "Plan_MiniVM.md. Por ahora solo --caps, --autotest y --plano.\n",
          argv[1]);
  return 126;
}
