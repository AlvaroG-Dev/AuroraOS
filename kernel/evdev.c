// kernel/evdev.c
//
// Ver evdev.h. Dos dispositivos fijos (teclado, raton) que agregan todo el
// hardware de entrada (PS/2 y USB HID).

#include "evdev.h"
#include "devtmpfs.h"
#include "heap.h"
#include "klog.h"
#include "mutex.h"
#include "spinlock.h"
#include "string.h"
#include "time.h"
#include "uaccess.h"
#include "vfs.h"
#include "wait.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define EV_SYN 0x00
#define EV_KEY 0x01
#define EV_REL 0x02
#define SYN_REPORT 0
#define SYN_DROPPED 3

#define REL_X 0
#define REL_Y 1
#define REL_HWHEEL 6
#define REL_WHEEL 8

#define BTN_LEFT 0x110
#define BTN_RIGHT 0x111
#define BTN_MIDDLE 0x112

#define KEY_BITS_BYTES 96 // KEY_MAX 0x2ff -> 768 bits
#define EVDEV_RING 512    // eventos por cliente (potencia de 2)
#define O_NONBLOCK_L 0x800

struct linux_input_event {
  int64_t tv_sec;
  int64_t tv_usec;
  uint16_t type;
  uint16_t code;
  int32_t value;
};
_Static_assert(sizeof(struct linux_input_event) == 24, "input_event");

struct evdev_dev;

#define EVDEV_DEV_MAGIC 0xE7DE5001u
#define EVDEV_CLIENT_MAGIC 0xE7C11E27u

typedef struct evdev_client {
  uint32_t magic; // EVDEV_CLIENT_MAGIC
  struct evdev_client *next;
  struct evdev_dev *dev;
  struct linux_input_event ring[EVDEV_RING];
  uint32_t head, tail; // contadores crecientes; indice = & (EVDEV_RING-1)
  bool nonblock;
  bool dropped;
  bool revoked;
  wait_queue_t wq;
} evdev_client_t;

typedef struct evdev_dev {
  uint32_t magic; // EVDEV_DEV_MAGIC
  int index;
  const char *name;
  const char *phys;
  uint16_t bustype, vendor, product, version;
  uint8_t evbits[4];
  uint8_t relbits[2];
  uint8_t keybits[KEY_BITS_BYTES];
  uint8_t keystate[KEY_BITS_BYTES];
  uint8_t mouse_buttons;
  spinlock_t lock;
  evdev_client_t *clients;
  evdev_client_t *grab;
} evdev_dev_t;

static evdev_dev_t g_kbd, g_mouse;

// ---------------------------------------------------------------------------
// Utilidades de bitmap
// ---------------------------------------------------------------------------
static inline void bit_set(uint8_t *m, int b) { m[b >> 3] |= (uint8_t)(1u << (b & 7)); }
static inline void bit_clr(uint8_t *m, int b) { m[b >> 3] &= (uint8_t)~(1u << (b & 7)); }
static inline int bit_get(const uint8_t *m, int b) { return (m[b >> 3] >> (b & 7)) & 1; }

// ---------------------------------------------------------------------------
// Emision
// ---------------------------------------------------------------------------
static void fill_time(struct linux_input_event *e) {
  uint64_t ms = tick_count; // KERNEL_HZ = 1000
  e->tv_sec = (int64_t)(ms / 1000);
  e->tv_usec = (int64_t)(ms % 1000) * 1000;
}

static uint32_t ring_space(const evdev_client_t *c) {
  return EVDEV_RING - (c->head - c->tail);
}

// Encola un paquete (n eventos + SYN_REPORT) en todos los clientes. Con
// dev->lock cogido.
static void emit_locked(evdev_dev_t *d, const struct linux_input_event *ev,
                        int n) {
  for (evdev_client_t *c = d->clients; c; c = c->next) {
    if (c->revoked || (d->grab && d->grab != c))
      continue;
    if (ring_space(c) < (uint32_t)n + 2) {
      c->dropped = true;
      continue;
    }
    for (int i = 0; i < n; i++)
      c->ring[c->head++ & (EVDEV_RING - 1)] = ev[i];
    struct linux_input_event syn;
    memset(&syn, 0, sizeof(syn));
    syn.type = EV_SYN;
    syn.code = SYN_REPORT;
    fill_time(&syn);
    c->ring[c->head++ & (EVDEV_RING - 1)] = syn;
    wake_up_all(&c->wq);
  }
}

