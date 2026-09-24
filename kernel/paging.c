#include "paging.h"
#include "cpu.h"
#include "klog.h"
#include "pmm.h"
#include "serial.h"
#include "string.h"
#include <stddef.h>

#define IA32_PAT_MSR 0x277
static uint64_t *kernel_pml4 = NULL;
int cpu_smap_enabled = 0;

/* Test-only allocation fault injection. -1 disables it; 0 fails now. */
static int paging_test_fail_alloc_after = -1;

static inline int paging_is_canonical(uint64_t virt) {
  return virt <= 0x00007FFFFFFFFFFFULL ||
         virt >= 0xFFFF800000000000ULL;
}

void paging_test_set_alloc_fail_after(int successful_allocs) {
  paging_test_fail_alloc_after = successful_allocs;
}

static uint64_t *alloc_page_table_early(void) {
  uint64_t phys = pmm_alloc_page();
  if (!phys) {
    LOG_ERR("[PAGING] ERROR: PMM sin páginas libres (early)");
    return NULL;
  }
  uint64_t *pt = (uint64_t *)phys;
  memset(pt, 0, PAGE_SIZE);
  return pt;
}

static uint64_t *alloc_page_table(void) {
  if (paging_test_fail_alloc_after == 0)
    return NULL;
  if (paging_test_fail_alloc_after > 0)
    paging_test_fail_alloc_after--;

  uint64_t phys = pmm_alloc_page();
  if (!phys) {
    LOG_ERR("[PAGING] ERROR: PMM sin páginas libres");
    return NULL;
  }
  uint64_t *pt = (uint64_t *)phys_to_virt(phys);
  memset(pt, 0, PAGE_SIZE);
  return pt;
}

static void pat_init(void) {
  uint64_t pat = rdmsr(IA32_PAT_MSR);
  pat &= ~(0xFFULL << 24);
  pat |= (0x01ULL << 24);
  wrmsr(IA32_PAT_MSR, pat);
  LOG_INFO("[PAGING] IA32_PAT MSR: PAT3 = Write-Combining (WC)");
}

static uint64_t *split_huge_page(uint64_t *pd, uint64_t pd_idx) {
  uint64_t huge_entry = pd[pd_idx];
  uint64_t huge_phys_base = huge_entry & ~((uint64_t)0x1FFFFF);
  uint64_t inherit_flags = (huge_entry & 0xFFF) & ~(PTE_HUGE | 0x080ULL);

  uint64_t *new_pt = alloc_page_table();
  if (!new_pt)
    return NULL;

  for (int i = 0; i < PAGE_ENTRIES; i++) {
    new_pt[i] = (huge_phys_base + (uint64_t)i * PAGE_SIZE) |
                (inherit_flags | PTE_PRESENT);
  }

  uint64_t new_pt_phys = virt_to_phys(new_pt);
  pd[pd_idx] = (new_pt_phys & PTE_FRAME) | PTE_PRESENT | PTE_WRITABLE |
               (huge_entry & PTE_USER);

  return new_pt;
}

static void clear_nx_range(uint64_t start, uint64_t end) {
  uint64_t pages_found = 0;
  uint64_t pages_cleared = 0;

  for (uint64_t va = start & ~0xFFFULL; va < end; va += PAGE_SIZE) {
    uint64_t pml4_idx = PML4_INDEX(va);
    uint64_t pdpt_idx = PDPT_INDEX(va);
    uint64_t pd_idx = PD_INDEX(va);
    uint64_t pt_idx = PT_INDEX(va);

    if (!(kernel_pml4[pml4_idx] & PTE_PRESENT))
      continue;
    uint64_t *pdpt =
        (uint64_t *)phys_to_virt(kernel_pml4[pml4_idx] & PTE_FRAME);
    if (!(pdpt[pdpt_idx] & PTE_PRESENT))
      continue;

    if (pdpt[pdpt_idx] & PTE_HUGE) {
      pages_found++;
      if (pdpt[pdpt_idx] & PTE_NX)
        pages_cleared++;
      pdpt[pdpt_idx] &= ~PTE_NX;
      continue;
    }

    uint64_t *pd = (uint64_t *)phys_to_virt(pdpt[pdpt_idx] & PTE_FRAME);
    if (!(pd[pd_idx] & PTE_PRESENT))
      continue;

    if (pd[pd_idx] & PTE_HUGE) {
      pages_found++;
      if (pd[pd_idx] & PTE_NX)
        pages_cleared++;
      pd[pd_idx] &= ~PTE_NX;
      continue;
    }

    uint64_t *pt = (uint64_t *)phys_to_virt(pd[pd_idx] & PTE_FRAME);
    if (!(pt[pt_idx] & PTE_PRESENT))
      continue;
    pages_found++;
    if (pt[pt_idx] & PTE_NX)
      pages_cleared++;
    pt[pt_idx] &= ~PTE_NX;
  }

  uint64_t cr3;
  __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
  __asm__ volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");

  LOG_INFO("[PAGING] clear_nx_range: %lu páginas encontradas, %lu limpiadas",
           (unsigned long)pages_found, (unsigned long)pages_cleared);
}

