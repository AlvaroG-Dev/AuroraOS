// ps2.h — controlador i8042 (0x60/0x64) con teclado y raton PS/2 de la mini-VM,
// con la semantica de QEMU (hw/input/pckbd.c con extended_state y hw/input/ps2.c):
// colas de 16 bytes por dispositivo con respuestas a comandos por delante de las
// teclas, OBF/AUX_OBF, IRQ1 e IRQ12 por nivel segun el byte de modo, traduccion
// del conjunto 2 al 1 (bit KCC), conjuntos 1/2/3, raton en modo flujo/remoto/eco,
// deteccion IntelliMouse (200,100,80 -> ID 3) y Explorer (200,200,80 -> ID 4).
//
// Entrada del anfitrion: teclado por /dev/input/event0 (codigos KEY_* de Linux,
// solo con la ventana de la VM enfocada; al perder el foco se sueltan las teclas
// pulsadas) y raton por los eventos de puntero crudo de la ventana (diferencias de
// posicion) mas la rueda de /dev/input/event1.
#ifndef AVM_PS2_H
#define AVM_PS2_H

static void irq_set(int irq, int level);

#define PS2_BUF 256
#define PS2_QSIZE 16

struct ps2q {
  uint8_t data[PS2_BUF];
  int rptr, wptr, cwptr, count;
  int write_cmd;
  int irq; // nivel de la linea del dispositivo hacia el controlador
};

static struct {
  struct ps2q q;
  int scan_enabled, scancode_set, translate, ledstate;
} g_kbd;

static struct {
  struct ps2q q;
  uint8_t status, resolution, sample_rate, wrap, type, detect;
  int dx, dy, dz, dw;
  uint8_t buttons;
} g_mouse;

static struct {
  uint8_t mode, status, outport, pending, obsrc, obdata, cbdata, write_cmd;
  int irq1, irq12;
} g_i8042;

static int g_reset_req;

#define KBD_STAT_OBF 0x01
#define KBD_STAT_SELFTEST 0x04
#define KBD_STAT_CMD 0x08
#define KBD_STAT_UNLOCKED 0x10
#define KBD_STAT_MOUSE_OBF 0x20
#define KBD_MODE_KBD_INT 0x01
#define KBD_MODE_MOUSE_INT 0x02
#define KBD_MODE_DISABLE_KBD 0x10
#define KBD_MODE_DISABLE_MOUSE 0x20
#define KBD_MODE_KCC 0x40
#define KBD_OUT_OBF 0x10
#define KBD_OUT_MOUSE_OBF 0x20
#define KBD_PENDING_CTRL_KBD 0x04
#define KBD_PENDING_CTRL_AUX 0x08
#define KBD_PENDING_KBD 0x10
#define KBD_PENDING_AUX 0x20
#define KBD_OBSRC_KBD 1
#define KBD_OBSRC_MOUSE 2
#define KBD_OBSRC_CTRL 4

