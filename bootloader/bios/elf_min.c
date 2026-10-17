/* bootloader/bios/elf_min.c
 *
 * Cargador ELF64 ET_EXEC. Reutiliza el algoritmo de efi_main.c
 * (build_page_tables + map_page_4k).
 *
 * Entrada: buffer con la imagen ELF entera.
 * Salida: entry_point (VA) y actualiza la PML4 con los mapeos.
 *
 * Todos los mapeos van con PTE = phys | 0x03 (present + rw). El kernel
 * reemplaza luego con paging_init y clear_nx_range.
 */
#include "bios.h"

#define ELF_MAG0 0x7F
#define ELF_MAG1 'E'
#define ELF_MAG2 'L'
#define ELF_MAG3 'F'
#define EM_X86_64 62
#define ET_EXEC 2
#define PT_LOAD 1

typedef struct {
  uint8_t e_ident[16];
  uint16_t e_type;
  uint16_t e_machine;
  uint32_t e_version;
  uint64_t e_entry;
  uint64_t e_phoff;
  uint64_t e_shoff;
  uint32_t e_flags;
  uint16_t e_ehsize;
  uint16_t e_phentsize;
  uint16_t e_phnum;
  uint16_t e_shentsize;
  uint16_t e_shnum;
  uint16_t e_shstrndx;
} __attribute__((packed)) elf_ehdr_t;

typedef struct {
  uint32_t p_type;
  uint32_t p_flags;
  uint64_t p_offset;
  uint64_t p_vaddr;
  uint64_t p_paddr;
  uint64_t p_filesz;
  uint64_t p_memsz;
  uint64_t p_align;
} __attribute__((packed)) elf_phdr_t;

/* Bump allocator (declarado en bios.h). Lo usamos para colocar los
 * segmentos del kernel en memoria fisica. */
extern uint32_t bump_alloc_pages(uint32_t n);

int elf_load(const uint8_t *elf_data, uint32_t elf_size, uint32_t *pml4,
             uint64_t *entry_out) {
  if (elf_size < sizeof(elf_ehdr_t))
    return -1;

  const elf_ehdr_t *eh = (const elf_ehdr_t *)elf_data;
  if (eh->e_ident[0] != ELF_MAG0 || eh->e_ident[1] != ELF_MAG1 ||
      eh->e_ident[2] != ELF_MAG2 || eh->e_ident[3] != ELF_MAG3) {
    bios_log("[ELF] firma invalida\n");
    return -1;
  }
  if (eh->e_machine != EM_X86_64) {
    bios_log("[ELF] no es x86_64\n");
    return -1;
  }
  if (eh->e_type != ET_EXEC) {
    bios_log("[ELF] no es ET_EXEC\n");
    return -1;
  }
  if (eh->e_phentsize != sizeof(elf_phdr_t)) {
    bios_log("[ELF] e_phentsize != 56\n");
    return -1;
  }
  if (eh->e_phoff + (uint64_t)eh->e_phnum * sizeof(elf_phdr_t) > elf_size) {
    bios_log("[ELF] phdrs fuera de la imagen\n");
    return -1;
  }

  const elf_phdr_t *ph = (const elf_phdr_t *)(elf_data + eh->e_phoff);

  for (uint16_t i = 0; i < eh->e_phnum; i++) {
    const elf_phdr_t *p = &ph[i];
    if (p->p_type != PT_LOAD)
      continue;
    if (p->p_filesz > p->p_memsz)
      return -1;
    if (p->p_offset + p->p_filesz > elf_size)
      return -1;

    uint32_t npages = (uint32_t)((p->p_memsz + 0xFFF) / 0x1000);
    if (npages == 0)
      npages = 1;

    uint32_t phys = bump_alloc_pages(npages);
    if (!phys) {
      bios_log("[ELF] bump alloc fallo (%u pages)\n", npages);
      return -1;
    }

    /* Copiar contenido file-backed. */
    if (p->p_filesz > 0) {
      copy_mem((void *)phys, elf_data + p->p_offset, (uint32_t)p->p_filesz);
    }
    /* BSS: resto de memsz. */
    if (p->p_memsz > p->p_filesz) {
      zero_mem((void *)(phys + p->p_filesz),
               (uint32_t)(p->p_memsz - p->p_filesz));
    }

    /* Mapear en la PML4: cada pagina virtual -> fisica correspondiente. */
    for (uint32_t k = 0; k < npages; k++) {
      if (paging_map_4k(pml4, p->p_vaddr + (uint64_t)k * 0x1000,
                        phys + (uint64_t)k * 0x1000) != 0) {
        bios_log("[ELF] paging_map_4k fallo\n");
        return -1;
      }
    }

    bios_log("[ELF] PT_LOAD %u: vaddr=0x%lx paddr=0x%lx pages=%u\n",
             (unsigned)i, (unsigned long)p->p_vaddr, (unsigned long)phys,
             (unsigned)npages);
  }

  *entry_out = eh->e_entry;
  return 0;
}