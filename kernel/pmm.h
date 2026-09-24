#pragma once
#include "paging.h" // PAGE_SIZE
#include <stdint.h>

// Prototipos públicos
void pmm_init(uint64_t memmap, uint64_t memmap_size, uint64_t memmap_desc_size);

// Relocaliza el bitmap a la ventana física (PHYS_MAP_BASE).
// Debe llamarse DESPUÉS de paging_init, cuando la ventana está mapeada.
void pmm_relocate_bitmap(void);

// Reservar/liberar una página (4KB)
uint64_t pmm_alloc_page(void);
void pmm_free_page(uint64_t phys_addr);

// Reservar/liberar múltiples páginas contiguas
uint64_t pmm_alloc_pages(uint64_t count);
void pmm_free_pages(uint64_t phys_addr, uint64_t count);

// Reservar un rango físico [start, end) como USADO. Ignora páginas ya
// reservadas. `end` es exclusivo. Requiere que el bitmap esté ya
// relocalizado (tras pmm_relocate_bitmap).
void pmm_reserve_range(uint64_t start, uint64_t end);

// Consultas de estado
uint64_t pmm_total_pages(void);
uint64_t pmm_free_pages_count(void);
// Test helper: valida un rango EFI sin realizar ninguna reserva.
int pmm_test_validate_efi_range(uint64_t phys, uint64_t pages, uint64_t *size_out);
