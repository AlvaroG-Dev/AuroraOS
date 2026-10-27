// kernel/vmap.h
//
// [LinuxKPI L4] Región del kernel para mapear páginas físicas sueltas de forma
// virtualmente contigua (vmap() de Linux, dma_alloc_wc, dma_buf_vmap). Ocupa
// su propia entrada del PML4 (VMAP_BASE), con el PDPT reservado al arrancar
// para que la vean todos los procesos (paging_clone_kernel_space copia la
// mitad alta del PML4 al crearlos).
#ifndef KERNEL_VMAP_H
#define KERNEL_VMAP_H

#include <stdint.h>

#define VMAP_BASE 0xFFFFD00000000000ULL // PML4 416
#define VMAP_SIZE (64ULL << 30)         // 64 GiB

void vmap_init(void);
// Reserva `size` bytes (redondeados a página) de direcciones, seguidos de una
// página de guarda sin mapear. 0 si no hay hueco.
uint64_t vmap_area_alloc(uint64_t size);
// Mapea `phys` en `va` (dentro de un área reservada) con `flags` de PTE
// (PTE_PRESENT se añade solo; p. ej. PTE_WRITABLE | PTE_NX | PTE_WRITECOMB).
int vmap_area_map(uint64_t va, uint64_t phys, uint64_t flags);
// Desmapea el área que empieza en `va`, hace shootdown del TLB y la libera.
// Contexto de tarea con las IRQ activas (shootdown).
void vmap_area_free(uint64_t va);
// Bytes reservados en uso (para pruebas).
uint64_t vmap_area_used(void);

#endif