static void make_ev(struct linux_input_event *e, uint16_t type, uint16_t code,
                    int32_t value) {
  memset(e, 0, sizeof(*e));
  fill_time(e);
  e->type = type;
  e->code = code;
  e->value = value;
}

// ---------------------------------------------------------------------------
// API para drivers
// ---------------------------------------------------------------------------
void evdev_kbd_key(uint16_t key, int value) {
  if (key == 0 || key >= KEY_BITS_BYTES * 8)
    return;
  evdev_dev_t *d = &g_kbd;
  unsigned long fl = spin_lock_irqsave(&d->lock);
  int down = bit_get(d->keystate, key);
  if (value) {
    value = down ? 2 : 1; // autorepeat si ya estaba pulsada
    bit_set(d->keystate, key);
  } else {
    if (!down) {
      spin_unlock_irqrestore(&d->lock, fl);
      return;
    }
    bit_clr(d->keystate, key);
  }
  struct linux_input_event e;
  make_ev(&e, EV_KEY, key, value);
  emit_locked(d, &e, 1);
  spin_unlock_irqrestore(&d->lock, fl);
}

// PS/2 set 1 -> KEY_*. Para codigos < 0x59 sin prefijo, KEY_* == scancode.
static const uint8_t ps2_ext_map[128] = {
    [0x1C] = 96,  /* KP_ENTER */ [0x1D] = 97,  /* RCTRL */
    [0x35] = 98,  /* KP_SLASH */ [0x37] = 99,  /* SYSRQ (PrtSc) */
    [0x38] = 100, /* RALT */     [0x47] = 102, /* HOME */
    [0x48] = 103, /* UP */       [0x49] = 104, /* PGUP */
    [0x4B] = 105, /* LEFT */     [0x4D] = 106, /* RIGHT */
    [0x4F] = 107, /* END */      [0x50] = 108, /* DOWN */
    [0x51] = 109, /* PGDN */     [0x52] = 110, /* INSERT */
    [0x53] = 111, /* DELETE */   [0x5B] = 125, /* LMETA */
    [0x5C] = 126, /* RMETA */    [0x5D] = 127, /* COMPOSE */
};

void evdev_ps2_key(uint8_t code, int ext, int pressed) {
  code &= 0x7F;
  uint16_t key = 0;
  if (ext)
    key = ps2_ext_map[code];
  else if (code >= 1 && code <= 0x58)
    key = code;
  if (key)
    evdev_kbd_key(key, pressed ? 1 : 0);
}

static const uint8_t hid2key[0x74] = {
    [0x04] = 30, 48, 46, 32, 18, 33, 34, 35, 23, 36, 37, 38, 50, 49, 24, 25,
    16, 19, 31, 20, 22, 47, 17, 45, 21, 44, /* 0x04..0x1D: a..z */
    [0x1E] = 2,  3,  4,  5,  6,  7,  8,  9,  10, 11, /* 1..9,0 */
    [0x28] = 28, 1,  14, 15, 57, 12, 13, 26, 27, 43, 43, 39, 40, 41, 51, 52,
    53, 58,                                         /* Enter..CapsLock */
    [0x3A] = 59, 60, 61, 62, 63, 64, 65, 66, 67, 68, 87, 88, /* F1..F12 */
    [0x46] = 99, 70, 119, 110, 102, 104, 111, 107, 109, 106, 105, 108, 103,
    [0x53] = 69, 98, 55, 74, 78, 96, 79, 80, 81, 75, 76, 77, 71, 72, 73, 82,
    83, 86, 127, 116, 117,
};

static const uint8_t hid_mod2key[8] = {29, 42, 56, 125, 97, 54, 100, 126};

