#include "elf.h"
#include "klog.h"
#include "paging.h"
#include "pmm.h"
#include "serial.h"
#include "string.h"

static int elf_validate_header(const Elf64_Ehdr *hdr) {
  if (hdr->e_ident[EI_MAG0] != ELFMAG0 || hdr->e_ident[EI_MAG1] != ELFMAG1 ||
      hdr->e_ident[EI_MAG2] != ELFMAG2 || hdr->e_ident[EI_MAG3] != ELFMAG3)
    return -1;

  if (hdr->e_ident[EI_CLASS] != ELFCLASS64 ||
      hdr->e_ident[EI_DATA] != ELFDATA2LSB ||
      hdr->e_ident[EI_VERSION] != EV_CURRENT)
    return -1;

  if (hdr->e_machine != EM_X86_64)
    return -1;
  if (hdr->e_type != ET_EXEC && hdr->e_type != ET_DYN)
    return -1;

  return 0;
}

int elf_validate(const void *data, size_t size) {
  if (size < sizeof(Elf64_Ehdr))
    return -1;
  return elf_validate_header((const Elf64_Ehdr *)data);
}

static int map_page_in_pml4(uint64_t *pml4, uint64_t virt, uint64_t flags) {
  uint64_t phys = pmm_alloc_page();
  if (!phys)
    return -1;
  if (paging_map_page_in(pml4, virt, phys, flags) != 0) {
    pmm_free_page(phys);
    return -1;
  }
  return 0;
}

int elf_load(const void *data, size_t size, uint64_t *pml4, uint64_t load_base,
             uint64_t *entry_out) {
  const Elf64_Ehdr *hdr = (const Elf64_Ehdr *)data;
  if (elf_validate_header(hdr) != 0) {
    LOG_ERR("[ELF] Cabecera inválida");
    return -1;
  }

  if (size < hdr->e_phoff + hdr->e_phnum * sizeof(Elf64_Phdr)) {
    LOG_ERR("[ELF] Tamaño insuficiente para program headers");
    return -1;
  }

  const Elf64_Phdr *phdrs =
      (const Elf64_Phdr *)((uint8_t *)data + hdr->e_phoff);

  for (uint16_t i = 0; i < hdr->e_phnum; i++) {
    const Elf64_Phdr *ph = &phdrs[i];
    if (ph->p_type != PT_LOAD)
      continue;

    uint64_t vaddr =
        (hdr->e_type == ET_DYN) ? (ph->p_vaddr + load_base) : ph->p_vaddr;
    uint64_t memsz = ph->p_memsz;
    uint64_t filesz = ph->p_filesz;
    uint64_t offset = ph->p_offset;

    uint64_t start_page = vaddr & ~0xFFFULL;
    uint64_t end_page = (vaddr + memsz + 0xFFF) & ~0xFFFULL;
    size_t num_pages = (end_page - start_page) / PAGE_SIZE;

    uint64_t page_flags = PTE_USER | PTE_PRESENT | PTE_WRITABLE;
    if (!(ph->p_flags & PF_X))
      page_flags |= PTE_NX;

    LOG_INFO("[ELF] Segmento: vaddr=%p memsz=%p flags=%p", (void *)vaddr,
             (void *)memsz, (void *)(uint64_t)ph->p_flags);

    for (size_t j = 0; j < num_pages; j++) {
      uint64_t page_vaddr = start_page + j * PAGE_SIZE;
      if (!paging_get_phys_in(pml4, page_vaddr)) {
        if (map_page_in_pml4(pml4, page_vaddr, page_flags) != 0) {
          LOG_ERR("[ELF] Error mapeando página");
          return -1;
        }
      }
    }

    // Copiar los datos del ejecutable a las páginas de usuario
    for (uint64_t off = 0; off < filesz;) {
      uint64_t page_vaddr = (vaddr + off) & ~0xFFFULL;
      uint64_t page_offset = (vaddr + off) & 0xFFFULL;
      uint64_t chunk = PAGE_SIZE - page_offset;
      if (chunk > filesz - off)
        chunk = filesz - off;

      uint64_t phys = paging_get_phys_in(pml4, page_vaddr);
      if (!phys) {
        LOG_ERR("[ELF] Física no encontrada para copia");
        return -1;
      }
      // FIX: phys es una dirección física. Hay que usar phys_to_virt para
      // acceder a su contenido (el CR3 activo puede ser el del usuario).
      uint8_t *dest = (uint8_t *)phys_to_virt(phys) + page_offset;
      const uint8_t *src = (const uint8_t *)data + offset + off;
      memcpy(dest, src, chunk);
      off += chunk;
    }

    // Rellenar BSS (memsz > filesz) con ceros
    if (memsz > filesz) {
      uint64_t bss_start = vaddr + filesz;
      uint64_t bss_end = vaddr + memsz;
      for (uint64_t off = 0; off < bss_end - bss_start;) {
        uint64_t page_vaddr = (bss_start + off) & ~0xFFFULL;
        uint64_t page_offset = (bss_start + off) & 0xFFFULL;
        uint64_t chunk = PAGE_SIZE - page_offset;
        if (chunk > (bss_end - bss_start - off))
          chunk = (bss_end - bss_start - off);

        uint64_t phys = paging_get_phys_in(pml4, page_vaddr);
        if (!phys) {
          LOG_ERR("[ELF] Física no encontrada para BSS");
          return -1;
        }
        // FIX: phys_to_virt.
        uint8_t *dest = (uint8_t *)phys_to_virt(phys) + page_offset;
        memset(dest, 0, chunk);
        off += chunk;
      }
    }
  }

  *entry_out =
      (hdr->e_type == ET_DYN) ? (hdr->e_entry + load_base) : hdr->e_entry;
  LOG_INFO("[ELF] Entry point: %p", (void *)*entry_out);
  return 0;
}