// Conjunto 2 -> conjunto 1 (translate_table de QEMU).
static const uint8_t ps2_xlate[256] = {
    0xff, 0x43, 0x41, 0x3f, 0x3d, 0x3b, 0x3c, 0x58, 0x64, 0x44, 0x42, 0x40, 0x3e, 0x0f, 0x29, 0x59,
    0x65, 0x38, 0x2a, 0x70, 0x1d, 0x10, 0x02, 0x5a, 0x66, 0x71, 0x2c, 0x1f, 0x1e, 0x11, 0x03, 0x5b,
    0x67, 0x2e, 0x2d, 0x20, 0x12, 0x05, 0x04, 0x5c, 0x68, 0x39, 0x2f, 0x21, 0x14, 0x13, 0x06, 0x5d,
    0x69, 0x31, 0x30, 0x23, 0x22, 0x15, 0x07, 0x5e, 0x6a, 0x72, 0x32, 0x24, 0x16, 0x08, 0x09, 0x5f,
    0x6b, 0x33, 0x25, 0x17, 0x18, 0x0b, 0x0a, 0x60, 0x6c, 0x34, 0x35, 0x26, 0x27, 0x19, 0x0c, 0x61,
    0x6d, 0x73, 0x28, 0x74, 0x1a, 0x0d, 0x62, 0x6e, 0x3a, 0x36, 0x1c, 0x1b, 0x75, 0x2b, 0x63, 0x76,
    0x55, 0x56, 0x77, 0x78, 0x79, 0x7a, 0x0e, 0x7b, 0x7c, 0x4f, 0x7d, 0x4b, 0x47, 0x7e, 0x7f, 0x6f,
    0x52, 0x53, 0x50, 0x4c, 0x4d, 0x48, 0x01, 0x45, 0x57, 0x4e, 0x51, 0x4a, 0x37, 0x49, 0x46, 0x54,
    0x80, 0x81, 0x82, 0x41, 0x54, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x8b, 0x8c, 0x8d, 0x8e, 0x8f,
    0x90, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0x9b, 0x9c, 0x9d, 0x9e, 0x9f,
    0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xab, 0xac, 0xad, 0xae, 0xaf,
    0xb0, 0xb1, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xbb, 0xbc, 0xbd, 0xbe, 0xbf,
    0xc0, 0xc1, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xcb, 0xcc, 0xcd, 0xce, 0xcf,
    0xd0, 0xd1, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xdb, 0xdc, 0xdd, 0xde, 0xdf,
    0xe0, 0xe1, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xeb, 0xec, 0xed, 0xee, 0xef,
    0xf0, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa, 0xfb, 0xfc, 0xfd, 0xfe, 0xff,
};
static uint8_t ps2_set1to2[128];

static void ps2_tables_init(void) {
  memset(ps2_set1to2, 0, sizeof(ps2_set1to2));
  for (int i = 0x7F; i >= 1; i--) // inversa de ps2_xlate: gana el menor codigo
    if (ps2_xlate[i] < 0x80)
      ps2_set1to2[ps2_xlate[i]] = (uint8_t)i;
  ps2_set1to2[0x41] = 0x83; // F7
  ps2_set1to2[0x54] = 0x84; // SysRq (Alt+ImprPant)
}

// ---------------------------------------------------------------------------
// Colas PS/2
// ---------------------------------------------------------------------------
static void i8042_dev_irq(int aux, int level);

static void ps2q_reset(struct ps2q *q) {
  q->rptr = q->wptr = q->count = 0;
  q->cwptr = -1;
}
static void ps2q_noirq(struct ps2q *q, int b) {
  if (q->count >= PS2_QSIZE)
    return;
  q->data[q->wptr] = (uint8_t)b;
  if (++q->wptr == PS2_BUF)
    q->wptr = 0;
  q->count++;
}
static void ps2q_raise(struct ps2q *q) { q->irq = 1; i8042_dev_irq(q == &g_mouse.q, 1); }
static void ps2q_lower(struct ps2q *q) { q->irq = 0; i8042_dev_irq(q == &g_mouse.q, 0); }
static void ps2_queue_n(struct ps2q *q, const uint8_t *b, int n) {
  if (PS2_QSIZE - q->count < n)
    return;
  for (int i = 0; i < n; i++)
    ps2q_noirq(q, b[i]);
  ps2q_raise(q);
}
static void ps2_queue(struct ps2q *q, int b) {
  uint8_t x = (uint8_t)b;
  ps2_queue_n(q, &x, 1);
}
// Respuestas a comandos: delante de lo encolado (cqueue de QEMU).
static void ps2_cqueue_n(struct ps2q *q, const uint8_t *b, int n) {
  q->rptr = (q->rptr - n) & (PS2_BUF - 1);
  q->cwptr = q->rptr;
  for (int i = 0; i < n; i++) {
    q->data[q->cwptr] = b[i];
    if (++q->cwptr >= PS2_BUF)
      q->cwptr = 0;
    q->count++;
  }
  ps2q_raise(q);
}
static void ps2_cqueue1(struct ps2q *q, int a) { uint8_t b[1] = {(uint8_t)a}; ps2_cqueue_n(q, b, 1); }
static void ps2_cqueue_reset(struct ps2q *q) {
  if (q->cwptr == -1)
    return;
  int cc = (q->cwptr - q->rptr) & (PS2_BUF - 1);
  q->count -= cc;
  q->rptr = q->cwptr;
  q->cwptr = -1;
}

