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

  uint8_t repeat_key;
  uint32_t repeat_start_tick;
  uint32_t last_repeat_tick;
} usb_hid_dev_t;

static usb_hid_dev_t *g_hid_devs[MAX_HID];
static int g_hid_count = 0;
static usb_hid_dev_t *g_hid_pending = NULL;

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
    [0x36] = ',',  [0x37] = '.',  [0x38] = '/',
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
    [0x36] = '<', [0x37] = '>',  [0x38] = '?',
};

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

// ---------------------------------------------------------------------------
// Report protocol: teclado
// ---------------------------------------------------------------------------
static void usb_hid_process_kbd_full(usb_hid_dev_t *h, const uint8_t *data,
                                     int len) {
  uint32_t now = (uint32_t)tick_count;

  // -------------------------------------------------------------------
  // 1. Modificadores (usage 0xE0..0xE7, Input Variable).
  //    Se emiten solo en la transición 0 -> 1.
  // -------------------------------------------------------------------
  for (int i = 0; i < h->report.num_fields; i++) {
    hid_field_t *f = &h->report.fields[i];
    if (f->usage_page != 0x07)
      continue;
    if (f->usage < 0xE0 || f->usage > 0xE7)
      continue;
    uint32_t val = hid_get_bits(data, f->bit_offset, f->report_size);
    uint32_t was = hid_get_bits(h->prev_data, f->bit_offset, f->report_size);
    if (val && !was)
      usb_hid_emit(h, (uint8_t)f->usage, 1, 0);
  }

  // -------------------------------------------------------------------
  // 2. Teclas del array (Input Data, Array, Report Size = 8).
  //    NO filtrar por f->usage: ese es el rango (0x00+c), no la tecla
  //    pulsada. El código real está en `val`.
  // -------------------------------------------------------------------
  for (int i = 0; i < h->report.num_fields; i++) {
    hid_field_t *f = &h->report.fields[i];
    if (f->usage_page != 0x07)
      continue;
    if (f->is_variable)
      continue;
    if (f->report_size != 8)
      continue;

    uint32_t val = hid_get_bits(data, f->bit_offset, 8);
    if (val == 0 || val >= 128)
      continue;

    uint32_t was = hid_get_bits(h->prev_data, f->bit_offset, 8);
    if (was == val) {
      // Sigue pulsada en el mismo slot → auto-repeat.
      if (val == h->repeat_key &&
          (now - h->repeat_start_tick) >= REPEAT_DELAY_MS &&
          (now - h->last_repeat_tick) >= REPEAT_INTERVAL_MS) {
        h->last_repeat_tick = now;
        usb_hid_emit(h, (uint8_t)val, 1, 0);
        char c = hid_kbd_ascii[val];
        if (c)
          tty_receive_char(tty_console(), c);
      }
      continue;
    }

    // Tecla nueva. Comprobar que no esté ya en un slot anterior (roll-over).
    int dup = 0;
    for (int j = 0; j < i; j++) {
      hid_field_t *g = &h->report.fields[j];
      if (g->usage_page != 0x07 || g->is_variable || g->report_size != 8)
        continue;
      uint32_t other = hid_get_bits(data, g->bit_offset, 8);
      if (other == val) {
        dup = 1;
        break;
      }
    }
    if (dup)
      continue;

    h->repeat_key = (uint8_t)val;
    h->repeat_start_tick = now;
    h->last_repeat_tick = now;
    usb_hid_emit(h, (uint8_t)val, 1, 0);
    char c = hid_kbd_ascii[val];
    if (c)
      tty_receive_char(tty_console(), c);
  }

  // Si la repeat_key ya no aparece en ningún slot, dejar de repetir.
  if (h->repeat_key) {
    int held = 0;
    for (int i = 0; i < h->report.num_fields; i++) {
      hid_field_t *f = &h->report.fields[i];
      if (f->usage_page != 0x07 || f->is_variable || f->report_size != 8)
        continue;
      if (hid_get_bits(data, f->bit_offset, 8) == h->repeat_key) {
        held = 1;
        break;
      }
    }
    if (!held)
      h->repeat_key = 0;
  }

  memcpy(h->prev_data, data, len < 64 ? len : 64);
}

// ---------------------------------------------------------------------------
// Report protocol: ratón
// ---------------------------------------------------------------------------
static void usb_hid_process_mouse_full(usb_hid_dev_t *h, const uint8_t *data,
                                       int len) {
  (void)len;
  int16_t dx = 0, dy = 0;
  uint8_t buttons = 0;

  for (int i = 0; i < h->report.num_fields; i++) {
    hid_field_t *f = &h->report.fields[i];
    if (f->is_constant)
      continue;
    uint32_t val = hid_get_bits(data, f->bit_offset, f->report_size);
    if (f->is_relative && f->report_size > 1 && f->report_size < 32) {
      if (val & (1U << (f->report_size - 1)))
        val |= ~((1U << f->report_size) - 1);
    }
    if (f->is_button) {
      if (val)
        buttons |= (uint8_t)(1u << (f->usage - 1));
    } else if (f->usage_page == 0x01) {
      switch (f->usage) {
      case 0x30:
        dx = (int16_t)val;
        break;
      case 0x31:
        dy = (int16_t)val;
        break;
      }
    }
  }
  usb_hid_emit(h, buttons, dx, dy);
  memcpy(h->prev_data, data, len < 64 ? len : 64);
}

// ---------------------------------------------------------------------------
// Boot protocol fallback (sin cambios)
// ---------------------------------------------------------------------------
static void usb_hid_process_kbd_boot(usb_hid_dev_t *h, const uint8_t *data,
                                     int len) {
  if (len < 8)
    return;
  uint8_t mods = data[0];
  int shift = !!(mods & 0x22);
  int ctrl = !!(mods & 0x11);
  for (int i = 2; i < 8; i++) {
    uint8_t k = data[i];
    if (k == 0 || k >= 128)
      continue;
    int was = 0;
    for (int j = 2; j < 8; j++)
      if (h->prev_data[j] == k) {
        was = 1;
        break;
      }
    if (was)
      continue;
    usb_hid_emit(h, k, 1, 0);
    char c = shift ? hid_kbd_ascii_shift[k] : hid_kbd_ascii[k];
    if (!c)
      continue;
    if (ctrl) {
      char lower = c;
      if (lower >= 'A' && lower <= 'Z')
        lower += 32;
      if (lower >= 'a' && lower <= 'z')
        c = (char)(lower - 'a' + 1);
    }
    tty_receive_char(tty_console(), c);
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

// ---------------------------------------------------------------------------
// Kthread
// ---------------------------------------------------------------------------
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

    if (h->use_report_protocol) {
      usb_control_transfer(dev, 0x21, USB_HID_REQ_SET_PROTOCOL, 1, itf->number,
                           NULL, 0, 5000);
    } else {
      LOG_INFO("[HID] %s: Fallback a Boot Protocol", h->name);
      h->report_len = (kind == HID_KIND_KEYBOARD) ? 8 : 4;
      usb_control_transfer(dev, 0x21, USB_HID_REQ_SET_PROTOCOL, 0, itf->number,
                           NULL, 0, 5000);
    }
    usb_control_transfer(dev, 0x21, USB_HID_REQ_SET_IDLE, 4, itf->number, NULL,
                         0, 5000);

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