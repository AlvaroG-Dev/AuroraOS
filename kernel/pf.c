// kernel/pf.c
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
#include "uaccess.h"
#include <stddef.h>

// ---------------------------------------------------------------------------
// Estadísticas
// ---------------------------------------------------------------------------
static volatile uint64_t pf_resolved = 0;
static volatile uint64_t pf_killed = 0;
static uint64_t next_mmap_addr = 0x0000000060000000ULL;
static spinlock_t mmap_addr_lock;

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
  // VMAs are user-space objects.  Never allow an end address above
  // USER_LIMIT: the upper half of the address space is shared by the kernel.
  if (!proc || start >= end || start >= USER_LIMIT || end > USER_LIMIT)
    return NULL;

  // VMAs must never overlap.  vma_find() returns the first matching
  // VMA, so allowing an overlap could hide an existing ELF/stack/heap
  // region and make page-fault handling use the wrong permissions/type.
  uint64_t vma_start = start & ~0xFFFULL;
  uint64_t vma_end = (end + 0xFFFULL) & ~0xFFFULL;
  if (vma_end <= vma_start)
    return NULL;

  for (vma_t *curr = proc->vma_list; curr; curr = curr->next) {
    if (vma_start < curr->end && vma_end > curr->start)
      return NULL;
  }

  vma_t *v = (vma_t *)kmalloc(sizeof(vma_t));
  if (!v)
    return NULL;
  v->start = vma_start;
  v->end = vma_end;
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
// [Fase C.2] Helper para asignar una página de usuario y ponerla a cero.
// Se usa en try_stack_growth, try_vma_demand, process_sbrk, process_spawn.
// ---------------------------------------------------------------------------
static uint64_t alloc_user_page_zeroed(void) {
  uint64_t phys = pmm_alloc_page();
  if (!phys)
    return 0;
  memset(phys_to_virt(phys), 0, PAGE_SIZE);
  return phys;
}

