// kernel/klog.c
#include "klog.h"
#include "serial.h"
#include "time.h"
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#define KLOG_RING_SIZE 16384

static klog_level_t min_level = KLOG_DEBUG;
static int klog_ready = 0;

static char ring[KLOG_RING_SIZE];
static volatile size_t ring_head = 0;
static volatile size_t ring_tail = 0;
static volatile size_t ring_count = 0;

// ---------------------------------------------------------------------------
// TSC (Time Stamp Counter)
// ---------------------------------------------------------------------------
static inline uint64_t rdtsc(void) {
  uint32_t lo, hi;
  __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
  return ((uint64_t)hi << 32) | lo;
}

// Frecuencia estimada del TSC. Valor por defecto típico de QEMU con
// -cpu qemu64 (~3.6 GHz). Si tu entorno difiere, ajusta o calibra.
static uint64_t tsc_freq_hz = 2600000000ULL;

// Instante de klog_init. Los timestamps son relativos a este instante,
// lo cual evita que un cambio de frecuencia (calibración) haga retroceder
// los timestamps de mensajes posteriores.
static uint64_t tsc_origin = 0;

void klog_set_tsc_freq(uint64_t hz) {
  if (hz > 0)
    tsc_freq_hz = hz;
}

uint64_t klog_get_tsc_freq(void) { return tsc_freq_hz; }

// ---------------------------------------------------------------------------
// Ring
// ---------------------------------------------------------------------------
static void ring_push(char c) {
  if (ring_count == KLOG_RING_SIZE) {
    ring_tail = (ring_tail + 1) % KLOG_RING_SIZE;
    ring_count--;
  }
  ring[ring_head] = c;
  ring_head = (ring_head + 1) % KLOG_RING_SIZE;
  ring_count++;
}

// ---------------------------------------------------------------------------
// Salida: ring + serial (SIN coger el lock — el llamante ya lo tiene)
// ---------------------------------------------------------------------------
static void out_char(char c) {
  ring_push(c);
  serial_putc_locked(c);
}

// ---------------------------------------------------------------------------
// Timestamp relativo a tsc_origin
// ---------------------------------------------------------------------------
static void format_timestamp(char *buf, size_t buflen) {
  uint64_t now = rdtsc();
  uint64_t delta = now - tsc_origin; // ciclos desde klog_init

  uint64_t secs = delta / tsc_freq_hz;
  uint64_t remainder = delta % tsc_freq_hz;
  uint64_t usec = (remainder * 1000000ULL) / tsc_freq_hz;

  // Formato: "    S.UUUUUU" → 5 dígitos de segundos, 6 de microsegundos
  char tmp[24];
  int n = 0;

  // Segundos con padding a 5
  char sbuf[20];
  int sn = 0;
  if (secs == 0) {
    sbuf[sn++] = '0';
  } else {
    uint64_t s = secs;
    while (s > 0) {
      sbuf[sn++] = '0' + (s % 10);
      s /= 10;
    }
  }
  while (sn < 5)
    sbuf[sn++] = ' ';
  while (sn > 0)
    tmp[n++] = sbuf[--sn];

  tmp[n++] = '.';

  // Microsegundos con padding a 6
  char ubuf[8];
  int un = 0;
  if (usec == 0) {
    ubuf[un++] = '0';
  } else {
    uint64_t u = usec;
    while (u > 0) {
      ubuf[un++] = '0' + (u % 10);
      u /= 10;
    }
  }
  while (un < 6)
    ubuf[un++] = '0';
  while (un > 0)
    tmp[n++] = ubuf[--un];

  int i = 0;
  for (int j = 0; j < n && i < (int)buflen - 1; j++)
    buf[i++] = tmp[j];
  buf[i] = '\0';
}

static const char *level_name(klog_level_t lvl) {
  switch (lvl) {
  case KLOG_PANIC:
    return "PANIC";
  case KLOG_ERR:
    return "ERROR";
  case KLOG_WARN:
    return "WARN ";
  case KLOG_INFO:
    return "INFO ";
  case KLOG_DEBUG:
    return "DEBUG";
  case KLOG_TRACE:
    return "TRACE";
  default:
    return "?????";
  }
}

