// power: apaga, reinicia o para el sistema con reboot(2).
//   power off | reboot | halt
#include <stdio.h>
#include <string.h>
#include <sys/reboot.h>
#include <unistd.h>

int main(int argc, char **argv) {
  const char *what = argc > 1 ? argv[1] : "off";
  int cmd;
  if (!strcmp(what, "off"))
    cmd = RB_POWER_OFF;
  else if (!strcmp(what, "reboot"))
    cmd = RB_AUTOBOOT;
  else if (!strcmp(what, "halt"))
    cmd = RB_HALT_SYSTEM;
  else {
    fprintf(stderr, "uso: power off|reboot|halt\n");
    return 2;
  }
  sync();
  if (reboot(cmd) < 0)
    perror("reboot");
  return 1;
}