// ---------------------------------------------------------------------------
// Stack growth
// ---------------------------------------------------------------------------
static int try_stack_growth(struct process *proc, uint64_t fault_addr,
                            uint64_t flags) {
  if (!proc || !proc->stack_low)
    return 0;
  if (fault_addr >= proc->stack_base)
    return 0;
  if (fault_addr < proc->stack_low)
    return 0;

  uint64_t page = fault_addr & ~0xFFFULL;
  uint64_t phys = alloc_user_page_zeroed(); // ← [C.2] zeroed
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
  int is_write = (err_code & 0x02) != 0;
  if (is_write && !(vma->flags & PTE_WRITABLE)) {
    return 0;
  }

  uint64_t page = fault_addr & ~0xFFFULL;
  uint64_t phys = alloc_user_page_zeroed(); // ← [C.2] zeroed siempre
  if (!phys)
    return 0;

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
// Resolver un fallo como demand paging (para el path de kernel).
// Devuelve 1 si resolvió, 0 si no.
// ---------------------------------------------------------------------------
static int pf_try_demand_paging(struct process *proc, uint64_t cr2,
                                uint64_t err) {
  if (!proc)
    return 0;

  if (try_stack_growth(proc, cr2, err))
    return 1;

  vma_t *vma = vma_find(proc, cr2);
  if (vma && try_vma_demand(proc, vma, cr2, err))
    return 1;

  return 0;
}

// ---------------------------------------------------------------------------
// Matar el proceso actual
// ---------------------------------------------------------------------------
void kill_current_process(registers_t *regs, const char *reason) {
  task_t *task = sched_current();
  process_t *proc = process_current();

  if (task) {
    LOG_ERR("[PF] Matando tarea ID=%u PID=%u: %s", task->id,
            proc ? proc->pid : 0, reason);
  }
  pf_killed++;

  (void)regs;
  process_exit_current(-1);
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

  // -------------------------------------------------------------------------
  // 1. Fallo en modo usuario: path clásico.
  // -------------------------------------------------------------------------
  if (user) {
    if (present) {
      kill_current_process(regs, "protection violation");
      return 1;
    }
    if (fetch) {
      kill_current_process(regs, "instruction fetch at unmapped");
      return 1;
    }

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
    LOG_ERR("[PF] PID=%u addr=%p rip=%p cr2=%p - not in any VMA",
            proc ? proc->pid : 0, (void *)cr2, (void *)regs->rip, (void *)cr2);
    kill_current_process(regs, "address not in any VMA");
    return 1;
  }

  // -------------------------------------------------------------------------
  // 2. [Fase C.2] Fallo en modo kernel.
  //
  //    Caso típico: uaccess intentando leer/escribir userland que no
  //    está mapeado todavía (por ejemplo, un mmap perezoso que el
  //    usuario no ha tocado, o un buffer de stack que aún no se ha
  //    expandido).
  //
  //    Estrategia:
  //      a) Si cr2 >= USER_LIMIT: es un bug del kernel. Panic.
  //      b) Si cr2 < USER_LIMIT:
  //         b.1) Intentar demand paging del proceso actual.
  //         b.2) Si no, buscar el rip en la tabla de uaccess fixups.
  //              Si está, saltar al fixup (que devuelve -EFAULT).
  //         b.3) Si no, es un bug del kernel. Panic.
  // -------------------------------------------------------------------------
  if (cr2 >= USER_LIMIT) {
    // Acceso del kernel a una dirección no canónica o reservada.
    return 0; // panic
  }

  // Fallo en kernel con cr2 en el rango de userland.
  struct process *proc = process_current();
  if (pf_try_demand_paging(proc, cr2, err)) {
    pf_resolved++;
    return 1;
  }

  // No se pudo resolver como demand paging. ¿Es un acceso recuperable
  // desde la tabla de fixups?
  uint64_t fixup = uaccess_lookup_fixup(regs->rip);
  if (fixup) {
    LOG_WARN("[PF] uaccess fixup: pid=%u rip=%p cr2=%p -> fixup=%p",
             proc ? proc->pid : 0, (void *)regs->rip, (void *)cr2,
             (void *)fixup);
    regs->rip = fixup;
    pf_resolved++;
    return 1;
  }

  // Fallo real en el kernel.
  return 0;
}

int handle_page_fault(registers_t *regs) {
  return handle_page_fault_inner(regs);
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------
void pf_init(void) {
  pf_resolved = 0;
  pf_killed = 0;
  spin_init(&mmap_addr_lock);
  LOG_INFO("[PF] Demand paging inicializado");
}

// ---------------------------------------------------------------------------
// Syscalls: mmap / munmap
// ---------------------------------------------------------------------------
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
    return -1;

  // Round up safely; reject lengths whose page rounding would overflow.
  if (length > UINT64_MAX - 0xFFFULL)
    return -1;
  length = (length + 0xFFF) & ~0xFFFULL;

  if (addr == 0) {
    // Automatic mmap addresses come from a global allocator. On SMP, two
    // CPUs can enter sys_mmap() concurrently, so the read/update of
    // next_mmap_addr must be atomic with respect to other callers.
    unsigned long lock_flags = spin_lock_irqsave(&mmap_addr_lock);
    addr = next_mmap_addr;
    uint64_t next = addr + length;
    if (next < addr || next > 0x00007F0000000000ULL) {
      spin_unlock_irqrestore(&mmap_addr_lock, lock_flags);
      return -1;
    }
    next_mmap_addr = next;
    spin_unlock_irqrestore(&mmap_addr_lock, lock_flags);
  } else {
    addr &= ~0xFFFULL;
  }

  // A fixed mapping must stay entirely below USER_LIMIT.  This is also
  // checked by vma_create(), but keep the syscall boundary explicit.
  if (addr >= USER_LIMIT || length > USER_LIMIT - addr)
    return -1;

  uint64_t pte_flags = PTE_USER | PTE_PRESENT;
  if (prot & MMAP_PROT_WRITE)
    pte_flags |= PTE_WRITABLE;
  if (!(prot & MMAP_PROT_EXEC))
    pte_flags |= PTE_NX;

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

  vma_t **pp = &proc->vma_list;
  int unmapped = 0;

  while (*pp) {
    vma_t *v = *pp;
    if (v->end <= addr || v->start >= end) {
      pp = &v->next;
      continue;
    }

    if (v->start >= addr && v->end <= end) {
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

    pp = &v->next;
  }

  LOG_TRACE("[MUNMAP] PID=%u addr=%p len=%lu -> %d VMAs", proc->pid,
            (void *)addr, (unsigned long)length, unmapped);
  return unmapped > 0 ? 0 : -1;
}