static uint8_t ps2_read_data(struct ps2q *q) {
  int val;
  if (q->count == 0) {
    int idx = q->rptr - 1;
    if (idx < 0)
      idx = PS2_BUF - 1;
    val = q->data[idx]; // como QEMU: sin datos, el ultimo byte (EMM386)
  } else {
    val = q->data[q->rptr];
    if (++q->rptr == PS2_BUF)
      q->rptr = 0;
    q->count--;
    if (q->rptr == q->cwptr)
      q->cwptr = -1;
    ps2q_lower(q);
    if (q->count)
      ps2q_raise(q);
  }
  return (uint8_t)val;
}

// ---------------------------------------------------------------------------
// Teclado
// ---------------------------------------------------------------------------
static void ps2_kbd_reset_dev(void) {
  g_kbd.scan_enabled = 1;
  g_kbd.scancode_set = 2;
  ps2q_reset(&g_kbd.q);
  g_kbd.ledstate = 0;
}

static void ps2_write_keyboard(int val) {
  struct ps2q *q = &g_kbd.q;
  ps2_cqueue_reset(q);
  switch (q->write_cmd) {
  default:
    switch (val) {
    case 0x00: ps2_cqueue1(q, 0xFA); break;
    case 0x05: ps2_cqueue1(q, 0xFE); break;
    case 0xF2: {
      uint8_t b[3] = {0xFA, 0xAB, (uint8_t)(g_kbd.translate ? 0x41 : 0x83)};
      ps2_cqueue_n(q, b, 3);
      break;
    }
    case 0xEE: ps2_cqueue1(q, 0xEE); break;
    case 0xF4: g_kbd.scan_enabled = 1; ps2_cqueue1(q, 0xFA); break;
    case 0xF0: case 0xED: case 0xF3: case 0xFC:
      q->write_cmd = val;
      ps2_cqueue1(q, 0xFA);
      break;
    case 0xF5: ps2_kbd_reset_dev(); g_kbd.scan_enabled = 0; ps2_cqueue1(q, 0xFA); break;
    case 0xF6: ps2_kbd_reset_dev(); g_kbd.scan_enabled = 1; ps2_cqueue1(q, 0xFA); break;
    case 0xFF: {
      ps2_kbd_reset_dev();
      uint8_t b[2] = {0xFA, 0xAA};
      ps2_cqueue_n(q, b, 2);
      break;
    }
    case 0xFA: ps2_cqueue1(q, 0xFA); break;
    default: ps2_cqueue1(q, 0xFE); break;
    }
    break;
  case 0xFC:
    ps2_cqueue1(q, 0xFA);
    q->write_cmd = -1;
    break;
  case 0xF0:
    if (val == 0) {
      uint8_t b[2] = {0xFA, (uint8_t)(g_kbd.translate ? ps2_xlate[g_kbd.scancode_set] : g_kbd.scancode_set)};
      ps2_cqueue_n(q, b, 2);
    } else if (val >= 1 && val <= 3) {
      g_kbd.scancode_set = val;
      ps2_cqueue1(q, 0xFA);
    } else {
      ps2_cqueue1(q, 0xFE);
    }
    q->write_cmd = -1;
    break;
  case 0xED:
    g_kbd.ledstate = val;
    ps2_cqueue1(q, 0xFA);
    q->write_cmd = -1;
    break;
  case 0xF3:
    ps2_cqueue1(q, 0xFA);
    q->write_cmd = -1;
    break;
  }
}

