// pit.h — 8254 (0x40-0x43) y puerto 0x61 (puerta del canal 2 / altavoz) de la
// mini-VM, con la semantica de QEMU (hw/timer/i8254*.c, hw/audio/pcspk.c):
// 3 canales, modos 0-5, latch de cuenta y de estado (read-back), accesos LSB/MSB/
// palabra, OUT por tiempo real del host (CLOCK_MONOTONIC) a 1.193182 MHz.
//
// IRQ0: el VMM no tiene temporizadores precisos; pit_poll() se llama en cada vuelta
// del bucle y genera los flancos que tocan. En los modos periodicos (2 y 3) los
// ticks perdidos se recuperan (hasta PIT_MAX_BACKLOG): un flanco nuevo solo cuando el
// anterior ya se acepto, como la reinyeccion del PIT de KVM. Asi el reloj de un
// huesped que cuenta ticks (ReactOS, Windows con HAL PIC) no se atrasa.
#ifndef AVM_PIT_H
#define AVM_PIT_H

#define PIT_FREQ 1193182ULL
#define PIT_MAX_BACKLOG 50

static uint64_t now_ns(void);
static void irq_set(int irq, int level);
static int irq_edge_pending(int irq);

struct pit_ch {
  int count;            // 1..0x10000
  uint16_t latched_count;
  uint8_t count_latched, status_latched, status, read_state, write_state, write_latch;
  uint8_t rw_mode, mode, bcd, gate;
  uint64_t load_ns;     // count_load_time
};
static struct {
  struct pit_ch ch[3];
  uint8_t data_on, refresh;
  // IRQ0
  uint64_t emitted;     // periodos ya contados desde load_ns
  unsigned backlog;
  int oneshot_done;
  int level;
} g_pit;

#define RW_LSB 1
#define RW_MSB 2
#define RW_WORD0 3
#define RW_WORD1 4

static uint64_t pit_ticks(const struct pit_ch *s, uint64_t now) {
  if (now < s->load_ns)
    return 0;
  __uint128_t t = (__uint128_t)(now - s->load_ns) * PIT_FREQ;
  return (uint64_t)(t / 1000000000ULL);
}

static int pit_get_count(const struct pit_ch *s, uint64_t now) {
  uint64_t d = pit_ticks(s, now);
  switch (s->mode) {
  case 0: case 1: case 4: case 5: return (int)((s->count - d) & 0xffff);
  case 3: return (int)(s->count - ((2 * d) % (uint64_t)s->count));
  default: return (int)(s->count - (d % (uint64_t)s->count));
  }
}

static int pit_get_out(const struct pit_ch *s, uint64_t now) {
  uint64_t d = pit_ticks(s, now), c = (uint64_t)s->count;
  switch (s->mode) {
  default:
  case 0: case 1: return d >= c;
  case 2: return (d % c) == 0 && d != 0;
  case 3: return (d % c) < ((c + 1) >> 1);
  case 4: case 5: return d == c;
  }
}

static void pit_irq_restart(void) {
  g_pit.emitted = 0;
  g_pit.backlog = 0;
  g_pit.oneshot_done = 0;
  if (g_pit.level) {
    g_pit.level = 0;
    irq_set(0, 0);
  }
}

static void pit_reset(void) {
  memset(&g_pit, 0, sizeof(g_pit));
  uint64_t t = now_ns();
  for (int i = 0; i < 3; i++) {
    struct pit_ch *s = &g_pit.ch[i];
    s->mode = 3;
    s->gate = i != 2;
    s->load_ns = t;
    s->count = 0x10000;
  }
}

static void pit_load_count(int ch, int val) {
  if (val == 0)
    val = 0x10000;
  g_pit.ch[ch].load_ns = now_ns();
  g_pit.ch[ch].count = val;
  if (ch == 0)
    pit_irq_restart();
}

static void pit_latch_count(struct pit_ch *s) {
  if (!s->count_latched) {
    s->latched_count = (uint16_t)pit_get_count(s, now_ns());
    s->count_latched = s->rw_mode;
  }
}

static void pit_write(uint16_t port, uint8_t val) {
  unsigned addr = port & 3;
  if (addr == 3) {
    int channel = val >> 6;
    if (channel == 3) { // read-back
      for (channel = 0; channel < 3; channel++) {
        struct pit_ch *s = &g_pit.ch[channel];
        if (val & (2 << channel)) {
          if (!(val & 0x20))
            pit_latch_count(s);
          if (!(val & 0x10) && !s->status_latched) {
            s->status = (uint8_t)((pit_get_out(s, now_ns()) << 7) | (s->rw_mode << 4) |
                                  (s->mode << 1) | s->bcd);
            s->status_latched = 1;
          }
        }
      }
    } else {
      struct pit_ch *s = &g_pit.ch[channel];
      int access = (val >> 4) & 3;
      if (access == 0) {
        pit_latch_count(s);
      } else {
        s->rw_mode = (uint8_t)access;
        s->read_state = (uint8_t)access;
        s->write_state = (uint8_t)access;
        s->mode = (val >> 1) & 7;
        if (s->mode > 5)
          s->mode -= 4; // 6 y 7 son alias de 2 y 3
        s->bcd = val & 1;
      }
    }
    return;
  }
  if (addr > 2)
    return;
  struct pit_ch *s = &g_pit.ch[addr];
  switch (s->write_state) {
  default:
  case RW_LSB: pit_load_count((int)addr, val); break;
  case RW_MSB: pit_load_count((int)addr, val << 8); break;
  case RW_WORD0: s->write_latch = val; s->write_state = RW_WORD1; break;
  case RW_WORD1:
    pit_load_count((int)addr, s->write_latch | (val << 8));
    s->write_state = RW_WORD0;
    break;
  }
}

