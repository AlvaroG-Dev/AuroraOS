// apic.h — LAPIC (un solo vCPU) e IOAPIC virtuales de la mini-VM (F3c).
//
// El LAPIC vive en 0xFEE00000 y el IOAPIC en 0xFEC00000; los accesos llegan al
// VMM como salidas MMIO (fallo EPT) y los resuelve emulate_mmio. Las
// interrupciones se entregan con AVM_INJECT_IRQ.
#ifndef AVM_APIC_H
#define AVM_APIC_H

#define LAPIC_BASE 0xFEE00000ULL
#define IOAPIC_BASE 0xFEC00000ULL
#define IOAPIC_PINS 24

static uint64_t now_ns(void);

static int g_apic_on;

// ---------------------------------------------------------------------------
// LAPIC
// ---------------------------------------------------------------------------
static uint32_t g_lreg[0x400 / 4]; // registros por indice (offset / 4)
static uint32_t g_irr[8], g_isr[8], g_tmr[8];
static int g_lt_running;
static uint64_t g_lt_start, g_lt_deadline;

#define LREG(off) g_lreg[(off) >> 2]
#define L_TPR 0x80
#define L_SVR 0xF0
#define L_LVT_TIMER 0x320
#define L_TMICT 0x380
#define L_TMCCT 0x390
#define L_TDCR 0x3E0

static void lapic_reset(void) {
  memset(g_lreg, 0, sizeof(g_lreg));
  memset(g_irr, 0, sizeof(g_irr));
  memset(g_isr, 0, sizeof(g_isr));
  memset(g_tmr, 0, sizeof(g_tmr));
  g_lt_running = 0;
  LREG(0x30) = 0x00050014; // version 0x14, 6 LVT
  LREG(0xE0) = 0xFFFFFFFFu;
  LREG(L_SVR) = 0xFF;      // desactivado por software
  for (unsigned o = 0x2F0; o <= 0x370; o += 0x10)
    LREG(o) = 0x10000;     // LVT enmascaradas
}

static int bm_hi(const uint32_t *bm) {
  for (int i = 7; i >= 0; i--)
    if (bm[i])
      return i * 32 + 31 - __builtin_clz(bm[i]);
  return -1;
}
static void bm_set(uint32_t *bm, int v) { bm[v >> 5] |= 1u << (v & 31); }
static void bm_clr(uint32_t *bm, int v) { bm[v >> 5] &= ~(1u << (v & 31)); }

static void lapic_set_irr(int vec, int level) {
  if (vec < 16 || vec > 255)
    return;
  bm_set(g_irr, vec);
  if (level)
    bm_set(g_tmr, vec);
  else
    bm_clr(g_tmr, vec);
}

static uint32_t lapic_ppr(void) {
  int is = bm_hi(g_isr);
  uint32_t a = LREG(L_TPR) & 0xF0, b = is < 0 ? 0 : (uint32_t)is & 0xF0;
  return a > b ? a : b;
}

// Vector entregable ahora (o -1).
static int lapic_deliverable(void) {
  if (!(LREG(L_SVR) & 0x100))
    return -1;
  int ir = bm_hi(g_irr);
  if (ir < 0)
    return -1;
  return ((uint32_t)ir & 0xF0) > lapic_ppr() ? ir : -1;
}

static void lapic_accept(int vec) {
  bm_clr(g_irr, vec);
  bm_set(g_isr, vec);
}

static void ioapic_eoi(int vec);

static uint64_t lt_tick_ns(void) {
  uint32_t r = LREG(L_TDCR);
  unsigned v = (r & 3) | ((r >> 1) & 4);
  return v == 7 ? 1 : (uint64_t)2 << v; // 1 GHz / divisor
}

static void lapic_poll(uint64_t now) {
  if (!g_lt_running || now < g_lt_deadline)
    return;
  uint32_t lvt = LREG(L_LVT_TIMER);
  if (!(lvt & 0x10000))
    lapic_set_irr((int)(lvt & 0xFF), 0);
  if ((lvt >> 17) & 1) { // periodico
    uint64_t period = (uint64_t)LREG(L_TMICT) * lt_tick_ns();
    if (!period)
      period = 1;
    g_lt_deadline += period * (1 + (now - g_lt_deadline) / period);
  } else {
    g_lt_running = 0;
  }
}

