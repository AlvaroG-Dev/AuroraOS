// kernel/pf.c
// Page fault handler con demand paging, VMA y stack growth.

#include "pf.h"
#include "cpu.h"
#include "gdt.h"
#include "heap.h"
#include "klog.h"
#include "paging.h"
#include "pmm.h"
#include "process.h"
#include "sched.h"
#include "serial.h"
#include "string.h"
#include <stddef.h>


// ---------------------------------------------------------------------------
// Estadísticas
// ---------------------------------------------------------------------------
static uint64_t pf_resolved = 0;
static uint64_t pf_killed = 0;

uint64_t pf_stats_resolved(void) { return pf_resolved; }
uint64_t pf_stats_killed(void) { return pf_killed; }

void pf_dump_stats(void) {
  LOG_INFO("[PF] Página faults: %lu resueltos, %lu fatales",
           (unsigned long)pf_resolved, (unsigned long)pf_killed);
}

// ---------------------------------------------------------------------------
// VMA list
// ---------------------------------------------------------------------------
vma_t *vma_find(struct process *proc, uint64_t addr) {
  if (!proc)
    return NULL;
  vma_t *v = proc->vma_list;
  while (v) {
    if (addr >= v->start && addr < v->end)
      return v;
    v = v->next;
  }
  return NULL;
}

vma_t *vma_create(struct process *proc, uint64_t start, uint64_t end,
                  uint64_t flags, uint32_t type) {
  if (!proc || start >= end)
    return NULL;

  vma_t *v = (vma_t *)kmalloc(sizeof(vma_t));
  if (!v)
    return NULL;
  v->start = start & ~0xFFFULL;
  v->end = (end + 0xFFF) & ~0xFFFULL;
  v->flags = flags;
  v->type = type;
  v->pad = 0;
  v->next = proc->vma_list;
  proc->vma_list = v;
  return v;
}

void vma_destroy_all(struct process *proc) {
  if (!proc)
    return;
  vma_t *v = proc->vma_list;
  while (v) {
    vma_t *next = v->next;
    kfree(v);
    v = next;
  }
  proc->vma_list = NULL;
}

// ---------------------------------------------------------------------------
// Stack growth
// ---------------------------------------------------------------------------
// El stack crece hacia abajo. Si el fallo está por debajo del stack
// actual pero dentro del rango permitido, mapeamos la página.
static int try_stack_growth(struct process *proc, uint64_t fault_addr,
                            uint64_t flags) {
  if (!proc || !proc->stack_low)
    return 0;

  // ¿Está la dirección por debajo del stack actual?
  if (fault_addr >= proc->stack_base)
    return 0;

  // ¿Está dentro del rango permitido?
  if (fault_addr < proc->stack_low)
    return 0;

  // Mapear la página del fallo
  uint64_t page = fault_addr & ~0xFFFULL;
  uint64_t phys = pmm_alloc_page();
  if (!phys)
    return 0;

  uint64_t map_flags = PTE_USER | PTE_WRITABLE | PTE_PRESENT | PTE_NX;
  (void)flags;

  if (paging_map_page_in((uint64_t *)phys_to_virt(proc->pml4_phys), page, phys,
                         map_flags) != 0) {
    pmm_free_page(phys);
    return 0;
  }

  LOG_TRACE("[PF] Stack growth: PID=%u addr=%p", proc->pid, (void *)page);
  return 1;
}

// ---------------------------------------------------------------------------
// Demanda paging sobre una VMA existente
// ---------------------------------------------------------------------------
static int try_vma_demand(struct process *proc, vma_t *vma, uint64_t fault_addr,
                          uint64_t err_code) {
  // ¿Es un fallo por escritura en una VMA no escribible?
  int is_write = (err_code & 0x02) != 0;
  if (is_write && !(vma->flags & PTE_WRITABLE)) {
    return 0; // violación de permisos
  }

  // Asignar y mapear la página
  uint64_t page = fault_addr & ~0xFFFULL;
  uint64_t phys = pmm_alloc_page();
  if (!phys)
    return 0;

  // Para VMA_ANON y VMA_STACK, inicializar a cero
  if (vma->type == VMA_ANON || vma->type == VMA_STACK) {
    memset(phys_to_virt(phys), 0, PAGE_SIZE);
  }

  uint64_t map_flags = vma->flags | PTE_PRESENT;

  if (paging_map_page_in((uint64_t *)phys_to_virt(proc->pml4_phys), page, phys,
                         map_flags) != 0) {
    pmm_free_page(phys);
    return 0;
  }

  LOG_TRACE("[PF] Demand paging: PID=%u addr=%p type=%u", proc->pid,
            (void *)page, vma->type);
  return 1;
}

// ---------------------------------------------------------------------------
// Matar el proceso actual
// ---------------------------------------------------------------------------
extern void task_die_hlt(void);

void kill_current_process(registers_t *regs, const char *reason) {
  struct process *proc = process_current();
  task_t *task = sched_current();

  if (task) {
    LOG_ERR("[PF] Matando tarea ID=%u PID=%u: %s", task->id,
            proc ? proc->pid : 0, reason);

    // Marcar el proceso como terminado y despertar al padre
    if (proc) {
      process_exit(proc, -1);
    }

    task->state = TASK_DEAD;

    regs->rip = (uint64_t)task_die_hlt;
    regs->cs = KERNEL_CS;
    regs->ss = KERNEL_DS;
    regs->rsp = this_cpu(kernel_stack);
    regs->rflags &= ~0x200ULL;
  }
  pf_killed++;
}

