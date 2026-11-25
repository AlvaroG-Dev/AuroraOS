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

// [COW] Refcount por frame. pmm_page_share añade una referencia (0 / -1 si no
// se puede: sin tabla o saturado). pmm_free_page suelta una referencia y solo
// libera el frame con la última. pmm_page_is_shared: hay más de un dueño.
int pmm_page_share(uint64_t phys);
int pmm_page_is_shared(uint64_t phys);
uint64_t pmm_cow_extra_total(void); // suma de referencias extra (diagnostico)

// Paginas fijadas (las de los memfd): solo su dueno las libera, tras
// pmm_page_pin(p, 0). Un free o una reasignacion por otro camino se rechaza y
// se anota aqui (diagnostico de la RAM corrupta de la mini-VM).
#define PMM_PIN_FRAMES 6
struct pmm_pin_stats {
  uint64_t bad_free;  // frees rechazados de paginas fijadas
  uint64_t bad_alloc; // paginas fijadas que el asignador iba a entregar
  uint64_t last_phys;
  uint32_t last_kind; // 0 = free, 1 = asignacion, 2 = memfd cerrado en uso
  uint32_t pad;
  uint64_t callers[PMM_PIN_FRAMES]; // direcciones de retorno del ultimo
};
void pmm_page_pin(uint64_t phys, int on);
void pmm_pin_get_stats(struct pmm_pin_stats *out);
// Anota un uso indebido detectado fuera de pmm (kind 2 = memfd cerrado con una
// VM aun usandolo).
void pmm_pin_report(int kind, uint64_t phys);

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