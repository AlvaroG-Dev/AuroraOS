// kernel/ps2.c
// Driver PS/2 (teclado + ratón) con tarea dedicada.
//
// Los IRQ handlers sólo leen bytes del puerto y los empujan a un buffer
// circular. La tarea ps2_thread procesa los bytes, produce eventos de
// input normalizados y (para teclado) traduce a ASCII para el TTY.

#include "ps2.h"
#include "idt.h"
#include "input.h"
#include "klog.h"
#include "sched.h"
#include "serial.h"
#include "tty.h" // [TTY]
#include "wait.h"
#include <stdint.h>

// ===========================================================================
// Utilidades de puerto
// ===========================================================================
static inline uint8_t inb(uint16_t port) {
  uint8_t ret;
  __asm__ volatile("inb %1, %0" : "=a"(ret) : "dN"(port));
  return ret;
}

static inline void outb(uint16_t port, uint8_t val) {
  __asm__ volatile("outb %0, %1" : : "a"(val), "dN"(port));
}

static inline void io_wait(void) { outb(0x80, 0x00); }

static int wait_input_buffer_empty(void) {
  for (int i = 0; i < 50000; i++) {
    if (!(inb(0x64) & 0x02))
      return 1;
    io_wait();
  }
  return 0;
}

static int wait_output_buffer_full(void) {
  for (int i = 0; i < 50000; i++) {
    if (inb(0x64) & 0x01)
      return 1;
    io_wait();
  }
  return 0;
}

static void drain_output(void) {
  int i = 0;
  while ((inb(0x64) & 0x01) && i++ < 64) {
    (void)inb(0x60);
    io_wait();
  }
}

// ===========================================================================
// Buffers de bytes crudos
// ===========================================================================
#define PS2_BYTE_QUEUE_SIZE 512

static volatile uint8_t byte_queue[PS2_BYTE_QUEUE_SIZE];
static volatile uint8_t byte_is_mouse[PS2_BYTE_QUEUE_SIZE];
static volatile uint16_t byte_head = 0;
static volatile uint16_t byte_tail = 0;

static wait_queue_t ps2_wq;

static bool ps2_has_byte(void *arg) {
  (void)arg;
  return byte_tail != byte_head;
}

// ===========================================================================
// IRQ handlers
// ===========================================================================
static void ps2_irq_common(int is_mouse) {
  uint8_t status = inb(0x64);
  if (!(status & 0x01))
    return;
  if ((status & 0x20) != (is_mouse ? 0x20 : 0))
    return;

  uint8_t data = inb(0x60);
  uint16_t next = (byte_head + 1) % PS2_BYTE_QUEUE_SIZE;
  if (next != byte_tail) {
    byte_queue[byte_head] = data;
    byte_is_mouse[byte_head] = (uint8_t)is_mouse;
    byte_head = next;
    wake_up_all(&ps2_wq);
  }
}

static void ps2_keyboard_irq(void) { ps2_irq_common(0); }
static void ps2_mouse_irq(void) { ps2_irq_common(1); }

// ===========================================================================
// Estado del ratón
// ===========================================================================
static int mouse_cycle = 0;
static uint8_t mouse_packet[3];

// ===========================================================================
// Traducción scancode → ASCII
//
// [FIX-TTY] El backspace envía 0x7F (DEL), no 0x08 (BS). El driver de
// teclado Linux hace lo mismo, y musl/ash esperan 0x7F como VERASE.
// Con 0x08, ash no reconoce el byte como "erase" (su cc[VERASE] es 0x7F)
// y lo inserta literal → aparecía basura tipo `]` o `J`.
// ===========================================================================
extern volatile uint64_t tick_count;

// Tabla Set 1 sin shift.
static const char scancode_ascii[128] = {
    0,   27,   '1',  '2', '3',  '4', '5', '6', '7', '8', '9', '0', '-',
    '=', 0x7F, '\t', 'q', 'w',  'e', 'r', 't', 'y', 'u', 'i', 'o', 'p',
    '[', ']',  '\n', 0,   'a',  's', 'd', 'f', 'g', 'h', 'j', 'k', 'l',
    ';', '\'', '`',  0,   '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',',
    '.', '/',  0,    '*', 0,    ' ', 0,   0,   0,   0,   0,   0,
    // Resto (0x40-0x7F): 0
};

static const char scancode_ascii_shift[128] = {
    0,   27,   '!',  '@', '#', '$', '%', '^', '&', '*', '(', ')', '_',
    '+', 0x7F, '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P',
    '{', '}',  '\n', 0,   'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L',
    ':', '"',  '~',  0,   '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<',
    '>', '?',  0,    '*', 0,   ' ', 0,   0,   0,   0,   0,   0,
};

