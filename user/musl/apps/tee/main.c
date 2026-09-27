// tee: lee stdin, escribe a stdout y a cada fichero.
// usage: tee [-a] file...

#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

int main(int argc, char **argv) {
  int append = 0;
  int first_file = 1;

  if (first_file < argc && argv[first_file][0] == '-' &&
      argv[first_file][1] == 'a' && argv[first_file][2] == '\0') {
    append = 1;
    first_file++;
  }

  int nfiles = argc - first_file;
  int fds[16];
  if (nfiles > 16)
    nfiles = 16;

  for (int i = 0; i < nfiles; i++) {
    int flags = O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC);
    fds[i] = open(argv[first_file + i], flags, 0644);
    if (fds[i] < 0) {
      perror(argv[first_file + i]);
      return 1;
    }
  }

  char buf[4096];
  ssize_t n;
  while ((n = read(STDIN_FILENO, buf, sizeof(buf))) > 0) {
    ssize_t off = 0;
    while (off < n) {
      ssize_t w = write(STDOUT_FILENO, buf + off, (size_t)(n - off));
      if (w < 0)
        break;
      off += w;
    }
    for (int i = 0; i < nfiles; i++) {
      off = 0;
      while (off < n) {
        ssize_t w = write(fds[i], buf + off, (size_t)(n - off));
        if (w < 0)
          break;
        off += w;
      }
    }
  }
  for (int i = 0; i < nfiles; i++)
    close(fds[i]);
  return n < 0 ? 1 : 0;
}