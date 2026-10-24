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
void (*klog_fb_hook)(char c, klog_level_t level, klog_fb_style_t style) = NULL;

typedef struct {
  char c;
  uint8_t style;
} klog_step_char_t;

#define KLOG_STEP_BUF_SIZE 512
static klog_step_char_t g_step_buf[KLOG_STEP_BUF_SIZE];
static size_t g_step_len = 0;
static int g_step_pending = 0;
static int g_step_capturing = 0;
static klog_level_t g_step_level = KLOG_INFO;

// Nivel y estilo "actuales" para el hook de framebuffer.
static klog_level_t g_fb_level = KLOG_INFO;
static klog_fb_style_t g_fb_style = KLOG_FB_BODY;

// ---------------------------------------------------------------------------
// TSC (Time Stamp Counter)
// ---------------------------------------------------------------------------
static inline uint64_t rdtsc(void) {
  uint32_t lo, hi;
  __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
  return ((uint64_t)hi << 32) | lo;
}

static uint64_t tsc_freq_hz = 2600000000ULL;
static uint64_t tsc_origin = 0;

void klog_set_tsc_freq(uint64_t hz) {
  /* [SAFETY] Rechazar frecuencias implausibles. Un valor pequeño
   * hace que format_timestamp() divida por un divisor casi cero y
   * genere timestamps absurdos tipo [2477825.764648]. Es mejor
   * mantener el default (2.6 GHz) que aceptar basura. */
  if (hz < 100000000ULL) /* < 100 MHz */
    return;
  tsc_freq_hz = hz;
}

uint64_t klog_get_tsc_freq(void) { return tsc_freq_hz; }

// ---------------------------------------------------------------------------
// Ring
// ---------------------------------------------------------------------------
static void ring_push(char c) {
  if (ring_count == KLOG_RING_SIZE) {
    // Ring lleno: hay que evictar desde el tail para hacer sitio.
    //
    // Si el tail está a mitad de una línea (porque el ring ya ha dado
    // varias vueltas y el último evict dejó el cursor en cualquier
    // sitio), avanzamos hasta el próximo '\n' ANTES de evictar nada.
    // Así la primera línea del ring siempre empieza limpia.
    //
    // El coste de este while es cero cuando el ring ya está estabilizado
    // en frontera de línea (el caso normal tras el arranque), porque el
    // primer byte del tail YA es '\n' — bueno, ya es el carácter
    // siguiente a un '\n'.
    if (ring[ring_tail] != '\n' || ring_tail == ring_head) {
      // Buscamos el próximo '\n' para dejarlo como primer byte
      // "limpiable". En la práctica, retrocedemos hasta el '\n'
      // anterior si lo hay, o avanzamos hasta el siguiente si no.
      //
      // Estrategia simple: mientras el byte del tail NO sea '\n',
      // lo descartamos. Tras el bucle, descartamos también el '\n'.
      while (ring_count > 0 && ring[ring_tail] != '\n') {
        ring_tail = (ring_tail + 1) % KLOG_RING_SIZE;
        ring_count--;
      }
      if (ring_count > 0 && ring[ring_tail] == '\n') {
        ring_tail = (ring_tail + 1) % KLOG_RING_SIZE;
        ring_count--;
      }
    } else {
      // Caso rápido: el tail ya está en '\n' (línea anterior limpia).
      // Avanzamos un byte para descartar el '\n' y dejamos hueco.
      ring_tail = (ring_tail + 1) % KLOG_RING_SIZE;
      ring_count--;
    }
  }

  ring[ring_head] = c;
  ring_head = (ring_head + 1) % KLOG_RING_SIZE;
  ring_count++;
}
// ---------------------------------------------------------------------------
// Salida: ring + serial (SIN coger el lock — el llamante ya lo tiene)
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Captura de "boot steps".
//
// klog_boot_step() NO emite nada inmediatamente: formatea la línea completa
// (con su prefijo) y la guarda en un buffer. Se emite cuando llega
// klog_boot_ok() (con "  [  OK  ]" al final), klog_boot_warn() (con
// "  [WARN] msg"), o cualquier otro LOG_* (que cierra el step con un
// newline y sigue con lo suyo en la línea siguiente).
//
// La captura guarda (char, style) para reproducir el coloreado de la
// consola de framebuffer al hacer el replay.
// ---------------------------------------------------------------------------

static void out_char(char c) {
  if (g_step_capturing) {
    if (g_step_len < KLOG_STEP_BUF_SIZE) {
      g_step_buf[g_step_len].c = c;
      g_step_buf[g_step_len].style = (uint8_t)g_fb_style;
      g_step_len++;
    }
    return;
  }

  ring_push(c);
  if (klog_fb_hook)
    klog_fb_hook(c, g_fb_level, g_fb_style);
  if (c == '\n')
    serial_putc_locked('\r');
  serial_putc_locked(c);
}

