/* bootloader/bios/paging.c
 *
 * Construccion de la PML4 inicial. Reproduce lo que el UEFI bootloader
 * hace en build_page_tables(): identity map [0, max_phys) con paginas
 * de 2 MB, mas huecos para añadir mapeos de 4 KB.
 */
#include "bios.h"

#define PML4_IDX(v) (((v) >> 39) & 0x1FF)
#define PDPT_IDX(v) (((v) >> 30) & 0x1FF)
#define PD_IDX(v) (((v) >> 21) & 0x1FF)
#define PT_IDX(v) (((v) >> 12) & 0x1FF)

uint32_t paging_build_identity_pml4(uint64_t max_phys) {
  uint32_t pml4_phys = PML4_ADDR;
  uint32_t pdpt_phys = PML4_ADDR + 0x1000;
  uint32_t pd_phys[4];
  for (int i = 0; i < 4; i++)
    pd_phys[i] = PML4_ADDR + 0x2000 + i * 0x1000;

  uint64_t *pml4 = (uint64_t *)pml4_phys;
  uint64_t *pdpt = (uint64_t *)pdpt_phys;

  zero_mem(pml4, 0x1000);
  zero_mem(pdpt, 0x1000);
  for (int i = 0; i < 4; i++)
    zero_mem((void *)pd_phys[i], 0x1000);

  pml4[0] = (uint64_t)pdpt_phys | 0x03;
  for (int pdx = 0; pdx < 4; pdx++) {
    pdpt[pdx] = (uint64_t)pd_phys[pdx] | 0x03;
    uint64_t *pd = (uint64_t *)pd_phys[pdx];
    for (int ptx = 0; ptx < 512; ptx++) {
      uint64_t phys =
          (uint64_t)pdx * 0x40000000ULL + (uint64_t)ptx * 0x200000ULL;
      if (phys >= (uint64_t)max_phys + 0x200000ULL)
        break;
      pd[ptx] = phys | 0x83; /* present + rw + huge */
    }
  }
  return pml4_phys;
}

/* Instala una tabla intermedia si falta. */
static uint32_t ensure_table(uint64_t *entry) {
  if (*entry & 1) {
    return (uint32_t)(*entry & 0x000FFFFFFFFFF000ULL);
  }
  uint32_t p = bump_alloc_page();
  if (!p)
    return 0;
  zero_mem((void *)p, 0x1000);
  *entry = (uint64_t)p | 0x03;
  return p;
}

int paging_map_4k(uint32_t *pml4, uint64_t virt, uint64_t phys) {
  uint32_t i4 = PML4_IDX(virt);
  uint32_t i3 = PDPT_IDX(virt);
  uint32_t i2 = PD_IDX(virt);
  uint32_t i1 = PT_IDX(virt);

  uint64_t *pml4e = (uint64_t *)pml4 + i4;
  uint32_t pdpt_phys = ensure_table(pml4e);
  if (!pdpt_phys)
    return -1;

  uint64_t *pdpt = (uint64_t *)pdpt_phys;
  uint32_t pd_phys = ensure_table(&pdpt[i3]);
  if (!pd_phys)
    return -1;

  uint64_t *pd = (uint64_t *)pd_phys;
  if (pd[i2] & 0x80) {
    /* Es una huge page de 2 MB. Sustituir por PT. */
    uint64_t huge = pd[i2];
    uint64_t huge_base = huge & 0x000FFFFFFFE00000ULL;
    uint32_t pt_phys = bump_alloc_page();
    if (!pt_phys)
      return -1;
    zero_mem((void *)pt_phys, 0x1000);
    uint64_t *pt = (uint64_t *)pt_phys;
    for (int k = 0; k < 512; k++) {
      pt[k] =
          (huge_base + (uint64_t)k * 0x1000) | 0x03 | (huge & 0xFFF & ~0x80);
    }
    pd[i2] = (uint64_t)pt_phys | 0x03;
  }

  uint32_t pt_phys = ensure_table(&pd[i2]);
  if (!pt_phys)
    return -1;

  uint64_t *pt = (uint64_t *)pt_phys;
  pt[i1] = (phys & 0x000FFFFFFFFFF000ULL) | 0x03;
  return 0;
}

int paging_map_2m(uint32_t *pml4, uint64_t virt, uint64_t phys) {
  if ((virt & 0x1FFFFF) || (phys & 0x1FFFFF))
    return -1;

  uint32_t i4 = PML4_IDX(virt);
  uint32_t i3 = PDPT_IDX(virt);
  uint32_t i2 = PD_IDX(virt);

  uint64_t *pml4e = (uint64_t *)pml4 + i4;
  uint32_t pdpt_phys = ensure_table(pml4e);
  if (!pdpt_phys)
    return -1;

  uint64_t *pdpt = (uint64_t *)pdpt_phys;
  uint32_t pd_phys = ensure_table(&pdpt[i3]);
  if (!pd_phys)
    return -1;

  uint64_t *pd = (uint64_t *)pd_phys;
  pd[i2] = (phys & 0x000FFFFFFFE00000ULL) | 0x83;
  return 0;
}