// kernel/paging.c

#include "paging.h"
#include "pmm.h"
#include "serial.h"
#include <stddef.h>

#define IA32_PAT_MSR 0x277

static inline void wrmsr(uint32_t msr, uint64_t val) {
  uint32_t low = (uint32_t)val;
  uint32_t high = (uint32_t)(val >> 32);
  __asm__ volatile("wrmsr" : : "c"(msr), "a"(low), "d"(high));
}

static inline uint64_t rdmsr(uint32_t msr) {
  uint32_t low, high;
  __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
  return ((uint64_t)high << 32) | low;
}

static void pat_init(void) {
  uint64_t pat = rdmsr(IA32_PAT_MSR);
  // Reconfigurar PAT3 (PWT=1, PCD=1) a Write-Combining (0x01)
  pat &= ~(0xFFULL << 24);
  pat |= (0x01ULL << 24);
  wrmsr(IA32_PAT_MSR, pat);
  serial_puts("[PAGING] IA32_PAT MSR: PAT3 = Write-Combining (WC)\n");
}

static uint64_t *alloc_page_table(void) {
  uint64_t phys = pmm_alloc_page();
  if (!phys) {
    serial_puts("[PAGING] ERROR: PMM sin paginas libres\n");
    return NULL;
  }
  uint64_t *pt = (uint64_t *)phys;
  __builtin_memset(pt, 0, PAGE_SIZE);
  return pt;
}

static uint64_t *kernel_pml4 = NULL;

void paging_init(uint64_t *boot_pml4) {
  kernel_pml4 = boot_pml4;
  pat_init();
  serial_puts("[PAGING] PML4 en ");
  serial_hex((uint64_t)boot_pml4);
  serial_puts(" | Modo: PMM dinamico\n");
}

uint64_t *paging_get_pml4(void) { return kernel_pml4; }

int paging_map_page(uint64_t virt, uint64_t phys, uint64_t flags) {
  uint64_t pml4_idx = PML4_INDEX(virt);
  uint64_t pdpt_idx = PDPT_INDEX(virt);
  uint64_t pd_idx = PD_INDEX(virt);
  uint64_t pt_idx = PT_INDEX(virt);

  uint64_t *pml4 = kernel_pml4;
  if (!(pml4[pml4_idx] & PTE_PRESENT)) {
    uint64_t *new_pdpt = alloc_page_table();
    if (!new_pdpt) return -1;
    pml4[pml4_idx] = ((uint64_t)new_pdpt & PTE_FRAME) | PTE_PRESENT | PTE_WRITABLE;
  }
  uint64_t *pdpt = (uint64_t *)(pml4[pml4_idx] & PTE_FRAME);

  if (!(pdpt[pdpt_idx] & PTE_PRESENT)) {
    uint64_t *new_pd = alloc_page_table();
    if (!new_pd) return -1;
    pdpt[pdpt_idx] = ((uint64_t)new_pd & PTE_FRAME) | PTE_PRESENT | PTE_WRITABLE;
  }
  uint64_t *pd = (uint64_t *)(pdpt[pdpt_idx] & PTE_FRAME);

  // --- SOPORTE PARA PAGINAS GIGANTES DE 2MB (UEFI IDENTITY MAP) ---
  if ((pd[pd_idx] & PTE_PRESENT) && (pd[pd_idx] & PTE_HUGE)) {
    // Si la pagina en PD ya es de 2MB, le aplicamos las banderas PAT/WC directamente a la entrada PDE
    pd[pd_idx] |= (flags & (PTE_WRITETHRU | PTE_NOCACHE | PTE_WRITABLE));
    paging_invalidate_tlb(virt);
    return 0;
  }

  if (!(pd[pd_idx] & PTE_PRESENT)) {
    uint64_t *new_pt = alloc_page_table();
    if (!new_pt) return -1;
    pd[pd_idx] = ((uint64_t)new_pt & PTE_FRAME) | PTE_PRESENT | PTE_WRITABLE;
  }
  uint64_t *pt = (uint64_t *)(pd[pd_idx] & PTE_FRAME);

  pt[pt_idx] = (phys & PTE_FRAME) | (flags & 0xFFF) | PTE_PRESENT;
  paging_invalidate_tlb(virt);
  return 0;
}

