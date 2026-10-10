// kernel/vdso/vdso_defs.h
#ifndef KERNEL_VDSO_DEFS_H
#define KERNEL_VDSO_DEFS_H

#include <stdint.h>

// [C.1] Direcciones fijas del vDSO en el espacio de usuario.
// Justo debajo de USER_LIMIT (0x0000800000000000) y por encima
// del stack (0x00007FFFF0000000 + 8 MB de growth). El kernel
// mapea estas dos páginas en cada exec y pasa VDSO_VMA_BASE
// a userspace vía AT_SYSINFO_EHDR.
//
// IMPORTANTE: si cambias estos valores, hay que cambiarlos también
// en vdso.S (los dos movabs $0x00007FFFFFFF0000).
#define VVAR_VMA_BASE 0x00007FFFFFFF0000ULL
#define VDSO_VMA_BASE 0x00007FFFFFFF1000ULL

// [C.1] Layout de la página compartida kernel↔userland.
// El kernel escribe desde el tick handler (solo CPU0), el vDSO
// lee en userspace. Acceso con seqlock:
//   - Writer: seq++ (queda impar), escribe campos, seq++ (par).
//   - Reader: lee seq, si impar reintenta; lee campos; relee seq
//     y comprueba que no cambió.
//
// Offsets hardcoded en vdso.S. NO los muevas sin actualizar el asm.
struct vdso_vvar {
  volatile uint32_t seq;        // +0x00, seqlock par/impar
  volatile uint32_t clock_mode; // +0x04, 0 = off, 1 = activo
  volatile uint64_t mono_ns;    // +0x08, CLOCK_MONOTONIC en ns
  volatile uint64_t real_ns;    // +0x10, CLOCK_REALTIME en ns
  uint64_t ticks_per_sec;       // +0x18, 1000 (informativo)
  uint64_t _pad[4];             // +0x20..+0x40
};
_Static_assert(sizeof(struct vdso_vvar) <= 4096, "vvar fits in one page");

#endif