// [6.1] Secuencias ANSI para teclas extendidas (0xE0 + code). Mismos
// códigos que emite un terminal Linux xterm con TERM=linux. Los
// consumidores reales son vi, less, top, ash (edición de línea).
static const char *ps2_extended_seq(uint8_t code) {
  switch (code) {
  case 0x48:
    return "\x1b[A"; // Up
  case 0x50:
    return "\x1b[B"; // Down
  case 0x4D:
    return "\x1b[C"; // Right
  case 0x4B:
    return "\x1b[D"; // Left
  case 0x47:
    return "\x1b[H"; // Home
  case 0x4F:
    return "\x1b[F"; // End
  case 0x49:
    return "\x1b[5~"; // Page Up
  case 0x51:
    return "\x1b[6~"; // Page Down
  case 0x53:
    return "\x1b[3~"; // Delete
  case 0x52:
    return "\x1b[2~"; // Insert
  case 0x57:
    return "\x1b[23~"; // F11
  case 0x58:
    return "\x1b[24~"; // F12
  case 0x1C:
    return "\r"; // Keypad Enter
  case 0x35:
    return "/"; // Keypad /
  default:
    return NULL;
  }
}

// [6.1] Secuencias ANSI para teclas no extendidas que no son ASCII:
// F1-F10, que en Set 1 van con scancodes propios (0x3B-0x44).
static const char *ps2_plain_seq(uint8_t code) {
  switch (code) {
  case 0x3B:
    return "\x1bOP"; // F1
  case 0x3C:
    return "\x1bOQ"; // F2
  case 0x3D:
    return "\x1bOR"; // F3
  case 0x3E:
    return "\x1bOS"; // F4
  case 0x3F:
    return "\x1b[15~"; // F5
  case 0x40:
    return "\x1b[17~"; // F6
  case 0x41:
    return "\x1b[18~"; // F7
  case 0x42:
    return "\x1b[19~"; // F8
  case 0x43:
    return "\x1b[20~"; // F9
  case 0x44:
    return "\x1b[21~"; // F10
  default:
    return NULL;
  }
}

static int left_shift_pressed = 0;
static int right_shift_pressed = 0;
static int ctrl_pressed = 0;

// [6.1] Prefijo 0xE0 (scancode extendido). Set 1 usa 0xE0 delante de
// las teclas que no caben en la tabla original: flechas, Home/End,
// Page Up/Down, Delete, Insert, F11/F12, teclado numérico extendido.
// Cuando llega 0xE0 marcamos este flag; el siguiente byte se decodifica
// contra la tabla extendida en vez de la normal.
static int extended_pending = 0;

// ===========================================================================
// Procesamiento de bytes → eventos de input + TTY
// ===========================================================================
static void process_keyboard_byte(uint8_t sc) {
  int pressed = !(sc & 0x80);
  uint8_t code = sc & 0x7F;

  // [6.1] Prefijo de scancode extendido. Consumir y esperar el
  // siguiente byte. No emitimos ningún evento al input subsystem por
  // el 0xE0 en sí; el code real llega en la siguiente iteración.
  if (sc == 0xE0) {
    extended_pending = 1;
    return;
  }

  // [6.1] Publicar evento en input subsystem solo en press. Es lo que
  // consume el compositor (aunque hoy solo mira el ratón; el teclado
  // va por tty). En release no hay evento.
  if (pressed) {
    input_event_t ev;
    ev.type = INPUT_EV_KEY;
    ev.code = code;
    ev.value = 1;
    ev.value2 = 0;
    ev.timestamp = (uint32_t)tick_count;
    input_push(&ev);
  }

  // Modificadores: se actualizan tanto en press como en release.
  if (code == 0x2A) {
    left_shift_pressed = pressed;
    extended_pending = 0;
    return;
  }
  if (code == 0x36) {
    right_shift_pressed = pressed;
    extended_pending = 0;
    return;
  }
  if (code == 0x1D) {
    ctrl_pressed = pressed;
    extended_pending = 0;
    return;
  }

  // Todo lo demás solo importa en press.
  if (!pressed) {
    extended_pending = 0;
    return;
  }

  tty_t *tty = tty_console();
  if (!tty) {
    extended_pending = 0;
    return;
  }

  // [6.1] Secuencias ANSI. Se procesan antes que la traducción ASCII
  // porque el code puede coincidir con una letra (0x48 = 'H' sin E0,
  // 0x47 = 'G' sin E0, etc.): la única diferencia es el prefijo 0xE0,
  // que extended_pending ya ha capturado.
  const char *seq =
      extended_pending ? ps2_extended_seq(code) : ps2_plain_seq(code);
  extended_pending = 0;

  if (seq) {
    // Emitir cada byte de la secuencia al TTY. El terminal app los
    // reenvía al master PTY, el slave los entrega al proceso lector
    // (shell, vi, less, top).
    for (const char *p = seq; *p; p++)
      tty_receive_char(tty, *p);
    return;
  }

  // Traducción ASCII normal (letras, dígitos, símbolos, espacio...).
  int shift_pressed = left_shift_pressed || right_shift_pressed;
  char c = shift_pressed ? scancode_ascii_shift[code] : scancode_ascii[code];
  if (c == 0)
    return;

  // [Ctrl] Convención ASCII: Ctrl+letra → 0x01..0x1A.
  if (ctrl_pressed) {
    char lower = c;
    if (lower >= 'A' && lower <= 'Z')
      lower = (char)(lower + 32);
    if (lower >= 'a' && lower <= 'z')
      c = (char)(lower - 'a' + 1);
  }

  tty_receive_char(tty, c);
}

