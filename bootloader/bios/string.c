/* bootloader/bios/string.c */
#include "bios.h"

void zero_mem(void *dst, uint32_t n) {
  uint8_t *p = (uint8_t *)dst;
  while (n--)
    *p++ = 0;
}

void copy_mem(void *dst, const void *src, uint32_t n) {
  uint8_t *d = (uint8_t *)dst;
  const uint8_t *s = (const uint8_t *)src;
  while (n--)
    *d++ = *s++;
}

int compare_mem(const void *a, const void *b, uint32_t n) {
  const uint8_t *pa = (const uint8_t *)a;
  const uint8_t *pb = (const uint8_t *)b;
  while (n--) {
    if (*pa != *pb)
      return (int)*pa - (int)*pb;
    pa++;
    pb++;
  }
  return 0;
}

uint32_t str_len(const char *s) {
  uint32_t n = 0;
  while (s[n])
    n++;
  return n;
}

int str_eq(const char *a, const char *b) {
  while (*a && *a == *b) {
    a++;
    b++;
  }
  return *a == *b;
}

int str_eq_n(const char *a, const char *b, uint32_t n) {
  for (uint32_t i = 0; i < n; i++) {
    if (a[i] != b[i])
      return 0;
  }
  return 1;
}

int str_eq_ci_n(const char *a, const char *b, uint32_t n) {
  for (uint32_t i = 0; i < n; i++) {
    char ca = a[i], cb = b[i];
    if (ca >= 'a' && ca <= 'z')
      ca -= 32;
    if (cb >= 'a' && cb <= 'z')
      cb -= 32;
    if (ca != cb)
      return 0;
  }
  return 1;
}

void str_copy(char *dst, const char *src, uint32_t max) {
  uint32_t i = 0;
  while (i + 1 < max && src[i]) {
    dst[i] = src[i];
    i++;
  }
  dst[i] = 0;
}