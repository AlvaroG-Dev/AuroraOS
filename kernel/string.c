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