// Bytes en conjunto 1 de una tecla. Devuelve cuantos.
static int ps2_key_set1(int key, int down, uint8_t *o) {
  static const uint8_t ext[128] = {
      [96] = 0x1C, [97] = 0x1D, [98] = 0x35, [100] = 0x38, [102] = 0x47, [103] = 0x48,
      [104] = 0x49, [105] = 0x4B, [106] = 0x4D, [107] = 0x4F, [108] = 0x50, [109] = 0x51,
      [110] = 0x52, [111] = 0x53, [125] = 0x5B, [126] = 0x5C, [127] = 0x5D,
  };
  uint8_t brk = down ? 0 : 0x80;
  if (key >= 1 && key <= 88 && key != 84) {
    o[0] = (uint8_t)(key | brk);
    return 1;
  }
  if (key == 99) { // ImprPant
    if (down) { o[0] = 0xE0; o[1] = 0x2A; o[2] = 0xE0; o[3] = 0x37; }
    else { o[0] = 0xE0; o[1] = 0xB7; o[2] = 0xE0; o[3] = 0xAA; }
    return 4;
  }
  if (key == 119) { // Pausa: solo al pulsar
    if (!down)
      return 0;
    static const uint8_t p[6] = {0xE1, 0x1D, 0x45, 0xE1, 0x9D, 0xC5};
    memcpy(o, p, 6);
    return 6;
  }
  if (key < 128 && ext[key]) {
    o[0] = 0xE0;
    o[1] = (uint8_t)(ext[key] | brk);
    return 2;
  }
  return 0;
}

// Tecla del anfitrion (KEY_* de Linux) hacia el huesped.
static void ps2_kbd_key(int key, int down) {
  if (!g_kbd.scan_enabled)
    return;
  uint8_t s1[8], out[16];
  int n = ps2_key_set1(key, down, s1), m = 0;
  if (!n)
    return;
  // Conjunto 1 tal cual si el huesped lee traducido (conjunto 2 + KCC) o pidio el 1.
  int raw2 = !g_kbd.translate && g_kbd.scancode_set != 1;
  for (int i = 0; i < n; i++) {
    uint8_t b = s1[i];
    if (!raw2 || b == 0xE0 || b == 0xE1) {
      out[m++] = b;
    } else {
      if (b & 0x80)
        out[m++] = 0xF0;
      out[m++] = ps2_set1to2[b & 0x7F];
    }
  }
  for (int i = 0; i < m; i++) // ps2_put_keycode: un byte cada vez (cola llena: se pierde)
    ps2_queue(&g_kbd.q, out[i]);
}

// ---------------------------------------------------------------------------
// Raton
// ---------------------------------------------------------------------------
static int ps2_mouse_send_packet(void) {
  struct ps2q *q = &g_mouse.q;
  int need = g_mouse.type ? 4 : 3;
  if (PS2_QSIZE - q->count < need)
    return 0;
  int dx1 = g_mouse.dx, dy1 = g_mouse.dy, dz1 = g_mouse.dz;
  if (dx1 > 127) dx1 = 127; else if (dx1 < -127) dx1 = -127;
  if (dy1 > 127) dy1 = 127; else if (dy1 < -127) dy1 = -127;
  unsigned b = 0x08 | ((dx1 < 0) << 4) | ((dy1 < 0) << 5) | (g_mouse.buttons & 7);
  ps2q_noirq(q, (int)b);
  ps2q_noirq(q, dx1 & 0xff);
  ps2q_noirq(q, dy1 & 0xff);
  switch (g_mouse.type) {
  default:
    g_mouse.dz = g_mouse.dw = 0;
    break;
  case 3:
    if (dz1 > 127) dz1 = 127; else if (dz1 < -127) dz1 = -127;
    ps2q_noirq(q, dz1 & 0xff);
    g_mouse.dz -= dz1;
    g_mouse.dw = 0;
    break;
  case 4:
    if (dz1 > 7) dz1 = 7; else if (dz1 < -7) dz1 = -7;
    ps2q_noirq(q, (dz1 & 0x0f) | ((g_mouse.buttons & 0x18) << 1));
    g_mouse.dz -= dz1;
    g_mouse.dw = 0;
    break;
  }
  ps2q_raise(q);
  g_mouse.dx -= dx1;
  g_mouse.dy -= dy1;
  return 1;
}