void evdev_hid_keys_sync(uint8_t prev[32], const uint8_t cur[32]) {
  for (int u = 1; u < 256; u++) {
    int was = bit_get(prev, u), now = bit_get(cur, u);
    if (was == now)
      continue;
    uint16_t key = 0;
    if (u >= 0xE0 && u <= 0xE7)
      key = hid_mod2key[u - 0xE0];
    else if (u < (int)sizeof(hid2key))
      key = hid2key[u];
    if (key)
      evdev_kbd_key(key, now);
  }
  memcpy(prev, cur, 32);
}

void evdev_mouse_report(int dx, int dy, uint8_t buttons) {
  evdev_dev_t *d = &g_mouse;
  struct linux_input_event ev[5];
  int n = 0;
  unsigned long fl = spin_lock_irqsave(&d->lock);
  uint8_t changed = (uint8_t)(d->mouse_buttons ^ buttons);
  static const uint16_t btn[3] = {BTN_LEFT, BTN_RIGHT, BTN_MIDDLE};
  for (int i = 0; i < 3; i++)
    if (changed & (1u << i))
      make_ev(&ev[n++], EV_KEY, btn[i], (buttons >> i) & 1);
  d->mouse_buttons = buttons;
  if (dx)
    make_ev(&ev[n++], EV_REL, REL_X, dx);
  if (dy)
    make_ev(&ev[n++], EV_REL, REL_Y, dy);
  if (n)
    emit_locked(d, ev, n);
  spin_unlock_irqrestore(&d->lock, fl);
}

void evdev_mouse_wheel(int vertical, int horizontal) {
  evdev_dev_t *d = &g_mouse;
  struct linux_input_event ev[2];
  int n = 0;
  if (vertical)
    make_ev(&ev[n++], EV_REL, REL_WHEEL, vertical);
  if (horizontal)
    make_ev(&ev[n++], EV_REL, REL_HWHEEL, horizontal);
  if (!n)
    return;
  unsigned long fl = spin_lock_irqsave(&d->lock);
  emit_locked(d, ev, n);
  spin_unlock_irqrestore(&d->lock, fl);
}

// ---------------------------------------------------------------------------
// VFS: open crea un cliente; el nodo es propio de cada open.
// ---------------------------------------------------------------------------
// node->priv es el dispositivo hasta open() y el cliente despues; un nodo que
// nunca se abrio (stat, ls) tambien pasa por close().
static evdev_client_t *node_client(vfs_node_t *node) {
  evdev_client_t *c = (evdev_client_t *)node->priv;
  return (c && c->magic == EVDEV_CLIENT_MAGIC) ? c : NULL;
}

static int evdev_open(vfs_node_t *node, int flags) {
  evdev_dev_t *d = (evdev_dev_t *)node->priv;
  if (!d || d->magic != EVDEV_DEV_MAGIC)
    return -ENODEV;
  evdev_client_t *c = (evdev_client_t *)kzalloc(sizeof(*c));
  if (!c)
    return -ENOMEM;
  c->magic = EVDEV_CLIENT_MAGIC;
  c->dev = d;
  c->nonblock = (flags & O_NONBLOCK_L) != 0;
  wait_queue_init(&c->wq);
  unsigned long fl = spin_lock_irqsave(&d->lock);
  c->next = d->clients;
  d->clients = c;
  spin_unlock_irqrestore(&d->lock, fl);
  node->priv = c;
  return 0;
}

static int evdev_close(vfs_node_t *node) {
  evdev_client_t *c = node_client(node);
  if (!c)
    return 0;
  evdev_dev_t *d = c->dev;
  unsigned long fl = spin_lock_irqsave(&d->lock);
  evdev_client_t **pp = &d->clients;
  while (*pp && *pp != c)
    pp = &(*pp)->next;
  if (*pp)
    *pp = c->next;
  if (d->grab == c)
    d->grab = NULL;
  spin_unlock_irqrestore(&d->lock, fl);
  node->priv = NULL;
  kfree(c);
  return 0;
}

static bool client_ready(void *a) {
  evdev_client_t *c = (evdev_client_t *)a;
  return c->head != c->tail || c->dropped || c->revoked;
}

