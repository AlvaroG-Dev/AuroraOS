#ifndef ELF_H
#define ELF_H

#include <stddef.h>
#include <stdint.h>

#define EI_MAG0 0
#define EI_MAG1 1
#define EI_MAG2 2
#define EI_MAG3 3
#define EI_CLASS 4
#define EI_DATA 5
#define EI_VERSION 6
#define EI_OSABI 7
#define EI_ABIVERSION 8
#define EI_PAD 9
#define EI_NIDENT 16

#define ELFMAG0 0x7F
#define ELFMAG1 'E'
#define ELFMAG2 'L'
#define ELFMAG3 'F'
#define ELFCLASS64 2
#define ELFDATA2LSB 1
#define EV_CURRENT 1
#define EM_X86_64 62

#define PT_LOAD 1

#define PF_X 1
#define PF_W 2
#define PF_R 4

#define ET_EXEC 2
#define ET_DYN 3

typedef struct {
  uint8_t e_ident[EI_NIDENT];
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
} Elf64_Ehdr;

typedef struct {
  uint32_t p_type;
  uint32_t p_flags;
  uint64_t p_offset;
  uint64_t p_vaddr;
  uint64_t p_paddr;
  uint64_t p_filesz;
  uint64_t p_memsz;
  uint64_t p_align;
} Elf64_Phdr;

int elf_validate(const void *data, size_t size);
int elf_load(const void *data, size_t size, uint64_t *pml4, uint64_t load_base,
             uint64_t *entry_out);

// ---------------------------------------------------------------------------
// Loader streaming: lee el ELF a través de un callback en vez de asumir
// que está en un buffer contiguo. Útil para cargar desde VFS/FAT32 sin
// consumir heap proporcional al tamaño del binario.
//
// Contrato del callback:
//   - read(ctx, offset, size, buf) debe copiar EXACTAMENTE `size` bytes
//     desde `offset` del fichero a `buf`.
//   - Devuelve `size` en éxito, negativo en error.
//   - Un retorno != size se considera error (no se soporta short reads).
//
// vma_start_out / vma_end_out: rango [start, end) page-aligned que cubre
// todos los PT_LOAD del ELF ya relocalizados. Si no hay PT_LOAD, se
// devuelve start = ~0ULL, end = 0 (mismo sentinel que usa process.c).
// ---------------------------------------------------------------------------
typedef int64_t (*elf_read_fn)(void *ctx, uint64_t offset, size_t size,
                               void *buf);

int elf_load_streaming(elf_read_fn read, void *ctx, uint64_t file_size,
                       uint64_t *pml4, uint64_t load_base, uint64_t *entry_out,
                       uint64_t *vma_start_out, uint64_t *vma_end_out);

#endif