#include "elf.h"
#include "heap.h" // ← NUEVO: kmalloc/kfree para el buffer temporal
#include "klog.h"
#include "paging.h"
#include "pmm.h"
#include "serial.h"
#include "string.h"
#include "uaccess.h"

// ---- reader de buffer para envolver elf_load sobre streaming -------------
struct elf_buf_ctx {
  const uint8_t *data;
  size_t size;
};

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
  if (!data || size < sizeof(Elf64_Ehdr))
    return -1;

  const Elf64_Ehdr *hdr = (const Elf64_Ehdr *)data;
  if (elf_validate_header(hdr) != 0)
    return -1;

  // The program-header table itself must fit completely inside the ELF.
  // Check multiplication/addition separately so malformed values cannot
  // wrap around and make the bounds check succeed.
  if (hdr->e_phentsize != sizeof(Elf64_Phdr) || hdr->e_phnum == 0)
    return -1;
  if (hdr->e_phoff > size)
    return -1;
  if ((uint64_t)hdr->e_phnum > (UINT64_MAX - hdr->e_phoff) / sizeof(Elf64_Phdr))
    return -1;

  uint64_t ph_end = hdr->e_phoff + (uint64_t)hdr->e_phnum * sizeof(Elf64_Phdr);
  if (ph_end > size)
    return -1;

  const Elf64_Phdr *phdrs =
      (const Elf64_Phdr *)((const uint8_t *)data + hdr->e_phoff);

  for (uint16_t i = 0; i < hdr->e_phnum; i++) {
    const Elf64_Phdr *ph = &phdrs[i];
    if (ph->p_type != PT_LOAD)
      continue;

    // ELF requires the file image to fit inside the file and the memory
    // image to be at least as large as the initialized file image.
    if (ph->p_offset > size || ph->p_filesz > size - ph->p_offset)
      return -1;
    if (ph->p_filesz > ph->p_memsz)
      return -1;

    // Reject virtual-address ranges that wrap around.
    if (ph->p_memsz > UINT64_MAX - ph->p_vaddr)
      return -1;
  }

  return 0;
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

static int64_t elf_buf_read(void *ctx, uint64_t offset, size_t size,
                            void *buf) {
  struct elf_buf_ctx *c = (struct elf_buf_ctx *)ctx;
  if (offset > c->size)
    return -1;
  if (size > c->size - offset)
    return -1;
  memcpy(buf, c->data + offset, size);
  return (int64_t)size;
}

// ---- núcleo: elf_load_streaming ------------------------------------------
#define ELF_PHDR_STACK 32