// Movimiento del anfitrion: dx a la derecha, dy hacia abajo (pantalla); dz rueda
// (positivo = hacia el usuario, como REL_WHEEL negativo).
static void ps2_mouse_event(int dx, int dy, int dz, int buttons) {
  if (!(g_mouse.status & 0x20)) // desactivado: no acumular (QEMU)
    return;
  g_mouse.dx += dx;
  g_mouse.dy -= dy;
  g_mouse.dz += dz;
  g_mouse.buttons = (uint8_t)buttons;
  if (!(g_mouse.status & 0x40)) {
    while (ps2_mouse_send_packet())
      if (!g_mouse.dx && !g_mouse.dy && !g_mouse.dz && !g_mouse.dw)
        break;
  }
}

static void ps2_mouse_reset_dev(void) {
  g_mouse.sample_rate = 100;
  g_mouse.resolution = 2;
  g_mouse.status = 0;
  g_mouse.type = 0;
  ps2q_reset(&g_mouse.q);
}

static void ps2_write_mouse(int val) {
  struct ps2q *q = &g_mouse.q;
  switch (q->write_cmd) {
  default:
    if (g_mouse.wrap) {
      if (val == 0xEC) {
        g_mouse.wrap = 0;
        ps2_queue(q, 0xFA);
        return;
      } else if (val != 0xFF) {
        ps2_queue(q, val);
        return;
      }
    }
    switch (val) {
    case 0xE6: g_mouse.status &= (uint8_t)~0x10; ps2_queue(q, 0xFA); break;
    case 0xE7: g_mouse.status |= 0x10; ps2_queue(q, 0xFA); break;
    case 0xEA: g_mouse.status &= (uint8_t)~0x40; ps2_queue(q, 0xFA); break;
    case 0xEE: g_mouse.wrap = 1; ps2_queue(q, 0xFA); break;
    case 0xF0: g_mouse.status |= 0x40; ps2_queue(q, 0xFA); break;
    case 0xF2: { uint8_t b[2] = {0xFA, g_mouse.type}; ps2_queue_n(q, b, 2); break; }
    case 0xE8: case 0xF3: q->write_cmd = val; ps2_queue(q, 0xFA); break;
    case 0xE9: {
      uint8_t b[4] = {0xFA, g_mouse.status, g_mouse.resolution, g_mouse.sample_rate};
      ps2_queue_n(q, b, 4);
      break;
    }
    case 0xEB: ps2_queue(q, 0xFA); ps2_mouse_send_packet(); break;
    case 0xF4: g_mouse.status |= 0x20; ps2_queue(q, 0xFA); break;
    case 0xF5: g_mouse.status &= (uint8_t)~0x20; ps2_queue(q, 0xFA); break;
    case 0xF6:
      g_mouse.sample_rate = 100;
      g_mouse.resolution = 2;
      g_mouse.status = 0;
      ps2_queue(q, 0xFA);
      break;
    case 0xFF: {
      ps2_mouse_reset_dev();
      uint8_t b[3] = {0xFA, 0xAA, g_mouse.type};
      ps2_queue_n(q, b, 3);
      break;
    }
    default: break;
    }
    break;
  case 0xF3:
    g_mouse.sample_rate = (uint8_t)val;
    switch (g_mouse.detect) {
    default:
    case 0: if (val == 200) g_mouse.detect = 1; break;
    case 1:
      g_mouse.detect = val == 100 ? 2 : val == 200 ? 3 : 0;
      break;
    case 2: if (val == 80) g_mouse.type = 3; g_mouse.detect = 0; break;
    case 3: if (val == 80) g_mouse.type = 4; g_mouse.detect = 0; break;
    }
    ps2_queue(q, 0xFA);
    q->write_cmd = -1;
    break;
  case 0xE8:
    g_mouse.resolution = (uint8_t)val;
    ps2_queue(q, 0xFA);
    q->write_cmd = -1;
    break;
  }
}

