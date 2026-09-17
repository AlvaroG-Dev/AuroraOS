#ifndef USER_STRING_H
#define USER_STRING_H

#include <stddef.h>
#include <stdint.h>

void *memset(void *dest, int val, size_t count);
void *memcpy(void *dest, const void *src, size_t count);
size_t strlen(const char *str);
char *strcpy(char *dest, const char *src);
int strcmp(const char *s1, const char *s2);
char *itoa(int64_t val, char *buf, int base);

#endif
