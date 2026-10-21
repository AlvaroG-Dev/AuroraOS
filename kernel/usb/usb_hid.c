// kernel/usb/usb_hid.c
#include "usb_hid.h"
#include "../heap.h"
#include "../input.h"
#include "../klog.h"
#include "../sched.h"
#include "../string.h"
#include "../tty.h"
#include "../uaccess.h"
#include "hid_parser.h"
#include "usb_core.h"

extern volatile uint64_t tick_count;
extern uint32_t fb_width;
extern uint32_t fb_height;

#define HID_KIND_KEYBOARD 1
#define HID_KIND_MOUSE 2
#define MAX_HID 8

#define REPEAT_DELAY_MS 500
#define REPEAT_INTERVAL_MS 32

typedef struct usb_hid_dev {
  usb_device_t *dev;
  usb_interface_t *itf;
  usb_endpoint_t *in_ep;
  int kind;
  int report_len;
  char name[32];

  int use_report_protocol;
  hid_report_t report;
  uint8_t prev_data[64];

  // Última posición escalada de informes HID con coordenadas absolutas.
  int32_t mouse_abs_x;
  int32_t mouse_abs_y;
  uint8_t mouse_abs_valid; // bit 0 = X, bit 1 = Y

  // Modificadores (uno por bit; usage 0xE0..0xE7 de la page 0x07).
  uint8_t mod_lctrl, mod_lshift, mod_lalt, mod_lgui;
  uint8_t mod_rctrl, mod_rshift, mod_ralt, mod_rgui;

  // Auto-repeat
  uint8_t repeat_key;
  uint32_t repeat_start_tick;
  uint32_t last_repeat_tick;
} usb_hid_dev_t;

static usb_hid_dev_t *g_hid_devs[MAX_HID];
static int g_hid_count = 0;
static usb_hid_dev_t *g_hid_pending = NULL;

// ===========================================================================
// Tablas HID usage page 0x07 → ASCII (sin shift / con shift)
// ===========================================================================
static const char hid_kbd_ascii[128] = {
    [0x04] = 'a',  [0x05] = 'b',  [0x06] = 'c',  [0x07] = 'd',  [0x08] = 'e',
    [0x09] = 'f',  [0x0A] = 'g',  [0x0B] = 'h',  [0x0C] = 'i',  [0x0D] = 'j',
    [0x0E] = 'k',  [0x0F] = 'l',  [0x10] = 'm',  [0x11] = 'n',  [0x12] = 'o',
    [0x13] = 'p',  [0x14] = 'q',  [0x15] = 'r',  [0x16] = 's',  [0x17] = 't',
    [0x18] = 'u',  [0x19] = 'v',  [0x1A] = 'w',  [0x1B] = 'x',  [0x1C] = 'y',
    [0x1D] = 'z',  [0x1E] = '1',  [0x1F] = '2',  [0x20] = '3',  [0x21] = '4',
    [0x22] = '5',  [0x23] = '6',  [0x24] = '7',  [0x25] = '8',  [0x26] = '9',
    [0x27] = '0',  [0x28] = '\r', [0x29] = 0x1B, [0x2A] = 0x7F, [0x2B] = '\t',
    [0x2C] = ' ',  [0x2D] = '-',  [0x2E] = '=',  [0x2F] = '[',  [0x30] = ']',
    [0x31] = '\\', [0x32] = '#',  [0x33] = ';',  [0x34] = '\'', [0x35] = '`',
    [0x36] = ',',  [0x37] = '.',  [0x38] = '/',  [0x54] = '/',  [0x55] = '*',
    [0x56] = '-',  [0x57] = '+',  [0x58] = '\r', [0x62] = '0',  [0x63] = '.',
};
static const char hid_kbd_ascii_shift[128] = {
    [0x04] = 'A', [0x05] = 'B',  [0x06] = 'C',  [0x07] = 'D',  [0x08] = 'E',
    [0x09] = 'F', [0x0A] = 'G',  [0x0B] = 'H',  [0x0C] = 'I',  [0x0D] = 'J',
    [0x0E] = 'K', [0x0F] = 'L',  [0x10] = 'M',  [0x11] = 'N',  [0x12] = 'O',
    [0x13] = 'P', [0x14] = 'Q',  [0x15] = 'R',  [0x16] = 'S',  [0x17] = 'T',
    [0x18] = 'U', [0x19] = 'V',  [0x1A] = 'W',  [0x1B] = 'X',  [0x1C] = 'Y',
    [0x1D] = 'Z', [0x1E] = '!',  [0x1F] = '@',  [0x20] = '#',  [0x21] = '$',
    [0x22] = '%', [0x23] = '^',  [0x24] = '&',  [0x25] = '*',  [0x26] = '(',
    [0x27] = ')', [0x28] = '\r', [0x29] = 0x1B, [0x2A] = 0x7F, [0x2B] = '\t',
    [0x2C] = ' ', [0x2D] = '_',  [0x2E] = '+',  [0x2F] = '{',  [0x30] = '}',
    [0x31] = '|', [0x32] = '~',  [0x33] = ':',  [0x34] = '"',  [0x35] = '~',
    [0x36] = '<', [0x37] = '>',  [0x38] = '?',  [0x54] = '/',  [0x55] = '*',
    [0x56] = '-', [0x57] = '+',  [0x58] = '\r', [0x62] = '0',  [0x63] = '.',
};

