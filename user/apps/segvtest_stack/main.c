// apps/segvtest_stack/main.c
// Stack overflow: recursion infinita hasta desbordar el stack permitido.
#include "../../syscall.h"

static volatile int depth_counter = 0;

static int infinite_recurse(void) {
  volatile char buf[1024];
  buf[0] = (char)depth_counter++;
  int r = infinite_recurse();
  return r + buf[0];
}

int main(void) {
  sys_print("[segv_stack] Desbordando el stack (recursion infinita)...\n");
  int r = infinite_recurse();
  (void)r;
  sys_print("[segv_stack] ERROR: el kernel NO me mato!\n");
  sys_exit(99);
  return 0;
}