// grep: busca patrón (literal) en ficheros.
// usage: grep pattern [file...]

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int grep_fd(int fd, const char *pat, const char *name) {
  int matched = 0;
  char line[4096];
  size_t len = 0;
  char c;
  ssize_t n;
  while ((n = read(fd, &c, 1)) == 1) {
    if (c == '\n' || len == sizeof(line) - 1) {
      line[len] = '\0';
      if (strstr(line, pat)) {
        matched = 1;
        if (name)
          printf("%s:", name);
        printf("%s\n", line);
      }
      len = 0;
      if (c != '\n') {
        // línea demasiado larga: descartamos el resto
        while ((n = read(fd, &c, 1)) == 1 && c != '\n')
          ;
        len = 0;
      }
    } else {
      line[len++] = c;
    }
  }
  if (len > 0) {
    line[len] = '\0';
    if (strstr(line, pat)) {
      matched = 1;
      if (name)
        printf("%s:", name);
      printf("%s\n", line);
    }
  }
  return matched;
}

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: grep pattern [file...]\n");
    return 2;
  }
  const char *pat = argv[1];
  int rc = 1;

  if (argc == 2) {
    if (grep_fd(STDIN_FILENO, pat, NULL))
      rc = 0;
  } else {
    for (int i = 2; i < argc; i++) {
      int fd = open(argv[i], O_RDONLY);
      if (fd < 0) {
        perror(argv[i]);
        continue;
      }
      if (grep_fd(fd, pat, argc > 3 ? argv[i] : NULL))
        rc = 0;
      close(fd);
    }
  }
  return rc;
}