#ifndef KERNEL_VDSO_H
#define KERNEL_VDSO_H
#include <stdint.h>

// [C] vDSO: inicializa las paginas vvar y vdso. Llamar una vez en boot.
void vdso_init(void);

// [C] Mapea vvar + vdso en el pml4 del proceso. Devuelve VDSO_VMA_BASE
// (para AT_SYSINFO_EHDR) o 0 si el vDSO no esta disponible.
uint64_t vdso_map_in(uint64_t *pml4);

// [C] Actualiza la pagina vvar. Llamar desde time_tick en CPU0.
void vdso_update_clock(void);

#endif