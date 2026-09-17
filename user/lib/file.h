#ifndef USER_FILE_H
#define USER_FILE_H

#include "../syscall.h"
#include <stddef.h>
#include <stdint.h>

int open(const char *path, int flags);
int close(int fd);
int64_t read(int fd, void *buf, size_t count);
int64_t write(int fd, const void *buf, size_t count);
int64_t lseek(int fd, int64_t offset, int whence);
int fstat(int fd, stat_t *st);

void puts(const char *str);
void printf(const char *fmt, ...);

#endif