static void process_mouse_byte(uint8_t data) {
  switch (mouse_cycle) {
  case 0:
    if (data & 0x08) {
      mouse_packet[0] = data;
      mouse_cycle = 1;
    }
    break;
  case 1:
    mouse_packet[1] = data;
    mouse_cycle = 2;
    break;
  case 2: {
    mouse_packet[2] = data;
    mouse_cycle = 0;

    if (mouse_packet[0] & 0xC0)
      break;

    int16_t raw_x = mouse_packet[1];
    int16_t raw_y = mouse_packet[2];
    if (mouse_packet[0] & 0x10)
      raw_x |= 0xFF00;
    if (mouse_packet[0] & 0x20)
      raw_y |= 0xFF00;
    raw_y = -raw_y;
    uint8_t btn = mouse_packet[0] & 0x07;

    input_event_t ev;
    ev.type = INPUT_EV_MOUSE;
    ev.code = btn;
    ev.value = raw_x;
    ev.value2 = raw_y;
    ev.timestamp = (uint32_t)tick_count;
    input_push(&ev);
    break;
  }
  }
}

// ===========================================================================
// Tarea dedicada
// ===========================================================================
static void ps2_thread(void) {
  LOG_INFO("[PS2] Tarea de procesamiento iniciada");

  while (1) {
    wait_event(&ps2_wq, ps2_has_byte, NULL);

    while (byte_tail != byte_head) {
      uint8_t b = byte_queue[byte_tail];
      int is_mouse = byte_is_mouse[byte_tail];
      byte_tail = (byte_tail + 1) % PS2_BYTE_QUEUE_SIZE;

      if (is_mouse)
        process_mouse_byte(b);
      else
        process_keyboard_byte(b);
    }
  }
}

// ===========================================================================
// Init del driver
// ===========================================================================
static int ps2_init_impl(void) {
  LOG_INFO("[PS2] Inicializando controlador PS/2...");

  wait_queue_init(&ps2_wq);

  wait_input_buffer_empty();
  outb(0x64, 0xA8);
  drain_output();

  wait_input_buffer_empty();
  outb(0x64, 0x20);
  if (wait_output_buffer_full()) {
    uint8_t cmd = inb(0x60);
    cmd |= 0x03;
    cmd &= ~0x30;
    wait_input_buffer_empty();
    outb(0x64, 0x60);
    wait_input_buffer_empty();
    outb(0x60, cmd);
  }

  int retries = 3, ack_ok = 0;
  while (retries--) {
    wait_input_buffer_empty();
    outb(0x64, 0xD4);
    wait_input_buffer_empty();
    outb(0x60, 0xF4);

    for (int i = 0; i < 10000; i++) {
      uint8_t s = inb(0x64);
      if ((s & 0x01) && (s & 0x20)) {
        if (inb(0x60) == 0xFA) {
          ack_ok = 1;
          break;
        }
      }
      io_wait();
    }
    if (ack_ok)
      break;
  }
  if (ack_ok)
    LOG_INFO("[PS2] Mouse habilitado (ACK 0xF4)");
  else
    LOG_WARN("[PS2] Mouse no respondió al comando 0xF4");

  irq_install_handler(1, ps2_keyboard_irq);
  irq_install_handler(12, ps2_mouse_irq);

  if (!sched_create_task(ps2_thread)) {
    LOG_ERR("[PS2] No se pudo crear la tarea de procesamiento");
    return -1;
  }

  return 0;
}

struct driver ps2_driver = {
    .name = "ps2",
    .init = ps2_init_impl,
    .shutdown = NULL,
};