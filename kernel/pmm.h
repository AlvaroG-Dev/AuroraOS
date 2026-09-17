#pragma once
#include "paging.h" // PAGE_SIZE
#include <stdint.h>

// Prototipos públicos
void pmm_init(uint64_t memmap, uint64_t memmap_size, uint64_t memmap_desc_size);

// Relocaliza el bitmap a la ventana física (PHYS_MAP_BASE).
// Debe llamarse DESPUÉS de paging_init, cuando la ventana está mapeada.
// Durante pmm_init se usa identity mapping porque la ventana aún no existe.
void pmm_relocate_bitmap(void);

// Reservar/liberar una página (4KB)
uint64_t pmm_alloc_page(void);
void pmm_free_page(uint64_t phys_addr);

// Reservar/liberar múltiples páginas contiguas
// Devuelve dirección física del primer byte (alineada a PAGE_SIZE) o 0 si falla
uint64_t pmm_alloc_pages(uint64_t count);
void pmm_free_pages(uint64_t phys_addr, uint64_t count);

// Consultas de estado
uint64_t pmm_total_pages(void);
uint64_t pmm_free_pages_count(void);