static int paging_map_huge_page_early(uint64_t virt, uint64_t phys,
                                      uint64_t flags) {
  if ((virt & 0x1FFFFF) != 0 || (phys & 0x1FFFFF) != 0) {
    LOG_ERR("[PAGING] paging_map_huge_page: alineación inválida");
    return -1;
  }

  uint64_t pml4_idx = PML4_INDEX(virt);
  uint64_t pdpt_idx = PDPT_INDEX(virt);
  uint64_t pd_idx = PD_INDEX(virt);

  uint64_t intermediate_flags = PTE_PRESENT | PTE_WRITABLE;

  if (!(kernel_pml4[pml4_idx] & PTE_PRESENT)) {
    uint64_t *new_pdpt = alloc_page_table_early();
    if (!new_pdpt)
      return -1;
    kernel_pml4[pml4_idx] =
        ((uint64_t)new_pdpt & PTE_FRAME) | intermediate_flags;
  }
  uint64_t *pdpt = (uint64_t *)(kernel_pml4[pml4_idx] & PTE_FRAME);

  if (!(pdpt[pdpt_idx] & PTE_PRESENT)) {
    uint64_t *new_pd = alloc_page_table_early();
    if (!new_pd)
      return -1;
    pdpt[pdpt_idx] = ((uint64_t)new_pd & PTE_FRAME) | intermediate_flags;
  }
  uint64_t *pd = (uint64_t *)(pdpt[pdpt_idx] & PTE_FRAME);

  pd[pd_idx] = (phys & ~0x1FFFFFULL) | (flags & 0xFFF) | PTE_PRESENT | PTE_HUGE;

  paging_invalidate_tlb(virt);
  return 0;
}

uint64_t paging_phys_window_size(uint64_t max_phys_addr) {
  if (max_phys_addr == 0)
    return 0;

  if (max_phys_addr >= PHYS_MAP_MAX_SIZE)
    return PHYS_MAP_MAX_SIZE;

  uint64_t remainder = max_phys_addr & 0x1FFFFFULL;
  if (remainder != 0)
    max_phys_addr += 0x200000ULL - remainder;

  return max_phys_addr;
}

static void map_phys_window(uint64_t max_phys_addr) {
  uint64_t size = paging_phys_window_size(max_phys_addr);
  if (size == 0) {
    LOG_WARN("[PAGING] map_phys_window: max_phys_addr == 0, no se mapea nada");
    return;
  }

  if (max_phys_addr > PHYS_MAP_MAX_SIZE) {
    LOG_WARN("[PAGING] RAM física excede la ventana directa: "
             "limitando el mapeo a %lu GB",
             (unsigned long)(PHYS_MAP_MAX_SIZE / (1024 * 1024 * 1024)));
  }

  LOG_INFO("[PAGING] Mapeando ventana fisica: %p - %p (%lu MB)",
           (void *)PHYS_MAP_BASE, (void *)(PHYS_MAP_BASE + size),
           (unsigned long)(size / (1024 * 1024)));

  uint64_t flags = PTE_WRITABLE | PTE_NOCACHE | PTE_GLOBAL | PTE_NX;

  for (uint64_t off = 0; off < size; off += 0x200000) {
    if (paging_map_huge_page_early(PHYS_MAP_BASE + off, off, flags) != 0) {
      LOG_ERR("[PAGING] map_phys_window: fallo mapeando %p",
              (void *)(PHYS_MAP_BASE + off));
      return;
    }
  }

  LOG_INFO("[PAGING] Ventana fisica mapeada (%lu entradas de 2 MB)",
           (unsigned long)(size / 0x200000));
}

