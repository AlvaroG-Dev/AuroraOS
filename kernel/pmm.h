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

// Como pmm_alloc_pages, pero todo el bloque queda por debajo de `limit`
// (exclusivo): para DMA con máscara de direcciones (dispositivos de 32 bits o
// menos). Busca desde la dirección más baja, alineado a la potencia de dos
// que cubre `count` (igual que el buddy). Se libera con pmm_free_pages.
// 0 si no hay hueco.
uint64_t pmm_alloc_pages_below(uint64_t count, uint64_t limit);

// Reservar un rango físico [start, end) como USADO. Ignora páginas ya
// reservadas. `end` es exclusivo. Requiere que el bitmap esté ya
// relocalizado (tras pmm_relocate_bitmap).
void pmm_reserve_range(uint64_t start, uint64_t end);

// Consultas de estado
uint64_t pmm_total_pages(void);
uint64_t pmm_free_pages_count(void);
// Test helper: valida un rango EFI sin realizar ninguna reserva.
int pmm_test_validate_efi_range(uint64_t phys, uint64_t pages,
                                uint64_t *size_out);

// [FIX B] Total de páginas en regiones EFI_CONVENTIONAL_MEMORY. Es el
// valor que debe usarse para "MemTotal" en /proc/meminfo y sysinfo(2).
// pmm_total_pages() sigue devolviendo el tamaño del bitmap (que incluye
// huecos MMIO/reservados) y solo lo usa el propio PMM internamente.
uint64_t pmm_total_usable_pages(void);

// ---------------------------------------------------------------------------
// Iterador sobre los rangos EFI_CONVENTIONAL_MEMORY detectados en
// pmm_init(). Uso típico: pre-touch de la ventana física sin tocar
// MMIO (framebuffer, IOAPIC, LAPIC, BARs PCI, etc.).
//
// El callback recibe [start, end) en direcciones FÍSICAS. Los rangos
// están ordenados por dirección ascendente tal como aparecen en el
// EFI memory map.
// ---------------------------------------------------------------------------
void pmm_for_each_usable(void (*cb)(uint64_t start, uint64_t end, void *ctx),
                         void *ctx);