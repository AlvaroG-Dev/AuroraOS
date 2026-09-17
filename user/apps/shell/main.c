// user/apps/shell/main.c
// Shell interactivo mínimo de Aurora OS.

#include "../../lib/file.h"
#include "../../lib/process.h"
#include "../../lib/string.h"
#include "../../syscall.h"


#define MAX_ARGS 8
#define LINE_MAX 256

static int parse_line(char *line, char *argv[MAX_ARGS]) {
  int argc = 0;
  char *p = line;

  while (*p && argc < MAX_ARGS) {
    while (*p == ' ' || *p == '\t')
      p++;
    if (!*p)
      break;

    argv[argc++] = p;
    while (*p && *p != ' ' && *p != '\t')
      p++;
    if (*p) {
      *p = '\0';
      p++;
    }
  }
  return argc;
}

static void cmd_help(void) {
  puts("Comandos disponibles:");
  puts("  help        - esta ayuda");
  puts("  echo <str>  - imprime el texto");
  puts("  cat <file>  - muestra un archivo");
  puts("  exit        - salir del shell");
  puts("  <path>      - ejecuta el binario (ej: apps/calc)");
}

static void cmd_echo(int argc, char *argv[]) {
  for (int i = 1; i < argc; i++) {
    if (i > 1)
      write(1, " ", 1);
    write(1, argv[i], strlen(argv[i]));
  }
  write(1, "\n", 1);
}

static void cmd_cat(const char *path) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) {
    printf("cat: no existe %s\n", path);
    return;
  }
  char buf[512];
  int64_t n;
  while ((n = read(fd, buf, sizeof(buf))) > 0) {
    write(1, buf, n);
  }
  close(fd);
}

static void cmd_run(const char *path) {
  int pid = spawn(path);
  if (pid < 0) {
    printf("shell: no se pudo ejecutar %s\n", path);
    return;
  }
  int status = 0;
  int r = waitpid(pid, &status, 0);
  printf("shell: %s terminó (pid=%d, exit=%d)\n", path, r, status);
}

int main(void) {
  puts("============================================");
  puts("  Aurora OS Shell v0.1");
  puts("  Escribe 'help' para ver los comandos.");
  puts("============================================");

  static char line[LINE_MAX];
  static char *argv[MAX_ARGS];

  while (1) {
    write(1, "aurora> ", 8);

    int64_t n = read(0, line, sizeof(line) - 1);
    if (n <= 0)
      continue;
    line[n] = '\0';

    int argc = parse_line(line, argv);
    if (argc == 0)
      continue;

    const char *cmd = argv[0];

    if (strcmp(cmd, "help") == 0) {
      cmd_help();
    } else if (strcmp(cmd, "echo") == 0) {
      cmd_echo(argc, argv);
    } else if (strcmp(cmd, "cat") == 0) {
      if (argc < 2)
        puts("uso: cat <path>");
      else
        cmd_cat(argv[1]);
    } else if (strcmp(cmd, "exit") == 0) {
      puts("Adiós.");
      sys_exit(0);
    } else {
      cmd_run(cmd);
    }
  }

  return 0;
}