// ===========================================================================
// Tabla HID usage page 0x07 → secuencias ANSI (igual patrón que ps2.c).
// Estas teclas NO son ASCII; se entregan al TTY como escape sequences.
// ===========================================================================
static const char *hid_usage_ansi(uint32_t usage) {
  switch (usage) {
  case 0x49:
    return "\x1b[2~"; // Insert
  case 0x4A:
    return "\x1b[H"; // Home
  case 0x4B:
    return "\x1b[5~"; // Page Up
  case 0x4C:
    return "\x1b[3~"; // Delete
  case 0x4D:
    return "\x1b[F"; // End
  case 0x4E:
    return "\x1b[6~"; // Page Down
  case 0x4F:
    return "\x1b[C"; // Right
  case 0x50:
    return "\x1b[D"; // Left
  case 0x51:
    return "\x1b[B"; // Down
  case 0x52:
    return "\x1b[A"; // Up
  case 0x3A:
    return "\x1bOP"; // F1
  case 0x3B:
    return "\x1bOQ"; // F2
  case 0x3C:
    return "\x1bOR"; // F3
  case 0x3D:
    return "\x1bOS"; // F4
  case 0x3E:
    return "\x1b[15~"; // F5
  case 0x3F:
    return "\x1b[17~"; // F6
  case 0x40:
    return "\x1b[18~"; // F7
  case 0x41:
    return "\x1b[19~"; // F8
  case 0x42:
    return "\x1b[20~"; // F9
  case 0x43:
    return "\x1b[21~"; // F10
  case 0x44:
    return "\x1b[23~"; // F11
  case 0x45:
    return "\x1b[24~"; // F12
  default:
    return NULL;
  }
}

// ===========================================================================
// Helpers
// ===========================================================================
static uint32_t hid_get_bits(const uint8_t *data, uint32_t bit_offset,
                             uint32_t size) {
  uint32_t val = 0;
  for (uint32_t b = 0; b < size; b++) {
    uint32_t bit = bit_offset + b;
    if (data[bit / 8] & (1 << (bit % 8)))
      val |= (1U << b);
  }
  return val;
}

/* Los offsets del parser son relativos al payload y no incluyen el byte
 * Report ID. Solo deben procesarse los campos del ID que llegó por USB. */
static int hid_field_matches_report(const usb_hid_dev_t *h,
                                    const uint8_t *data,
                                    const hid_field_t *f) {
  return !h->report.has_report_id || data[0] == f->report_id;
}

static uint32_t hid_field_bits(const usb_hid_dev_t *h, const uint8_t *data,
                               const hid_field_t *f) {
  if (!hid_field_matches_report(h, data, f))
    return 0;
  uint32_t bit_offset =
      f->bit_offset + (h->report.has_report_id ? 8u : 0u);
  return hid_get_bits(data, bit_offset, f->report_size);
}