void paging_init(uint64_t *boot_pml4, uint64_t max_phys_addr) {
  kernel_pml4 = boot_pml4;
  uint64_t boot_pml4_phys = (uint64_t)boot_pml4;

  pat_init();

  {
    uint64_t cr4 = read_cr4();
    LOG_INFO("[PAGING] CR4 inicial = %p (LA57=%d PCIDE=%d SMEP=%d SMAP=%d)",
             (void *)cr4, (int)((cr4 >> 12) & 1), (int)((cr4 >> 17) & 1),
             (int)((cr4 >> 20) & 1), (int)((cr4 >> 21) & 1));

    int cr4_changed = 0;

    if (cr4 & (1ULL << 12)) {
      LOG_WARN("[PAGING] LA57 activo, desactivando...");
      cr4 &= ~(1ULL << 12);
      cr4_changed = 1;
    }

    if (cr4 & (1ULL << 17)) {
      LOG_INFO("[PAGING] PCID activo, desactivando...");
      cr4 &= ~(1ULL << 17);
      cr4_changed = 1;
    }

    if (cr4_changed) {
      write_cr4(cr4);
      uint64_t cr4_after = read_cr4();
      LOG_INFO("[PAGING] CR4 tras desactivar = %p (LA57=%d PCIDE=%d)",
               (void *)cr4_after, (int)((cr4_after >> 12) & 1),
               (int)((cr4_after >> 17) & 1));
    } else {
      LOG_INFO("[PAGING] CR4 ya estaba limpio (sin LA57, sin PCID)");
    }
  }

  map_phys_window(max_phys_addr);

  kernel_pml4 = (uint64_t *)phys_to_virt(boot_pml4_phys);
  LOG_INFO("[PAGING] kernel_pml4 virtual = %p (físico = %p)",
           (void *)kernel_pml4, (void *)boot_pml4_phys);

  {
    LOG_INFO("[PAGING] Identity map 0x7000-0x9000 (trampoline SMP)");
    int ok = 1;
    for (uint64_t pa = 0x7000; pa < 0x9000; pa += PAGE_SIZE) {
      int rc = paging_map_page_in(kernel_pml4, pa, pa,
                                  PTE_PRESENT | PTE_WRITABLE | PTE_GLOBAL);
      if (rc != 0) {
        LOG_ERR("[PAGING] fallo mapeando 0x%llx (rc=%d)",
                (unsigned long long)pa, rc);
        ok = 0;
      }
    }
    if (ok) {
      LOG_INFO("[PAGING] Identity map 0x7000-0x9000 OK");
    }
  }

  if (cpu_has_nx()) {
    uint64_t efer = rdmsr(MSR_EFER);
    efer |= EFER_NXE;
    wrmsr(MSR_EFER, efer);
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
    LOG_INFO("[PAGING] NX habilitado (EFER.NXE=1)");
  } else {
    LOG_WARN("[PAGING] CPU sin soporte NX");
  }

  extern uint8_t __text_start;
  extern uint8_t __text_end;
  clear_nx_range((uint64_t)&__text_start, (uint64_t)&__text_end);
  LOG_INFO("[PAGING] .text del kernel marcado como ejecutable");

  if (cpu_has_smep()) {
    uint64_t cr4 = read_cr4();
    cr4 |= (1ULL << 20);
    write_cr4(cr4);
    LOG_INFO("[PAGING] SMEP habilitado (CR4.SMEP=1)");
  } else {
    LOG_WARN("[PAGING] CPU sin soporte SMEP");
  }

  if (cpu_has_smap()) {
    uint64_t cr4 = read_cr4();
    cr4 |= (1ULL << 21);
    write_cr4(cr4);
    cpu_smap_enabled = 1;
    LOG_INFO("[PAGING] SMAP habilitado (CR4.SMAP=1)");
  } else {
    LOG_WARN("[PAGING] CPU sin soporte SMAP");
  }

  uint64_t cr4_final = read_cr4();
  LOG_INFO("[PAGING] CR4 final = %p (LA57=%d PCIDE=%d SMEP=%d SMAP=%d)",
           (void *)cr4_final, (int)((cr4_final >> 12) & 1),
           (int)((cr4_final >> 17) & 1), (int)((cr4_final >> 20) & 1),
           (int)((cr4_final >> 21) & 1));

  LOG_INFO("[PAGING] PML4 virtual en %p | Modo: PHYS_MAP_BASE + MMIO_MAP_BASE",
           (void *)kernel_pml4);
}

