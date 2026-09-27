// rm: borra fichero(s).
// usage: rm file...

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: rm file...\n");
    return 1;
  }
  int rc = 0;
  for (int i = 1; i < argc; i++) {
    if (unlink(argv[i]) != 0) {
      fprintf(stderr, "rm: %s: %s\n", argv[i], strerror(errno));
      rc = 1;
    }
  }
  return rc;
}