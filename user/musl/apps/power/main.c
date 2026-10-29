// power: apaga, reinicia o para el sistema con reboot(2).
//   power off | reboot | halt
// Tambien como poweroff / reboot / halt (cada uno incluye este fichero y se
// distingue por argv[0]).
#include <stdio.h>
#include <string.h>
#include <sys/reboot.h>
#include <unistd.h>

int main(int argc, char **argv) {
  const char *base = strrchr(argv[0], '/');
  base = base ? base + 1 : argv[0];
  const char *what = argc > 1 ? argv[1] : "off";
  if (!strcmp(base, "poweroff"))
    what = "off";
  else if (!strcmp(base, "reboot"))
    what = "reboot";
  else if (!strcmp(base, "halt"))
    what = "halt";
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
