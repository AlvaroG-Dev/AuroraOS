// head: primeras N líneas de cada fichero (default 10).
// usage: head [-n N] [file...]

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static void head_fd(int fd, long max_lines) {
  if (max_lines <= 0)
    return;
  char buf[4096];
  long lines = 0;
  ssize_t n;
  while ((n = read(fd, buf, sizeof(buf))) > 0) {
    ssize_t start = 0;
    for (ssize_t i = 0; i < n; i++) {
      if (buf[i] == '\n') {
        if (write(STDOUT_FILENO, buf + start, (size_t)(i + 1 - start)) < 0)
          return;
        lines++;
        if (lines >= max_lines)
          return;
        start = i + 1;
      }
    }
    if (start < n) {
      if (write(STDOUT_FILENO, buf + start, (size_t)(n - start)) < 0)
        return;
    }
  }
}

int main(int argc, char **argv) {
  long max_lines = 10;
  int first_file = 1;

  if (first_file < argc && argv[first_file][0] == '-' &&
      argv[first_file][1] == 'n' && argv[first_file][2] == '\0') {
    if (first_file + 1 >= argc) {
      fprintf(stderr, "head: -n requiere argumento\n");
      return 1;
    }
    max_lines = strtol(argv[first_file + 1], NULL, 10);
    first_file += 2;
  }

  if (first_file >= argc) {
    head_fd(STDIN_FILENO, max_lines);
    return 0;
  }
  int rc = 0;
  for (int i = first_file; i < argc; i++) {
    int fd = open(argv[i], O_RDONLY);
    if (fd < 0) {
      perror(argv[i]);
      rc = 1;
      continue;
    }
    head_fd(fd, max_lines);
    close(fd);
  }
  return rc;
}