#include "file.h"
#include "string.h"
#include <stdarg.h>

int open(const char *path, int flags) {
  return sys_open(path, flags);
}

int close(int fd) {
  return sys_close(fd);
}

int64_t read(int fd, void *buf, size_t count) {
  return sys_read(fd, buf, count);
}

int64_t write(int fd, const void *buf, size_t count) {
  return sys_write(fd, buf, count);
}

int64_t lseek(int fd, int64_t offset, int whence) {
  return sys_seek(fd, offset, whence);
}

int fstat(int fd, stat_t *st) {
  return sys_fstat(fd, st);
}

void puts(const char *str) {
  if (!str) return;
  write(1, str, strlen(str));
  write(1, "\n", 1);
}

void printf(const char *fmt, ...) {
  if (!fmt) return;
  va_list args;
  va_start(args, fmt);

  for (size_t i = 0; fmt[i] != '\0'; i++) {
    if (fmt[i] == '%' && fmt[i+1] != '\0') {
      i++;
      switch (fmt[i]) {
        case 's': {
          const char *s = va_arg(args, const char *);
          if (!s) s = "(null)";
          write(1, s, strlen(s));
          break;
        }
        case 'd':
        case 'i': {
          int d = va_arg(args, int);
          char buf[32];
          itoa((int64_t)d, buf, 10);
          write(1, buf, strlen(buf));
          break;
        }
        case 'u': {
          unsigned int u = va_arg(args, unsigned int);
          char buf[32];
          itoa((int64_t)u, buf, 10);
          write(1, buf, strlen(buf));
          break;
        }
        case 'x':
        case 'X': {
          unsigned int x = va_arg(args, unsigned int);
          char buf[32];
          itoa((int64_t)x, buf, 16);
          write(1, buf, strlen(buf));
          break;
        }
        case 'c': {
          char c = (char)va_arg(args, int);
          write(1, &c, 1);
          break;
        }
        case '%': {
          write(1, "%", 1);
          break;
        }
        default:
          write(1, &fmt[i-1], 2);
          break;
      }
    } else {
      write(1, &fmt[i], 1);
    }
  }

  va_end(args);
}
