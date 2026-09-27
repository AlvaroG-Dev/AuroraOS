#include "../../lib/file.h"
#include "../../lib/string.h"

#define MAX_LINES 512
#define MAX_LINE_LEN 128

// En .bss, no en pila: 64 KB no caben en el stack de usuario (16 KB).
static char lines[MAX_LINES][MAX_LINE_LEN];
static int line_count = 0;

static int read_all_lines(int fd) {
  char buf[512];
  int64_t n;
  size_t cur_len = 0;
  while ((n = read(fd, buf, sizeof(buf))) > 0) {
    for (int64_t i = 0; i < n; i++) {
      char c = buf[i];
      if (c == '\n') {
        if (line_count >= MAX_LINES)
          return -1;
        lines[line_count][cur_len] = '\0';
        line_count++;
        cur_len = 0;
      } else if (cur_len + 1 < MAX_LINE_LEN) {
        lines[line_count][cur_len++] = c;
      }
    }
  }
  if (cur_len > 0 && line_count < MAX_LINES) {
    lines[line_count][cur_len] = '\0';
    line_count++;
  }
  return n < 0 ? -1 : 0;
}

int main(int argc, char **argv) {
  int fd = 0;
  if (argc > 1) {
    fd = open(argv[1], O_RDONLY);
    if (fd < 0) {
      printf("sort: no existe %s\n", argv[1]);
      return 1;
    }
  }

  if (read_all_lines(fd) < 0) {
    puts("sort: demasiadas líneas o error de lectura");
    if (fd != 0)
      close(fd);
    return 1;
  }
  if (fd != 0)
    close(fd);

  // Insertion sort in-place.
  for (int i = 1; i < line_count; i++) {
    char tmp[MAX_LINE_LEN];
    strcpy(tmp, lines[i]);
    int j = i - 1;
    while (j >= 0 && strcmp(lines[j], tmp) > 0) {
      strcpy(lines[j + 1], lines[j]);
      j--;
    }
    strcpy(lines[j + 1], tmp);
  }

  for (int i = 0; i < line_count; i++) {
    write(1, lines[i], strlen(lines[i]));
    write(1, "\n", 1);
  }
  return 0;
}