int elf_load_streaming(elf_read_fn read, void *ctx, uint64_t file_size,
                       uint64_t *pml4, uint64_t load_base, uint64_t *entry_out,
                       uint64_t *vma_start_out, uint64_t *vma_end_out) {
  if (!read || !pml4 || !entry_out || !vma_start_out || !vma_end_out)
    return -1;

  *entry_out = 0;
  *vma_start_out = ~0ULL;
  *vma_end_out = 0;

  // --- 1. Leer Elf64_Ehdr ---------------------------------------------------
  if (file_size < sizeof(Elf64_Ehdr)) {
    LOG_ERR("[ELF] Imagen demasiado pequeña (%llu < %llu)",
            (unsigned long long)file_size,
            (unsigned long long)sizeof(Elf64_Ehdr));
    return -1;
  }
  Elf64_Ehdr ehdr;
  if (read(ctx, 0, sizeof(ehdr), &ehdr) != (int64_t)sizeof(ehdr)) {
    LOG_ERR("[ELF] No se pudo leer Elf64_Ehdr");
    return -1;
  }
  if (elf_validate_header(&ehdr) != 0) {
    LOG_ERR("[ELF] Imagen ELF inválida");
    return -1;
  }

  // --- 2. Validar tabla de program headers ---------------------------------
  if (ehdr.e_phentsize != sizeof(Elf64_Phdr) || ehdr.e_phnum == 0) {
    LOG_ERR("[ELF] e_phentsize/e_phnum inválidos");
    return -1;
  }
  if (ehdr.e_phoff > file_size) {
    LOG_ERR("[ELF] e_phoff fuera del fichero");
    return -1;
  }
  if ((uint64_t)ehdr.e_phnum >
      (UINT64_MAX - ehdr.e_phoff) / sizeof(Elf64_Phdr)) {
    LOG_ERR("[ELF] Overflow en e_phnum * sizeof(Phdr)");
    return -1;
  }
  uint64_t ph_end = ehdr.e_phoff + (uint64_t)ehdr.e_phnum * sizeof(Elf64_Phdr);
  if (ph_end > file_size) {
    LOG_ERR("[ELF] Tabla de program headers fuera del fichero");
    return -1;
  }

  // --- 3. Leer program headers ---------------------------------------------
  size_t ph_bytes = (size_t)ehdr.e_phnum * sizeof(Elf64_Phdr);
  Elf64_Phdr local_phdrs[ELF_PHDR_STACK];
  Elf64_Phdr *phdrs = local_phdrs;
  int phdrs_on_heap = 0;
  if (ehdr.e_phnum > ELF_PHDR_STACK) {
    phdrs = (Elf64_Phdr *)kmalloc(ph_bytes);
    if (!phdrs) {
      LOG_ERR("[ELF] Sin memoria para %u program headers", ehdr.e_phnum);
      return -ENOMEM;
    }
    phdrs_on_heap = 1;
  }
  if (read(ctx, ehdr.e_phoff, ph_bytes, phdrs) != (int64_t)ph_bytes) {
    if (phdrs_on_heap)
      kfree(phdrs);
    LOG_ERR("[ELF] No se pudieron leer los program headers");
    return -1;
  }

  int rc = -1;
  const uint64_t user_canon_max = 0x00007FFFFFFFFFFFULL;
  const uint64_t user_canon_limit = 0x0000800000000000ULL;

  // --- 4. Preflight de todos los PT_LOAD -----------------------------------
  // Mismo criterio que la versión buffer: nada se mapea hasta saber que
  // TODOS los segmentos son válidos y canónicos.
  for (uint16_t i = 0; i < ehdr.e_phnum; i++) {
    const Elf64_Phdr *ph = &phdrs[i];
    if (ph->p_type != PT_LOAD)
      continue;

    if (ph->p_offset > file_size || ph->p_filesz > file_size - ph->p_offset) {
      LOG_ERR("[ELF] Segmento %u fuera del fichero (off=%llu filesz=%llu)", i,
              (unsigned long long)ph->p_offset,
              (unsigned long long)ph->p_filesz);
      goto out;
    }
    if (ph->p_filesz > ph->p_memsz) {
      LOG_ERR("[ELF] Segmento %u: p_filesz > p_memsz", i);
      goto out;
    }
    if (ph->p_memsz == 0)
      continue;

    if (ph->p_memsz > UINT64_MAX - ph->p_vaddr) {
      LOG_ERR("[ELF] Segmento %u: overflow en p_vaddr+p_memsz", i);
      goto out;
    }

    uint64_t vaddr = ph->p_vaddr;
    if (ehdr.e_type == ET_DYN) {
      if (vaddr > UINT64_MAX - load_base) {
        LOG_ERR("[ELF] Segmento %u: overflow en relocación", i);
        goto out;
      }
      vaddr += load_base;
    }
    if (vaddr > user_canon_max || ph->p_memsz > user_canon_limit - vaddr) {
      LOG_ERR("[ELF] Segmento %u fuera del espacio de usuario", i);
      goto out;
    }
  }

  // --- 5. Entry point -------------------------------------------------------
  {
    uint64_t entry = ehdr.e_entry;
    if (ehdr.e_type == ET_DYN) {
      if (entry > UINT64_MAX - load_base) {
        LOG_ERR("[ELF] Overflow en entry point");
        goto out;
      }
      entry += load_base;
    }
    if (entry > user_canon_max) {
      LOG_ERR("[ELF] Entry point fuera del espacio de usuario");
      goto out;
    }
    *entry_out = entry;
  }

  // --- 6. Mapear y copiar cada PT_LOAD -------------------------------------
  // Buffer temporal de lectura. Reutilizado entre segmentos.
  // PAGE_SIZE es suficiente porque cada chunk que pedimos está acotado
  // por el borde de página.
  uint8_t *tmp = (uint8_t *)kmalloc(PAGE_SIZE);
  if (!tmp) {
    rc = -ENOMEM;
    goto out;
  }

  uint64_t vma_start = ~0ULL;
  uint64_t vma_end = 0;

  for (uint16_t i = 0; i < ehdr.e_phnum; i++) {
    const Elf64_Phdr *ph = &phdrs[i];
    if (ph->p_type != PT_LOAD || ph->p_memsz == 0)
      continue;

    uint64_t vaddr = ph->p_vaddr;
    if (ehdr.e_type == ET_DYN)
      vaddr += load_base;

    uint64_t memsz = ph->p_memsz;
    uint64_t filesz = ph->p_filesz;
    uint64_t offset = ph->p_offset;

    uint64_t start_page = vaddr & ~0xFFFULL;
    uint64_t end_page = (vaddr + memsz + 0xFFFULL) & ~0xFFFULL;
    size_t num_pages = (end_page - start_page) / PAGE_SIZE;

    // Acumular el rango VMA global.
    if (start_page < vma_start)
      vma_start = start_page;
    if (end_page > vma_end)
      vma_end = end_page;

    uint64_t page_flags = PTE_USER | PTE_PRESENT;
    if (ph->p_flags & PF_W)
      page_flags |= PTE_WRITABLE;
    if (!(ph->p_flags & PF_X))
      page_flags |= PTE_NX;

    LOG_INFO("[ELF] Segmento: vaddr=%p memsz=%p flags=%p", (void *)vaddr,
             (void *)memsz, (void *)(uint64_t)ph->p_flags);

    for (size_t j = 0; j < num_pages; j++) {
      uint64_t page_vaddr = start_page + j * PAGE_SIZE;
      if (!paging_get_phys_in(pml4, page_vaddr)) {
        if (map_page_in_pml4(pml4, page_vaddr, page_flags) != 0) {
          LOG_ERR("[ELF] Error mapeando página");
          kfree(tmp);
          goto out;
        }
      }
    }

    // Copiar datos del fichero a las páginas mapeadas, leyendo por chunks.
    for (uint64_t off = 0; off < filesz;) {
      uint64_t page_vaddr = (vaddr + off) & ~0xFFFULL;
      uint64_t page_offset = (vaddr + off) & 0xFFFULL;
      uint64_t chunk = PAGE_SIZE - page_offset;
      if (chunk > filesz - off)
        chunk = filesz - off;

      uint64_t phys = paging_get_phys_in(pml4, page_vaddr);
      if (!phys) {
        LOG_ERR("[ELF] Física no encontrada para copia");
        kfree(tmp);
        goto out;
      }

      if (read(ctx, offset + off, chunk, tmp) != (int64_t)chunk) {
        LOG_ERR("[ELF] Short read copiando segmento (off=%llu chunk=%llu)",
                (unsigned long long)(offset + off), (unsigned long long)chunk);
        kfree(tmp);
        rc = -EIO;
        goto out;
      }

      uint8_t *dest = (uint8_t *)phys_to_virt(phys) + page_offset;
      memcpy(dest, tmp, chunk);
      off += chunk;
    }

    // Rellenar BSS.
    if (memsz > filesz) {
      uint64_t bss_start = vaddr + filesz;
      uint64_t bss_end = vaddr + memsz;
      for (uint64_t off = 0; off < bss_end - bss_start;) {
        uint64_t page_vaddr = (bss_start + off) & ~0xFFFULL;
        uint64_t page_offset = (bss_start + off) & 0xFFFULL;
        uint64_t chunk = PAGE_SIZE - page_offset;
        if (chunk > (bss_end - bss_start - off))
          chunk = bss_end - bss_start - off;

        uint64_t phys = paging_get_phys_in(pml4, page_vaddr);
        if (!phys) {
          LOG_ERR("[ELF] Física no encontrada para BSS");
          kfree(tmp);
          goto out;
        }
        uint8_t *dest = (uint8_t *)phys_to_virt(phys) + page_offset;
        memset(dest, 0, chunk);
        off += chunk;
      }
    }
  }

  kfree(tmp);
  *vma_start_out = vma_start;
  *vma_end_out = vma_end;
  LOG_INFO("[ELF] Entry point: %p (VMA %p - %p)", (void *)*entry_out,
           (void *)vma_start, (void *)vma_end);
  rc = 0;

out:
  if (phdrs_on_heap)
    kfree(phdrs);
  return rc;
}

// ---- wrapper de compatibilidad: buffer contiguo → streaming --------------
int elf_load(const void *data, size_t size, uint64_t *pml4, uint64_t load_base,
             uint64_t *entry_out) {
  struct elf_buf_ctx ctx = {.data = (const uint8_t *)data, .size = size};
  uint64_t vma_s, vma_e;
  return elf_load_streaming(elf_buf_read, &ctx, size, pml4, load_base,
                            entry_out, &vma_s, &vma_e);
}