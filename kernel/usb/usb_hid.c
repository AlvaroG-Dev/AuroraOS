// kernel/usb/usb_hid.c
//
// Driver HID genérico (boot protocol). Teclado + ratón.
// Un kthread por device, timeout infinito.
//
// Auto-repeat: SET_IDLE(4) hace que el device reenvíe el report cada
// 16 ms aunque no cambie. Aprovechamos ese flujo periódico para emitir
// repeticiones de la tecla mantenida (500 ms delay, 32 ms intervalo,
// como Linux input core).

#include "usb_hid.h"
#include "../heap.h"
#include "../input.h"
#include "../klog.h"
#include "../sched.h"
#include "../string.h"
#include "../tty.h"
#include "../uaccess.h"
#include "usb_core.h"

extern volatile uint64_t tick_count;

#define HID_KIND_KEYBOARD 1
#define HID_KIND_MOUSE 2
#define MAX_HID 8

// Auto-repeat (mismos valores que Linux input core)
#define REPEAT_DELAY_MS 500
#define REPEAT_INTERVAL_MS 32

typedef struct usb_hid_dev {
  usb_device_t *dev;
  usb_interface_t *itf;
  usb_endpoint_t *in_ep;
  int kind;
  int report_len;
  uint8_t prev_report[8];
  char name[32];

  // Auto-repeat state (solo keyboard).
  uint8_t repeat_key;         // keycode HID que está repitiendo, 0 = ninguno
  uint32_t repeat_start_tick; // tick_count cuando empezó a estar pulsada
  uint32_t last_repeat_tick;  // tick_count del último repeat emitido
} usb_hid_dev_t;

static usb_hid_dev_t *g_hid_devs[MAX_HID];
static int g_hid_count = 0;
static usb_hid_dev_t *g_hid_pending = NULL;

// ===========================================================================
// Tablas HID usage → ASCII (US layout, boot protocol)
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

