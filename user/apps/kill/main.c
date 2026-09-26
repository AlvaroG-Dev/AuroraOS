#include "../../lib/file.h"
#include "../../lib/process.h"
#include "../../lib/string.h"
#include "../../syscall.h"

int main(int argc, char **argv) {
  if (argc < 2) {
    puts("uso: kill [-sig] <pid>");
    return 1;
  }

  int sig = SIGTERM;
  int argi = 1;
  if (argv[argi][0] == '-') {
    sig = atoi(argv[argi] + 1);
    argi++;
  }
  if (argi >= argc) {
    puts("uso: kill [-sig] <pid>");
    return 1;
  }

  int pid = atoi(argv[argi]);
  if (pid <= 0) {
    puts("kill: pid inválido");
    return 1;
  }

  if (kill(pid, sig) != 0) {
    printf("kill: no se pudo enviar señal %d a pid %d\n", sig, pid);
    return 1;
  }
  return 0;
}