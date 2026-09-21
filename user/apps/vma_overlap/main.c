#include "syscall.h"
#include <stdint.h>

#define MMAP_PROT_READ  0x1
#define MMAP_PROT_WRITE 0x2

static int failed = 0;

static void check_rejected(const char *name, uint64_t addr) {
  long rc = (long)sys_mmap(addr, 4096,
                           MMAP_PROT_READ | MMAP_PROT_WRITE,
                           0, -1, 0);
  if (rc >= 0) {
    sys_print("[vma_overlap] ERROR: ");
    sys_print(name);
    sys_print(" fue aceptado\n");
    failed++;
  } else {
    sys_print("[vma_overlap] OK: ");
    sys_print(name);
    sys_print(" fue rechazado\n");
  }
}

int main(void) {
  sys_print("[vma_overlap] === VMA overlap regression test ===\n");

  // The ELF loader creates a VMA starting at the process load base.
  // AuroraOS user images currently load at 0x400000.
  check_rejected("solapamiento ELF", 0x400000);

  // The user stack VMA starts below USER_STACK_BASE and covers the stack.
  // Probe an address known to be inside the mapped stack.
  check_rejected("solapamiento stack", 0x00007ffff0000000ULL);

  // An adjacent page outside the ELF VMA should remain valid.  The ELF VMA
  // spans 0x400000..0x600000, so 0x600000 is its exclusive end.
  long adjacent = (long)sys_mmap(0x600000, 4096,
                                  MMAP_PROT_READ | MMAP_PROT_WRITE,
                                  0, -1, 0);
  if (adjacent != 0x600000) {
    sys_print("[vma_overlap] ERROR: mapping adyacente no fue aceptado\n");
    failed++;
  } else {
    sys_print("[vma_overlap] OK: mapping adyacente aceptado\n");
    sys_munmap((void *)0x600000, 4096);
  }

  sys_print("[vma_overlap] ====================================\n");
  if (failed) {
    sys_print("[vma_overlap] FALLOS: ");
    // Avoid formatting dependencies in the tiny user runtime.
    sys_print("al menos una comprobacion fallo\n");
    return 1;
  }

  sys_print("[vma_overlap] Todas las comprobaciones PASARON\n");
  return 0;
}