static int32_t hid_field_signed_value(const hid_field_t *f, uint32_t value) {
  if (f->report_size > 0 && f->report_size < 32 &&
      (f->is_relative || f->logical_min < 0) &&
      (value & (1u << (f->report_size - 1)))) {
    value |= ~((1u << f->report_size) - 1u);
  }
  return (int32_t)value;
}

static int32_t hid_absolute_to_screen(const hid_field_t *f, uint32_t value,
                                      int vertical) {
  int64_t v = hid_field_signed_value(f, value);
  int64_t minv = f->logical_min;
  int64_t maxv = f->logical_max;
  uint32_t extent = vertical ? fb_height : fb_width;

  if (extent <= 1 || maxv <= minv)
    return 0;
  if (v < minv)
    v = minv;
  if (v > maxv)
    v = maxv;

  return (int32_t)(((v - minv) * (int64_t)(extent - 1)) / (maxv - minv));
}

/* Los HID no Boot (p. ej. VirtualBox USB Tablet) se clasifican por las
 * usages de entrada del Report Descriptor, no por bInterfaceProtocol. */
static int hid_infer_kind(const hid_report_t *report) {
  int keyboard = 0;
  int has_x = 0;
  int has_y = 0;

  for (int i = 0; i < report->num_fields; i++) {
    const hid_field_t *f = &report->fields[i];
    if (f->is_constant)
      continue;
    if (f->usage_page == 0x07 && f->usage >= 0x04 && f->usage <= 0xE7)
      keyboard = 1;
    if (f->usage_page == 0x01 && f->usage == 0x30)
      has_x = 1;
    if (f->usage_page == 0x01 && f->usage == 0x31)
      has_y = 1;
  }

  if (keyboard)
    return HID_KIND_KEYBOARD;
  if (has_x && has_y)
    return HID_KIND_MOUSE;
  return 0;
}

static void usb_hid_emit(usb_hid_dev_t *h, uint8_t code, int value,
                         int value2) {
  input_event_t ev;
  ev.type = h->kind == HID_KIND_KEYBOARD ? INPUT_EV_KEY : INPUT_EV_MOUSE;
  ev.code = code;
  ev.value = value;
  ev.value2 = value2;
  ev.timestamp = (uint32_t)tick_count;
  input_push(&ev);
}

static inline int hid_mods_shift(const usb_hid_dev_t *h) {
  return h->mod_lshift || h->mod_rshift;
}
static inline int hid_mods_ctrl(const usb_hid_dev_t *h) {
  return h->mod_lctrl || h->mod_rctrl;
}

// Emite UNA pulsación al input subsystem y al TTY.
//
// Orden de decisión:
//   1. Si la tecla tiene secuencia ANSI (flechas, F1-F12, etc.) → ANSI al TTY.
//   2. Si no, traducción ASCII aplicando shift y luego ctrl.
//   3. Si no hay traducción, solo el input_event (el compositor puede
//      hacer algo con ella).
static void usb_hid_kbd_emit_key(usb_hid_dev_t *h, uint32_t usage) {
  if (usage == 0 || usage >= 128)
    return;

  // El input subsystem recibe siempre la tecla (usage HID).
  usb_hid_emit(h, (uint8_t)usage, 1, 0);

  // Teclas extendidas: secuencia ANSI al TTY.
  const char *seq = hid_usage_ansi(usage);
  if (seq) {
    tty_t *tty = tty_console();
    if (tty) {
      for (const char *p = seq; *p; p++)
        tty_receive_char(tty, *p);
    }
    return;
  }

  // Teclas ASCII con modificadores.
  char c =
      hid_mods_shift(h) ? hid_kbd_ascii_shift[usage] : hid_kbd_ascii[usage];
  if (c == 0)
    return;

  if (hid_mods_ctrl(h)) {
    char lower = c;
    if (lower >= 'A' && lower <= 'Z')
      lower += 32;
    if (lower >= 'a' && lower <= 'z')
      c = (char)(lower - 'a' + 1);
  }

  tty_t *tty = tty_console();
  if (tty)
    tty_receive_char(tty, c);
}

