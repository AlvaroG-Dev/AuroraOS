#include "../../lib/file.h"

int main(int argc, char **argv) {
  if (argc < 2) {
    puts("uso: cat <file>...");
    return 1;
  }
  int rc = 0;
  for (int i = 1; i < argc; i++) {
    int fd = open(argv[i], O_RDONLY);
    if (fd < 0) {
      printf("[cat] no existe %s\n", argv[i]);
      rc = 1;
      continue;
    }
    char buf[512];
    int64_t n;
    int64_t total = 0;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
      write(1, buf, n);
      total += n;
    }
    close(fd);
    printf("\n[cat] %u bytes leídos\n", (unsigned int)total);
  }
  return rc;
}