static int64_t evdev_read(vfs_node_t *node, uint64_t off, size_t size,
                          void *buf) {
  (void)off;
  evdev_client_t *c = node_client(node);
  if (!c)
    return -ENODEV;
  if (size < sizeof(struct linux_input_event))
    return -EINVAL;
  for (;;) {
    evdev_dev_t *d = c->dev;
    unsigned long fl = spin_lock_irqsave(&d->lock);
    if (c->revoked) {
      spin_unlock_irqrestore(&d->lock, fl);
      return -ENODEV;
    }
    size_t max = size / sizeof(struct linux_input_event);
    size_t n = 0;
    struct linux_input_event *out = (struct linux_input_event *)buf;
    if (c->dropped) {
      c->dropped = false;
      c->tail = c->head; // descartar lo pendiente, como Linux
      make_ev(&out[n++], EV_SYN, SYN_DROPPED, 0);
    }
    while (n < max && c->tail != c->head)
      out[n++] = c->ring[c->tail++ & (EVDEV_RING - 1)];
    spin_unlock_irqrestore(&d->lock, fl);
    if (n)
      return (int64_t)(n * sizeof(struct linux_input_event));
    if (c->nonblock)
      return -EAGAIN;
    int rc = wait_event_interruptible(&c->wq, client_ready, c);
    if (rc < 0)
      return rc;
  }
}

static int64_t evdev_write(vfs_node_t *node, uint64_t off, size_t size,
                           const void *buf) {
  (void)node;
  (void)off;
  (void)buf;
  return (int64_t)size; // LEDs, FF...: se ignoran
}

static int evdev_poll(vfs_node_t *node, short events) {
  evdev_client_t *c = node_client(node);
  if (!c)
    return 0;
  int rev = 0;
  if ((events & 1) && client_ready(c))
    rev |= 1;
  if (events & 4)
    rev |= 4;
  if (c->revoked)
    rev |= 0x18; // POLLERR | POLLHUP
  return rev;
}

static wait_queue_t *evdev_poll_wq(vfs_node_t *node) {
  evdev_client_t *c = node_client(node);
  return c ? &c->wq : NULL;
}

// ---- ioctl ----------------------------------------------------------------
static int64_t put_user_bytes(uint64_t arg, size_t len, const void *src,
                              size_t srclen) {
  if (!len)
    return 0;
  if (!access_ok((void *)arg, len))
    return -EFAULT;
  uint8_t tmp[KEY_BITS_BYTES + 8];
  if (len > sizeof(tmp))
    len = sizeof(tmp);
  memset(tmp, 0, len);
  memcpy(tmp, src, srclen < len ? srclen : len);
  if (copy_to_user((void *)arg, tmp, len) < 0)
    return -EFAULT;
  return (int64_t)len;
}

static int64_t put_user_str(uint64_t arg, size_t len, const char *s) {
  size_t l = strlen(s) + 1;
  if (l > len)
    l = len;
  if (!l)
    return 0;
  if (!access_ok((void *)arg, l))
    return -EFAULT;
  char tmp[128];
  if (l > sizeof(tmp))
    l = sizeof(tmp);
  memcpy(tmp, s, l);
  tmp[l - 1] = '\0';
  if (copy_to_user((void *)arg, tmp, l) < 0)
    return -EFAULT;
  return (int64_t)l;
}

