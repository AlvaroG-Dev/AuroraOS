#ifndef PAGING_H
#define PAGING_H

#include <stdint.h>

#define PAGE_SIZE 0x1000
#define PAGE_ENTRIES 512
#define KERNEL_VMA 0xFFFFFFFF80000000ULL
#define HEAP_VMA 0xFFFFFFFF82000000ULL

// ---------------------------------------------------------------------------
// Ventanas de memoria física
// ---------------------------------------------------------------------------
// PHYS_MAP_BASE: mapea toda la RAM física 1:1 con páginas de 2 MB.
//   Flags: PTE_WRITABLE | PTE_NOCACHE | PTE_GLOBAL | PTE_NX
// MMIO_MAP_BASE: mapea MMIO (framebuffer, PCI BARs) con páginas de 4 KB.
//   Flags: los que pida el driver (PTE_WRITECOMB o PTE_NOCACHE).
//
// Ambas están en la mitad superior del espacio virtual (PML4 entries 256-511),
// así que paging_clone_kernel_space las copia automáticamente al PML4 de
// cada proceso. Cualquier CR3 puede acceder a ellas.
//
//   PHYS_MAP_BASE = 0xFFFF800000000000  (PML4 entry 256)
//   MMIO_MAP_BASE = 0xFFFFC00000000000  (PML4 entry 384)
//   KERNEL_VMA    = 0xFFFFFFFF80000000  (PML4 entry 511)
#define PHYS_MAP_BASE 0xFFFF800000000000ULL
#define MMIO_MAP_BASE 0xFFFFC00000000000ULL

// Cobertura máxima de la ventana física de RAM: 512 GB (todo el rango que
// cabe en una PML4 entry). El mapa real se limita a la RAM detectada.
#define PHYS_MAP_MAX_SIZE (512ULL * 1024 * 1024 * 1024)

#define PTE_PRESENT 0x001
#define PTE_WRITABLE 0x002
#define PTE_USER 0x004
#define PTE_WRITETHRU 0x008
#define PTE_NOCACHE 0x010
#define PTE_ACCESSED 0x020
#define PTE_DIRTY 0x040
#define PTE_HUGE 0x080
#define PTE_GLOBAL 0x100
#define PTE_FRAME 0x000FFFFFFFFFF000ULL
#define PTE_NX (1ULL << 63)
#define PTE_WRITECOMB (PTE_WRITETHRU | PTE_NOCACHE)

#define PML4_INDEX(v) (((v) >> 39) & 0x1FF)
#define PDPT_INDEX(v) (((v) >> 30) & 0x1FF)
#define PD_INDEX(v) (((v) >> 21) & 0x1FF)
#define PT_INDEX(v) (((v) >> 12) & 0x1FF)

void paging_init(uint64_t *boot_pml4, uint64_t max_phys_addr);
uint64_t *paging_get_pml4(void);
int paging_map_page(uint64_t virt, uint64_t phys, uint64_t flags);
int paging_map_range(uint64_t virt, uint64_t phys, uint64_t size,
                     uint64_t flags);
int paging_unmap_page(uint64_t virt);
uint64_t paging_get_phys(uint64_t virt);
void paging_invalidate_tlb(uint64_t virt);

// VMM: Aloja/libera páginas virtuales continuas usando el PMM
int vmm_alloc_pages(uint64_t vaddr, uint64_t num_pages, uint64_t flags);
void vmm_free_pages(uint64_t vaddr, uint64_t num_pages);

// === FUNCIONES PARA GESTIÓN DE PROCESOS ===
uint64_t paging_clone_kernel_space(void);
int paging_map_page_in(uint64_t *pml4, uint64_t virt, uint64_t phys,
                       uint64_t flags);
int paging_unmap_page_in(uint64_t *pml4, uint64_t virt);
uint64_t paging_get_phys_in(uint64_t *pml4, uint64_t virt);
void paging_free_user_space(uint64_t pml4_phys);

// ---------------------------------------------------------------------------
// Traducción de direcciones físicas <-> virtuales (ventana de RAM)
// ---------------------------------------------------------------------------
static inline void *phys_to_virt(uint64_t phys) {
  return (void *)(PHYS_MAP_BASE + phys);
}

static inline uint64_t virt_to_phys(const void *virt) {
  return (uint64_t)virt - PHYS_MAP_BASE;
}

static inline uint64_t *phys_to_ptr(uint64_t phys) {
  return (uint64_t *)phys_to_virt(phys);
}

// ---------------------------------------------------------------------------
// Huge pages (2 MB) para el mapeo de la ventana física
// ---------------------------------------------------------------------------
int paging_map_huge_page(uint64_t virt, uint64_t phys, uint64_t flags);

// ---------------------------------------------------------------------------
// MMIO: mapear regiones de MMIO en MMIO_MAP_BASE
// ---------------------------------------------------------------------------
// La misma dirección física siempre se mapea en la misma dirección virtual
// (MMIO_MAP_BASE + phys). Mapear dos veces la misma región con flags distintos
// sobrescribe el primer mapeo.
//
// Devuelve un puntero virtual al inicio del mapeo (equivalente a
// MMIO_MAP_BASE + phys), o NULL si falla.
void *mmio_map(uint64_t phys, uint64_t size, uint64_t flags);

// Deshace un mapeo hecho con mmio_map.
void mmio_unmap(uint64_t phys, uint64_t size);

#endif