// ---------------------------------------------------------------------------
// Controlador i8042 (pckbd.c con extended_state)
// ---------------------------------------------------------------------------
static void i8042_update_irq_lines(void) {
  int k = 0, m = 0;
  if (g_i8042.status & KBD_STAT_OBF) {
    if (g_i8042.status & KBD_STAT_MOUSE_OBF) {
      if (g_i8042.mode & KBD_MODE_MOUSE_INT)
        m = 1;
    } else if ((g_i8042.mode & KBD_MODE_KBD_INT) && !(g_i8042.mode & KBD_MODE_DISABLE_KBD)) {
      k = 1;
    }
  }
  if (k != g_i8042.irq1) { g_i8042.irq1 = k; irq_set(1, k); }
  if (m != g_i8042.irq12) { g_i8042.irq12 = m; irq_set(12, m); }
}

static uint8_t i8042_pending(void) {
  return g_i8042.pending & (uint8_t)(~g_i8042.mode | ~(KBD_PENDING_KBD | KBD_PENDING_AUX));
}

static void i8042_update_irq(void) {
  uint8_t p = i8042_pending();
  g_i8042.status &= (uint8_t)~(KBD_STAT_OBF | KBD_STAT_MOUSE_OBF);
  g_i8042.outport &= (uint8_t)~(KBD_OUT_OBF | KBD_OUT_MOUSE_OBF);
  if (p) {
    g_i8042.status |= KBD_STAT_OBF;
    g_i8042.outport |= KBD_OUT_OBF;
    if (p & KBD_PENDING_CTRL_KBD) {
      g_i8042.obsrc = KBD_OBSRC_CTRL;
    } else if (p & KBD_PENDING_CTRL_AUX) {
      g_i8042.status |= KBD_STAT_MOUSE_OBF;
      g_i8042.outport |= KBD_OUT_MOUSE_OBF;
      g_i8042.obsrc = KBD_OBSRC_CTRL;
    } else if (p & KBD_PENDING_KBD) {
      g_i8042.obsrc = KBD_OBSRC_KBD;
    } else {
      g_i8042.status |= KBD_STAT_MOUSE_OBF;
      g_i8042.outport |= KBD_OUT_MOUSE_OBF;
      g_i8042.obsrc = KBD_OBSRC_MOUSE;
    }
  }
  i8042_update_irq_lines();
}

static void i8042_safe_update_irq(void) {
  if (g_i8042.status & KBD_STAT_OBF)
    return;
  if (i8042_pending())
    i8042_update_irq();
}

static void i8042_dev_irq(int aux, int level) {
  uint8_t bit = aux ? KBD_PENDING_AUX : KBD_PENDING_KBD;
  if (level)
    g_i8042.pending |= bit;
  else
    g_i8042.pending &= (uint8_t)~bit;
  i8042_safe_update_irq();
}

static void i8042_queue(int b, int aux) {
  g_i8042.cbdata = (uint8_t)b;
  g_i8042.pending &= (uint8_t)~(KBD_PENDING_CTRL_KBD | KBD_PENDING_CTRL_AUX);
  g_i8042.pending |= aux ? KBD_PENDING_CTRL_AUX : KBD_PENDING_CTRL_KBD;
  i8042_safe_update_irq();
}

static void i8042_deassert(void) {
  g_i8042.status &= (uint8_t)~(KBD_STAT_OBF | KBD_STAT_MOUSE_OBF);
  g_i8042.outport &= (uint8_t)~(KBD_OUT_OBF | KBD_OUT_MOUSE_OBF);
  i8042_update_irq_lines();
}

static void i8042_outport_write(uint8_t v) {
  g_i8042.outport = v;
  if (!(v & 1))
    g_reset_req = 1;
}