// Reproduce el contenido del step capturado, respetando el estilo por
// carácter. NO añade newline al final.
static void replay_step_locked(void) {
  for (size_t i = 0; i < g_step_len; i++) {
    char c = g_step_buf[i].c;
    ring_push(c);
    if (klog_fb_hook)
      klog_fb_hook(c, g_step_level, (klog_fb_style_t)g_step_buf[i].style);
    if (c == '\n')
      serial_putc_locked('\r');
    serial_putc_locked(c);
  }
}

// Cierra un step pendiente con un newline, sin marcador.
static void flush_step_locked(void) {
  if (!g_step_pending)
    return;
  g_step_pending = 0;
  replay_step_locked();
  out_char('\n');
}

// ---------------------------------------------------------------------------
// Timestamp relativo a tsc_origin
// ---------------------------------------------------------------------------
static void format_timestamp(char *buf, size_t buflen) {
  uint64_t now = rdtsc();
  uint64_t delta = now - tsc_origin;

  uint64_t secs = delta / tsc_freq_hz;
  uint64_t remainder = delta % tsc_freq_hz;
  uint64_t usec = (remainder * 1000000ULL) / tsc_freq_hz;

  char tmp[24];
  int n = 0;

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

  g_fb_level = level;

  g_fb_style = KLOG_FB_TS;
  out_char('[');
  for (char *p = ts; *p; p++)
    out_char(*p);
  out_char(']');
  out_char(' ');

  g_fb_style = KLOG_FB_LEVEL;
  const char *name = level_name(level);
  for (const char *p = name; *p; p++)
    out_char(*p);
  out_char(' ');

  g_fb_style = KLOG_FB_BODY;
}

// ---------------------------------------------------------------------------
// Números
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
  tsc_origin = rdtsc();
  klog_ready = 1;
}

void klog_set_level(klog_level_t lvl) { min_level = lvl; }
klog_level_t klog_get_level(void) { return min_level; }

// ---------------------------------------------------------------------------
// [FIX] Todas las funciones públicas de klog usan el mismo patrón:
//   acquire → if (reentered) return → escribir → release
//
// Si venimos de una reentrada (LOG desde dentro de otro LOG en el
// mismo CPU), descartamos el mensaje. Es infinitamente mejor perder
// un log a colgar el kernel en un deadlock irrecuperable.
// ---------------------------------------------------------------------------

void klog_vprintf(klog_level_t level, const char *fmt, va_list ap) {
  if (!klog_ready || level > min_level)
    return;

  unsigned long flags;
  serial_lock_acquire(&flags);
  if (flags == SERIAL_LOCK_REENTERED)
    return;

  flush_step_locked();

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
  if (flags == SERIAL_LOCK_REENTERED) {
    va_end(ap);
    return;
  }

  // Un log cualquiera cierra el boot step que estuviera abierto: así los
  // mensajes internos de un init no se pegan al "[INIT] XXX..." de arriba.
  flush_step_locked();

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
  if (flags == SERIAL_LOCK_REENTERED)
    return;

  flush_step_locked();

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
  if (flags == SERIAL_LOCK_REENTERED)
    return;

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
  if (flags == SERIAL_LOCK_REENTERED)
    return;

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
  if (flags == SERIAL_LOCK_REENTERED)
    return;

  emit_prefix(level);
  emit_num(val, 10, 0);
  out_char('\n');
  serial_lock_release(flags);
}

// ---------------------------------------------------------------------------
// Calibración del TSC contra el PIT
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Lee la frecuencia del TSC vía CPUID. Es la fuente canónica, no depende
// del PIT ni de interrupciones, y funciona incluso con el CPU a P-state
// mínimo (el TSC es "invariant" en CPUs modernas).
// Devuelve 0 si no se puede determinar.
// ---------------------------------------------------------------------------
static uint64_t tsc_freq_from_cpuid(void) {
  uint32_t a, b, c, d;

  /* CPUID.0x15: ratio TSC / crystal clock.
   * Sólo lo aceptamos si la frecuencia resultante es plausible. */
  __asm__ volatile("cpuid"
                   : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                   : "a"(0x15), "c"(0));
  if (a != 0 && b != 0) {
    uint32_t crystal_hz = c ? c : 24000000;
    uint64_t freq = (uint64_t)crystal_hz * b / a;

    /* [FIX-TCG] QEMU TCG expone leaf 0x15 con basura. Requerimos
     * al menos 100 MHz (ninguna CPU x86-64 real tiene TSC por debajo). */
    if (freq >= 100000000ULL)
      return freq;
  }

  /* CPUID.0x16: frecuencia base en MHz. EAX = base. */
  __asm__ volatile("cpuid"
                   : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                   : "a"(0x16), "c"(0));
  /* [FIX-TCG] Mismo criterio: si devuelve < 100 MHz, es basura. */
  if (a >= 100)
    return (uint64_t)a * 1000000ULL;

  return 0;
}