static int64_t evdev_ioctl(vfs_node_t *node, unsigned long req, uint64_t arg) {
  evdev_client_t *c = node_client(node);
  if (!c)
    return -ENODEV;
  evdev_dev_t *d = c->dev;
  if (((req >> 8) & 0xFF) != 'E')
    return -ENOTTY;
  unsigned nr = (unsigned)(req & 0xFF);
  size_t len = (size_t)((req >> 16) & 0x3FFF);

  if (nr >= 0x20 && nr < 0x40) { // EVIOCGBIT(ev, len)
    unsigned ev = nr - 0x20;
    switch (ev) {
    case 0:
      return put_user_bytes(arg, len, d->evbits, sizeof(d->evbits));
    case EV_KEY:
      return put_user_bytes(arg, len, d->keybits, sizeof(d->keybits));
    case EV_REL:
      return put_user_bytes(arg, len, d->relbits, sizeof(d->relbits));
    default:
      return put_user_bytes(arg, len, "", 0);
    }
  }
  if (nr >= 0x40 && nr < 0x80) // EVIOCGABS
    return -EINVAL;

  switch (nr) {
  case 0x01: { // EVIOCGVERSION
    int v = 0x010001;
    if (!access_ok((void *)arg, 4) || copy_to_user((void *)arg, &v, 4) < 0)
      return -EFAULT;
    return 0;
  }
  case 0x02: { // EVIOCGID
    uint16_t id[4] = {d->bustype, d->vendor, d->product, d->version};
    if (!access_ok((void *)arg, 8) || copy_to_user((void *)arg, id, 8) < 0)
      return -EFAULT;
    return 0;
  }
  case 0x06:
    return put_user_str(arg, len, d->name);
  case 0x07:
    return put_user_str(arg, len, d->phys);
  case 0x08:
    return -ENOENT; // sin uniq
  case 0x09: // EVIOCGPROP
    return put_user_bytes(arg, len, "", 0);
  case 0x18: { // EVIOCGKEY
    unsigned long fl = spin_lock_irqsave(&d->lock);
    uint8_t snap[KEY_BITS_BYTES];
    memcpy(snap, d->keystate, sizeof(snap));
    spin_unlock_irqrestore(&d->lock, fl);
    return put_user_bytes(arg, len, snap, sizeof(snap));
  }
  case 0x19: // EVIOCGLED
  case 0x1b: // EVIOCGSW
    return put_user_bytes(arg, len, "", 0);
  case 0x90: { // EVIOCGRAB (arg = valor)
    unsigned long fl = spin_lock_irqsave(&d->lock);
    int64_t rc = 0;
    if (arg) {
      if (d->grab && d->grab != c)
        rc = -EBUSY;
      else
        d->grab = c;
    } else if (d->grab == c) {
      d->grab = NULL;
    } else {
      rc = -EINVAL;
    }
    spin_unlock_irqrestore(&d->lock, fl);
    return rc;
  }
  case 0x91: { // EVIOCREVOKE
    unsigned long fl = spin_lock_irqsave(&d->lock);
    c->revoked = true;
    if (d->grab == c)
      d->grab = NULL;
    spin_unlock_irqrestore(&d->lock, fl);
    wake_up_all(&c->wq);
    return 0;
  }
  case 0xA0: { // EVIOCSCLOCKID
    int clk = 0;
    if (!access_ok((void *)arg, 4) || copy_from_user(&clk, (void *)arg, 4) < 0)
      return -EFAULT;
    if (clk != 0 && clk != 1 && clk != 7)
      return -EINVAL;
    return 0; // siempre se emite tiempo desde el arranque
  }
  default:
    return -ENOTTY;
  }
}

