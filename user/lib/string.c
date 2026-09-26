#include "string.h"

void *memset(void *dest, int val, size_t count) {
  uint8_t *d = (uint8_t *)dest;
  for (size_t i = 0; i < count; i++)
    d[i] = (uint8_t)val;
  return dest;
}

void *memcpy(void *dest, const void *src, size_t count) {
  uint8_t *d = (uint8_t *)dest;
  const uint8_t *s = (const uint8_t *)src;
  for (size_t i = 0; i < count; i++)
    d[i] = s[i];
  return dest;
}

// [libc] memmove: copia correcta cuando las regiones se solapan.
// Si dest < src, copia ascendente; si dest > src, descendente.
void *memmove(void *dest, const void *src, size_t count) {
  uint8_t *d = (uint8_t *)dest;
  const uint8_t *s = (const uint8_t *)src;
  if (d == s || count == 0)
    return dest;
  if (d < s) {
    for (size_t i = 0; i < count; i++)
      d[i] = s[i];
  } else {
    for (size_t i = count; i > 0; i--)
      d[i - 1] = s[i - 1];
  }
  return dest;
}

int memcmp(const void *a, const void *b, size_t count) {
  const uint8_t *pa = (const uint8_t *)a;
  const uint8_t *pb = (const uint8_t *)b;
  for (size_t i = 0; i < count; i++) {
    if (pa[i] != pb[i])
      return (int)pa[i] - (int)pb[i];
  }
  return 0;
}

size_t strlen(const char *str) {
  size_t len = 0;
  while (str[len])
    len++;
  return len;
}

char *strcpy(char *dest, const char *src) {
  char *d = dest;
  while ((*d++ = *src++))
    ;
  return dest;
}

// [libc] strncpy POSIX: copia hasta n chars; si src < n, rellena con \0.
char *strncpy(char *dest, const char *src, size_t n) {
  size_t i = 0;
  for (; i < n && src[i]; i++)
    dest[i] = src[i];
  for (; i < n; i++)
    dest[i] = '\0';
  return dest;
}

char *strcat(char *dest, const char *src) {
  char *d = dest + strlen(dest);
  while ((*d++ = *src++))
    ;
  return dest;
}

// [libc] strncat POSIX: siempre termina con \0.
char *strncat(char *dest, const char *src, size_t n) {
  char *d = dest + strlen(dest);
  size_t i = 0;
  for (; i < n && src[i]; i++)
    d[i] = src[i];
  d[i] = '\0';
  return dest;
}

int strcmp(const char *s1, const char *s2) {
  while (*s1 && (*s1 == *s2)) {
    s1++;
    s2++;
  }
  return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}

int strncmp(const char *s1, const char *s2, size_t n) {
  for (size_t i = 0; i < n; i++) {
    unsigned char c1 = (unsigned char)s1[i];
    unsigned char c2 = (unsigned char)s2[i];
    if (c1 != c2)
      return (int)c1 - (int)c2;
    if (c1 == '\0')
      return 0;
  }
  return 0;
}

// Devuelve puntero al primer c en s, o NULL.
char *strchr(const char *s, int c) {
  if (!s)
    return NULL;
  char ch = (char)c;
  while (*s) {
    if (*s == ch)
      return (char *)s;
    s++;
  }
  if (ch == '\0')
    return (char *)s;
  return NULL;
}

// Devuelve puntero al último c en s, o NULL.
char *strrchr(const char *s, int c) {
  if (!s)
    return NULL;
  char ch = (char)c;
  const char *last = NULL;
  while (*s) {
    if (*s == ch)
      last = s;
    s++;
  }
  if (ch == '\0')
    return (char *)s;
  return (char *)last;
}

int atoi(const char *s) {
  if (!s)
    return 0;
  while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')
    s++;
  int sign = 1;
  if (*s == '-') {
    sign = -1;
    s++;
  } else if (*s == '+') {
    s++;
  }
  int n = 0;
  while (*s >= '0' && *s <= '9') {
    n = n * 10 + (*s - '0');
    s++;
  }
  return sign * n;
}

char *itoa(int64_t val, char *buf, int base) {
  if (base < 2 || base > 36)
    return buf;
  char *ptr = buf;
  int is_neg = 0;
  if (val < 0 && base == 10) {
    is_neg = 1;
    val = -val;
  }
  uint64_t uval = (uint64_t)val;
  char tmp[64];
  int i = 0;
  if (uval == 0) {
    tmp[i++] = '0';
  } else {
    while (uval > 0) {
      int rem = uval % base;
      tmp[i++] = (rem < 10) ? ('0' + rem) : ('a' + rem - 10);
      uval /= base;
    }
  }
  if (is_neg)
    *ptr++ = '-';
  while (i > 0)
    *ptr++ = tmp[--i];
  *ptr = '\0';
  return buf;
}