void klog_calibrate_tsc(uint32_t ticks_to_wait, uint32_t pit_hz) {
  /* =====================================================================
   * Nueva estrategia de calibración:
   * 1. CPUID.15/16 (Arquitectura x86): Altamente fiable, inmune a P-states.
   * 2. PIT (Fallback): Útil en hardware antiguo o VMs con TCG, pero
   *    propenso a jitter y pérdida de ticks.
   * ===================================================================== */

  uint64_t freq_cpuid = tsc_freq_from_cpuid();

  // 1. Intentar CPUID primero (rango plausible: 100 MHz a 15 GHz)
  if (freq_cpuid >= 100000000ULL && freq_cpuid <= 15000000000ULL) {
    klog_set_tsc_freq(freq_cpuid);
    LOG_INFO("[TSC] Calibrado vía CPUID: %lu MHz",
             (unsigned long)(freq_cpuid / 1000000));
    return;
  }

  // 2. Fallback: PIT (solo si CPUID falló o devolvió basura)
  uint64_t freq_pit = 0;
  if (ticks_to_wait > 0 && pit_hz > 0) {
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

    uint64_t cycles = tsc_end - tsc_start;
    uint64_t real_ticks = tick_count - start_tick;

    LOG_INFO("[TSC-CAL] PIT: pidió=%u obtuvo=%lu pit_hz=%u cycles=%lu",
             ticks_to_wait, (unsigned long)real_ticks, pit_hz,
             (unsigned long)cycles);

    if (real_ticks > 0) {
      freq_pit = (cycles * pit_hz) / real_ticks;
    }
  }

  if (freq_pit >= 100000000ULL && freq_pit <= 15000000000ULL) {
    klog_set_tsc_freq(freq_pit);
    LOG_INFO("[TSC] Calibrado vía PIT (fallback): %lu MHz",
             (unsigned long)(freq_pit / 1000000));
    return;
  }

  LOG_WARN("[TSC] Calibración fallida (CPUID y PIT inválidos); manteniendo "
           "default %lu MHz",
           (unsigned long)(klog_get_tsc_freq() / 1000000));
}