static vfs_ops_t evdev_ops = {
    .read = evdev_read,
    .write = evdev_write,
    .open = evdev_open,
    .close = evdev_close,
    .poll = evdev_poll,
    .poll_wq = evdev_poll_wq,
    .ioctl = evdev_ioctl,
};

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------
void evdev_init(void) {
  memset(&g_kbd, 0, sizeof(g_kbd));
  memset(&g_mouse, 0, sizeof(g_mouse));

  g_kbd.magic = EVDEV_DEV_MAGIC;
  g_kbd.index = 0;
  g_kbd.name = "AuroraOS Keyboard";
  g_kbd.phys = "aurora/input0";
  g_kbd.bustype = 0x06; // BUS_VIRTUAL
  g_kbd.vendor = 0x1AF4;
  g_kbd.product = 0x0001;
  g_kbd.version = 1;
  bit_set(g_kbd.evbits, EV_SYN);
  bit_set(g_kbd.evbits, EV_KEY);
  for (int k = 1; k < 128; k++)
    bit_set(g_kbd.keybits, k);
  spin_init(&g_kbd.lock);

  g_mouse.magic = EVDEV_DEV_MAGIC;
  g_mouse.index = 1;
  g_mouse.name = "AuroraOS Mouse";
  g_mouse.phys = "aurora/input1";
  g_mouse.bustype = 0x06;
  g_mouse.vendor = 0x1AF4;
  g_mouse.product = 0x0002;
  g_mouse.version = 1;
  bit_set(g_mouse.evbits, EV_SYN);
  bit_set(g_mouse.evbits, EV_KEY);
  bit_set(g_mouse.evbits, EV_REL);
  bit_set(g_mouse.keybits, BTN_LEFT);
  bit_set(g_mouse.keybits, BTN_RIGHT);
  bit_set(g_mouse.keybits, BTN_MIDDLE);
  bit_set(g_mouse.relbits, REL_X);
  bit_set(g_mouse.relbits, REL_Y);
  bit_set(g_mouse.relbits, REL_WHEEL);
  bit_set(g_mouse.relbits, REL_HWHEEL);
  spin_init(&g_mouse.lock);

  // major 13 = input; event0 = minor 64.
  devtmpfs_register("input/event0", DEVT_MKDEV(13, 64), 0660, 0, 0,
                    &evdev_ops, &g_kbd);
  extern void vt_init(void);
  vt_init();
  devtmpfs_register("input/event1", DEVT_MKDEV(13, 65), 0660, 0, 0,
                    &evdev_ops, &g_mouse);
  evdev_publish_sysfs(); // solo memoria: no depende del orden de init del VFS
}

// ---------------------------------------------------------------------------
// /sys y /run/udev (lo que lee libudev/libinput)
// ---------------------------------------------------------------------------
#include "sysfs_dyn.h"

// Mapa de bits como en sysfs: palabras de 64 bits, la mas alta primero, la
// primera sin ceros a la izquierda y el resto con 16 digitos.
static void fmt_bitmap(char *out, size_t cap, const uint8_t *bits, int nbytes) {
  static const char hx[] = "0123456789abcdef";
  int nwords = (nbytes + 7) / 8;
  int top = -1;
  uint64_t w[KEY_BITS_BYTES / 8 + 1];
  for (int i = 0; i < nwords; i++) {
    w[i] = 0;
    for (int b = 0; b < 8 && i * 8 + b < nbytes; b++)
      w[i] |= (uint64_t)bits[i * 8 + b] << (8 * b);
    if (w[i])
      top = i;
  }
  size_t o = 0;
  if (top < 0) {
    if (cap > 1) {
      out[0] = '0';
      out[1] = 0;
    }
    return;
  }
  for (int i = top; i >= 0 && o + 18 < cap; i--) {
    int started = (i != top);
    for (int nib = 15; nib >= 0; nib--) {
      int d = (int)((w[i] >> (4 * nib)) & 0xF);
      if (!started && d == 0 && nib > 0)
        continue;
      started = 1;
      out[o++] = hx[d];
    }
    if (i)
      out[o++] = ' ';
  }
  out[o] = 0;
}

static char g_udev_kind[2]; // 0 = teclado, 1 = raton

