// user/musl/apps/probe/main.c
#include <unistd.h>
int main(void) {
  write(1, "PROBE\n", 6);
  return 0;
}