// ===========================================================================
// Report Protocol: teclado
// ===========================================================================
static void usb_hid_process_kbd_full(usb_hid_dev_t *h, const uint8_t *data,
                                     int len) {
  uint32_t now = (uint32_t)tick_count;

  // 1. Actualizar modificadores (usage 0xE0..0xE7, bits individuales).
  for (int i = 0; i < h->report.num_fields; i++) {
    hid_field_t *f = &h->report.fields[i];
    if (!hid_field_matches_report(h, data, f))
      continue;
    if (f->usage_page != 0x07)
      continue;
    if (f->usage < 0xE0 || f->usage > 0xE7)
      continue;
    uint32_t v = hid_field_bits(h, data, f) ? 1 : 0;
    uint32_t was =
        hid_field_bits(h, h->prev_data, f) ? 1 : 0;
    switch (f->usage) {
    case 0xE0:
      h->mod_lctrl = (uint8_t)v;
      break;
    case 0xE1:
      h->mod_lshift = (uint8_t)v;
      break;
    case 0xE2:
      h->mod_lalt = (uint8_t)v;
      break;
    case 0xE3:
      h->mod_lgui = (uint8_t)v;
      break;
    case 0xE4:
      h->mod_rctrl = (uint8_t)v;
      break;
    case 0xE5:
      h->mod_rshift = (uint8_t)v;
      break;
    case 0xE6:
      h->mod_ralt = (uint8_t)v;
      break;
    case 0xE7:
      h->mod_rgui = (uint8_t)v;
      break;
    }
    // Emitir evento de modificador al input subsystem al pulsar.
    if (v && !was)
      usb_hid_emit(h, (uint8_t)f->usage, 1, 0);
  }

  // 2. Teclas del array (Input Data, Array, Report Size = 8).
  for (int i = 0; i < h->report.num_fields; i++) {
    hid_field_t *f = &h->report.fields[i];
    if (!hid_field_matches_report(h, data, f))
      continue;
    if (f->usage_page != 0x07)
      continue;
    if (f->is_variable)
      continue;
    if (f->report_size != 8)
      continue;

    uint32_t val = hid_field_bits(h, data, f);
    if (val == 0 || val >= 128)
      continue;

    uint32_t was = hid_field_bits(h, h->prev_data, f);
    if (was == val) {
      // Sigue pulsada en el mismo slot → auto-repeat.
      if (val == h->repeat_key &&
          (now - h->repeat_start_tick) >= REPEAT_DELAY_MS &&
          (now - h->last_repeat_tick) >= REPEAT_INTERVAL_MS) {
        h->last_repeat_tick = now;
        usb_hid_kbd_emit_key(h, val);
      }
      continue;
    }

    // Tecla nueva. Comprobar roll-over (ya está en un slot anterior).
    int dup = 0;
    for (int j = 0; j < i; j++) {
      hid_field_t *g = &h->report.fields[j];
      if (g->usage_page != 0x07 || g->is_variable || g->report_size != 8)
        continue;
      if (hid_get_bits(data, g->bit_offset, 8) == val) {
        dup = 1;
        break;
      }
    }
    if (dup)
      continue;

    h->repeat_key = (uint8_t)val;
    h->repeat_start_tick = now;
    h->last_repeat_tick = now;
    usb_hid_kbd_emit_key(h, val);
  }

  // 3. Si la repeat_key ya no aparece en ningún slot, dejar de repetir.
  if (h->repeat_key) {
    int held = 0;
    for (int i = 0; i < h->report.num_fields; i++) {
      hid_field_t *f = &h->report.fields[i];
      if (f->usage_page != 0x07 || f->is_variable || f->report_size != 8)
        continue;
      if (hid_field_bits(h, data, f) == h->repeat_key) {
        held = 1;
        break;
      }
    }
    if (!held)
      h->repeat_key = 0;
  }

  memcpy(h->prev_data, data, len < 64 ? len : 64);
}

