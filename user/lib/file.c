#include "file.h"
#include "string.h"
#include <stdarg.h>

int mkdir(const char *path) { return sys_mkdir(path); }
int unlink(const char *path) { return sys_unlink(path); }
int create(const char *path) { return sys_create(path); }
int readdir(const char *path, uint64_t index, dirent_t *out) {
  return sys_readdir(path, index, out);
}

int open(const char *path, int flags) { return sys_open(path, flags); }

int close(int fd) { return sys_close(fd); }

int64_t read(int fd, void *buf, size_t count) {
  return sys_read(fd, buf, count);
}

int64_t write(int fd, const void *buf, size_t count) {
  return sys_write(fd, buf, count);
}

int64_t lseek(int fd, int64_t offset, int whence) {
  return sys_seek(fd, offset, whence);
}

int fstat(int fd, stat_t *st) { return sys_fstat(fd, st); }

void puts(const char *str) {
  if (!str)
    return;
  write(1, str, strlen(str));
  write(1, "\n", 1);
}

void printf(const char *fmt, ...) {
  if (!fmt)
    return;
  va_list args;
  va_start(args, fmt);

  for (size_t i = 0; fmt[i] != '\0'; i++) {
    if (fmt[i] == '%' && fmt[i + 1] != '\0') {
      i++;
      switch (fmt[i]) {
      case 's': {
        const char *s = va_arg(args, const char *);
        if (!s)
          s = "(null)";
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
        write(1, &fmt[i - 1], 2);
        break;
      }
    } else {
      write(1, &fmt[i], 1);
    }
  }

  va_end(args);
}

int copy_file(const char *src, const char *dst) {
  if (!src || !dst)
    return -1;

  int in = open(src, 0x0000 /* O_RDONLY */);
  if (in < 0)
    return -1;

  // Intenta abrir el destino. Si no existe, lo crea y reintenta.
  int out = open(dst, 0x0001 /* O_WRONLY */);
  if (out < 0) {
    if (create(dst) != 0) {
      close(in);
      return -1;
    }
    out = open(dst, 0x0001 /* O_WRONLY */);
    if (out < 0) {
      close(in);
      return -1;
    }
  }

  char buf[4096];
  int rc = 0;
  for (;;) {
    int64_t n = read(in, buf, sizeof(buf));
    if (n < 0) {
      rc = -1;
      break;
    }
    if (n == 0)
      break;

    int64_t written = 0;
    while (written < n) {
      int64_t w = write(out, buf + written, (size_t)(n - written));
      if (w <= 0) {
        rc = -1;
        goto done;
      }
      written += w;
    }
  }

done:
  close(in);
  close(out);
  return rc;
}

int rename_path(const char *oldpath, const char *newpath) {
  return sys_rename(oldpath, newpath);
}

int chdir(const char *path) { return sys_chdir(path); }
int getcwd(char *buf, size_t size) { return sys_getcwd(buf, size); }