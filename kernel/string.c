// kernel/string.c
// Implementación de funciones de strings y memoria para el kernel.
//
// Estas funciones son necesarias porque el kernel compila con -ffreestanding
// (sin libc). Además, el compilador puede emitir llamadas implícitas a
// memcpy/memset/memmove al inicializar estructuras o copiar arrays grandes.

#include "string.h"
#include <stdint.h>

// ===========================================================================
// Memoria
// ===========================================================================

void *memset(void *dest, int c, size_t n) {
  uint8_t *d = (uint8_t *)dest;
  uint8_t v = (uint8_t)c;

  // Si n es grande, usar rep stosb (ERMSB en CPUs modernas es rápido).
  // Para n pequeño, un bucle simple es más rápido (evita setup de rep).
  if (n >= 64) {
    __asm__ volatile("rep stosb" : "+D"(d), "+c"(n) : "a"(v) : "memory");
    return dest;
  }

  while (n--) {
    *d++ = v;
  }
  return dest;
}

void *memcpy(void *dest, const void *src, size_t n) {
  uint8_t *d = (uint8_t *)dest;
  const uint8_t *s = (const uint8_t *)src;

  if (n >= 64) {
    __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
    return dest;
  }

  while (n--) {
    *d++ = *s++;
  }
  return dest;
}

void *memmove(void *dest, const void *src, size_t n) {
  uint8_t *d = (uint8_t *)dest;
  const uint8_t *s = (const uint8_t *)src;

  if (d == s || n == 0) {
    return dest;
  }

  if (d < s) {
    // Copia hacia adelante
    while (n--) {
      *d++ = *s++;
    }
  } else {
    // Copia hacia atrás (solapamiento)
    d += n;
    s += n;
    while (n--) {
      *--d = *--s;
    }
  }
  return dest;
}

int memcmp(const void *a, const void *b, size_t n) {
  const uint8_t *pa = (const uint8_t *)a;
  const uint8_t *pb = (const uint8_t *)b;
  while (n--) {
    if (*pa != *pb) {
      return (int)*pa - (int)*pb;
    }
    pa++;
    pb++;
  }
  return 0;
}

void *memchr(const void *s, int c, size_t n) {
  const unsigned char *p = (const unsigned char *)s;
  unsigned char target = (unsigned char)c;
  for (size_t i = 0; i < n; i++) {
    if (p[i] == target)
      return (void *)(p + i);
  }
  return NULL;
}

// ===========================================================================
// Strings
// ===========================================================================

size_t strlen(const char *s) {
  const char *p = s;
  while (*p) {
    p++;
  }
  return (size_t)(p - s);
}

int strcmp(const char *a, const char *b) {
  while (*a && (*a == *b)) {
    a++;
    b++;
  }
  return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
  while (n && *a && (*a == *b)) {
    a++;
    b++;
    n--;
  }
  if (n == 0) {
    return 0;
  }
  return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

char *strcpy(char *dest, const char *src) {
  char *d = dest;
  while ((*d++ = *src++)) {
    // vacío
  }
  return dest;
}

char *strncpy(char *dest, const char *src, size_t n) {
  char *d = dest;
  while (n && *src) {
    *d++ = *src++;
    n--;
  }
  // Rellenar con ceros si src es más corto
  while (n--) {
    *d++ = '\0';
  }
  return dest;
}

char *strchr(const char *s, int c) {
  while (*s) {
    if (*s == (char)c) {
      return (char *)s;
    }
    s++;
  }
  if (c == '\0') {
    return (char *)s;
  }
  return NULL;
}

// ---------------------------------------------------------------------------
// snprintf / vsnprintf
//
// Implementación compacta sin dependencias de libc. Soporta:
//   %s %d %i %u %x %X %c %p %%
//   modificadores l, ll, z
//   width con cero a la izquierda (%02x, %08x, %5d)
//
// No soporta floats, precisión, justificación con espacios, ni flags
// complejos. Es suficiente para logs y mensajes internos del kernel.
// ---------------------------------------------------------------------------

struct ksn_out {
  char *buf;
  size_t cap;
  size_t pos;
};

static void ksn_char(struct ksn_out *o, char c) {
  if (o->pos + 1 < o->cap)
    o->buf[o->pos] = c;
  o->pos++;
}

static void ksn_str(struct ksn_out *o, const char *s) {
  while (*s)
    ksn_char(o, *s++);
}

static void ksn_uint(struct ksn_out *o, uint64_t val, int base, int min_digits,
                     int upper) {
  const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
  char tmp[32];
  int i = 0;

  if (val == 0) {
    tmp[i++] = '0';
  } else {
    while (val > 0 && i < (int)sizeof(tmp)) {
      tmp[i++] = digits[val % (uint64_t)base];
      val /= (uint64_t)base;
    }
  }
  while (i < min_digits && i < (int)sizeof(tmp))
    tmp[i++] = '0';
  while (i-- > 0)
    ksn_char(o, tmp[i]);
}

static void ksn_int(struct ksn_out *o, int64_t v, int min_digits) {
  if (v < 0) {
    ksn_char(o, '-');
    // Manejo del INT64_MIN sin overflow (aunque es raro en el kernel).
    uint64_t u = (uint64_t)(-(v + 1)) + 1;
    ksn_uint(o, u, 10, min_digits, 0);
  } else {
    ksn_uint(o, (uint64_t)v, 10, min_digits, 0);
  }
}

int vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap) {
  struct ksn_out o = {buf, cap, 0};

  if (!buf || cap == 0) {
    o.buf = NULL;
    o.cap = 0;
  }

  while (*fmt) {
    if (*fmt != '%') {
      ksn_char(&o, *fmt++);
      continue;
    }
    fmt++;

    int min_digits = 0;
    int is_long = 0;
    int is_llong = 0;
    int is_size = 0;

    while (*fmt >= '0' && *fmt <= '9') {
      min_digits = min_digits * 10 + (*fmt - '0');
      fmt++;
    }
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
      ksn_int(&o, v, min_digits);
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
      ksn_uint(&o, v, 10, min_digits, 0);
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
      ksn_uint(&o, v, 16, min_digits ? min_digits : 1, *fmt == 'X');
      break;
    }
    case 'p': {
      uint64_t v = (uint64_t)va_arg(ap, void *);
      ksn_char(&o, '0');
      ksn_char(&o, 'x');
      ksn_uint(&o, v, 16, 16, 0);
      break;
    }
    case 'c':
      ksn_char(&o, (char)va_arg(ap, int));
      break;
    case 's': {
      const char *s = va_arg(ap, const char *);
      if (!s)
        s = "(null)";
      ksn_str(&o, s);
      break;
    }
    case '%':
      ksn_char(&o, '%');
      break;
    default:
      ksn_char(&o, '%');
      ksn_char(&o, *fmt);
      break;
    }

    if (*fmt)
      fmt++;
  }

  if (cap > 0) {
    size_t term = (o.pos < cap) ? o.pos : cap - 1;
    buf[term] = '\0';
  }
  return (int)o.pos;
}

int snprintf(char *buf, size_t cap, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int r = vsnprintf(buf, cap, fmt, ap);
  va_end(ap);
  return r;
}