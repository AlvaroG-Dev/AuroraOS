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

// ---------------------------------------------------------------------------
// [libc] printf-family formateado a buffer.
//
// Implementación compacta: soporta %s %d %i %u %x %X %c %p %%,
// modificadores l/ll/z y width con cero a la izquierda (%02x, %08x...).
// No soporta flags complejos (justificación, precisión de floats, etc.).
// ---------------------------------------------------------------------------
struct outbuf {
  char *buf;
  size_t cap;
  size_t pos;
};

static void ob_char(struct outbuf *o, char c) {
  if (o->pos + 1 < o->cap)
    o->buf[o->pos] = c;
  o->pos++;
}

static void ob_str(struct outbuf *o, const char *s) {
  while (*s)
    ob_char(o, *s++);
}

static void ob_uint(struct outbuf *o, uint64_t val, int base, int min_digits,
                    int upper) {
  const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
  char tmp[32];
  int i = 0;
  if (val == 0)
    tmp[i++] = '0';
  else
    while (val) {
      tmp[i++] = digits[val % base];
      val /= base;
    }
  while (i < min_digits)
    tmp[i++] = '0';
  while (i-- > 0)
    ob_char(o, tmp[i]);
}

static void ob_int(struct outbuf *o, int64_t v, int min_digits) {
  if (v < 0) {
    ob_char(o, '-');
    ob_uint(o, (uint64_t)(-v), 10, min_digits, 0);
  } else {
    ob_uint(o, (uint64_t)v, 10, min_digits, 0);
  }
}

int vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap) {
  struct outbuf o = {buf, cap, 0};
  if (!buf || cap == 0) {
    // Contar longitud sin escribir.
    o.buf = NULL;
    o.cap = 0;
  }
  while (*fmt) {
    if (*fmt != '%') {
      ob_char(&o, *fmt++);
      continue;
    }
    fmt++;

    int min_digits = 0;
    int is_long = 0, is_llong = 0, is_size = 0;

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
      ob_int(&o, v, min_digits);
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
      ob_uint(&o, v, 10, min_digits, 0);
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
      ob_uint(&o, v, 16, min_digits ? min_digits : 1, *fmt == 'X');
      break;
    }
    case 'p': {
      uint64_t v = (uint64_t)va_arg(ap, void *);
      ob_char(&o, '0');
      ob_char(&o, 'x');
      ob_uint(&o, v, 16, 16, 0);
      break;
    }
    case 'c':
      ob_char(&o, (char)va_arg(ap, int));
      break;
    case 's': {
      const char *s = va_arg(ap, const char *);
      if (!s)
        s = "(null)";
      ob_str(&o, s);
      break;
    }
    case '%':
      ob_char(&o, '%');
      break;
    default:
      ob_char(&o, '%');
      ob_char(&o, *fmt);
      break;
    }
    if (*fmt)
      fmt++;
  }

  // Terminador. Si cap>0, siempre escribimos '\0'.
  if (cap > 0) {
    size_t term = (o.pos < cap) ? o.pos : cap - 1;
    buf[term] = '\0';
  }
  return (int)o.pos;
}

int snprintf(char *buf, size_t size, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int r = vsnprintf(buf, size, fmt, ap);
  va_end(ap);
  return r;
}