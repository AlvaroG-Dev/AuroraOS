#include "syscall.h"
#include <stdint.h>

#define MMAP_PROT_READ  0x1
#define MMAP_PROT_WRITE 0x2

static int failed = 0;

static void ok(const char *msg) {
  sys_print("[munmap_partial] OK: ");
  sys_print(msg);
  sys_print("\n");
}

static void error(const char *msg) {
  sys_print("[munmap_partial] ERROR: ");
  sys_print(msg);
  sys_print("\n");
  failed++;
}

static long map_at(uint64_t addr, uint64_t pages) {
  return (long)sys_mmap(addr, pages * 4096ULL,
                         MMAP_PROT_READ | MMAP_PROT_WRITE, 0, -1, 0);
}

int main(void) {
  const uint64_t base = 0x600000ULL;

  sys_print("[munmap_partial] === munmap partial VMA regression ===\n");

  // Create one four-page VMA.
  if (map_at(base, 4) != (long)base) {
    error("mmap inicial fallo");
    return 1;
  }

  // Unmap the two middle pages. This must split [base, base+0x4000)
  // into [base, base+0x1000) and [base+0x3000, base+0x4000).
  if (sys_munmap(base + 0x1000, 0x2000) != 0)
    error("munmap parcial central fue rechazado");
  else
    ok("munmap central dividio la VMA");

  // The hole must be reusable, while the two surviving pieces prevent
  // overlapping mappings around it.
  if (map_at(base + 0x1000, 2) != (long)(base + 0x1000))
    error("hueco central no se pudo volver a mapear");
  else
    ok("hueco central quedo libre");

  // Now exercise a left-edge partial unmap on the remapped two-page VMA.
  if (sys_munmap(base + 0x1000, 0x1000) != 0)
    error("munmap del borde izquierdo fue rechazado");
  else
    ok("munmap del borde izquierdo recorto la VMA");

  if (map_at(base + 0x1000, 1) != (long)(base + 0x1000))
    error("borde izquierdo no quedo reutilizable");
  else
    ok("borde izquierdo quedo libre");

  // Exercise the right edge as well.
  if (sys_munmap(base + 0x2000, 0x1000) != 0)
    error("munmap del borde derecho fue rechazado");
  else
    ok("munmap del borde derecho recorto la VMA");

  if (map_at(base + 0x2000, 1) != (long)(base + 0x2000))
    error("borde derecho no quedo reutilizable");
  else
    ok("borde derecho quedo libre");

  // Clean up all four pages. Several adjacent VMAs now cover the range.
  if (sys_munmap(base, 0x4000) != 0)
    error("limpieza final fallo");
  else
    ok("limpieza final elimino las VMAs restantes");

  if (failed) {
    sys_print("[munmap_partial] FALLOS: al menos una comprobacion fallo\n");
    return 1;
  }

  sys_print("[munmap_partial] Todas las comprobaciones PASARON\n");
  return 0;
}
