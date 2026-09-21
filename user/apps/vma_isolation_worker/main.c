#include "syscall.h"
#include <stdint.h>

#define TEST_ADDR 0x70000000ULL
#define MMAP_PROT_READ  0x1
#define MMAP_PROT_WRITE 0x2

int main(void) {
  int pid = sys_getpid();

  long p = (long)sys_mmap(TEST_ADDR, 4096,
                          MMAP_PROT_READ | MMAP_PROT_WRITE,
                          0, -1, 0);
  if (p != (long)TEST_ADDR) {
    sys_print("[vma_isolation_worker] ERROR: mmap fallo\n");
    return 1;
  }

  volatile uint64_t *mem = (volatile uint64_t *)TEST_ADDR;
  uint64_t value = (uint64_t)(uint32_t)pid * 0x100000001ULL + 0x1234ULL;
  *mem = value;

  if (*mem != value) {
    sys_print("[vma_isolation_worker] ERROR: lectura propia incorrecta\n");
    sys_munmap(TEST_ADDR, 4096);
    return 2;
  }

  vm_debug_info_t info;
  if (sys_vm_debug_info(TEST_ADDR, &info) != 0 ||
      info.cr3_phys == 0 || info.phys == 0) {
    sys_print("[vma_isolation_worker] ERROR: no se pudo obtener CR3/PA\n");
    sys_munmap(TEST_ADDR, 4096);
    return 4;
  }

  // El kernel imprime CR3 y PA. La prueba comprueba además que la traducción
  // pertenece al espacio de direcciones del proceso actual.
  sys_yield();

  if (*mem != value) {
    sys_print("[vma_isolation_worker] ERROR: memoria no aislada\n");
    sys_munmap(TEST_ADDR, 4096);
    return 3;
  }

  // Dejamos la página mapeada hasta la salida para que las observaciones de
  // los distintos workers puedan compararse mientras los procesos siguen
  // vivos. El kernel liberará el address space al hacer reap del proceso.
  sys_print("[vma_isolation_worker] CR3/PA obtenidos correctamente\n");
  return 0;
}
