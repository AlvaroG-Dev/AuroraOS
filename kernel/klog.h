// kernel/klog.h
#pragma once
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>


// Niveles de log (menor número = más severo)
typedef enum {
  KLOG_PANIC = 0, // sistema inservible, se cuelga
  KLOG_ERR = 1,   // error recuperable pero grave
  KLOG_WARN = 2,  // algo va mal pero sigue
  KLOG_INFO = 3,  // eventos normales (por defecto)
  KLOG_DEBUG = 4, // trazas de desarrollo
  KLOG_TRACE = 5, // muy verboso
  KLOG_LEVEL_MAX = 6
} klog_level_t;

// Inicialización: llama ANTES que cualquier printk (típicamente en serial_init)
void klog_init(void);

// Cambiar nivel mínimo visible. Todo lo de nivel > min_level se descarta.
void klog_set_level(klog_level_t min_level);
klog_level_t klog_get_level(void);

// API principal
void klog_printf(klog_level_t level, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
void klog_vprintf(klog_level_t level, const char *fmt, va_list ap);
void klog_puts(klog_level_t level, const char *s);
void klog_putc(klog_level_t level, char c);

// Helpers de formato (reemplazan a serial_hex/serial_putn)
void klog_hex(klog_level_t level, uint64_t val, int min_digits);
void klog_dec(klog_level_t level, uint64_t val);

// Ajuste de frecuencia del TSC (para timestamping)
void klog_set_tsc_freq(uint64_t hz);
uint64_t klog_get_tsc_freq(void);
void klog_calibrate_tsc(uint32_t ticks_to_wait, uint32_t pit_hz);

// [libc] Escribe bytes crudos al klog sin prefijo ni timestamp.
// Lo usa /dev/kmsg en su write.
void klog_write_raw(const char *buf, size_t n);

// Macros cómodos (módulo embebido en el mensaje)
#define LOG_PANIC(...) klog_printf(KLOG_PANIC, __VA_ARGS__)
#define LOG_ERR(...) klog_printf(KLOG_ERR, __VA_ARGS__)
#define LOG_WARN(...) klog_printf(KLOG_WARN, __VA_ARGS__)
#define LOG_INFO(...) klog_printf(KLOG_INFO, __VA_ARGS__)
#define LOG_DEBUG(...) klog_printf(KLOG_DEBUG, __VA_ARGS__)
#define LOG_TRACE(...) klog_printf(KLOG_TRACE, __VA_ARGS__)

// Acceso al ring buffer (para /dev/kmsg futuro y dmesg)
size_t klog_read(char *out, size_t max_len); // lee todo lo disponible
size_t klog_available(void);                 // bytes pendientes
void klog_clear(void);                       // limpia el buffer