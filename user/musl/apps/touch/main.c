// touch minimalista. Sin utimensat (no lo tenemos todavía), así que
// O_CREAT sin O_TRUNC: si el fichero existe no se toca el contenido,
// y solo el open actualiza mtime en los FS que lo hagan.
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: touch FILE...\n");
    return 1;
  }
  int rc = 0;
  for (int i = 1; i < argc; i++) {
    int fd = open(argv[i], O_WRONLY | O_CREAT, 0644);
    if (fd < 0) {
      perror(argv[i]);
      rc = 1;
      continue;
    }
    close(fd);
  }
  return rc;
}