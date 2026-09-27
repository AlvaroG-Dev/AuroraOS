// sort: ordena líneas.
// usage: sort [file]

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct line {
  char *s;
  size_t n;
};

static int cmp_line(const void *a, const void *b) {
  const struct line *la = a, *lb = b;
  return strcmp(la->s, lb->s);
}

int main(int argc, char **argv) {
  int fd = STDIN_FILENO;
  if (argc > 1) {
    fd = open(argv[1], O_RDONLY);
    if (fd < 0) {
      perror(argv[1]);
      return 1;
    }
  }

  size_t cap = 256, n = 0;
  struct line *lines = malloc(cap * sizeof(*lines));
  if (!lines)
    return 1;

  // Acumular todo el fichero en memoria.
  size_t bufcap = 4096, buflen = 0;
  char *buf = malloc(bufcap);
  if (!buf)
    return 1;
  ssize_t r;
  while ((r = read(fd, buf + buflen, bufcap - buflen - 1)) > 0) {
    buflen += (size_t)r;
    if (buflen + 1 >= bufcap) {
      bufcap *= 2;
      char *nb = realloc(buf, bufcap);
      if (!nb) {
        free(buf);
        free(lines);
        return 1;
      }
      buf = nb;
    }
  }
  if (fd != STDIN_FILENO)
    close(fd);
  buf[buflen] = '\0';

  // Partir en líneas.
  char *p = buf;
  while (*p) {
    char *nl = strchr(p, '\n');
    size_t len = nl ? (size_t)(nl - p) : strlen(p);
    if (n == cap) {
      cap *= 2;
      struct line *nl_arr = realloc(lines, cap * sizeof(*lines));
      if (!nl_arr) {
        free(buf);
        free(lines);
        return 1;
      }
      lines = nl_arr;
    }
    lines[n].s = p;
    lines[n].n = len;
    n++;
    if (!nl)
      break;
    *nl = '\0';
    p = nl + 1;
  }

  qsort(lines, n, sizeof(*lines), cmp_line);

  for (size_t i = 0; i < n; i++) {
    write(STDOUT_FILENO, lines[i].s, lines[i].n);
    write(STDOUT_FILENO, "\n", 1);
  }
  free(buf);
  free(lines);
  return 0;
}