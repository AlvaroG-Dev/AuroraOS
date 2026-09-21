#include "syscall.h"
#include <stdint.h>

#define TEST_ADDR 0x70000000ULL
#define MMAP_PROT_READ  0x1
#define MMAP_PROT_WRITE 0x2

int main(void) {
  long p = (long)sys_mmap(TEST_ADDR, 4096,
                          MMAP_PROT_READ | MMAP_PROT_WRITE,
                          0, -1, 0);
  if (p != (long)TEST_ADDR) {
    sys_print("[vma_isolation_worker] ERROR: mmap fallo\n");
    return 1;
  }

  volatile uint64_t *mem = (volatile uint64_t *)TEST_ADDR;
  uint64_t value = (uint64_t)(uint32_t)sys_getpid() * 0x100000001ULL + 0x1234ULL;
  *mem = value;

  if (*mem != value) {
    sys_print("[vma_isolation_worker] ERROR: lectura propia incorrecta\n");
    sys_munmap(TEST_ADDR, 4096);
    return 2;
  }

  sys_yield();

  if (*mem != value) {
    sys_print("[vma_isolation_worker] ERROR: memoria no aislada\n");
    sys_munmap(TEST_ADDR, 4096);
    return 3;
  }

  sys_munmap(TEST_ADDR, 4096);
  return 0;
}
