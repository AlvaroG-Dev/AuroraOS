#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

static int cat_fd(int fd, const char *name) {
  char buf[4096];
  ssize_t n;
  while ((n = read(fd, buf, sizeof(buf))) > 0) {
    ssize_t off = 0;
    while (off < n) {
      ssize_t w = write(STDOUT_FILENO, buf + off, (size_t)(n - off));
      if (w < 0) {
        perror("write");
        return 1;
      }
      off += w;
    }
  }
  if (n < 0) {
    perror(name);
    return 1;
  }
  return 0;
}

int main(int argc, char **argv) {
  if (argc < 2)
    return cat_fd(STDIN_FILENO, "stdin");
  int rc = 0;
  for (int i = 1; i < argc; i++) {
    int fd = open(argv[i], O_RDONLY);
    if (fd < 0) {
      perror(argv[i]);
      rc = 1;
      continue;
    }
    if (cat_fd(fd, argv[i]) != 0)
      rc = 1;
    close(fd);
  }
  return rc;
}