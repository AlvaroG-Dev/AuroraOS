// kernel/serial.c
// Driver serial COM1

#include "serial.h"
#include <stdint.h>
#include <stdarg.h>

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "dN"(port));
    return ret;
}

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "dN"(port));
}

static int serial_ready(void) {
    return inb(SERIAL_PORT_COM1 + 5) & 0x20;
}

void serial_init(void) {
    outb(SERIAL_PORT_COM1 + 1, 0x00);
    outb(SERIAL_PORT_COM1 + 3, 0x80);
    outb(SERIAL_PORT_COM1 + 0, 0x03);
    outb(SERIAL_PORT_COM1 + 1, 0x00);
    outb(SERIAL_PORT_COM1 + 3, 0x03);
    outb(SERIAL_PORT_COM1 + 2, 0xC7);
    outb(SERIAL_PORT_COM1 + 4, 0x0B);
}

void serial_putc(char c) {
    if (c == '\n') {
        while (!serial_ready());
        outb(SERIAL_PORT_COM1, '\r');
    }
    while (!serial_ready());
    outb(SERIAL_PORT_COM1, c);
}

void serial_puts(const char *str) {
    if (!str) {
        serial_puts("(null)");
        return;
    }
    while (*str) serial_putc(*str++);
}

void serial_putn(uint64_t n, int base, int width) {
    const char *digits = "0123456789ABCDEF";
    char buf[32];
    int i = 0;
    if (n == 0) {
        serial_putc('0');
        return;
    }
    while (n > 0) {
        buf[i++] = digits[n % base];
        n /= base;
    }
    while (i < width) { serial_putc('0'); width--; }
    while (i-- > 0) serial_putc(buf[i]);
}

void serial_printf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);

    while (*fmt) {
        if (*fmt == '%' && *(fmt + 1)) {
            fmt++;
            switch (*fmt) {
                case 's': serial_puts(va_arg(args, const char*)); break;
                case 'd': {
                    int val = va_arg(args, int);
                    if (val < 0) {
                        serial_putc('-');
                        val = -val;
                    }
                    serial_putn((uint64_t)val, 10, 0);
                    break;
                }
                case 'u': serial_putn(va_arg(args, unsigned int), 10, 0); break;
                case 'x': serial_putn(va_arg(args, unsigned int), 16, 0); break;
                case 'X': serial_putn(va_arg(args, unsigned int), 16, 8); break;
                case 'p': serial_puts("0x"); serial_putn((uint64_t)va_arg(args, void*), 16, 16); break;
                case 'l': 
                    if (*(fmt+1) == 'x') {
                        fmt++;
                        serial_putn(va_arg(args, uint64_t), 16, 16);
                    } else if (*(fmt+1) == 'd') {
                        fmt++;
                        int64_t val = va_arg(args, int64_t);
                        if (val < 0) {
                            serial_putc('-');
                            val = -val;
                        }
                        serial_putn((uint64_t)val, 10, 0);
                    }
                    break;
                case 'c': serial_putc((char)va_arg(args, int)); break;
                case '%': serial_putc('%'); break;
                default: serial_putc(*fmt); break;
            }
        } else {
            serial_putc(*fmt);
        }
        fmt++;
    }
    va_end(args);
}

void serial_hex(uint64_t val) {
    serial_puts("0x");
    serial_putn(val, 16, 16);
}
