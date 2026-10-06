// kernel/serial.h
#pragma once
#include <stddef.h>
#include <stdint.h>

#define SERIAL_PORT_COM1 0x3F8

// [FIX] Valor centinela: serial_lock_acquire() lo devuelve cuando NO ha
// podido coger el lock porque el CPU actual ya lo tenía (reentrada).
//
// El llamante DEBE comprobar este valor y descartar el mensaje:
//   unsigned long flags;
//   serial_lock_acquire(&flags);
//   if (flags == SERIAL_LOCK_REENTERED) return;   // <-- CRÍTICO
//   ... usar serial_putc_locked ...
//   serial_lock_release(flags);
//
// Sin esta comprobación, un LOG_* dentro de otro LOG_* (o dentro de un
// handler de IRQ disparado durante un LOG) produce un deadlock del que
// no se sale: el CPU spinnearía contra un lock que él mismo tiene cogido.
#define SERIAL_LOCK_REENTERED 0xFFFFFFFFFFFFFFFFULL

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
//
// serial_lock_acquire() puede devolver SERIAL_LOCK_REENTERED en *flags
// (ver arriba). En ese caso el lock NO está cogido por nosotros y
// serial_lock_release() es no-op.
void serial_lock_acquire(unsigned long *flags);
void serial_lock_release(unsigned long flags);

// Versión sin lock. Sólo usar si ya tienes el lock cogido.
void serial_putc_locked(char c);

// [FIX] Contador de caracteres descartados por timeout o reentrada.
uint64_t serial_dropped_count(void);