static void emit_prefix(klog_level_t level) {
  char ts[32];
  format_timestamp(ts, sizeof(ts));
  out_char('[');
  for (char *p = ts; *p; p++)
    out_char(*p);
  out_char(']');
  out_char(' ');
  const char *name = level_name(level);
  for (const char *p = name; *p; p++)
    out_char(*p);
  out_char(' ');
}

// ---------------------------------------------------------------------------
// Números (usados por klog_hex / klog_dec)
// ---------------------------------------------------------------------------
static void emit_num(uint64_t n, int base, int min_digits) {
  const char *digits = "0123456789abcdef";
  char buf[32];
  int i = 0;
  if (n == 0)
    buf[i++] = '0';
  else
    while (n > 0) {
      buf[i++] = digits[n % base];
      n /= base;
    }
  while (i < min_digits)
    buf[i++] = '0';
  while (i-- > 0)
    out_char(buf[i]);
}

static void emit_signed(int64_t v, int base, int min_digits) {
  if (v < 0) {
    out_char('-');
    emit_num((uint64_t)(-v), base, min_digits);
  } else
    emit_num((uint64_t)v, base, min_digits);
}

// ---------------------------------------------------------------------------
// vprintf (sin lock — el llamante lo tiene)
// ---------------------------------------------------------------------------
static void klog_vprintf_locked(klog_level_t level, const char *fmt,
                                va_list ap) {
  emit_prefix(level);

  while (*fmt) {
    if (*fmt != '%') {
      out_char(*fmt++);
      continue;
    }
    fmt++;

    int min_digits = 0;
    while (*fmt >= '0' && *fmt <= '9') {
      min_digits = min_digits * 10 + (*fmt - '0');
      fmt++;
    }

    int is_long = 0, is_llong = 0, is_size = 0;
    while (*fmt == 'l') {
      if (is_long)
        is_llong = 1;
      is_long = 1;
      fmt++;
    }
    if (*fmt == 'z') {
      is_size = 1;
      fmt++;
    }

    switch (*fmt) {
    case 'd':
    case 'i': {
      int64_t v;
      if (is_llong || is_long)
        v = va_arg(ap, int64_t);
      else if (is_size)
        v = (int64_t)va_arg(ap, size_t);
      else
        v = va_arg(ap, int);
      emit_signed(v, 10, min_digits);
      break;
    }
    case 'u': {
      uint64_t v;
      if (is_llong || is_long)
        v = va_arg(ap, uint64_t);
      else if (is_size)
        v = va_arg(ap, size_t);
      else
        v = va_arg(ap, unsigned int);
      emit_num(v, 10, min_digits);
      break;
    }
    case 'x':
    case 'X': {
      uint64_t v;
      if (is_llong || is_long)
        v = va_arg(ap, uint64_t);
      else if (is_size)
        v = va_arg(ap, size_t);
      else
        v = va_arg(ap, unsigned int);
      emit_num(v, 16, min_digits ? min_digits : 1);
      break;
    }
    case 'p': {
      uint64_t v = (uint64_t)va_arg(ap, void *);
      out_char('0');
      out_char('x');
      emit_num(v, 16, 16);
      break;
    }
    case 'c':
      out_char((char)va_arg(ap, int));
      break;
    case 's': {
      const char *s = va_arg(ap, const char *);
      if (!s)
        s = "(null)";
      while (*s)
        out_char(*s++);
      break;
    }
    case '%':
      out_char('%');
      break;
    default:
      out_char('%');
      out_char(*fmt);
      break;
    }
    fmt++;
  }
}

// ---------------------------------------------------------------------------
// API pública
// ---------------------------------------------------------------------------

void klog_init(void) {
  ring_head = ring_tail = ring_count = 0;
  min_level = KLOG_DEBUG;
  tsc_origin = rdtsc(); // ← fija el origen de los timestamps
  klog_ready = 1;
}

void klog_set_level(klog_level_t lvl) { min_level = lvl; }
klog_level_t klog_get_level(void) { return min_level; }

void klog_vprintf(klog_level_t level, const char *fmt, va_list ap) {
  if (!klog_ready || level > min_level)
    return;

  unsigned long flags;
  serial_lock_acquire(&flags);
  klog_vprintf_locked(level, fmt, ap);
  serial_lock_release(flags);
}