// ---------------------------------------------------------------------------
// Acceso al ring (todas con el mismo patrón reentrante)
// ---------------------------------------------------------------------------
size_t klog_read(char *out, size_t max_len) {
  if (!out || max_len == 0)
    return 0;

  unsigned long flags;
  serial_lock_acquire(&flags);
  if (flags == SERIAL_LOCK_REENTERED)
    return 0;

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

void klog_clear(void) {
  // Nota: ring_head/tail/count son volatile size_t. Escribirlos sin lock
  // desde aquí es benigno pero puede perder bytes concurrentes. Aceptable.
  ring_head = ring_tail = ring_count = 0;
}

void klog_write_raw(const char *buf, size_t n) {
  if (!klog_ready || !buf || n == 0)
    return;

  unsigned long flags;
  serial_lock_acquire(&flags);
  if (flags == SERIAL_LOCK_REENTERED)
    return;

  for (size_t i = 0; i < n; i++)
    out_char(buf[i]);

  serial_lock_release(flags);
}

size_t klog_peek(char *out, size_t max_len) {
  if (!out || max_len == 0)
    return 0;

  unsigned long flags;
  serial_lock_acquire(&flags);
  if (flags == SERIAL_LOCK_REENTERED)
    return 0;

  size_t n = 0;
  size_t tail = ring_tail;
  size_t count = ring_count;
  while (n < max_len && count > 0) {
    out[n++] = ring[tail];
    tail = (tail + 1) % KLOG_RING_SIZE;
    count--;
  }
  serial_lock_release(flags);
  return n;
}

size_t klog_size(void) { return KLOG_RING_SIZE; }

// ---------------------------------------------------------------------------
// Helpers de estado de arranque (estilo Linux)
//
// Todos comparten patrón: acquire → reentry check → emitir → release.
// boot_vprintf_locked() reusa la lógica de formato de klog_vprintf_locked
// pero NO emite newline al final; el llamante decide si cerrar la línea.
// ---------------------------------------------------------------------------
static void boot_vprintf_locked(klog_level_t level, const char *fmt,
                                va_list ap) {
  emit_prefix(level);
  // Reusamos el formateador principal, pero sin el '\n' automático:
  // klog_vprintf_locked() ya no añade '\n' por sí solo (lo hace
  // klog_printf). Así que podemos llamarlo directamente.
  klog_vprintf_locked(level, fmt, ap);
}

void klog_boot_step(const char *fmt, ...) {
  if (!klog_ready || !fmt)
    return;

  unsigned long flags;
  serial_lock_acquire(&flags);
  if (flags == SERIAL_LOCK_REENTERED)
    return;

  // Si había un step abierto sin cerrar, lo volcamos primero.
  flush_step_locked();

  g_step_len = 0;
  g_step_capturing = 1;
  g_step_level = KLOG_INFO;

  va_list ap;
  va_start(ap, fmt);
  klog_vprintf_locked(KLOG_INFO, fmt, ap);
  va_end(ap);

  g_step_capturing = 0;
  g_step_pending = 1;

  serial_lock_release(flags);
}

void klog_boot_ok(void) {
  if (!klog_ready)
    return;

  unsigned long flags;
  serial_lock_acquire(&flags);
  if (flags == SERIAL_LOCK_REENTERED)
    return;

  if (g_step_pending) {
    g_step_pending = 0;
    replay_step_locked();
    g_fb_level = KLOG_INFO;
    g_fb_style = KLOG_FB_OK;
    const char *s = "  [  OK  ]";
    while (*s)
      out_char(*s++);
    g_fb_style = KLOG_FB_BODY;
    out_char('\n');
  }
  // Sin step pendiente: no hacemos nada. Significa que este OK
  // corresponde a un step cuyo init imprimió sus propios logs, y esos
  // logs ya cerraron la línea. Emitir un [  OK  ] huérfano aquí sería
  // ruido.

  serial_lock_release(flags);
}

void klog_boot_warn(const char *fmt, ...) {
  if (!klog_ready || !fmt)
    return;

  unsigned long flags;
  serial_lock_acquire(&flags);
  if (flags == SERIAL_LOCK_REENTERED)
    return;

  va_list ap;
  va_start(ap, fmt);

  if (g_step_pending) {
    g_step_pending = 0;
    replay_step_locked();
    g_fb_level = KLOG_WARN;
    g_fb_style = KLOG_FB_BODY;
    const char *pre = "  [WARN] ";
    while (*pre)
      out_char(*pre++);
    klog_vprintf_locked(KLOG_WARN, fmt, ap);
    out_char('\n');
  } else {
    klog_vprintf_locked(KLOG_WARN, fmt, ap);
    out_char('\n');
  }

  va_end(ap);
  serial_lock_release(flags);
}

void klog_boot_fail(const char *fmt, ...) {
  if (!klog_ready || !fmt)
    return;

  unsigned long flags;
  serial_lock_acquire(&flags);
  if (flags == SERIAL_LOCK_REENTERED)
    return;

  va_list ap;
  va_start(ap, fmt);

  if (g_step_pending) {
    g_step_pending = 0;
    replay_step_locked();
    g_fb_level = KLOG_ERR;
    g_fb_style = KLOG_FB_BODY;
    const char *pre = "  [FAIL] ";
    while (*pre)
      out_char(*pre++);
    klog_vprintf_locked(KLOG_ERR, fmt, ap);
    out_char('\n');
  } else {
    klog_vprintf_locked(KLOG_ERR, fmt, ap);
    out_char('\n');
  }

  va_end(ap);
  serial_lock_release(flags);
}

void klog_boot_info(const char *fmt, ...) {
  if (!klog_ready || !fmt)
    return;

  unsigned long flags;
  serial_lock_acquire(&flags);
  if (flags == SERIAL_LOCK_REENTERED)
    return;

  // Un boot_info es una línea independiente: cerramos cualquier step
  // abierto antes de emitirlo.
  flush_step_locked();

  va_list ap;
  va_start(ap, fmt);
  klog_vprintf_locked(KLOG_INFO, fmt, ap);
  va_end(ap);
  out_char('\n');

  serial_lock_release(flags);
}

// ---------------------------------------------------------------------------
// [dmesg] Número de líneas en el ring. No consume.
// ---------------------------------------------------------------------------
uint64_t klog_line_count(void) {
  unsigned long flags;
  serial_lock_acquire(&flags);
  if (flags == SERIAL_LOCK_REENTERED)
    return 0;

  uint64_t n = 0;
  size_t tail = ring_tail;
  size_t count = ring_count;
  while (count > 0) {
    if (ring[tail] == '\n')
      n++;
    tail = (tail + 1) % KLOG_RING_SIZE;
    count--;
  }
  serial_lock_release(flags);
  return n;
}

// ---------------------------------------------------------------------------
// [dmesg] Snapshot NO destructivo del ring, terminado en NUL.
// Devuelve los bytes escritos (sin contar el NUL).
// ---------------------------------------------------------------------------
size_t klog_snapshot(char *out, size_t max_len) {
  if (!out || max_len < 2)
    return 0;
  size_t n = klog_peek(out, max_len - 1);
  out[n] = '\0';
  return n;
}