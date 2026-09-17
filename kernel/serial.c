// kernel/serial.c
// Driver serial COM1 — thread-safe y con lock compartido para klog

#include "serial.h"
#include "io.h"
#include "spinlock.h"
#include <stdint.h>
#include <stdarg.h>

// Lock global que protege todas las escrituras al puerto serie.
// Es el mismo lock que klog usa para envolver mensajes completos.
static spinlock_t serial_lock;

// ---------------------------------------------------------------------------
// Helpers de bajo nivel (sin lock — el llamante debe tenerlo)
// ---------------------------------------------------------------------------

static int serial_ready_locked(void) {
  return inb(SERIAL_PORT_COM1 + 5) & 0x20;
}

// Escribe un byte al puerto, con timeout. Debe llamarse con el lock cogido.
static void serial_write_byte_locked(char c) {
  int timeout = 100000;
  while (!serial_ready_locked() && timeout-- > 0);
  if (timeout <= 0) return;
  outb(SERIAL_PORT_COM1, c);
}

void serial_putc_locked(char c) {
  if (c == '\n') {
    serial_write_byte_locked('\r');
  }
  serial_write_byte_locked(c);
}

// ---------------------------------------------------------------------------
// API pública
// ---------------------------------------------------------------------------

void serial_init(void) {
  spin_init(&serial_lock);

  outb(SERIAL_PORT_COM1 + 1, 0x00);
  outb(SERIAL_PORT_COM1 + 3, 0x80);
  outb(SERIAL_PORT_COM1 + 0, 0x03);
  outb(SERIAL_PORT_COM1 + 1, 0x00);
  outb(SERIAL_PORT_COM1 + 3, 0x03);
  outb(SERIAL_PORT_COM1 + 2, 0xC7);
  outb(SERIAL_PORT_COM1 + 4, 0x0B);
}

void serial_lock_acquire(unsigned long *flags) {
  *flags = spin_lock_irqsave(&serial_lock);
}

void serial_lock_release(unsigned long flags) {
  spin_unlock_irqrestore(&serial_lock, flags);
}

void serial_putc(char c) {
  unsigned long flags;
  serial_lock_acquire(&flags);
  serial_putc_locked(c);
  serial_lock_release(flags);
}

void serial_puts(const char *str) {
  if (!str) {
    serial_puts("(null)");
    return;
  }
  unsigned long flags;
  serial_lock_acquire(&flags);
  while (*str) serial_putc_locked(*str++);
  serial_lock_release(flags);
}

void serial_putn(uint64_t n, int base, int width) {
    const char *digits = "0123456789ABCDEF";
    char buf[32];
    int i = 0;
  
    unsigned long flags;
    serial_lock_acquire(&flags);
  
    if (n == 0) {
      buf[i++] = '0';
    } else {
      while (n > 0) {
        buf[i++] = digits[n % base];
        n /= base;
      }
    }
  
    // Padding con ceros a la izquierda
    int pad = width - i;
    if (pad < 0) pad = 0;
    for (int k = 0; k < pad; k++) serial_putc_locked('0');
  
    // Dígitos en orden inverso
    while (i-- > 0) serial_putc_locked(buf[i]);
  
    serial_lock_release(flags);
}

