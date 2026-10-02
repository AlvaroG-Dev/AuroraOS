// kernel/pf.h
#ifndef PF_H
#define PF_H

#include "idt.h"
#include <stdint.h>

// Tipos de VMA
#define VMA_ANON 0
#define VMA_STACK 1
#define VMA_ELF 2
#define VMA_FILE 3 // [3.5] mmap file-backed

struct process;
struct file_descriptor;

// Límites
#define MAX_STACK_GROWTH (64 * 1024) // 64 KB de stack growth permitido

typedef struct vma {
  uint64_t start;
  uint64_t end;
  uint64_t flags;
  uint32_t type;
  uint32_t pad;
  struct vma *next;

  // [3.5] Solo VMA_FILE. Puntero al file_descriptor_t del fichero
  // backing. El VMA mantiene una referencia (ref_count++) para que
  // el fichero sobreviva al close() del usuario.
  struct file_descriptor *file_fd;

  // [3.5] Offset del byte 0 del VMA dentro del fichero. Alineado a
  // página (Linux exige offset alineado en mmap).
  uint64_t file_offset;
} vma_t;

vma_t *vma_create_file(struct process *proc, uint64_t start, uint64_t end,
                       uint64_t flags, struct file_descriptor *fd,
                       uint64_t file_offset);

// Inicialización
void pf_init(void);

// Handler principal. Devuelve 1 si resolvió el fallo, 0 si no.
int handle_page_fault(registers_t *regs);

// Mata el proceso actual y salta a task_die_hlt.
void kill_current_process(registers_t *regs, const char *reason);

// VMA helpers
vma_t *vma_find(struct process *proc, uint64_t addr);
vma_t *vma_create(struct process *proc, uint64_t start, uint64_t end,
                  uint64_t flags, uint32_t type);
void vma_destroy_all(struct process *proc);

// [MMAP] Desmapea todos los VMAs que se solapen con [start, end) y
// libera sus páginas físicas. start y end deben estar alineados a
// página. Es la primitiva común de sys_munmap y sys_mmap(MAP_FIXED).
//
// Devuelve el número de VMAs tocados (>=0) o un errno negativo (solo
// -ENOMEM, si falla el kmalloc del split de un VMA).
int64_t vma_unmap_range(struct process *proc, uint64_t start, uint64_t end);

// Syscalls
int64_t sys_mmap(struct process *proc, uint64_t addr, uint64_t length,
                 uint64_t prot, uint64_t flags, int fd, uint64_t offset);
int64_t sys_munmap(struct process *proc, uint64_t addr, uint64_t length);

// [4.3] mprotect(addr, len, prot). Actualiza los VMAs y las PTEs.
int64_t sys_mprotect(struct process *proc, uint64_t addr, uint64_t length,
                     uint64_t prot);

// [B] mremap. Crece/encoge/realoca un rango mmap'd existente.
// Soporta crecimiento in-place si hay hueco contiguo, o move+copy si
// MREMAP_MAYMOVE. No soporta MREMAP_FIXED.
int64_t sys_mremap(struct process *proc, uint64_t old_addr, uint64_t old_size,
                   uint64_t new_size, uint64_t flags, uint64_t new_addr);

// Estadísticas
uint64_t pf_stats_resolved(void);
uint64_t pf_stats_killed(void);
void pf_dump_stats(void);

#endif