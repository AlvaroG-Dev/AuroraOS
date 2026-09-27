#include "../../lib/file.h"
#include "../../lib/string.h"

static int head_fd(int fd, int n) {
  if (n <= 0)
    return 0;
  char buf[512];
  int64_t got;
  int lines = 0;
  while ((got = read(fd, buf, sizeof(buf))) > 0) {
    for (int64_t i = 0; i < got; i++) {
      write(1, &buf[i], 1);
      if (buf[i] == '\n') {
        lines++;
        if (lines >= n)
          return 0;
      }
    }
  }
  return got < 0 ? -1 : 0;
}

int main(int argc, char **argv) {
  int n = 10;
  int argi = 1;

  if (argi + 1 < argc && strcmp(argv[argi], "-n") == 0) {
    n = atoi(argv[argi + 1]);
    argi += 2;
  } else if (argi < argc && argv[argi][0] == '-' && argv[argi][1] >= '0' &&
             argv[argi][1] <= '9') {
    n = atoi(argv[argi] + 1);
    argi++;
  }

  if (argi >= argc) {
    if (head_fd(0, n) < 0) {
      puts("head: error leyendo stdin");
      return 1;
    }
    return 0;
  }

  for (int i = argi; i < argc; i++) {
    int fd = open(argv[i], O_RDONLY);
    if (fd < 0) {
      printf("head: no existe %s\n", argv[i]);
      continue;
    }
    head_fd(fd, n);
    close(fd);
  }
  return 0;
}