static uint32_t lapic_read(uint32_t off) {
  off &= 0xFFC;
  if (off >= 0x100 && off < 0x180) return g_isr[(off - 0x100) >> 4];
  if (off >= 0x180 && off < 0x200) return g_tmr[(off - 0x180) >> 4];
  if (off >= 0x200 && off < 0x280) return g_irr[(off - 0x200) >> 4];
  switch (off) {
  case 0x90: return lapic_ppr();
  case L_TMCCT: {
    if (!g_lt_running)
      return 0;
    uint64_t tick = lt_tick_ns(), el = (now_ns() - (g_lt_deadline - (uint64_t)LREG(L_TMICT) * tick)) / tick;
    uint32_t init = LREG(L_TMICT);
    if (!init)
      return 0;
    if ((LREG(L_LVT_TIMER) >> 17) & 1)
      return init - (uint32_t)(el % init);
    return el >= init ? 0 : init - (uint32_t)el;
  }
  default:
    return off < 0x400 ? LREG(off) : 0;
  }
}

static void lapic_write(uint32_t off, uint32_t v) {
  off &= 0xFFC;
  switch (off) {
  case 0x20: case 0xD0: case 0xE0: case 0x310: case 0x2F0: case 0x330: case 0x340:
  case 0x350: case 0x360: case 0x370:
    LREG(off) = v;
    break;
  case L_TPR: LREG(off) = v & 0xFF; break;
  case L_SVR: LREG(off) = v & 0x3FF; break;
  case 0xB0: { // EOI
    int is = bm_hi(g_isr);
    if (is >= 0) {
      int lvl = (g_tmr[is >> 5] >> (is & 31)) & 1;
      bm_clr(g_isr, is);
      if (lvl)
        ioapic_eoi(is);
    }
    break;
  }
  case 0x280: LREG(off) = 0; break;
  case 0x300: { // ICR: sin otros procesadores; solo IPI propias
    LREG(off) = v & ~0x1000u;
    unsigned sh = (v >> 18) & 3, mode = (v >> 8) & 7;
    if ((sh == 1 || sh == 2) && mode == 0)
      lapic_set_irr((int)(v & 0xFF), 0);
    break;
  }
  case L_LVT_TIMER: LREG(off) = v; break;
  case L_TMICT:
    LREG(off) = v;
    if (v) {
      g_lt_running = 1;
      g_lt_start = now_ns();
      g_lt_deadline = g_lt_start + (uint64_t)v * lt_tick_ns();
    } else {
      g_lt_running = 0;
    }
    break;
  case L_TDCR: LREG(off) = v & 0xB; break;
  default: break; // registros de solo lectura
  }
}

// ---------------------------------------------------------------------------
// IOAPIC
// ---------------------------------------------------------------------------
static uint32_t g_io_sel, g_io_id;
static uint64_t g_io_red[IOAPIC_PINS];
static uint32_t g_io_level; // estado logico de cada linea

static void ioapic_reset(void) {
  g_io_sel = 0;
  g_io_id = 0;
  g_io_level = 0;
  for (int i = 0; i < IOAPIC_PINS; i++)
    g_io_red[i] = 0x10000; // enmascarada
}

static void ioapic_deliver(int pin) {
  uint64_t e = g_io_red[pin];
  unsigned mode = (unsigned)(e >> 8) & 7;
  if (mode != 0 && mode != 1) // solo fija / menor prioridad (un unico destino)
    return;
  int level = (int)((e >> 15) & 1);
  lapic_set_irr((int)(e & 0xFF), level);
  if (level)
    g_io_red[pin] |= 1ULL << 14; // IRR remoto
}

