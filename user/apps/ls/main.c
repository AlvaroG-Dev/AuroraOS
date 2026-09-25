// apps/ls/main.c
#include "../../lib/file.h"
#include "../../lib/string.h"
#include "../../syscall.h"


int main(void) {
  const char *path = "/boot";
  dirent_t d;
  int idx = 0;
  int n = 0;

  sys_print("[ls] listando ");
  sys_print(path);
  sys_print("\n");

  while (1) {
    int rc = readdir(path, idx, &d);
    if (rc < 0) {
      char buf[32];
      sys_print("[ls] error: ");
      itoa(rc, buf, 10);
      sys_print(buf);
      sys_print("\n");
      sys_exit(1);
    }
    if (rc == 0 || d.name[0] == '\0')
      break;

    // Formato: "[DIR] name/" o "[FILE] name (size)".
    if (d.type == 2) { // VFS_DIRECTORY
      sys_print("  [DIR]  ");
      sys_print(d.name);
      sys_print("/\n");
    } else {
      sys_print("  [FILE] ");
      sys_print(d.name);
      sys_print(" (");
      char buf[32];
      itoa((int64_t)d.size, buf, 10);
      sys_print(buf);
      sys_print(")\n");
    }
    idx++;
    n++;
  }

  char buf[32];
  sys_print("[ls] ");
  itoa(n, buf, 10);
  sys_print(buf);
  sys_print(" entries\n");

  sys_exit(0);
  return 0;
}