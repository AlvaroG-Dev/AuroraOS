// kernel/paging.h

#ifndef PAGING_H
#define PAGING_H

#include <stdint.h>

#define PAGE_SIZE      0x1000
#define PAGE_ENTRIES   512
#define KERNEL_VMA     0xFFFFFFFF80000000ULL
#define HEAP_VMA       0xFFFFFFFF82000000ULL

#define PTE_PRESENT    0x001
#define PTE_WRITABLE   0x002
#define PTE_USER       0x004
#define PTE_WRITETHRU  0x008
#define PTE_NOCACHE    0x010
#define PTE_ACCESSED   0x020
#define PTE_DIRTY      0x040
#define PTE_HUGE       0x080
#define PTE_GLOBAL     0x100
#define PTE_FRAME      0x000FFFFFFFFFF000ULL

// --- FORMA MODERNA PAT ---
// PWT (Bit 3 = 1) + PCD (Bit 4 = 1) selecciona el indice PAT3.
#define PTE_WRITECOMB  (PTE_WRITETHRU | PTE_NOCACHE)

#define PML4_INDEX(v)  (((v) >> 39) & 0x1FF)
#define PDPT_INDEX(v)  (((v) >> 30) & 0x1FF)
#define PD_INDEX(v)    (((v) >> 21) & 0x1FF)
#define PT_INDEX(v)    (((v) >> 12) & 0x1FF)

void paging_init(uint64_t *boot_pml4);
uint64_t *paging_get_pml4(void);
int paging_map_page(uint64_t virt, uint64_t phys, uint64_t flags);
int paging_map_range(uint64_t virt, uint64_t phys, uint64_t size, uint64_t flags);
int paging_unmap_page(uint64_t virt);
uint64_t paging_get_phys(uint64_t virt);
void paging_invalidate_tlb(uint64_t virt);

// VMM: aloja/libera páginas virtuales continuas usando el PMM
int  vmm_alloc_pages(uint64_t vaddr, uint64_t num_pages, uint64_t flags);
void vmm_free_pages(uint64_t vaddr, uint64_t num_pages);

#endif