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
  if (flags & 0x10) // instruction fetch: the user stack is NX
    return 0;

  uint64_t page = fault_addr & ~0xFFFULL;
  uint64_t phys = alloc_user_page_zeroed();
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
  int is_fetch = (err_code & 0x10) != 0;
  if (is_write && !(vma->flags & PTE_WRITABLE))
    return 0;
  if (is_fetch && (vma->flags & PTE_NX))
    return 0;

  uint64_t page = fault_addr & ~0xFFFULL;
  uint64_t phys = alloc_user_page_zeroed();

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
  // -------------------------------------------------------------------------
  if (cr2 >= USER_LIMIT) {
    return 0;
  }

  struct process *proc = process_current();
  if (pf_try_demand_paging(proc, cr2, err)) {
    pf_resolved++;
    return 1;
  }

  uint64_t fixup = uaccess_lookup_fixup(regs->rip);
  if (fixup) {
    LOG_WARN("[PF] uaccess fixup: pid=%u rip=%p cr2=%p -> fixup=%p",
             proc ? proc->pid : 0, (void *)regs->rip, (void *)cr2,
             (void *)fixup);
    regs->rip = fixup;
    pf_resolved++;
    return 1;
  }

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

  // Align the requested interval to pages, but do all arithmetic with
  // overflow checks.  munmap operates on the half-open interval [addr, end).
  if (length > UINT64_MAX - 0xFFFULL)
    return -1;
  addr &= ~0xFFFULL;
  length = (length + 0xFFFULL) & ~0xFFFULL;
  if (addr >= USER_LIMIT || length > USER_LIMIT - addr)
    return -1;

  uint64_t end = addr + length;

  // A partial unmap can split a VMA into two independent VMAs.  Allocate all
  // required right-hand VMA nodes first so an allocation failure leaves the
  // VMA list and mappings untouched.
  vma_t *new_vmas = NULL;
  vma_t **new_vmas_tail = &new_vmas;
  for (vma_t *v = proc->vma_list; v; v = v->next) {
    if (v->end <= addr || v->start >= end)
      continue;
    if (addr > v->start && end < v->end) {
      vma_t *right = (vma_t *)kmalloc(sizeof(vma_t));
      if (!right) {
        while (new_vmas) {
          vma_t *next = new_vmas->next;
          kfree(new_vmas);
          new_vmas = next;
        }
        return -1;
      }
      right->start = end;
      right->end = v->end;
      right->flags = v->flags;
      right->type = v->type;
      right->pad = 0;
      right->next = NULL;
      *new_vmas_tail = right;
      new_vmas_tail = &right->next;
    }
  }

  int unmapped = 0;

  vma_t **pp = &proc->vma_list;
  while (*pp) {
    vma_t *v = *pp;
    if (v->end <= addr || v->start >= end) {
      pp = &v->next;
      continue;
    }

    uint64_t unmap_start = addr > v->start ? addr : v->start;
    uint64_t unmap_end = end < v->end ? end : v->end;

    // Free every page covered by the requested interval.  VMA boundaries
    // are page aligned, so this covers exactly the removed portion.
    for (uint64_t p = unmap_start; p < unmap_end; p += PAGE_SIZE) {
      uint64_t phys =
          paging_get_phys_in((uint64_t *)phys_to_virt(proc->pml4_phys), p);
      if (phys) {
        paging_unmap_page_in((uint64_t *)phys_to_virt(proc->pml4_phys), p);
        pmm_free_page(phys);
      }
    }

    if (addr <= v->start && end >= v->end) {
      // Entire VMA removed.
      *pp = v->next;
      kfree(v);
      unmapped++;
      continue;
    }

    if (addr <= v->start) {
      // Trim the left edge: keep [end, old_end).
      v->start = end;
      unmapped++;
      pp = &v->next;
      continue;
    }

    if (end >= v->end) {
      // Trim the right edge: keep [old_start, addr).
      v->end = addr;
      unmapped++;
      pp = &v->next;
      continue;
    }

    // Middle split: keep the original VMA as the left part and insert the
    // preallocated right part immediately after it.
    vma_t *right = new_vmas;
    new_vmas = new_vmas->next;
    right->next = v->next;
    v->end = addr;
    v->next = right;
    unmapped++;
    pp = &right->next;
  }

  while (new_vmas) {
    vma_t *next = new_vmas->next;
    kfree(new_vmas);
    new_vmas = next;
  }

  LOG_TRACE("[MUNMAP] PID=%u addr=%p len=%lu -> %d VMAs", proc->pid,
            (void *)addr, (unsigned long)length, unmapped);
  return unmapped > 0 ? 0 : -1;
}