// ===========================================================================
// Report Protocol: ratón (sin cambios)
// ===========================================================================
static void usb_hid_process_mouse_full(usb_hid_dev_t *h, const uint8_t *data,
                                       int len) {
  int32_t dx = 0, dy = 0;
  int32_t abs_x = 0, abs_y = 0;
  int has_abs_x = 0, has_abs_y = 0;
  uint8_t buttons = 0;

  for (int i = 0; i < h->report.num_fields; i++) {
    hid_field_t *f = &h->report.fields[i];
    if (f->is_constant || !hid_field_matches_report(h, data, f))
      continue;

    uint32_t raw = hid_field_bits(h, data, f);
    int32_t val = hid_field_signed_value(f, raw);
    if (f->is_button) {
      if (val && f->usage >= 1 && f->usage <= 8)
        buttons |= (uint8_t)(1u << (f->usage - 1));
    } else if (f->usage_page == 0x01) {
      switch (f->usage) {
      case 0x30: // X
        if (f->is_relative) {
          dx = val;
        } else {
          abs_x = hid_absolute_to_screen(f, raw, 0);
          has_abs_x = 1;
        }
        break;
      case 0x31: // Y
        if (f->is_relative) {
          dy = val;
        } else {
          abs_y = hid_absolute_to_screen(f, raw, 1);
          has_abs_y = 1;
        }
        break;
      }
    }
  }

  // Tabletas HID envían coordenadas absolutas. El compositor actual consume
  // deltas, así que se escala a pantalla y se resta la última posición.
  if (has_abs_x) {
    dx = (h->mouse_abs_valid & 0x01) ? abs_x - h->mouse_abs_x : 0;
    h->mouse_abs_x = abs_x;
    h->mouse_abs_valid |= 0x01;
  }
  if (has_abs_y) {
    dy = (h->mouse_abs_valid & 0x02) ? abs_y - h->mouse_abs_y : 0;
    h->mouse_abs_y = abs_y;
    h->mouse_abs_valid |= 0x02;
  }

  // INPUT_EV_MOUSE transporta deltas firmados de 16 bits.
  if (dx > 32767)
    dx = 32767;
  if (dx < -32768)
    dx = -32768;
  if (dy > 32767)
    dy = 32767;
  if (dy < -32768)
    dy = -32768;

  usb_hid_emit(h, buttons, (int16_t)dx, (int16_t)dy);
  memcpy(h->prev_data, data, len < 64 ? len : 64);
}

// ===========================================================================
// Boot Protocol fallback: teclado.
//
// El report de boot USB trae los usages HID igual (no scancodes PS/2),
// así que reutilizamos las mismas tablas y helpers.
// ===========================================================================
static void usb_hid_process_kbd_boot(usb_hid_dev_t *h, const uint8_t *data,
                                     int len) {
  if (len < 8)
    return;
  uint8_t mods = data[0];
  h->mod_lctrl = !!(mods & 0x01);
  h->mod_lshift = !!(mods & 0x02);
  h->mod_lalt = !!(mods & 0x04);
  h->mod_lgui = !!(mods & 0x08);
  h->mod_rctrl = !!(mods & 0x10);
  h->mod_rshift = !!(mods & 0x20);
  h->mod_ralt = !!(mods & 0x40);
  h->mod_rgui = !!(mods & 0x80);

  uint32_t now = (uint32_t)tick_count;

  for (int i = 2; i < 8; i++) {
    uint8_t k = data[i];
    if (k == 0 || k >= 128)
      continue;

    uint8_t was = 0;
    for (int j = 2; j < 8; j++)
      if (h->prev_data[j] == k) {
        was = 1;
        break;
      }
    if (was) {
      if (k == h->repeat_key &&
          (now - h->repeat_start_tick) >= REPEAT_DELAY_MS &&
          (now - h->last_repeat_tick) >= REPEAT_INTERVAL_MS) {
        h->last_repeat_tick = now;
        usb_hid_kbd_emit_key(h, k);
      }
      continue;
    }

    int dup = 0;
    for (int j = 2; j < i; j++)
      if (data[j] == k) {
        dup = 1;
        break;
      }
    if (dup)
      continue;

    h->repeat_key = k;
    h->repeat_start_tick = now;
    h->last_repeat_tick = now;
    usb_hid_kbd_emit_key(h, k);
  }

  if (h->repeat_key) {
    int held = 0;
    for (int j = 2; j < 8; j++)
      if (data[j] == h->repeat_key) {
        held = 1;
        break;
      }
    if (!held)
      h->repeat_key = 0;
  }

  memcpy(h->prev_data, data, 8);
}

