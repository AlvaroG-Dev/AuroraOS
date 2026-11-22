// pic.h — par de 8259A (maestro 0x20/0x21, esclavo 0xA0/0xA1) y ELCR (0x4D0/0x4D1)
// de la mini-VM, con la semantica del i8259 de QEMU: flanco/nivel por ELCR o LTIM,
// fin de interrupcion especifico/no especifico, rotacion, auto-EOI, mascara especial,
// modo poll, anidamiento completamente especial y lectura de IRR/ISR.
//
// La salida del maestro va al pin INTR del vCPU (LINT0 del LAPIC en modo ExtINT).
// El VMM pide al kernel que inyecte pic_peek_vector() y, solo cuando el kernel
// confirma la entrega, llama a pic_intack_vector() (el "INTA" del procesador).
#ifndef AVM_PIC_H
#define AVM_PIC_H

struct i8259 {
  uint8_t irr, imr, isr, last_irr, elcr, elcr_mask;
  uint8_t priority_add, irq_base, read_reg_select, poll, special_mask, init_state;
  uint8_t auto_eoi, rotate_on_auto_eoi, special_fully_nested_mode, init4, single_mode, ltim;
  uint8_t master;
};
static struct i8259 g_pic[2];

static int pic_get_priority(const struct i8259 *s, int mask) {
  if (!mask)
    return 8;
  int p = 0;
  while (!(mask & (1 << ((p + s->priority_add) & 7))))
    p++;
  return p;
}

// IRQ (0..7) que el chip presentaria ahora, o -1.
static int pic_get_irq(const struct i8259 *s) {
  int mask = s->irr & ~s->imr;
  int prio = pic_get_priority(s, mask);
  if (prio == 8)
    return -1;
  mask = s->isr;
  if (s->special_mask)
    mask &= ~s->imr;
  if (s->special_fully_nested_mode && s->master)
    mask &= ~(1 << 2);
  int cur = pic_get_priority(s, mask);
  return prio < cur ? (prio + s->priority_add) & 7 : -1;
}

static void pic_set_irq1(struct i8259 *s, int irq, int level);

// Salida del esclavo -> IRQ2 del maestro (nivel).
static void pic_update(void) {
  pic_set_irq1(&g_pic[0], 2, pic_get_irq(&g_pic[1]) >= 0);
}

static void pic_set_irq1(struct i8259 *s, int irq, int level) {
  int mask = 1 << irq;
  if (s->ltim || (s->elcr & mask)) {
    if (level) {
      s->irr |= mask;
      s->last_irr |= mask;
    } else {
      s->irr &= ~mask;
      s->last_irr &= ~mask;
    }
  } else {
    if (level) {
      if (!(s->last_irr & mask))
        s->irr |= mask;
      s->last_irr |= mask;
    } else {
      s->last_irr &= ~mask;
    }
  }
  if (s == &g_pic[1])
    pic_update();
}

// Linea ISA 0..15 (nivel logico).
static void pic_set_irq(int irq, int level) {
  if (irq < 8)
    pic_set_irq1(&g_pic[0], irq, level);
  else if (irq < 16)
    pic_set_irq1(&g_pic[1], irq - 8, level);
}

static void pic_reset_common(struct i8259 *s) {
  s->last_irr = 0;
  s->irr &= s->elcr;
  s->imr = 0;
  s->isr = 0;
  s->priority_add = 0;
  s->irq_base = 0;
  s->read_reg_select = 0;
  s->poll = 0;
  s->special_mask = 0;
  s->init_state = 0;
  s->auto_eoi = 0;
  s->rotate_on_auto_eoi = 0;
  s->special_fully_nested_mode = 0;
  s->init4 = 0;
  s->single_mode = 0;
}

static void pic_reset(void) {
  memset(g_pic, 0, sizeof(g_pic));
  g_pic[0].master = 1;
  g_pic[0].elcr_mask = 0xf8;
  g_pic[1].elcr_mask = 0xde;
  pic_reset_common(&g_pic[0]);
  pic_reset_common(&g_pic[1]);
}

// INTR del procesador: hay interrupcion presentada por el maestro.
static int pic_output(void) { return pic_get_irq(&g_pic[0]) >= 0; }

static void pic_intack1(struct i8259 *s, int irq) {
  if (s->auto_eoi) {
    if (s->rotate_on_auto_eoi)
      s->priority_add = (uint8_t)((irq + 1) & 7);
  } else {
    s->isr |= (uint8_t)(1 << irq);
  }
  if (!s->ltim && !(s->elcr & (1 << irq))) // los de nivel no se borran aqui
    s->irr &= (uint8_t)~(1 << irq);
  if (s == &g_pic[1])
    pic_update();
}