// ---------------------------------------------------------------------------
// Handler principal
// ---------------------------------------------------------------------------
static int handle_page_fault_inner(registers_t *regs) {
  uint64_t cr2 = read_cr2();
  uint64_t err = regs->error_code;

  int present = err & 0x01;
  int user = err & 0x04;
  int fetch = err & 0x10;

  if (present) {
    if (user) {
      kill_current_process(regs, "protection violation");
      return 1;
    }
    return 0;
  }

  if (fetch) {
    if (user) {
      kill_current_process(regs, "instruction fetch at unmapped");
      return 1;
    }
    return 0;
  }

  if (user) {
    struct process *proc = process_current();
    if (!proc) {
      kill_current_process(regs, "no process context");
      return 1;
    }

    if (try_stack_growth(proc, cr2, err)) {
      pf_resolved++;
      return 1;
    }

    vma_t *vma = vma_find(proc, cr2);
    if (vma) {
      if (try_vma_demand(proc, vma, cr2, err)) {
        pf_resolved++;
        return 1;
      }
      kill_current_process(regs, "VMA permission violation");
      return 1;
    }

    if (cr2 >= proc->stack_guard && cr2 < proc->stack_guard + PAGE_SIZE) {
      kill_current_process(regs, "stack overflow (guard page)");
      return 1;
    }
    kill_current_process(regs, "address not in any VMA");
    return 1;
  }

  return 0;
}

extern void task_die_hlt(void);

int handle_page_fault(registers_t *regs) {
  // Ya no hace falta cambiar a kernel CR3: phys_to_virt funciona con
  // cualquier CR3. Todos los accesos a page tables en handle_page_fault_inner
  // usan phys_to_virt.
  return handle_page_fault_inner(regs);
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------
void pf_init(void) {
  pf_resolved = 0;
  pf_killed = 0;
  LOG_INFO("[PF] Demand paging inicializado");
}

// ---------------------------------------------------------------------------
// Syscalls: mmap / munmap
// ---------------------------------------------------------------------------
// Próxima dirección anónima por encima del heap.
static uint64_t next_mmap_addr = 0x0000000060000000ULL;

#define MMAP_PROT_READ 0x1
#define MMAP_PROT_WRITE 0x2
#define MMAP_PROT_EXEC 0x4

int64_t sys_mmap(struct process *proc, uint64_t addr, uint64_t length,
                 uint64_t prot, uint64_t flags, int fd, uint64_t offset) {
  (void)flags;
  (void)fd;
  (void)offset;

  if (!proc || length == 0)
    return -1;
  if (length > 0x10000000ULL)
    return -1; // 256 MB máximo por mmap

  // Alinear
  length = (length + 0xFFF) & ~0xFFFULL;

  // Elegir dirección
  if (addr == 0) {
    addr = next_mmap_addr;
    next_mmap_addr += length;
    if (next_mmap_addr > 0x00007F0000000000ULL) {
      return -1; // sin espacio
    }
  } else {
    addr &= ~0xFFFULL;
  }

  // Construir flags PTE
  uint64_t pte_flags = PTE_USER | PTE_PRESENT;
  if (prot & MMAP_PROT_WRITE)
    pte_flags |= PTE_WRITABLE;
  if (!(prot & MMAP_PROT_EXEC))
    pte_flags |= PTE_NX;

  // Crear VMA (lazy: no mapeamos páginas)
  vma_t *v = vma_create(proc, addr, addr + length, pte_flags, VMA_ANON);
  if (!v)
    return -1;

  LOG_TRACE("[MMAP] PID=%u addr=%p len=%lu prot=%lx", proc->pid, (void *)addr,
            (unsigned long)length, (unsigned long)prot);

  return (int64_t)addr;
}

int64_t sys_munmap(struct process *proc, uint64_t addr, uint64_t length) {
  if (!proc || length == 0)
    return -1;

  addr &= ~0xFFFULL;
  length = (length + 0xFFF) & ~0xFFFULL;

  uint64_t end = addr + length;

  // Buscar y eliminar VMA(s) que caigan en el rango
  vma_t **pp = &proc->vma_list;
  int unmapped = 0;

  while (*pp) {
    vma_t *v = *pp;
    if (v->end <= addr || v->start >= end) {
      pp = &v->next;
      continue;
    }

    // Caso simple: VMA completamente dentro del rango → eliminar
    if (v->start >= addr && v->end <= end) {
      // Liberar páginas mapeadas
      for (uint64_t p = v->start; p < v->end; p += PAGE_SIZE) {
        uint64_t phys =
            paging_get_phys_in((uint64_t *)phys_to_virt(proc->pml4_phys), p);
        if (phys) {
          paging_unmap_page_in((uint64_t *)phys_to_virt(proc->pml4_phys), p);
          pmm_free_page(phys);
        }
      }
      *pp = v->next;
      kfree(v);
      unmapped++;
      continue;
    }

    // Casos parciales: no soportados en Fase 1
    // (podríamos partir la VMA, pero lo dejamos simple)
    pp = &v->next;
  }

  LOG_TRACE("[MUNMAP] PID=%u addr=%p len=%lu -> %d VMAs", proc->pid,
            (void *)addr, (unsigned long)length, unmapped);
  return unmapped > 0 ? 0 : -1;
}