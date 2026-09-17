// kernel/serial.h
#pragma once
#include <stdint.h>
#include <stddef.h>

#define SERIAL_PORT_COM1 0x3F8

void serial_init(void);

// API pública (cada llamada coge el lock internamente)
void serial_putc(char c);
void serial_puts(const char *str);
void serial_putn(uint64_t n, int base, int width);
void serial_printf(const char *fmt, ...);
void serial_hex(uint64_t val);

// Lock compartido con klog.
// El llamante debe cogerlo con serial_lock_acquire() y soltarlo con
// serial_lock_release(). Dentro de la sección crítica, usar SÓLO
// serial_putc_locked() para evitar doble lock.
void serial_lock_acquire(unsigned long *flags);
void serial_lock_release(unsigned long flags);

// Versión sin lock. Sólo usar si ya tienes el lock cogido.
void serial_putc_locked(char c);