void klog_printf(klog_level_t level, const char *fmt, ...) {
  if (!klog_ready || level > min_level)
    return;

  va_list ap;
  va_start(ap, fmt);

  unsigned long flags;
  serial_lock_acquire(&flags);
  klog_vprintf_locked(level, fmt, ap);
  out_char('\n');
  serial_lock_release(flags);

  va_end(ap);
}

void klog_puts(klog_level_t level, const char *s) {
  if (!klog_ready || !s || level > min_level)
    return;

  unsigned long flags;
  serial_lock_acquire(&flags);
  emit_prefix(level);
  while (*s)
    out_char(*s++);
  out_char('\n');
  serial_lock_release(flags);
}

void klog_putc(klog_level_t level, char c) {
  if (!klog_ready || level > min_level)
    return;

  unsigned long flags;
  serial_lock_acquire(&flags);
  emit_prefix(level);
  out_char(c);
  out_char('\n');
  serial_lock_release(flags);
}

void klog_hex(klog_level_t level, uint64_t val, int min_digits) {
  if (!klog_ready || level > min_level)
    return;

  unsigned long flags;
  serial_lock_acquire(&flags);
  emit_prefix(level);
  out_char('0');
  out_char('x');
  emit_num(val, 16, min_digits);
  out_char('\n');
  serial_lock_release(flags);
}

void klog_dec(klog_level_t level, uint64_t val) {
  if (!klog_ready || level > min_level)
    return;

  unsigned long flags;
  serial_lock_acquire(&flags);
  emit_prefix(level);
  emit_num(val, 10, 0);
  out_char('\n');
  serial_lock_release(flags);
}

// ---------------------------------------------------------------------------
// Calibración del TSC contra el PIT
// ---------------------------------------------------------------------------
// Nota: cambiar tsc_freq_hz a mitad del boot NO hace retroceder los
// timestamps de mensajes posteriores, porque format_timestamp usa
// delta = rdtsc() - tsc_origin. El único efecto es que los mensajes
// previos a la calibración usaron la frecuencia por defecto (imprecisa).
void klog_calibrate_tsc(uint32_t ticks_to_wait, uint32_t pit_hz) {
  if (ticks_to_wait == 0 || pit_hz == 0)
    return;

  uint64_t start_tick = tick_count;
  while (tick_count == start_tick) {
    __asm__ volatile("hlt");
  }

  uint64_t tsc_start = rdtsc();
  uint64_t target_tick = tick_count + ticks_to_wait;

  while (tick_count < target_tick) {
    __asm__ volatile("hlt");
  }

  uint64_t tsc_end = rdtsc();
  uint64_t elapsed_cycles = tsc_end - tsc_start;
  uint64_t freq = (elapsed_cycles * pit_hz) / ticks_to_wait;
  klog_set_tsc_freq(freq);
}

// ---------------------------------------------------------------------------
// Acceso al ring
// ---------------------------------------------------------------------------
size_t klog_read(char *out, size_t max_len) {
  if (!out || max_len == 0)
    return 0;
  // [libc] Lock: klog_read la llaman /dev/kmsg y potencialmente varias
  // CPUs a la vez. Modifica ring_tail y ring_count sin protección en la
  // versión anterior.
  unsigned long flags;
  serial_lock_acquire(&flags);
  size_t n = 0;
  while (n < max_len && ring_count > 0) {
    out[n++] = ring[ring_tail];
    ring_tail = (ring_tail + 1) % KLOG_RING_SIZE;
    ring_count--;
  }
  serial_lock_release(flags);
  return n;
}

size_t klog_available(void) { return ring_count; }

void klog_clear(void) { ring_head = ring_tail = ring_count = 0; }

// ---------------------------------------------------------------------------
// [libc] Escritura cruda (sin prefijo ni timestamp) al ring+serial.
// Usado por /dev/kmsg::write. La longitud está acotada por el caller
// (sys_write ya validó access_ok).
// ---------------------------------------------------------------------------
void klog_write_raw(const char *buf, size_t n) {
  if (!klog_ready || !buf || n == 0)
    return;
  unsigned long flags;
  serial_lock_acquire(&flags);
  for (size_t i = 0; i < n; i++)
    out_char(buf[i]);
  serial_lock_release(flags);
}