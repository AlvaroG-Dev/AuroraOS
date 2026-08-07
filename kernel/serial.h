// kernel/serial.h
// Driver serial COM1 para logging en desarrollo

#ifndef SERIAL_H
#define SERIAL_H

#include <stdint.h>

#define SERIAL_PORT_COM1 0x3F8

void serial_init(void);
void serial_putc(char c);
void serial_puts(const char *str);
void serial_putn(uint64_t n, int base, int width);
void serial_printf(const char *fmt, ...);
void serial_hex(uint64_t val);

#endif
