#include "../../lib/file.h"

int main(int argc, char **argv) {
  const char *path = (argc > 1) ? argv[1] : ".";

  printf("[ls] listando %s\n", path);

  dirent_t ent;
  int count = 0;
  for (uint64_t i = 0;; i++) {
    int r = readdir(path, i, &ent);
    if (r < 0) {
      printf("[ls] error %d en %s\n", r, path);
      return 1;
    }
    if (r == 0 || ent.name[0] == '\0')
      break;
    if (ent.type == 2 /* VFS_DIRECTORY */)
      printf("  [DIR]  %s/\n", ent.name);
    else
      printf("  [FILE] %s (%u)\n", ent.name, (unsigned int)ent.size);
    count++;
  }
  printf("[ls] %d entries\n", count);
  return 0;
}