static uint8_t pit_read(uint16_t port) {
  unsigned addr = port & 3;
  if (addr == 3)
    return 0;
  struct pit_ch *s = &g_pit.ch[addr];
  int ret, count;
  if (s->status_latched) {
    s->status_latched = 0;
    ret = s->status;
  } else if (s->count_latched) {
    switch (s->count_latched) {
    default:
    case RW_LSB: ret = s->latched_count & 0xff; s->count_latched = 0; break;
    case RW_MSB: ret = s->latched_count >> 8; s->count_latched = 0; break;
    case RW_WORD0: ret = s->latched_count & 0xff; s->count_latched = RW_MSB; break;
    }
  } else {
    count = pit_get_count(s, now_ns());
    switch (s->read_state) {
    default:
    case RW_LSB: ret = count & 0xff; break;
    case RW_MSB: ret = (count >> 8) & 0xff; break;
    case RW_WORD0: ret = count & 0xff; s->read_state = RW_WORD1; break;
    case RW_WORD1: ret = (count >> 8) & 0xff; s->read_state = RW_WORD0; break;
    }
  }
  return (uint8_t)ret;
}

static void pit_set_gate(int ch, int val) {
  struct pit_ch *s = &g_pit.ch[ch];
  if ((s->mode == 1 || s->mode == 5 || s->mode == 2 || s->mode == 3) && s->gate < val) {
    s->load_ns = now_ns(); // flanco de subida: vuelve a contar
    if (ch == 0)
      pit_irq_restart();
  }
  s->gate = (uint8_t)val;
}

// Puerto 0x61 (pcspk de QEMU): bit0 puerta 2, bit1 altavoz, bit4 refresco, bit5 OUT2.
static uint8_t port61_read(void) {
  g_pit.refresh ^= 1u << 4;
  return (uint8_t)(g_pit.ch[2].gate | (g_pit.data_on << 1) | g_pit.refresh |
                   (pit_get_out(&g_pit.ch[2], now_ns()) << 5));
}
static void port61_write(uint8_t v) {
  g_pit.data_on = (v >> 1) & 1;
  pit_set_gate(2, v & 1);
}

// Genera los flancos de IRQ0 que tocan. Devuelve el instante (ns) del proximo
// evento del canal 0, o ~0 si no hay.
static uint64_t pit_poll(uint64_t now) {
  struct pit_ch *s = &g_pit.ch[0];
  uint64_t c = (uint64_t)s->count;
  uint64_t d = pit_ticks(s, now);
  switch (s->mode) {
  case 2: case 3: {
    uint64_t periods = d / c;
    if (periods > g_pit.emitted) {
      uint64_t add = periods - g_pit.emitted;
      g_pit.backlog = add + g_pit.backlog > PIT_MAX_BACKLOG ? PIT_MAX_BACKLOG : g_pit.backlog + (unsigned)add;
      g_pit.emitted = periods;
    }
    // Un flanco por vuelta, y solo si el anterior ya lo acepto alguien.
    if (g_pit.backlog && !irq_edge_pending(0)) {
      irq_set(0, 1);
      irq_set(0, 0);
      g_pit.backlog--;
    }
    if (g_pit.backlog)
      return now; // queda atraso: volver pronto
    __uint128_t nt = (__uint128_t)(g_pit.emitted + 1) * c * 1000000000ULL / PIT_FREQ;
    return s->load_ns + (uint64_t)nt + 1;
  }
  case 0: case 1:
    if (d >= c) {
      if (!g_pit.level) {
        g_pit.level = 1;
        irq_set(0, 1);
      }
      return ~0ULL;
    }
    break;
  case 4: case 5:
    if (d >= c) {
      if (!g_pit.oneshot_done) {
        g_pit.oneshot_done = 1;
        irq_set(0, 1);
        irq_set(0, 0);
      }
      return ~0ULL;
    }
    break;
  }
  if (s->mode == 1 || s->mode == 5)
    if (!s->gate)
      return ~0ULL;
  return s->load_ns + (uint64_t)((__uint128_t)c * 1000000000ULL / PIT_FREQ) + 1;
}

#endif
