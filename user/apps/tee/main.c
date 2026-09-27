#include "../../lib/file.h"
#include "../../lib/string.h"

int main(int argc, char **argv) {
  int append = 0;
  int argi = 1;
  if (argi < argc && strcmp(argv[argi], "-a") == 0) {
    append = 1;
    argi++;
  }
  if (argi >= argc) {
    puts("uso: tee [-a] <file>");
    return 1;
  }

  const char *path = argv[argi];
  int fd = open(path, O_WRONLY);
  if (fd < 0) {
    if (create(path) != 0) {
      printf("tee: no se pudo crear %s\n", path);
      return 1;
    }
    fd = open(path, O_WRONLY);
    if (fd < 0) {
      printf("tee: no se pudo abrir %s\n", path);
      return 1;
    }
  }
  if (append) {
    lseek(fd, 0, SEEK_END);
  } else {
    close(fd);
    unlink(path);
    create(path);
    fd = open(path, O_WRONLY);
    if (fd < 0) {
      printf("tee: no se pudo reabrir %s\n", path);
      return 1;
    }
  }

  char buf[512];
  int64_t n;
  while ((n = read(0, buf, sizeof(buf))) > 0) {
    write(1, buf, (size_t)n);
    write(fd, buf, (size_t)n);
  }
  close(fd);
  return 0;
}