// ===========================================================================
// Emite una pulsación al subsistema de input + TTY.
// ===========================================================================
static void usb_hid_kbd_emit(usb_hid_dev_t *h, uint8_t k, int shift, int ctrl) {
  uint32_t now = (uint32_t)tick_count;

  input_event_t ev;
  ev.type = INPUT_EV_KEY;
  ev.code = k;
  ev.value = 1;
  ev.value2 = 0;
  ev.timestamp = now;
  input_push(&ev);

  char c = shift ? hid_kbd_ascii_shift[k] : hid_kbd_ascii[k];
  if (c == 0)
    return;

  if (ctrl) {
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
// Procesa un report de teclado.
//
// Con SET_IDLE(4), el device envía report cada 16 ms aunque no cambie.
// Eso nos da un "latido" para hacer auto-repeat.
// ===========================================================================
static void usb_hid_kbd_handle(usb_hid_dev_t *h, const uint8_t *report,
                               int len) {
  if (len < 8)
    return;

  uint32_t now = (uint32_t)tick_count;
  uint8_t mods = report[0];
  int shift = !!(mods & 0x22); // LShift | RShift
  int ctrl = !!(mods & 0x11);  // LCtrl  | RCtrl

  for (int i = 2; i < 8; i++) {
    uint8_t k = report[i];
    if (k == 0 || k >= 128)
      continue;

    int was_pressed = 0;
    for (int j = 2; j < 8; j++) {
      if (h->prev_report[j] == k) {
        was_pressed = 1;
        break;
      }
    }

    int emit = 0;
    if (!was_pressed) {
      // Primera pulsación.
      emit = 1;
      h->repeat_key = k;
      h->repeat_start_tick = now;
      h->last_repeat_tick = now;
    } else if (k == h->repeat_key && h->repeat_key != 0) {
      // Sigue pulsada. ¿Toca repetir?
      if ((now - h->repeat_start_tick) >= REPEAT_DELAY_MS &&
          (now - h->last_repeat_tick) >= REPEAT_INTERVAL_MS) {
        emit = 1;
        h->last_repeat_tick = now;
      }
    }

    if (emit)
      usb_hid_kbd_emit(h, k, shift, ctrl);
  }

  // Si la tecla de repeat ya no está pulsada, dejar de repetir.
  if (h->repeat_key != 0) {
    int still_held = 0;
    for (int j = 2; j < 8; j++) {
      if (report[j] == h->repeat_key) {
        still_held = 1;
        break;
      }
    }
    if (!still_held)
      h->repeat_key = 0;
  }

  memcpy(h->prev_report, report, 8);
}

// ===========================================================================
// Procesa un report de ratón (boot protocol, 3 o 4 bytes).
//
// QEMU's usb-mouse sobre qemu-xhci reporta con signos opuestos al HID
// estándar respecto al compositor de Aurora (que sigue la convención
// de ps2.c). Invertimos los dos ejes.
// ===========================================================================
static void usb_hid_mouse_handle(usb_hid_dev_t *h, const uint8_t *report,
                                 int len) {
  (void)h;
  if (len < 3)
    return;

  uint8_t btn = report[0] & 0x07;
  int16_t raw_dx = (int8_t)report[1];
  int16_t raw_dy = (int8_t)report[2];
  int16_t screen_dx = (int16_t)(raw_dx); // X: sin invertir (ya correcto)
  int16_t screen_dy = raw_dy;            // ya viene "positivo = abajo"

  input_event_t ev;
  ev.type = INPUT_EV_MOUSE;
  ev.code = btn;
  ev.value = screen_dx;
  ev.value2 = screen_dy;
  ev.timestamp = (uint32_t)tick_count;
  input_push(&ev);
}

// ===========================================================================
// Kthread por device. Timeout infinito: solo despierta cuando llega un
// report del HC (que con SET_IDLE(4) ocurre al menos cada 16 ms).
// ===========================================================================
static void usb_hid_device_kthread(void) {
  usb_hid_dev_t *d = g_hid_pending;
  g_hid_pending = NULL;
  if (!d) {
    LOG_ERR("[HID] kthread sin pending device");
    return;
  }

  LOG_INFO("[HID] %s: kthread iniciado (report_len=%d)", d->name,
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
      if (d->kind == HID_KIND_KEYBOARD)
        usb_hid_kbd_handle(d, buf, actual);
      else if (d->kind == HID_KIND_MOUSE)
        usb_hid_mouse_handle(d, buf, actual);
    } else if (rc < 0) {
      err_count++;
      if (err_count == 1 || (err_count & 0x3F) == 0)
        LOG_WARN("[HID] %s: transfer rc=%d (fallo #%d)", d->name, rc,
                 err_count);
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

// ===========================================================================
// Probe
// ===========================================================================
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
    if (kind == 0) {
      LOG_INFO("[HID] iface %u: HID proto=%u (no boot kbd/mouse, skip)",
               itf->number, itf->protocol);
      continue;
    }

    usb_endpoint_t *in_ep = NULL;
    for (int j = 0; j < itf->num_endpoints; j++) {
      usb_endpoint_t *ep = &itf->endpoints[j];
      if ((ep->addr & 0x80) && ep->type == USB_EP_INTERRUPT) {
        in_ep = ep;
        break;
      }
    }
    if (!in_ep) {
      LOG_WARN("[HID] iface %u: sin interrupt IN", itf->number);
      continue;
    }

    usb_hid_dev_t *h = (usb_hid_dev_t *)kzalloc(sizeof(*h));
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

    // SET_PROTOCOL(boot=0). bmRequestType = class|interface|host-to-dev.
    int rc = usb_control_transfer(dev, 0x21, USB_HID_REQ_SET_PROTOCOL, 0,
                                  itf->number, NULL, 0, 5000);
    if (rc < 0)
      LOG_WARN("[HID] %s: SET_PROTOCOL(0) falló: %d", h->name, rc);

    // SET_IDLE(4) = 16 ms. El device reenvía el report aunque no cambie.
    // Necesario para auto-repeat software.
    rc = usb_control_transfer(dev, 0x21, USB_HID_REQ_SET_IDLE, 4, itf->number,
                              NULL, 0, 5000);
    if (rc < 0)
      LOG_WARN("[HID] %s: SET_IDLE(4) falló: %d", h->name, rc);

    // Configure Endpoint (interrupt IN).
    rc = dev->hcd->configure_ep(dev, in_ep);
    if (rc < 0) {
      LOG_ERR("[HID] %s: configure_ep falló: %d", h->name, rc);
      kfree(h);
      continue;
    }

    LOG_INFO("[HID] %s: iface %u, EP%u IN mps=%u interval=%u", h->name,
             itf->number, in_ep->addr & 0x0F, in_ep->max_packet,
             in_ep->interval);

    if (g_hid_count >= MAX_HID) {
      LOG_ERR("[HID] tabla llena");
      kfree(h);
      continue;
    }
    g_hid_devs[g_hid_count++] = h;

    if (usb_hid_spawn_kthread(h) != 0) {
      LOG_ERR("[HID] %s: no se pudo lanzar kthread", h->name);
      g_hid_count--;
      kfree(h);
      continue;
    }

    probed++;
  }
  return probed;
}