static void i8042_write_command(uint8_t val) {
  if ((val & 0xF0) == 0xF0)
    val = (val & 1) ? 0xFF : 0xFE;
  switch (val) {
  case 0x20: i8042_queue(g_i8042.mode, 0); break;
  case 0x60: case 0xD1: case 0xD2: case 0xD3: case 0xD4: g_i8042.write_cmd = val; break;
  case 0xA7: g_i8042.mode |= KBD_MODE_DISABLE_MOUSE; break;
  case 0xA8: g_i8042.mode &= (uint8_t)~KBD_MODE_DISABLE_MOUSE; i8042_safe_update_irq(); break;
  case 0xA9: i8042_queue(0x00, 0); break;
  case 0xAA: g_i8042.status |= KBD_STAT_SELFTEST; i8042_queue(0x55, 0); break;
  case 0xAB: i8042_queue(0x00, 0); break;
  case 0xAD: g_i8042.mode |= KBD_MODE_DISABLE_KBD; break;
  case 0xAE: g_i8042.mode &= (uint8_t)~KBD_MODE_DISABLE_KBD; i8042_safe_update_irq(); break;
  case 0xC0: i8042_queue(0x80, 0); break;
  case 0xD0: i8042_queue(g_i8042.outport, 0); break;
  case 0xDF: g_i8042.outport |= 2; break;
  case 0xDD: g_i8042.outport &= (uint8_t)~2; break;
  case 0xFE: g_reset_req = 1; break;
  default: break;
  }
}

static uint8_t i8042_read_data(void) {
  if (g_i8042.status & KBD_STAT_OBF) {
    i8042_deassert();
    if (g_i8042.obsrc & KBD_OBSRC_KBD) {
      g_i8042.obdata = ps2_read_data(&g_kbd.q);
    } else if (g_i8042.obsrc & KBD_OBSRC_MOUSE) {
      g_i8042.obdata = ps2_read_data(&g_mouse.q);
    } else if (g_i8042.obsrc & KBD_OBSRC_CTRL) {
      g_i8042.obdata = g_i8042.cbdata;
      g_i8042.pending &= (uint8_t)~(KBD_PENDING_CTRL_KBD | KBD_PENDING_CTRL_AUX);
      if (i8042_pending())
        i8042_update_irq();
    }
  }
  return g_i8042.obdata;
}

static void i8042_write_data(uint8_t val) {
  switch (g_i8042.write_cmd) {
  case 0:
    ps2_write_keyboard(val);
    g_i8042.mode &= (uint8_t)~KBD_MODE_DISABLE_KBD;
    i8042_safe_update_irq();
    break;
  case 0x60:
    g_i8042.mode = val;
    g_kbd.translate = (val & KBD_MODE_KCC) != 0;
    i8042_update_irq_lines();
    i8042_safe_update_irq();
    break;
  case 0xD2: i8042_queue(val, 0); break;
  case 0xD3: i8042_queue(val, 1); break;
  case 0xD1: i8042_outport_write(val); break;
  case 0xD4:
    ps2_write_mouse(val);
    g_i8042.mode &= (uint8_t)~KBD_MODE_DISABLE_MOUSE;
    i8042_safe_update_irq();
    break;
  default: break;
  }
  g_i8042.write_cmd = 0;
}

static void i8042_reset(void) {
  ps2_tables_init();
  memset(&g_kbd, 0, sizeof(g_kbd));
  memset(&g_mouse, 0, sizeof(g_mouse));
  memset(&g_i8042, 0, sizeof(g_i8042));
  ps2q_reset(&g_kbd.q);
  ps2q_reset(&g_mouse.q);
  g_kbd.q.write_cmd = g_mouse.q.write_cmd = -1;
  g_kbd.scan_enabled = 1;
  g_kbd.scancode_set = 2;
  g_i8042.mode = KBD_MODE_KBD_INT | KBD_MODE_MOUSE_INT;
  g_i8042.status = KBD_STAT_CMD | KBD_STAT_UNLOCKED;
  g_i8042.outport = 0x01 | 0x02 | 0xCC;
}

