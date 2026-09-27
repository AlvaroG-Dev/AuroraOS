// wc: cuenta líneas, palabras y bytes.
// usage: wc [-l] [-w] [-c] [file...]

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void count_fd(int fd, unsigned long *lines, unsigned long *words,
                     unsigned long *bytes, int do_lines, int do_words,
                     int do_bytes) {
  unsigned char buf[4096];
  int in_word = 0;
  ssize_t n;
  while ((n = read(fd, buf, sizeof(buf))) > 0) {
    if (do_bytes)
      *bytes += (unsigned long)n;
    if (do_lines || do_words) {
      for (ssize_t i = 0; i < n; i++) {
        unsigned char c = buf[i];
        if (do_lines && c == '\n')
          (*lines)++;
        if (do_words) {
          if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
              c == '\v') {
            in_word = 0;
          } else if (!in_word) {
            in_word = 1;
            (*words)++;
          }
        }
      }
    }
  }
}

int main(int argc, char **argv) {
  int do_lines = 0, do_words = 0, do_bytes = 0;
  int first_file = 1;

  while (first_file < argc && argv[first_file][0] == '-' &&
         argv[first_file][1] != '\0') {
    for (const char *p = argv[first_file] + 1; *p; p++) {
      if (*p == 'l')
        do_lines = 1;
      else if (*p == 'w')
        do_words = 1;
      else if (*p == 'c')
        do_bytes = 1;
      else {
        fprintf(stderr, "wc: opción desconocida -%c\n", *p);
        return 1;
      }
    }
    first_file++;
  }
  if (!do_lines && !do_words && !do_bytes)
    do_lines = do_words = do_bytes = 1;

  int rc = 0;
  if (first_file >= argc) {
    unsigned long l = 0, w = 0, b = 0;
    count_fd(STDIN_FILENO, &l, &w, &b, do_lines, do_words, do_bytes);
    if (do_lines)
      printf("%lu ", l);
    if (do_words)
      printf("%lu ", w);
    if (do_bytes)
      printf("%lu ", b);
    printf("\n");
  } else {
    for (int i = first_file; i < argc; i++) {
      int fd = open(argv[i], O_RDONLY);
      if (fd < 0) {
        perror(argv[i]);
        rc = 1;
        continue;
      }
      unsigned long l = 0, w = 0, b = 0;
      count_fd(fd, &l, &w, &b, do_lines, do_words, do_bytes);
      close(fd);
      if (do_lines)
        printf("%lu ", l);
      if (do_words)
        printf("%lu ", w);
      if (do_bytes)
        printf("%lu ", b);
      printf("%s\n", argv[i]);
    }
  }
  return rc;
}