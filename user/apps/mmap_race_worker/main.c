// apps/mmap_race_worker/main.c
#include "../../syscall.h"

static void print_hex(const char *prefix, uint64_t value) {
  static const char hex[] = "0123456789abcdef";
  char buf[19];
  int i = 0;
  buf[i++] = '0'; buf[i++] = 'x';
  int started = 0;
  for (int shift = 60; shift >= 0; shift -= 4) {
    uint8_t digit = (uint8_t)((value >> shift) & 0xF);
    if (digit || started || shift == 0) {
      buf[i++] = hex[digit];
      started = 1;
    }
  }
  buf[i] = 0;

  char out[128];
  int oi = 0, pi = 0, bi = 0;
  while (prefix[pi]) out[oi++] = prefix[pi++];
  while (buf[bi]) out[oi++] = buf[bi++];
  out[oi++] = '\n'; out[oi] = 0;
  sys_print(out);
}

int main(void) {
  int pid = sys_getpid();
  void *addr = sys_mmap(0, 4096, 0x1 | 0x2, 0, -1, 0);
  if ((int64_t)addr < 0) {
    sys_print("[mmap_race_worker] ERROR: mmap fallo\n");
    sys_exit(1);
  }

  print_hex("[mmap_race_worker] mapping=", (uint64_t)addr);

  volatile uint8_t *p = (volatile uint8_t *)addr;
  *p = (uint8_t)pid;

  sys_munmap((uint64_t)addr, 4096);
  sys_exit(0);
  return 0;
}