static int i8042_port(uint16_t p) { return p == 0x60 || p == 0x64; }
static uint8_t i8042_in(uint16_t p) { return p == 0x60 ? i8042_read_data() : g_i8042.status; }
static void i8042_out(uint16_t p, uint8_t v) {
  if (p == 0x60)
    i8042_write_data(v);
  else
    i8042_write_command(v);
}

// ---------------------------------------------------------------------------
// Entrada del anfitrion
// ---------------------------------------------------------------------------
struct avm_input_event {
  int64_t sec, usec;
  uint16_t type, code;
  int32_t value;
};

static int g_in_kbd = -1, g_in_mouse = -1;
static int g_in_focus = 1;           // la ventana tiene el foco (se crea enfocada)
static uint8_t g_in_held[128];       // teclas pulsadas enviadas al huesped
static int g_ptr_in, g_ptr_x, g_ptr_y, g_ptr_btn;

static void host_input_open(void) {
  g_in_kbd = open("/dev/input/event0", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
  g_in_mouse = open("/dev/input/event1", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
  memset(g_in_held, 0, sizeof(g_in_held));
  g_ptr_in = 0;
}

static void host_release_all(void) {
  for (int k = 1; k < 128; k++)
    if (g_in_held[k]) {
      g_in_held[k] = 0;
      ps2_kbd_key(k, 0);
    }
  if (g_ptr_btn) {
    g_ptr_btn = 0;
    ps2_mouse_event(0, 0, 0, 0);
  }
}

static void host_focus(int on) {
  if (!on)
    host_release_all();
  g_in_focus = on;
}

// Tecla que llega por la ventana (sin /dev/input): solo pulsaciones, se suelta enseguida.
static void host_window_key(uint8_t set1, int down) {
  if (g_in_kbd >= 0)
    return;
  int key = set1 & 0x7F;
  if (!key)
    return;
  ps2_kbd_key(key, down);
  if (down)
    ps2_kbd_key(key, 0);
}

static void host_pointer(int x, int y, int buttons, int leave) {
  if (leave) {
    g_ptr_in = 0;
    if (g_ptr_btn) {
      g_ptr_btn = 0;
      ps2_mouse_event(0, 0, 0, 0);
    }
    return;
  }
  // izq=bit0, der=bit1, medio=bit2: igual que el byte PS/2
  int dx = g_ptr_in ? x - g_ptr_x : 0, dy = g_ptr_in ? y - g_ptr_y : 0;
  g_ptr_in = 1;
  g_ptr_x = x;
  g_ptr_y = y;
  if (dx || dy || buttons != g_ptr_btn) {
    g_ptr_btn = buttons;
    ps2_mouse_event(dx, dy, 0, buttons);
  }
}

static void host_input_poll(void) {
  struct avm_input_event ev[32];
  if (g_in_kbd >= 0) {
    ssize_t n;
    while ((n = read(g_in_kbd, ev, sizeof(ev))) > 0) {
      for (int i = 0; i < (int)(n / (ssize_t)sizeof(ev[0])); i++) {
        if (ev[i].type != 1 || ev[i].code >= 128 || !g_in_focus)
          continue;
        int k = ev[i].code, v = ev[i].value;
        if (v == 0 && !g_in_held[k])
          continue; // se pulso fuera de la VM
        g_in_held[k] = v != 0;
        ps2_kbd_key(k, v != 0);
      }
    }
  }
  if (g_in_mouse >= 0) {
    ssize_t n;
    int dz = 0;
    while ((n = read(g_in_mouse, ev, sizeof(ev))) > 0)
      for (int i = 0; i < (int)(n / (ssize_t)sizeof(ev[0])); i++)
        if (ev[i].type == 2 && ev[i].code == 8)
          dz -= ev[i].value;
    if (dz && g_in_focus && g_ptr_in)
      ps2_mouse_event(0, 0, dz, g_ptr_btn);
  }
}

#endif