static void ioapic_line(int pin, int level) {
  if (pin < 0 || pin >= IOAPIC_PINS)
    return;
  int old = (g_io_level >> pin) & 1;
  if (level)
    g_io_level |= 1u << pin;
  else
    g_io_level &= ~(1u << pin);
  if (!level || old)
    return;
  uint64_t e = g_io_red[pin];
  if (e & 0x10000)
    return;
  if (!((e >> 15) & 1) || !((e >> 14) & 1))
    ioapic_deliver(pin);
}

static void ioapic_eoi(int vec) {
  for (int p = 0; p < IOAPIC_PINS; p++) {
    uint64_t e = g_io_red[p];
    if (((e >> 15) & 1) && ((e >> 14) & 1) && (int)(e & 0xFF) == vec) {
      g_io_red[p] &= ~(1ULL << 14);
      if (((g_io_level >> p) & 1) && !(e & 0x10000))
        ioapic_deliver(p);
    }
  }
}

static uint32_t ioapic_read(uint32_t off) {
  off &= 0xFF;
  if (off == 0x00)
    return g_io_sel;
  if (off != 0x10)
    return 0;
  uint32_t r = g_io_sel & 0xFF;
  if (r == 0) return g_io_id << 24;
  if (r == 1) return 0x00170011;
  if (r == 2) return g_io_id << 24;
  if (r >= 0x10 && r < 0x10 + 2 * IOAPIC_PINS) {
    uint64_t e = g_io_red[(r - 0x10) >> 1];
    return (r & 1) ? (uint32_t)(e >> 32) : (uint32_t)e;
  }
  return 0;
}

static void ioapic_write(uint32_t off, uint32_t v) {
  off &= 0xFF;
  if (off == 0x00) {
    g_io_sel = v & 0xFF;
    return;
  }
  if (off == 0x40) { // EOI
    ioapic_eoi((int)(v & 0xFF));
    return;
  }
  if (off != 0x10)
    return;
  uint32_t r = g_io_sel & 0xFF;
  if (r == 0) {
    g_io_id = (v >> 24) & 0xF;
  } else if (r >= 0x10 && r < 0x10 + 2 * IOAPIC_PINS) {
    int pin = (int)((r - 0x10) >> 1);
    uint64_t e = g_io_red[pin];
    if (r & 1) {
      e = (e & 0xFFFFFFFFULL) | ((uint64_t)v << 32);
    } else {
      uint64_t keep = e & ((1ULL << 14) | (1ULL << 12)); // IRR remoto / estado: solo lectura
      e = (e & ~0xFFFFFFFFULL) | (v & ~((1u << 14) | (1u << 12))) | keep;
      if (!((e >> 15) & 1))
        e &= ~(1ULL << 14); // en modo flanco no hay IRR remoto
    }
    g_io_red[pin] = e;
    if (!(r & 1) && !(e & 0x10000) && ((e >> 15) & 1) && !((e >> 14) & 1) &&
        ((g_io_level >> pin) & 1))
      ioapic_deliver(pin); // linea de nivel ya activa y recien desenmascarada
  }
}

static int apic_mmio_hit(uint64_t gpa) {
  return g_apic_on && ((gpa >> 12) == (LAPIC_BASE >> 12) || (gpa >> 12) == (IOAPIC_BASE >> 12));
}

static uint64_t apic_mmio_read(uint64_t gpa, int size) {
  uint32_t off = (uint32_t)(gpa & 0xFFF);
  uint32_t v = (gpa >> 12) == (LAPIC_BASE >> 12) ? lapic_read(off) : ioapic_read(off);
  v >>= 8 * (off & 3);
  return size == 1 ? v & 0xFF : size == 2 ? v & 0xFFFF : v;
}

static void apic_mmio_write(uint64_t gpa, int size, uint64_t val) {
  uint32_t off = (uint32_t)(gpa & 0xFFF);
  if (size != 4 || (off & 3)) // los registros se escriben de 32 en 32 bits
    return;
  if ((gpa >> 12) == (LAPIC_BASE >> 12))
    lapic_write(off, (uint32_t)val);
  else
    ioapic_write(off, (uint32_t)val);
}

#endif