// Vector que daria el ciclo INTA ahora (sin hacerlo). -1 si no hay salida.
static int pic_peek_vector(void) {
  int irq = pic_get_irq(&g_pic[0]);
  if (irq < 0)
    return -1;
  if (irq == 2) {
    int irq2 = pic_get_irq(&g_pic[1]);
    return g_pic[1].irq_base + (irq2 >= 0 ? irq2 : 7);
  }
  return g_pic[0].irq_base + irq;
}

// Ciclo INTA (pic_read_irq de QEMU). Devuelve el vector.
static int pic_intack_vector(void) {
  int irq = pic_get_irq(&g_pic[0]), intno;
  if (irq >= 0) {
    if (irq == 2) {
      int irq2 = pic_get_irq(&g_pic[1]);
      if (irq2 >= 0)
        pic_intack1(&g_pic[1], irq2);
      else
        irq2 = 7; // espuria del esclavo
      intno = g_pic[1].irq_base + irq2;
      pic_intack1(&g_pic[0], irq);
    } else {
      intno = g_pic[0].irq_base + irq;
      pic_intack1(&g_pic[0], irq);
    }
  } else {
    intno = g_pic[0].irq_base + 7; // espuria del maestro
  }
  return intno;
}

static void pic_ioport_write(struct i8259 *s, int a0, uint8_t val) {
  if (!a0) {
    if (val & 0x10) { // ICW1
      pic_reset_common(s);
      s->init_state = 1;
      s->init4 = val & 1;
      s->single_mode = val & 2;
      s->ltim = val & 8;
    } else if (val & 0x08) { // OCW3
      if (val & 0x04)
        s->poll = 1;
      if (val & 0x02)
        s->read_reg_select = val & 1;
      if (val & 0x40)
        s->special_mask = (val >> 5) & 1;
    } else { // OCW2
      int cmd = val >> 5, prio, irq;
      switch (cmd) {
      case 0: case 4:
        s->rotate_on_auto_eoi = (uint8_t)(cmd >> 2);
        break;
      case 1: case 5:
        prio = pic_get_priority(s, s->isr);
        if (prio != 8) {
          irq = (prio + s->priority_add) & 7;
          s->isr &= (uint8_t)~(1 << irq);
          if (cmd == 5)
            s->priority_add = (uint8_t)((irq + 1) & 7);
        }
        break;
      case 3:
        s->isr &= (uint8_t)~(1 << (val & 7));
        break;
      case 6:
        s->priority_add = (uint8_t)((val + 1) & 7);
        break;
      case 7:
        irq = val & 7;
        s->isr &= (uint8_t)~(1 << irq);
        s->priority_add = (uint8_t)((irq + 1) & 7);
        break;
      default:
        break;
      }
    }
  } else {
    switch (s->init_state) {
    case 0: s->imr = val; break;
    case 1:
      s->irq_base = val & 0xf8;
      s->init_state = s->single_mode ? (s->init4 ? 3 : 0) : 2;
      break;
    case 2: s->init_state = s->init4 ? 3 : 0; break;
    case 3:
      s->special_fully_nested_mode = (val >> 4) & 1;
      s->auto_eoi = (val >> 1) & 1;
      s->init_state = 0;
      break;
    }
  }
  if (s == &g_pic[1])
    pic_update();
}

static uint8_t pic_ioport_read(struct i8259 *s, int a0) {
  int ret;
  if (s->poll) {
    ret = pic_get_irq(s);
    if (ret >= 0) {
      pic_intack1(s, ret);
      ret |= 0x80;
    } else {
      ret = 0;
    }
    s->poll = 0;
  } else if (!a0) {
    ret = s->read_reg_select ? s->isr : s->irr;
  } else {
    ret = s->imr;
  }
  return (uint8_t)ret;
}

static int pic_port(uint16_t p) {
  return p == 0x20 || p == 0x21 || p == 0xA0 || p == 0xA1 || p == 0x4D0 || p == 0x4D1;
}

static uint8_t pic_in(uint16_t p) {
  switch (p) {
  case 0x20: case 0x21: return pic_ioport_read(&g_pic[0], p & 1);
  case 0xA0: case 0xA1: return pic_ioport_read(&g_pic[1], p & 1);
  case 0x4D0: return g_pic[0].elcr;
  case 0x4D1: return g_pic[1].elcr;
  default: return 0xff;
  }
}

static void pic_out(uint16_t p, uint8_t v) {
  switch (p) {
  case 0x20: case 0x21: pic_ioport_write(&g_pic[0], p & 1, v); break;
  case 0xA0: case 0xA1: pic_ioport_write(&g_pic[1], p & 1, v); break;
  case 0x4D0: g_pic[0].elcr = v & g_pic[0].elcr_mask; break;
  case 0x4D1: g_pic[1].elcr = v & g_pic[1].elcr_mask; break;
  default: break;
  }
}

#endif