uint64_t *paging_get_pml4(void) { return kernel_pml4; }

int paging_map_page(uint64_t virt, uint64_t phys, uint64_t flags) {
  if (!paging_is_canonical(virt))
    return -1;
  return paging_map_page_in(kernel_pml4, virt, phys, flags);
}

int paging_map_range(uint64_t virt, uint64_t phys, uint64_t size,
                     uint64_t flags) {
  if (size == 0)
    return 0;

  uint64_t last_virt = virt + size - 1;
  uint64_t last_phys = phys + size - 1;
  if (last_virt < virt || last_phys < phys)
    return -1;
  if (last_phys > PTE_FRAME + PAGE_SIZE - 1)
    return -1;

  if (!paging_is_canonical(virt) || !paging_is_canonical(last_virt))
    return -1;
  int virt_low = virt <= 0x00007FFFFFFFFFFFULL;
  int last_virt_low = last_virt <= 0x00007FFFFFFFFFFFULL;
  int virt_high = virt >= 0xFFFF800000000000ULL;
  int last_virt_high = last_virt >= 0xFFFF800000000000ULL;
  if (virt_low != last_virt_low || virt_high != last_virt_high)
    return -1;

  for (uint64_t off = 0; off < size; off += PAGE_SIZE) {
    if (paging_map_page(virt + off, phys + off, flags) != 0)
      return -1;
  }
  return 0;
}

int paging_unmap_page(uint64_t virt) {
  if (!paging_is_canonical(virt))
    return -1;
  uint64_t pml4_idx = PML4_INDEX(virt);
  uint64_t pdpt_idx = PDPT_INDEX(virt);
  uint64_t pd_idx = PD_INDEX(virt);
  uint64_t pt_idx = PT_INDEX(virt);

  uint64_t *pml4 = kernel_pml4;
  if (!(pml4[pml4_idx] & PTE_PRESENT))
    return -1;
  uint64_t *pdpt = (uint64_t *)phys_to_virt(pml4[pml4_idx] & PTE_FRAME);

  if (!(pdpt[pdpt_idx] & PTE_PRESENT))
    return -1;
  uint64_t *pd = (uint64_t *)phys_to_virt(pdpt[pdpt_idx] & PTE_FRAME);

  if (!(pd[pd_idx] & PTE_PRESENT))
    return -1;
  if (pd[pd_idx] & PTE_HUGE)
    return -1;
  uint64_t *pt = (uint64_t *)phys_to_virt(pd[pd_idx] & PTE_FRAME);

  pt[pt_idx] = 0;
  paging_invalidate_tlb(virt);
  return 0;
}

uint64_t paging_get_phys(uint64_t virt) {
  if (!paging_is_canonical(virt))
    return 0;
  return paging_get_phys_in(kernel_pml4, virt);
}

void paging_invalidate_tlb(uint64_t virt) {
  __asm__ volatile("invlpg (%0)" : : "r"(virt) : "memory");
}