static void usb_hid_process_mouse_boot(usb_hid_dev_t *h, const uint8_t *data,
                                       int len) {
  if (len < 3)
    return;
  uint8_t btn = data[0] & 0x07;
  int16_t dx = (int8_t)data[1];
  int16_t dy = (int8_t)data[2];
  usb_hid_emit(h, btn, dx, dy);
  memcpy(h->prev_data, data, len < 8 ? len : 8);
}

// ===========================================================================
// Kthread
// ===========================================================================
static void usb_hid_device_kthread(void) {
  usb_hid_dev_t *d = g_hid_pending;
  g_hid_pending = NULL;
  if (!d)
    return;

  LOG_INFO("[HID] %s: kthread iniciado (%s, %d bytes)", d->name,
           d->use_report_protocol ? "Report Protocol" : "Boot Protocol",
           d->report_len);

  uint8_t buf[64];
  int err_count = 0;

  while (1) {
    int actual = 0;
    int len = d->report_len;
    if (len <= 0 || len > (int)sizeof(buf))
      len = 8;

    int rc =
        d->dev->hcd->interrupt_xfer(d->dev, d->in_ep, buf, len, 0, &actual);
    if (rc == 0 && actual > 0) {
      err_count = 0;
      if (d->kind == HID_KIND_KEYBOARD) {
        if (d->use_report_protocol)
          usb_hid_process_kbd_full(d, buf, actual);
        else
          usb_hid_process_kbd_boot(d, buf, actual);
      } else if (d->kind == HID_KIND_MOUSE) {
        if (d->use_report_protocol)
          usb_hid_process_mouse_full(d, buf, actual);
        else
          usb_hid_process_mouse_boot(d, buf, actual);
      }
    } else if (rc < 0) {
      if (++err_count == 1 || (err_count & 0x3F) == 0)
        LOG_WARN("[HID] %s: transfer rc=%d", d->name, rc);
      for (int i = 0; i < 50; i++)
        sched_yield();
    }
  }
}

static int usb_hid_spawn_kthread(usb_hid_dev_t *h) {
  while (g_hid_pending != NULL)
    sched_yield();
  g_hid_pending = h;
  if (!sched_create_task(usb_hid_device_kthread)) {
    g_hid_pending = NULL;
    return -ENOMEM;
  }
  while (g_hid_pending != NULL)
    sched_yield();
  return 0;
}

int usb_hid_get_report_descriptor(usb_device_t *dev, uint8_t intf_num,
                                  void *buf, uint16_t len) {
  return usb_control_transfer(dev, 0x81, 0x06, (0x22 << 8) | 0, intf_num, buf,
                              len, 5000);
}

