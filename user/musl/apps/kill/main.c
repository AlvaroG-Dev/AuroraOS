// kill: envía señal a un PID.
// usage: kill [-SIG] pid
//   default SIGTERM (15).

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
  int sig = SIGTERM;
  int first_pid = 1;

  if (first_pid < argc && argv[first_pid][0] == '-') {
    const char *s = argv[first_pid] + 1;
    if (s[0] >= '0' && s[0] <= '9')
      sig = atoi(s);
    else {
      // Mapas mínimos de nombres comunes.
      if (strcmp(s, "TERM") == 0 || strcmp(s, "SIGTERM") == 0)
        sig = SIGTERM;
      else if (strcmp(s, "KILL") == 0 || strcmp(s, "SIGKILL") == 0)
        sig = SIGKILL;
      else if (strcmp(s, "INT") == 0 || strcmp(s, "SIGINT") == 0)
        sig = SIGINT;
      else if (strcmp(s, "HUP") == 0 || strcmp(s, "SIGHUP") == 0)
        sig = SIGHUP;
      else {
        fprintf(stderr, "kill: señal desconocida: %s\n", s);
        return 1;
      }
    }
    first_pid++;
  }

  if (first_pid >= argc) {
    fprintf(stderr, "usage: kill [-SIG] pid\n");
    return 1;
  }

  int rc = 0;
  for (int i = first_pid; i < argc; i++) {
    int pid = atoi(argv[i]);
    if (pid <= 0) {
      fprintf(stderr, "kill: pid inválido: %s\n", argv[i]);
      rc = 1;
      continue;
    }
    if (kill(pid, sig) != 0) {
      fprintf(stderr, "kill: %d: error\n", pid);
      rc = 1;
    }
  }
  return rc;
}