int vmm_alloc_pages(uint64_t vaddr, uint64_t num_pages, uint64_t flags) {
  if (num_pages == 0)
    return -1;

  for (uint64_t i = 0; i < num_pages; i++) {
    uint64_t phys = pmm_alloc_page();
    if (!phys) {
      for (uint64_t j = 0; j < i; j++) {
        uint64_t v = vaddr + j * PAGE_SIZE;
        uint64_t p = paging_get_phys(v);
        if (p) {
          paging_unmap_page(v);
          pmm_free_page(p);
        }
      }
      return -1;
    }
    if (paging_map_page(vaddr + i * PAGE_SIZE, phys, flags) != 0) {
      pmm_free_page(phys);
      for (uint64_t j = 0; j < i; j++) {
        uint64_t v = vaddr + j * PAGE_SIZE;
        uint64_t p = paging_get_phys(v);
        if (p) {
          paging_unmap_page(v);
          pmm_free_page(p);
        }
      }
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

uint64_t paging_clone_kernel_space(void) {
  uint64_t phys = pmm_alloc_page();
  if (!phys) {
    LOG_ERR("[PAGING] ERROR: no se pudo asignar PML4 para proceso");
    return 0;
  }
  uint64_t *new_pml4 = (uint64_t *)phys_to_virt(phys);
  memset(new_pml4, 0, PAGE_SIZE);

  for (int i = 256; i < 512; i++) {
    new_pml4[i] = kernel_pml4[i];
  }

  return phys;
}

int paging_map_page_in(uint64_t *pml4, uint64_t virt, uint64_t phys,
                       uint64_t flags) {
  if (!pml4 || !paging_is_canonical(virt))
    return -1;

  uint64_t pml4_idx = PML4_INDEX(virt);
  if ((flags & PTE_USER) && pml4_idx >= 256)
    return -1;

  uint64_t pdpt_idx = PDPT_INDEX(virt);
  uint64_t pd_idx = PD_INDEX(virt);
  uint64_t pt_idx = PT_INDEX(virt);

  uint64_t intermediate_flags = PTE_PRESENT | PTE_WRITABLE;
  if (flags & PTE_USER)
    intermediate_flags |= PTE_USER;

  uint64_t old_pml4 = pml4[pml4_idx];
  uint64_t old_pdpt = 0;
  int created_pml4 = 0;
  int created_pdpt = 0;

  if (!(old_pml4 & PTE_PRESENT)) {
    uint64_t *new_pdpt = alloc_page_table();
    if (!new_pdpt)
      return -1;
    pml4[pml4_idx] = (virt_to_phys(new_pdpt) & PTE_FRAME) | intermediate_flags;
    created_pml4 = 1;
  } else {
    if (flags & PTE_USER)
      pml4[pml4_idx] |= PTE_USER;
    if (flags & PTE_WRITABLE)
      pml4[pml4_idx] |= PTE_WRITABLE;
  }
  uint64_t *pdpt = (uint64_t *)phys_to_virt(pml4[pml4_idx] & PTE_FRAME);
  old_pdpt = pdpt[pdpt_idx];

  if (!(old_pdpt & PTE_PRESENT)) {
    uint64_t *new_pd = alloc_page_table();
    if (!new_pd) {
      if (created_pml4) {
        pmm_free_page(pml4[pml4_idx] & PTE_FRAME);
        pml4[pml4_idx] = old_pml4;
      }
      return -1;
    }
    pdpt[pdpt_idx] = (virt_to_phys(new_pd) & PTE_FRAME) | intermediate_flags;
    created_pdpt = 1;
  } else {
    if (flags & PTE_USER)
      pdpt[pdpt_idx] |= PTE_USER;
    if (flags & PTE_WRITABLE)
      pdpt[pdpt_idx] |= PTE_WRITABLE;
  }
  uint64_t *pd = (uint64_t *)phys_to_virt(pdpt[pdpt_idx] & PTE_FRAME);

  if ((pd[pd_idx] & PTE_PRESENT) && (pd[pd_idx] & PTE_HUGE)) {
    uint64_t *split_pt = split_huge_page(pd, pd_idx);
    if (!split_pt) {
      if (created_pdpt) {
        pmm_free_page(pdpt[pdpt_idx] & PTE_FRAME);
        pdpt[pdpt_idx] = old_pdpt;
      }
      if (created_pml4) {
        pmm_free_page(pml4[pml4_idx] & PTE_FRAME);
        pml4[pml4_idx] = old_pml4;
      }
      return -1;
    }
  }

  if (!(pd[pd_idx] & PTE_PRESENT)) {
    uint64_t *new_pt = alloc_page_table();
    if (!new_pt) {
      if (created_pdpt) {
        pmm_free_page(pdpt[pdpt_idx] & PTE_FRAME);
        pdpt[pdpt_idx] = old_pdpt;
      }
      if (created_pml4) {
        pmm_free_page(pml4[pml4_idx] & PTE_FRAME);
        pml4[pml4_idx] = old_pml4;
      }
      return -1;
    }
    pd[pd_idx] = (virt_to_phys(new_pt) & PTE_FRAME) | intermediate_flags;
  } else {
    if (flags & PTE_USER)
      pd[pd_idx] |= PTE_USER;
    if (flags & PTE_WRITABLE)
      pd[pd_idx] |= PTE_WRITABLE;
  }
  uint64_t *pt = (uint64_t *)phys_to_virt(pd[pd_idx] & PTE_FRAME);

  pt[pt_idx] = (phys & PTE_FRAME) | (flags & (0xFFF | PTE_NX)) | PTE_PRESENT;
  paging_invalidate_tlb(virt);
  return 0;
}

int paging_unmap_page_in(uint64_t *pml4, uint64_t virt) {
  if (!pml4 || !paging_is_canonical(virt))
    return -1;
  uint64_t pml4_idx = PML4_INDEX(virt);
  uint64_t pdpt_idx = PDPT_INDEX(virt);
  uint64_t pd_idx = PD_INDEX(virt);
  uint64_t pt_idx = PT_INDEX(virt);

  if (!(pml4[pml4_idx] & PTE_PRESENT))
    return -1;
  uint64_t *pdpt = (uint64_t *)phys_to_virt(pml4[pml4_idx] & PTE_FRAME);
  if (!(pdpt[pdpt_idx] & PTE_PRESENT))
    return -1;
  uint64_t *pd = (uint64_t *)phys_to_virt(pdpt[pdpt_idx] & PTE_FRAME);
  if (!(pd[pd_idx] & PTE_PRESENT))
    return -1;
  if (pd[pd_idx] & PTE_HUGE)
    return -1;
  uint64_t *pt = (uint64_t *)phys_to_virt(pd[pd_idx] & PTE_FRAME);
  if (!(pt[pt_idx] & PTE_PRESENT))
    return -1;

  pt[pt_idx] = 0;
  paging_invalidate_tlb(virt);
  return 0;
}

uint64_t paging_get_phys_in(uint64_t *pml4, uint64_t virt) {
  if (!pml4 || !paging_is_canonical(virt))
    return 0;
  uint64_t pml4_idx = PML4_INDEX(virt);
  uint64_t pdpt_idx = PDPT_INDEX(virt);
  uint64_t pd_idx = PD_INDEX(virt);
  uint64_t pt_idx = PT_INDEX(virt);

  if (!(pml4[pml4_idx] & PTE_PRESENT))
    return 0;
  uint64_t *pdpt = (uint64_t *)phys_to_virt(pml4[pml4_idx] & PTE_FRAME);

  if (!(pdpt[pdpt_idx] & PTE_PRESENT))
    return 0;
  uint64_t *pd = (uint64_t *)phys_to_virt(pdpt[pdpt_idx] & PTE_FRAME);

  if (!(pd[pd_idx] & PTE_PRESENT))
    return 0;
  if (pd[pd_idx] & PTE_HUGE) {
    return (pd[pd_idx] & PTE_FRAME) + (virt & 0x1FFFFF);
  }
  uint64_t *pt = (uint64_t *)phys_to_virt(pd[pd_idx] & PTE_FRAME);

  if (!(pt[pt_idx] & PTE_PRESENT))
    return 0;
  return (pt[pt_idx] & PTE_FRAME) | (virt & 0xFFF);
}

void paging_free_user_space(uint64_t pml4_phys) {
  if (!pml4_phys)
    return;

  uint64_t *pml4 = (uint64_t *)phys_to_virt(pml4_phys);

  for (int i = 0; i < 256; i++) {
    if (!(pml4[i] & PTE_PRESENT))
      continue;
    uint64_t *pdpt = (uint64_t *)phys_to_virt(pml4[i] & PTE_FRAME);

    for (int j = 0; j < 512; j++) {
      if (!(pdpt[j] & PTE_PRESENT))
        continue;
      uint64_t *pd = (uint64_t *)phys_to_virt(pdpt[j] & PTE_FRAME);

      for (int k = 0; k < 512; k++) {
        if (!(pd[k] & PTE_PRESENT))
          continue;

        if (pd[k] & PTE_HUGE) {
          pd[k] = 0;
          continue;
        }

        uint64_t *pt = (uint64_t *)phys_to_virt(pd[k] & PTE_FRAME);
        for (int l = 0; l < 512; l++) {
          if (!(pt[l] & PTE_PRESENT))
            continue;
          pmm_free_page(pt[l] & PTE_FRAME);
          pt[l] = 0;
        }
        pmm_free_page(pd[k] & PTE_FRAME);
        pd[k] = 0;
      }
      pmm_free_page(pdpt[j] & PTE_FRAME);
      pdpt[j] = 0;
    }
    pmm_free_page(pml4[i] & PTE_FRAME);
    pml4[i] = 0;
  }
  pmm_free_page(pml4_phys);
}

void *mmio_map(uint64_t phys, uint64_t size, uint64_t flags) {
  if (size == 0)
    return NULL;

  const uint64_t pte_max_phys = PTE_FRAME + PAGE_SIZE - 1;
  const uint64_t mmio_window_size = (1ULL << 39);
  const uint64_t mmio_max_phys = mmio_window_size - 1;
  const uint64_t max_phys =
      pte_max_phys < mmio_max_phys ? pte_max_phys : mmio_max_phys;

  if (phys > max_phys || size - 1 > max_phys - phys)
    return NULL;

  uint64_t last_phys = phys + size - 1;
  uint64_t start = phys & ~0xFFFULL;
  uint64_t end = (last_phys & ~0xFFFULL) + PAGE_SIZE;

  if (end > mmio_window_size)
    return NULL;

  uint64_t pte_flags = flags | PTE_PRESENT;

  for (uint64_t page = start; page < end; page += PAGE_SIZE) {
    uint64_t virt = MMIO_MAP_BASE + page;
    if (paging_map_page(virt, page, pte_flags) != 0) {
      for (uint64_t p = start; p < page; p += PAGE_SIZE) {
        paging_unmap_page(MMIO_MAP_BASE + p);
      }
      LOG_ERR("[MMIO] mmio_map: fallo mapeando phys=%p", (void *)page);
      return NULL;
    }
  }

  LOG_DEBUG("[MMIO] Mapeado phys=%p size=%lu -> virt=%p flags=%p", (void *)phys,
            (unsigned long)size, (void *)(MMIO_MAP_BASE + phys), (void *)flags);

  return (void *)(MMIO_MAP_BASE + phys);
}

void mmio_unmap(uint64_t phys, uint64_t size) {
  if (size == 0)
    return;

  const uint64_t pte_max_phys = PTE_FRAME + PAGE_SIZE - 1;
  const uint64_t mmio_window_size = (1ULL << 39);
  const uint64_t mmio_max_phys = mmio_window_size - 1;
  const uint64_t max_phys =
      pte_max_phys < mmio_max_phys ? pte_max_phys : mmio_max_phys;

  if (phys > max_phys || size - 1 > max_phys - phys)
    return;

  uint64_t last_phys = phys + size - 1;
  uint64_t start = phys & ~0xFFFULL;
  uint64_t end = (last_phys & ~0xFFFULL) + PAGE_SIZE;

  if (end > mmio_window_size)
    return;

  for (uint64_t page = start; page < end; page += PAGE_SIZE) {
    paging_unmap_page(MMIO_MAP_BASE + page);
  }
}