int usb_hid_probe(usb_device_t *dev) {
  if (!dev || !dev->hcd || !dev->hcd->configure_ep || !dev->hcd->interrupt_xfer)
    return -EINVAL;
  usb_config_t *cfg = dev->configs[0];
  if (!cfg)
    return -EINVAL;

  int probed = 0;
  for (int i = 0; i < cfg->num_interface_structs; i++) {
    usb_interface_t *itf = &cfg->interfaces[i];
    if (itf->class != USB_HID_CLASS)
      continue;

    int kind = 0;
    if (itf->subclass == USB_HID_SUBCLASS_BOOT) {
      if (itf->protocol == USB_HID_PROTO_KBD)
        kind = HID_KIND_KEYBOARD;
      else if (itf->protocol == USB_HID_PROTO_MOUSE)
        kind = HID_KIND_MOUSE;
    }

    usb_endpoint_t *in_ep = NULL;
    for (int j = 0; j < itf->num_endpoints; j++) {
      usb_endpoint_t *ep = &itf->endpoints[j];
      if ((ep->addr & 0x80) && ep->type == USB_EP_INTERRUPT) {
        in_ep = ep;
        break;
      }
    }
    if (!in_ep)
      continue;

    usb_hid_dev_t *h = kzalloc(sizeof(*h));
    if (!h)
      return -ENOMEM;
    h->dev = dev;
    h->itf = itf;
    h->in_ep = in_ep;
    h->kind = kind;
    h->report_len = in_ep->max_packet;
    if (h->report_len > 8)
      h->report_len = 8;
    snprintf(h->name, sizeof(h->name), "usbhid-%s-%u",
             kind == HID_KIND_KEYBOARD ? "kbd" : "mouse", itf->number);

    uint8_t *raw_desc = kmalloc(1024);
    if (raw_desc) {
      int rc = usb_hid_get_report_descriptor(dev, itf->number, raw_desc, 1024);
      if (rc > 0) {
        if (hid_parse_report_descriptor(raw_desc, rc, &h->report) == 0 &&
            h->report.num_fields > 0 && h->report.input_len > 0) {
          h->use_report_protocol = 1;
          h->report_len =
              h->report.input_len + (h->report.has_report_id ? 1 : 0);
          if (h->report_len > 64)
            h->report_len = 64;
          LOG_INFO("[HID] %s: Report Descriptor OK (in=%d out=%d feat=%d id=%d "
                   "campos=%d)",
                   h->name, h->report.input_len, h->report.output_len,
                   h->report.feature_len, h->report.has_report_id,
                   h->report.num_fields);
        }
      }
      kfree(raw_desc);
    }

    if (kind == 0 && h->use_report_protocol)
      kind = hid_infer_kind(&h->report);
    h->kind = kind;
    snprintf(h->name, sizeof(h->name), "usbhid-%s-%u",
             kind == HID_KIND_KEYBOARD ? "kbd" : "mouse", itf->number);

    /* Un dispositivo HID que no es Boot ya usa Report Protocol de forma
     * predeterminada. No enviarle SET_PROTOCOL/SET_IDLE: tablets genéricas
     * de VirtualBox pueden responder STALL y forzar recuperación de EP0. */
    if (itf->subclass == USB_HID_SUBCLASS_BOOT) {
      if (h->use_report_protocol) {
        (void)usb_control_transfer(dev, 0x21, USB_HID_REQ_SET_PROTOCOL, 1,
                                   itf->number, NULL, 0, 1000);
      } else {
        if (kind == 0) {
          LOG_INFO("[HID] iface %u omitida: Boot Protocol no reconocido",
                   itf->number);
          kfree(h);
          continue;
        }
        LOG_INFO("[HID] %s: Fallback a Boot Protocol", h->name);
        h->report_len = (kind == HID_KIND_KEYBOARD) ? 8 : 4;
        (void)usb_control_transfer(dev, 0x21, USB_HID_REQ_SET_PROTOCOL, 0,
                                   itf->number, NULL, 0, 1000);
      }
      (void)usb_control_transfer(dev, 0x21, USB_HID_REQ_SET_IDLE, 4,
                                 itf->number, NULL, 0, 1000);
    } else if (!h->use_report_protocol || kind == 0) {
      LOG_INFO("[HID] iface %u omitida: Report Descriptor no compatible",
               itf->number);
      kfree(h);
      continue;
    }

    int rc = dev->hcd->configure_ep(dev, in_ep);
    if (rc < 0) {
      kfree(h);
      continue;
    }

    LOG_INFO("[HID] %s: iface %u, EP%u IN mps=%u interval=%u", h->name,
             itf->number, in_ep->addr & 0x0F, in_ep->max_packet,
             in_ep->interval);

    if (g_hid_count >= MAX_HID) {
      kfree(h);
      continue;
    }
    g_hid_devs[g_hid_count++] = h;

    if (usb_hid_spawn_kthread(h) != 0) {
      g_hid_count--;
      kfree(h);
      continue;
    }
    probed++;
  }
  return probed;
}