static void sysfs_register_dev(evdev_dev_t *d) {
  char base[96], p[128], v[256], keys[160], rel[32], ev[16];
  int k = d->index;
  int minor = 64 + k;
  snprintf(base, sizeof(base), "devices/virtual/input/input%d", k);
  fmt_bitmap(ev, sizeof(ev), d->evbits, sizeof(d->evbits));
  fmt_bitmap(keys, sizeof(keys), d->keybits, sizeof(d->keybits));
  fmt_bitmap(rel, sizeof(rel), d->relbits, sizeof(d->relbits));

#define SD_FILE(sub, ...)                                                      \
  do {                                                                         \
    snprintf(p, sizeof(p), "%s/%s", base, sub);                                \
    snprintf(v, sizeof(v), __VA_ARGS__);                                       \
    sysfs_dyn_set_file(p, v);                                                  \
  } while (0)

  SD_FILE("name", "%s\n", d->name);
  SD_FILE("phys", "%s\n", d->phys);
  SD_FILE("uniq", "\n");
  SD_FILE("properties", "0\n");
  SD_FILE("id/bustype", "%04x\n", d->bustype);
  SD_FILE("id/vendor", "%04x\n", d->vendor);
  SD_FILE("id/product", "%04x\n", d->product);
  SD_FILE("id/version", "%04x\n", d->version);
  SD_FILE("capabilities/ev", "%s\n", ev);
  SD_FILE("capabilities/key", "%s\n", keys);
  SD_FILE("capabilities/rel", "%s\n", rel);
  SD_FILE("capabilities/abs", "0\n");
  SD_FILE("capabilities/msc", "0\n");
  SD_FILE("capabilities/led", "0\n");
  SD_FILE("capabilities/snd", "0\n");
  SD_FILE("capabilities/ff", "0\n");
  SD_FILE("capabilities/sw", "0\n");
  SD_FILE("uevent",
          "PRODUCT=%x/%x/%x/%x\nNAME=\"%s\"\nPHYS=\"%s\"\nPROP=0\nEV=%s\n"
          "KEY=%s\nREL=%s\n",
          d->bustype, d->vendor, d->product, d->version, d->name, d->phys, ev,
          keys, rel);

  snprintf(p, sizeof(p), "%s/subsystem", base);
  sysfs_dyn_add_symlink(p, "/sys/class/input");

  char evdir[112];
  snprintf(evdir, sizeof(evdir), "%s/event%d", base, k);
  snprintf(p, sizeof(p), "%s/dev", evdir);
  snprintf(v, sizeof(v), "13:%d\n", minor);
  sysfs_dyn_set_file(p, v);
  snprintf(p, sizeof(p), "%s/uevent", evdir);
  snprintf(v, sizeof(v), "MAJOR=13\nMINOR=%d\nDEVNAME=input/event%d\n", minor, k);
  sysfs_dyn_set_file(p, v);
  snprintf(p, sizeof(p), "%s/subsystem", evdir);
  sysfs_dyn_add_symlink(p, "/sys/class/input");

  snprintf(p, sizeof(p), "class/input/input%d", k);
  snprintf(v, sizeof(v), "/sys/%s", base);
  sysfs_dyn_add_symlink(p, v);
  snprintf(p, sizeof(p), "class/input/event%d", k);
  snprintf(v, sizeof(v), "/sys/%s", evdir);
  sysfs_dyn_add_symlink(p, v);
  snprintf(p, sizeof(p), "dev/char/13:%d", minor);
  sysfs_dyn_add_symlink(p, v);
#undef SD_FILE
}

static void write_text_file(const char *path, const char *text) {
  if (vfs_create(path, 0644) != 0) {
    // puede existir ya
  }
  vfs_node_t *n = vfs_lookup(path);
  if (!n)
    return;
  if (n->ops && n->ops->write)
    n->ops->write(n, 0, strlen(text), text);
  vfs_node_free(n);
}

// Base de datos de udev en /run/udev/data (la lee libudev para ID_INPUT_*).
// Se llama una vez montado el tmpfs de /run.
void evdev_publish_udev(void) {
  vfs_mkdir("/run/udev", 0755);
  vfs_mkdir("/run/udev/data", 0755);
  static const char kbd_props[] =
      "I:1\nE:ID_INPUT=1\nE:ID_INPUT_KEY=1\nE:ID_INPUT_KEYBOARD=1\n"
      "G:seat\nQ:seat\nV:1\n";
  static const char mouse_props[] =
      "I:1\nE:ID_INPUT=1\nE:ID_INPUT_MOUSE=1\nG:seat\nQ:seat\nV:1\n";
  write_text_file("/run/udev/data/c13:64", kbd_props);
  write_text_file("/run/udev/data/c13:65", mouse_props);
  write_text_file("/run/udev/data/+input:input0", kbd_props);
  write_text_file("/run/udev/data/+input:input1", mouse_props);
  (void)g_udev_kind;
}

void evdev_publish_sysfs(void) {
  sysfs_register_dev(&g_kbd);
  sysfs_register_dev(&g_mouse);
}