int paging_map_range(uint64_t virt, uint64_t phys, uint64_t size, uint64_t flags) {
  for (uint64_t off = 0; off < size; off += PAGE_SIZE) {
    if (paging_map_page(virt + off, phys + off, flags) != 0)
      return -1;
  }
  return 0;
}

int paging_unmap_page(uint64_t virt) {
  uint64_t pml4_idx = PML4_INDEX(virt);
  uint64_t pdpt_idx = PDPT_INDEX(virt);
  uint64_t pd_idx = PD_INDEX(virt);
  uint64_t pt_idx = PT_INDEX(virt);

  uint64_t *pml4 = kernel_pml4;
  if (!(pml4[pml4_idx] & PTE_PRESENT))
    return -1;
  uint64_t *pdpt = (uint64_t *)(pml4[pml4_idx] & PTE_FRAME);

  if (!(pdpt[pdpt_idx] & PTE_PRESENT))
    return -1;
  uint64_t *pd = (uint64_t *)(pdpt[pdpt_idx] & PTE_FRAME);

  if (!(pd[pd_idx] & PTE_PRESENT))
    return -1;
  uint64_t *pt = (uint64_t *)(pd[pd_idx] & PTE_FRAME);

  pt[pt_idx] = 0;
  paging_invalidate_tlb(virt);
  return 0;
}

uint64_t paging_get_phys(uint64_t virt) {
  uint64_t pml4_idx = PML4_INDEX(virt);
  uint64_t pdpt_idx = PDPT_INDEX(virt);
  uint64_t pd_idx = PD_INDEX(virt);
  uint64_t pt_idx = PT_INDEX(virt);

  uint64_t *pml4 = kernel_pml4;
  if (!(pml4[pml4_idx] & PTE_PRESENT))
    return 0;
  uint64_t *pdpt = (uint64_t *)(pml4[pml4_idx] & PTE_FRAME);

  if (!(pdpt[pdpt_idx] & PTE_PRESENT))
    return 0;
  uint64_t *pd = (uint64_t *)(pdpt[pdpt_idx] & PTE_FRAME);

  if (!(pd[pd_idx] & PTE_PRESENT))
    return 0;
  uint64_t *pt = (uint64_t *)(pd[pd_idx] & PTE_FRAME);

  if (!(pt[pt_idx] & PTE_PRESENT))
    return 0;
  return (pt[pt_idx] & PTE_FRAME) | (virt & 0xFFF);
}

void paging_invalidate_tlb(uint64_t virt) {
  __asm__ volatile("invlpg (%0)" : : "r"(virt) : "memory");
}

// ---------------------------------------------------------------------------
// VMM: Aloja/libera regiones virtuales usando el PMM
// ---------------------------------------------------------------------------

int vmm_alloc_pages(uint64_t vaddr, uint64_t num_pages, uint64_t flags) {
  for (uint64_t i = 0; i < num_pages; i++) {
    uint64_t phys = pmm_alloc_page();
    if (!phys) {
      // Revertir lo ya mapeado
      for (uint64_t j = 0; j < i; j++) {
        uint64_t v = vaddr + j * PAGE_SIZE;
        uint64_t p = paging_get_phys(v);
        paging_unmap_page(v);
        pmm_free_page(p);
      }
      serial_puts("[VMM] ERROR: Sin memoria fisica para mapear\n");
      return -1;
    }
    if (paging_map_page(vaddr + i * PAGE_SIZE, phys, flags) != 0) {
      pmm_free_page(phys);
      serial_puts("[VMM] ERROR: paging_map_page fallo\n");
      return -1;
    }
  }
  return 0;
}

void vmm_free_pages(uint64_t vaddr, uint64_t num_pages) {
  for (uint64_t i = 0; i < num_pages; i++) {
    uint64_t v = vaddr + i * PAGE_SIZE;
    uint64_t phys = paging_get_phys(v);
    if (phys) {
      paging_unmap_page(v);
      pmm_free_page(phys);
    }
  }
}
