// kernel/pf.h
#ifndef PF_H
#define PF_H

#include "idt.h"
#include <stdint.h>

// Tipos de VMA
#define VMA_ANON 0
#define VMA_STACK 1
#define VMA_ELF 2

struct process;

// Límites
#define MAX_STACK_GROWTH (64 * 1024) // 64 KB de stack growth permitido

typedef struct vma {
  uint64_t start; // inicio (inclusive)
  uint64_t end;   // fin (exclusive)
  uint64_t flags; // PTE_USER | PTE_WRITABLE | PTE_NX
  uint32_t type;  // VMA_ANON, VMA_STACK, VMA_ELF
  uint32_t pad;
  struct vma *next;
} vma_t;

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

// Syscalls
int64_t sys_mmap(struct process *proc, uint64_t addr, uint64_t length,
                 uint64_t prot, uint64_t flags, int fd, uint64_t offset);
int64_t sys_munmap(struct process *proc, uint64_t addr, uint64_t length);

// Estadísticas
uint64_t pf_stats_resolved(void);
uint64_t pf_stats_killed(void);
void pf_dump_stats(void);

#endif