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
#include "signal.h"
#include "string.h"
#include "uaccess.h"
#include <stddef.h>

// ---------------------------------------------------------------------------
// Estadísticas
// ---------------------------------------------------------------------------
static volatile uint64_t pf_resolved = 0;
static volatile uint64_t pf_killed = 0;

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
  if (is_write && !(vma->flags & PTE_WRITABLE)) {
    return 0;
  }

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
      // [4.3] Violación de permisos (escribir a R, leer a PROT_NONE, ...).
      // Si el proceso tiene handler, se entrega SIGSEGV. Si no, mata.
      signal_deliver_from_exception(SIGSEGV, regs);
      pf_resolved++;
      return 1;
    }
    if (fetch) {
      signal_deliver_from_exception(SIGSEGV, regs);
      pf_resolved++;
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
  // 2. Fallo en modo kernel.
  // -------------------------------------------------------------------------
  if (cr2 >= USER_LIMIT) {
    return 0;
  }

  struct process *proc = process_current();

  // Solo demand-page si la página NO está presente. Un fallo sobre una
  // página presente-pero-RO (bit 0 = 1) NO se resuelve remapeando.
  if ((err & 0x01) == 0 && pf_try_demand_paging(proc, cr2, err)) {
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

  // Fallo de kernel sin fixup y con la página presente. Dos casos:
  //   a) SMAP bloqueó un acceso a userland sin stac() (bug en un path
  //      que NO es uaccess; p.ej. paging_map_page_in escribiendo la
  //      VA del usuario en vez de phys_to_virt(phys)).
  //   b) La instrucción está dentro de uaccess pero no en la tabla
  //      de fixups (bug en uaccess_faults.asm: falta registrar esa IP).
  //
  // En cualquiera de los dos, es un bug del kernel que NO debe tirar
  // todo el sistema. Matamos el proceso actual y seguimos.
  if ((err & 0x01) != 0 && proc) {
    LOG_ERR("[PF] kernel access to user page without fixup: "
            "pid=%u rip=%p cr2=%p err=%lx - matando proceso",
            proc->pid, (void *)regs->rip, (void *)cr2, err);
    kill_current_process(regs, "kernel access to user page (no fixup)");
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
  LOG_INFO("[PF] Demand paging inicializado");
}

// ---------------------------------------------------------------------------
// Syscalls: mmap / munmap
// ---------------------------------------------------------------------------
#define MMAP_PROT_READ 0x1
#define MMAP_PROT_WRITE 0x2
#define MMAP_PROT_EXEC 0x4

#define MMAP_MAP_SHARED 0x01
#define MMAP_MAP_PRIVATE 0x02
#define MMAP_MAP_FIXED 0x10
#define MMAP_MAP_ANONYMOUS 0x20

// ---------------------------------------------------------------------------
// vma_unmap_range — primitiva común de munmap y mmap(MAP_FIXED).
//
// Libera todos los VMAs (y sus páginas físicas) que se solapen con
// [start, end). start y end deben estar alineados a página.
//
// Devuelve:
//   >= 0  número de VMAs tocados (0 si el rango estaba libre, que NO
//         es error: MAP_FIXED sobre hueco es lo normal).
//   < 0   -ENOMEM si falla el split de un VMA (nada se modifica).
// ---------------------------------------------------------------------------
int64_t vma_unmap_range(struct process *proc, uint64_t start, uint64_t end) {
  if (!proc || start >= end)
    return -EINVAL;
  if (start >= USER_LIMIT || end > USER_LIMIT)
    return -EINVAL;

  // Preasignar los VMAs "right" que puedan hacer falta para splits.
  // Si un split falla, abortamos SIN tocar nada.
  vma_t *new_vmas = NULL;
  vma_t **new_vmas_tail = &new_vmas;
  for (vma_t *v = proc->vma_list; v; v = v->next) {
    if (v->end <= start || v->start >= end)
      continue;
    if (start > v->start && end < v->end) {
      vma_t *right = (vma_t *)kmalloc(sizeof(vma_t));
      if (!right) {
        while (new_vmas) {
          vma_t *next = new_vmas->next;
          kfree(new_vmas);
          new_vmas = next;
        }
        return -ENOMEM;
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

  int touched = 0;
  vma_t **pp = &proc->vma_list;
  while (*pp) {
    vma_t *v = *pp;
    if (v->end <= start || v->start >= end) {
      pp = &v->next;
      continue;
    }

    uint64_t unmap_start = start > v->start ? start : v->start;
    uint64_t unmap_end = end < v->end ? end : v->end;

    for (uint64_t p = unmap_start; p < unmap_end; p += PAGE_SIZE) {
      uint64_t phys =
          paging_get_phys_in((uint64_t *)phys_to_virt(proc->pml4_phys), p);
      if (phys) {
        paging_unmap_page_in((uint64_t *)phys_to_virt(proc->pml4_phys), p);
        pmm_free_page(phys);
      }
    }

    if (start <= v->start && end >= v->end) {
      // VMA completamente cubierto: eliminar.
      *pp = v->next;
      kfree(v);
      touched++;
      continue;
    }
    if (start <= v->start) {
      // Recortar por la izquierda: queda [end, v->end).
      v->start = end;
      touched++;
      pp = &v->next;
      continue;
    }
    if (end >= v->end) {
      // Recortar por la derecha: queda [v->start, start).
      v->end = start;
      touched++;
      pp = &v->next;
      continue;
    }
    // Split por el medio: queda [v->start, start) + [end, v->end).
    vma_t *right = new_vmas;
    new_vmas = new_vmas->next;
    right->next = v->next;
    v->end = start;
    v->next = right;
    touched++;
    pp = &right->next;
  }

  while (new_vmas) {
    vma_t *next = new_vmas->next;
    kfree(new_vmas);
    new_vmas = next;
  }
  return touched;
}

// ---------------------------------------------------------------------------
// munmap(addr, length)
//
// Linux devuelve 0 siempre que la operación sea válida, incluso si el
// rango no tenía nada mapeado. La versión anterior devolvía -1 en ese
// caso: incorrecto y rompía el patrón "reserva con mmap, libera
// parcialmente" que usa musl al final de algunas rutas.
// ---------------------------------------------------------------------------
int64_t sys_munmap(struct process *proc, uint64_t addr, uint64_t length) {
  if (!proc || length == 0)
    return -EINVAL;
  if (length > UINT64_MAX - 0xFFFULL)
    return -EINVAL;

  addr &= ~0xFFFULL;
  length = (length + 0xFFFULL) & ~0xFFFULL;
  if (addr >= USER_LIMIT || length > USER_LIMIT - addr)
    return -EINVAL;

  int64_t r = vma_unmap_range(proc, addr, addr + length);
  if (r < 0)
    return r;
  return 0;
}

// ---------------------------------------------------------------------------
// [4.3] mprotect.
//
// 1. Alinea addr/len.
// 2. Traduce PROT_* a flags PTE.
// 3. Recorre los VMAs que solapan [addr, end):
//      - completamente dentro → cambia flags
//      - solapa izquierda (v->start < addr < v->end ≤ end) → split,
//        derecha [addr, v->end) con nuevos flags
//      - solapa derecha (v->start ≥ addr, v->end > end) → split,
//        izquierda [v->start, end) con nuevos flags
//      - cubre rango entero → split en 3
// 4. Para cada página presente en [addr, end), actualiza la PTE.
// 5. TLB shootdown de cada página tocada.
//
// No crea páginas nuevas. Las páginas no presentes se demand-page-arán
// con los flags del VMA cuando se toquen.
// ---------------------------------------------------------------------------
int64_t sys_mprotect(struct process *proc, uint64_t addr, uint64_t length,
                     uint64_t prot) {
  if (!proc)
    return -EINVAL;
  if (length == 0)
    return 0;
  if ((addr & 0xFFFULL) != 0)
    return -EINVAL;
  if (length > UINT64_MAX - 0xFFFULL)
    return -EINVAL;
  uint64_t len = (length + 0xFFFULL) & ~0xFFFULL;
  uint64_t end = addr + len;
  if (end < addr || end > USER_LIMIT)
    return -ENOMEM;

  // [4.3] En x86 no hay bit "no read": PROT_NONE se representa
  // quitando PTE_USER. Userland accede → #PF(protection violation)
  // → SIGSEGV.
  uint64_t new_flags = PTE_PRESENT;
  if (prot & (MMAP_PROT_READ | MMAP_PROT_WRITE | MMAP_PROT_EXEC))
    new_flags |= PTE_USER;
  if (prot & MMAP_PROT_WRITE)
    new_flags |= PTE_WRITABLE;
  if (!(prot & MMAP_PROT_EXEC))
    new_flags |= PTE_NX;

  // Preasignar hasta 2 VMAs nuevos por cada VMA que solape.
  int need = 0;
  for (vma_t *v = proc->vma_list; v; v = v->next) {
    if (v->end <= addr || v->start >= end)
      continue;
    if (addr > v->start && end < v->end)
      need++; // cubre rango entero: 2 splits
    else if (addr > v->start || end < v->end)
      need++; // un solo split
  }

  vma_t *pool = NULL;
  for (int i = 0; i < need; i++) {
    vma_t *p = (vma_t *)kmalloc(sizeof(vma_t));
    if (!p) {
      while (pool) {
        vma_t *n = pool->next;
        kfree(pool);
        pool = n;
      }
      return -ENOMEM;
    }
    p->next = pool;
    pool = p;
  }

  vma_t **pp = &proc->vma_list;
  while (*pp) {
    vma_t *v = *pp;
    if (v->end <= addr || v->start >= end) {
      pp = &v->next;
      continue;
    }

    int left_cov = (v->start >= addr); // v->start dentro del rango
    int right_cov = (v->end <= end);   // v->end dentro del rango

    if (left_cov && right_cov) {
      // VMA completamente dentro: solo flags.
      v->flags = new_flags;
      pp = &v->next;
    } else if (left_cov) {
      // Se extiende hacia la derecha. Split: [v->start, end) nuevos,
      // [end, v->end) viejos.
      vma_t *right = pool;
      pool = pool->next;
      right->start = end;
      right->end = v->end;
      right->flags = v->flags;
      right->type = v->type;
      right->pad = 0;
      right->next = v->next;

      v->end = end;
      v->flags = new_flags;
      v->next = right;
      pp = &right->next;
    } else if (right_cov) {
      // Se extiende hacia la izquierda. Split: [v->start, addr) viejos,
      // [addr, v->end) nuevos.
      vma_t *mid = pool;
      pool = pool->next;
      mid->start = addr;
      mid->end = v->end;
      mid->flags = new_flags;
      mid->type = v->type;
      mid->pad = 0;
      mid->next = v->next;

      v->end = addr;
      v->next = mid;
      pp = &mid->next;
    } else {
      // Cubre el rango entero: split en 3.
      vma_t *mid = pool;
      pool = pool->next;
      vma_t *right = pool;
      pool = pool->next;

      mid->start = addr;
      mid->end = end;
      mid->flags = new_flags;
      mid->type = v->type;
      mid->pad = 0;
      mid->next = right;

      right->start = end;
      right->end = v->end;
      right->flags = v->flags;
      right->type = v->type;
      right->pad = 0;
      right->next = v->next;

      v->end = addr;
      v->next = mid;
      pp = &right->next;
    }
  }

  // Devolver pool sobrante.
  while (pool) {
    vma_t *n = pool->next;
    kfree(pool);
    pool = n;
  }

  // Actualizar PTEs de las páginas presentes.
  uint64_t *pml4 = (uint64_t *)phys_to_virt(proc->pml4_phys);
  for (uint64_t p = addr; p < end; p += PAGE_SIZE) {
    uint64_t phys = paging_get_phys_in(pml4, p);
    if (phys) {
      paging_map_page_in(pml4, p, phys & PTE_FRAME, new_flags);
      paging_invalidate_tlb_global(p);
    }
  }

  LOG_TRACE("[MPROTECT] PID=%u [%p, %p) prot=%lx", proc->pid, (void *)addr,
            (void *)end, (unsigned long)prot);
  return 0;
}

// ---------------------------------------------------------------------------
// mmap(addr, length, prot, flags, fd, offset)
//
// Soporta únicamente mapeos anónimos. File-backed devolverá -ENODEV
// hasta que exista VMA_FILE y el demand pager sepa leer del inodo.
//
// Diferencias clave entre las dos formas:
//
//   MAP_FIXED:  addr y length deben estar alineados a página.
//               El rango [addr, addr+length) se usa EXACTAMENTE.
//               Si había algo, se descarta (como un munmap previo).
//
//   Sin MAP_FIXED y con addr != 0: addr es un HINT. Si el rango está
//               libre, se usa. Si no, se elige otra dirección desde
//               proc->next_mmap_addr.
//
//   Sin MAP_FIXED y addr == 0: el kernel elige.
// ---------------------------------------------------------------------------
int64_t sys_mmap(struct process *proc, uint64_t addr, uint64_t length,
                 uint64_t prot, uint64_t flags, int fd, uint64_t offset) {
  (void)fd;
  (void)offset;

  if (!proc || length == 0)
    return -EINVAL;

  // Por ahora solo mapeos anónimos. File-backed requiere VMA_FILE.
  if (!(flags & MMAP_MAP_ANONYMOUS))
    return -ENODEV;

  // Rango alineado y con overflow comprobado.
  if (length > UINT64_MAX - 0xFFFULL)
    return -EINVAL;
  uint64_t len = (length + 0xFFFULL) & ~0xFFFULL;
  if (len == 0)
    return -EINVAL;

  // Protecciones → PTE.
  uint64_t pte_flags = PTE_USER | PTE_PRESENT;
  if (prot & MMAP_PROT_WRITE)
    pte_flags |= PTE_WRITABLE;
  if (!(prot & MMAP_PROT_EXEC))
    pte_flags |= PTE_NX;

  uint64_t base;

  if (flags & MMAP_MAP_FIXED) {
    // Alineación obligatoria, como en Linux.
    if ((addr & 0xFFFULL) != 0 || (length & 0xFFFULL) != 0)
      return -EINVAL;
    if (addr >= USER_LIMIT || len > USER_LIMIT - addr)
      return -EINVAL;
    base = addr;

    // MAP_FIXED: descartar lo que hubiera.
    int64_t ur = vma_unmap_range(proc, base, base + len);
    if (ur < 0)
      return ur;
  } else {
    // addr != 0 es un hint; addr == 0 deja al kernel elegir.
    base = 0;
    if (addr != 0) {
      uint64_t hint = addr & ~0xFFFULL;
      if (hint < USER_LIMIT && len <= USER_LIMIT - hint) {
        uint64_t hint_end = hint + len;
        vma_t *v = proc->vma_list;
        int collision = 0;
        while (v) {
          if (hint < v->end && hint_end > v->start) {
            collision = 1;
            break;
          }
          v = v->next;
        }
        if (!collision)
          base = hint;
      }
    }

    if (base == 0) {
      uint64_t next = proc->next_mmap_addr;
      if (next < 0x0000000040000000ULL)
        next = 0x0000000060000000ULL;
      uint64_t candidate = (next + 0xFFFULL) & ~0xFFFULL;
      uint64_t end = candidate + len;
      if (end < candidate || end > 0x00007F0000000000ULL)
        return -ENOMEM;
      base = candidate;
      proc->next_mmap_addr = end;
    }
  }

  vma_t *v = vma_create(proc, base, base + len, pte_flags, VMA_ANON);
  if (!v)
    return -ENOMEM;

  LOG_TRACE("[MMAP] PID=%u base=%p len=%lu prot=%lx flags=%lx", proc->pid,
            (void *)base, (unsigned long)len, (unsigned long)prot,
            (unsigned long)flags);

  return (int64_t)base;
}