void serial_printf(const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);

  unsigned long flags;
  serial_lock_acquire(&flags);

  while (*fmt) {
    if (*fmt == '%' && *(fmt + 1)) {
      fmt++;
      switch (*fmt) {
        case 's': {
          const char *s = va_arg(args, const char *);
          if (!s) s = "(null)";
          while (*s) serial_putc_locked(*s++);
          break;
        }
        case 'd': {
          int val = va_arg(args, int);
          if (val < 0) {
            serial_putc_locked('-');
            val = -val;
          }
          // itoa inline sin volver a coger el lock
          char tmp[16];
          int n = 0;
          if (val == 0) tmp[n++] = '0';
          while (val > 0) { tmp[n++] = '0' + (val % 10); val /= 10; }
          while (n-- > 0) serial_putc_locked(tmp[n]);
          break;
        }
        case 'u': {
          unsigned int val = va_arg(args, unsigned int);
          char tmp[16];
          int n = 0;
          if (val == 0) tmp[n++] = '0';
          while (val > 0) { tmp[n++] = '0' + (val % 10); val /= 10; }
          while (n-- > 0) serial_putc_locked(tmp[n]);
          break;
        }
        case 'x': {
          unsigned int val = va_arg(args, unsigned int);
          const char *hex = "0123456789abcdef";
          char tmp[16];
          int n = 0;
          if (val == 0) tmp[n++] = '0';
          while (val > 0) { tmp[n++] = hex[val & 0xF]; val >>= 4; }
          while (n-- > 0) serial_putc_locked(tmp[n]);
          break;
        }
        case 'X': {
            unsigned int val = va_arg(args, unsigned int);
            const char *hex = "0123456789ABCDEF";
            char tmp[16];
            int n = 0;
            if (val == 0) tmp[n++] = '0';
            while (val > 0) { tmp[n++] = hex[val & 0xF]; val >>= 4; }
            // Padding
            int pad = 8 - n;
            if (pad < 0) pad = 0;
            for (int k = 0; k < pad; k++) serial_putc_locked('0');
            while (n-- > 0) serial_putc_locked(tmp[n]);
            break;
          }
          case 'p': {
            serial_putc_locked('0');
            serial_putc_locked('x');
            uint64_t val = (uint64_t)va_arg(args, void *);
            const char *hex = "0123456789abcdef";
            char tmp[16];
            int n = 0;
            if (val == 0) tmp[n++] = '0';
            while (val > 0) { tmp[n++] = hex[val & 0xF]; val >>= 4; }
            // Padding
            int pad = 16 - n;
            if (pad < 0) pad = 0;
            for (int k = 0; k < pad; k++) serial_putc_locked('0');
            while (n-- > 0) serial_putc_locked(tmp[n]);
            break;
          }
          case 'l': {
            if (*(fmt + 1) == 'x') {
              fmt++;
              uint64_t val = va_arg(args, uint64_t);
              const char *hex = "0123456789abcdef";
              char tmp[16];
              int n = 0;
              if (val == 0) tmp[n++] = '0';
              while (val > 0) { tmp[n++] = hex[val & 0xF]; val >>= 4; }
              // Padding
              int pad = 16 - n;
              if (pad < 0) pad = 0;
              for (int k = 0; k < pad; k++) serial_putc_locked('0');
              while (n-- > 0) serial_putc_locked(tmp[n]);
          } else if (*(fmt + 1) == 'd') {
            fmt++;
            int64_t val = va_arg(args, int64_t);
            if (val < 0) {
              serial_putc_locked('-');
              val = -val;
            }
            char tmp[24];
            int n = 0;
            if (val == 0) tmp[n++] = '0';
            while (val > 0) { tmp[n++] = '0' + (val % 10); val /= 10; }
            while (n-- > 0) serial_putc_locked(tmp[n]);
          }
          break;
        }
        case 'c': serial_putc_locked((char)va_arg(args, int)); break;
        case '%': serial_putc_locked('%'); break;
        default:  serial_putc_locked(*fmt); break;
      }
    } else {
      serial_putc_locked(*fmt);
    }
    fmt++;
  }

  serial_lock_release(flags);
  va_end(args);
}

void serial_hex(uint64_t val) {
    const char *hex = "0123456789abcdef";
    char tmp[16];
    int n = 0;
  
    unsigned long flags;
    serial_lock_acquire(&flags);
  
    serial_putc_locked('0');
    serial_putc_locked('x');
  
    if (val == 0) {
      tmp[n++] = '0';
    } else {
      while (val > 0) {
        tmp[n++] = hex[val & 0xF];
        val >>= 4;
      }
    }
  
    // Padding: hasta 16 dígitos hex
    int pad = 16 - n;
    if (pad < 0) pad = 0;
    for (int k = 0; k < pad; k++) serial_putc_locked('0');
  
    // Dígitos en orden inverso
    while (n-- > 0) serial_putc_locked(tmp[n]);
  
    serial_lock_release(flags);
}