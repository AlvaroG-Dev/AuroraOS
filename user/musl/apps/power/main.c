// power: apaga, reinicia o para el sistema con reboot(2).
//   power off | reboot | halt
// Tambien como poweroff / reboot / halt (cada uno incluye este fichero y se
// distingue por argv[0]).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/reboot.h>
#include <unistd.h>

int main(int argc, char **argv) {
  const char *base = strrchr(argv[0], '/');
  base = base ? base + 1 : argv[0];
  const char *what = argc > 1 ? argv[1] : "off";
  if (!strcmp(base, "shutdown")) {
    // shutdown [-h|-P|-H|-r] [now|+min|min]; -h/-P apagan (por defecto).
    what = "off";
    long delay_min = 0;
    for (int i = 1; i < argc; i++) {
      const char *a = argv[i];
      if (!strcmp(a, "-r"))
        what = "reboot";
      else if (!strcmp(a, "-H"))
        what = "halt";
      else if (!strcmp(a, "-h") || !strcmp(a, "-P"))
        what = "off";
      else if (!strcmp(a, "now"))
        delay_min = 0;
      else if (a[0] == '+' || (a[0] >= '0' && a[0] <= '9'))
        delay_min = atol(a[0] == '+' ? a + 1 : a);
      else if (a[0] == '-')
        ; // opciones ignoradas (-k, -t...)
      else
        fprintf(stderr, "shutdown: argumento no reconocido: %s\n", a);
    }
    if (delay_min > 0) {
      fprintf(stderr, "shutdown: en %ld min (%s)\n", delay_min, what);
      sleep((unsigned)delay_min * 60);
    }
  }
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
    fprintf(stderr, "uso: power off|reboot|halt  |  shutdown [-h|-r|-H] [now|+min]\n");
    return 2;
  }
  sync();
  if (reboot(cmd) < 0)
    perror("reboot");
  return 1;
}
