// ls minimalista. Sin -l, sin color, sin ordenar. Solo lista nombres y
// marca directorios con "/". El tipo viene de d_type (de getdents64),
// así que no hace falta un stat() por entrada.
//
// Ejercita: openat, getdents64, close. También el path resolution del
// kernel (cwd, ".", "/initrd", etc.).

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int list_dir(const char *path) {
  DIR *d = opendir(path);
  if (!d) {
    fprintf(stderr, "ls: %s: %s\n", path, strerror(errno));
    return 1;
  }
  struct dirent *ent;
  int n = 0;
  while ((ent = readdir(d)) != NULL) {
    const char *suffix = (ent->d_type == DT_DIR) ? "/" : "";
    printf("  %s%s\n", ent->d_name, suffix);
    n++;
  }
  closedir(d);
  printf("[ls] %d entries\n", n);
  return 0;
}

int main(int argc, char **argv) {
  if (argc < 2)
    return list_dir(".");
  int rc = 0;
  for (int i = 1; i < argc; i++) {
    if (argc > 2)
      printf("%s:\n", argv[i]);
    if (list_dir(argv[i]) != 0)
      rc = 1;
  }
  return rc;
}