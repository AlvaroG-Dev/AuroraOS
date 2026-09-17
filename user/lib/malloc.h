#ifndef USER_MALLOC_H
#define USER_MALLOC_H

#include <stddef.h>
#include <stdint.h>

void *malloc(size_t size);
void free(void *ptr);
void *calloc(size_t num, size_t size);
void *realloc(void *ptr, size_t size);

#endif
