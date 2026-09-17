#include "string.h"

void *memset(void *dest, int val, size_t count) {
  uint8_t *d = (uint8_t *)dest;
  for (size_t i = 0; i < count; i++) {
    d[i] = (uint8_t)val;
  }
  return dest;
}

void *memcpy(void *dest, const void *src, size_t count) {
  uint8_t *d = (uint8_t *)dest;
  const uint8_t *s = (const uint8_t *)src;
  for (size_t i = 0; i < count; i++) {
    d[i] = s[i];
  }
  return dest;
}

size_t strlen(const char *str) {
  size_t len = 0;
  while (str[len]) len++;
  return len;
}

char *strcpy(char *dest, const char *src) {
  char *d = dest;
  while ((*d++ = *src++));
  return dest;
}

int strcmp(const char *s1, const char *s2) {
  while (*s1 && (*s1 == *s2)) {
    s1++;
    s2++;
  }
  return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}

char *itoa(int64_t val, char *buf, int base) {
  if (base < 2 || base > 36) return buf;
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
  if (is_neg) *ptr++ = '-';
  while (i > 0) {
    *ptr++ = tmp[--i];
  }
  *ptr = '\0';
  return buf;
}
