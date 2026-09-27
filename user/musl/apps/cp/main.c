// cp: copia fichero.
// usage: cp source dest

#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

int main(int argc, char **argv) {
  if (argc != 3) {
    fprintf(stderr, "usage: cp source dest\n");
    return 1;
  }
  int in = open(argv[1], O_RDONLY);
  if (in < 0) {
    perror(argv[1]);
    return 1;
  }
  int out = open(argv[2], O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (out < 0) {
    perror(argv[2]);
    close(in);
    return 1;
  }
  char buf[4096];
  ssize_t n;
  int rc = 0;
  while ((n = read(in, buf, sizeof(buf))) > 0) {
    ssize_t off = 0;
    while (off < n) {
      ssize_t w = write(out, buf + off, (size_t)(n - off));
      if (w < 0) {
        perror("write");
        rc = 1;
        goto out;
      }
      off += w;
    }
  }
  if (n < 0) {
    perror("read");
    rc = 1;
  }
out:
  close(in);
  close(out);
  return rc;
}