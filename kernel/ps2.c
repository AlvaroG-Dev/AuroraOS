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
// ===========================================================================
extern volatile uint64_t tick_count;

// Tabla Set 1 sin shift.
static const char scancode_ascii[128] = {
    0,   27,   '1',  '2', '3',  '4', '5', '6', '7', '8', '9', '0', '-',
    '=', '\b', '\t', 'q', 'w',  'e', 'r', 't', 'y', 'u', 'i', 'o', 'p',
    '[', ']',  '\n', 0,   'a',  's', 'd', 'f', 'g', 'h', 'j', 'k', 'l',
    ';', '\'', '`',  0,   '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',',
    '.', '/',  0,    '*', 0,    ' ', 0,   0,   0,   0,   0,   0,
    // Resto (0x40-0x7F): 0
};

static const char scancode_ascii_shift[128] = {
    0,   27,   '!',  '@', '#', '$', '%', '^', '&', '*', '(', ')', '_',
    '+', '\b', '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P',
    '{', '}',  '\n', 0,   'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L',
    ':', '"',  '~',  0,   '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<',
    '>', '?',  0,    '*', 0,   ' ', 0,   0,   0,   0,   0,   0,
};

static int shift_pressed = 0;

// ===========================================================================
// Procesamiento de bytes → eventos de input + TTY
// ===========================================================================
static void process_keyboard_byte(uint8_t sc) {
  input_event_t ev;
  ev.type = INPUT_EV_KEY;
  ev.code = sc;
  ev.value = (sc & 0x80) ? 0 : 1;
  ev.value2 = 0;
  ev.timestamp = (uint32_t)tick_count;
  input_push(&ev);

  // [TTY] traducción a ASCII y entrega.
  int pressed = !(sc & 0x80);
  uint8_t code = sc & 0x7F;

  if (code == 0x2A || code == 0x36) {
    shift_pressed = pressed;
    return;
  }

  if (!pressed)
    return;

  char c = shift_pressed ? scancode_ascii_shift[code] : scancode_ascii[code];
  if (c == 0)
    return;

  tty_t *tty = tty_default();
  if (tty)
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