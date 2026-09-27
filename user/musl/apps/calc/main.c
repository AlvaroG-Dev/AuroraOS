// calc: evaluador de expresiones aritméticas simples.
// usage: calc <num> <op> <num>
//   op ∈ { + - x / * }
//   o: calc (modo interactivo, lee líneas de stdin)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int eval(long a, const char *op, long b, long *out) {
  if (op[0] == '\0' || op[1] != '\0')
    return -1;
  switch (op[0]) {
  case '+':
    *out = a + b;
    return 0;
  case '-':
    *out = a - b;
    return 0;
  case '*':
  case 'x':
    *out = a * b;
    return 0;
  case '/':
    if (b == 0)
      return -2;
    *out = a / b;
    return 0;
  case '%':
    if (b == 0)
      return -2;
    *out = a % b;
    return 0;
  default:
    return -1;
  }
}

static void do_calc(const char *a, const char *op, const char *b) {
  long x = strtol(a, NULL, 0);
  long y = strtol(b, NULL, 0);
  long r;
  int rc = eval(x, op, y, &r);
  if (rc == -1)
    fprintf(stderr, "calc: operador desconocido '%s'\n", op);
  else if (rc == -2)
    fprintf(stderr, "calc: división por cero\n");
  else
    printf("%ld\n", r);
}

int main(int argc, char **argv) {
  if (argc == 4) {
    do_calc(argv[1], argv[2], argv[3]);
    return 0;
  }

  // Modo interactivo: líneas "<a> <op> <b>".
  char line[256];
  while (fgets(line, sizeof(line), stdin)) {
    char *a = strtok(line, " \t\n");
    char *op = strtok(NULL, " \t\n");
    char *b = strtok(NULL, " \t\n");
    if (!a || !op || !b)
      continue;
    if (strcmp(a, "quit") == 0)
      break;
    do_calc(a, op, b);
  }
  return 0;
}