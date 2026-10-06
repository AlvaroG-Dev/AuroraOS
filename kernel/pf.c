// kernel/pf.c
#include "pf.h"
#include "cpu.h"
#include "gdt.h"
#include "heap.h"
#include "klog.h"
#include "mutex.h"
#include "paging.h"
#include "pmm.h"
#include "process.h"
#include "sched.h"
#include "serial.h"
#include "signal.h"
#include "string.h"
#include "swap.h"
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
// [3.5] vma_release_fd: libera la referencia al file_descriptor_t de un
// VMA_FILE. Si ref_count llega a 0, libera el nodo y el fd (igual que
// vfs_close_for_proc pero sin tocar la tabla del proceso).
// ---------------------------------------------------------------------------
static void vma_release_fd(vma_t *v) {
  if (!v || !v->file_fd)
    return;
  file_descriptor_t *f = v->file_fd;
  v->file_fd = NULL;

  int left = __atomic_sub_fetch(&f->ref_count, 1, __ATOMIC_ACQ_REL);
  if (left == 0) {
    if (f->node)
      vfs_node_free(f->node);
    kfree(f);
  } else if (left < 0) {
    LOG_ERR("[PF] ref_count underflow fd=%p (%d)", (void *)f, left);
  }
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

  vma_t *v = (vma_t *)kzalloc(sizeof(vma_t));
  if (!v)
    return NULL;
  v->start = vma_start;
  v->end = vma_end;
  v->flags = flags;
  v->type = type;
  v->pad = 0;

  // Insertar ordenado por dirección ascendente.
  vma_t **pp = &proc->vma_list;
  while (*pp && (*pp)->start < v->start)
    pp = &(*pp)->next;
  v->next = *pp;
  *pp = v;
  return v;
}

// ---------------------------------------------------------------------------
// [3.5] vma_create_file: como vma_create pero para VMA_FILE. Toma una
// referencia al file_descriptor_t (que sobrevive al close del usuario).
// ---------------------------------------------------------------------------
vma_t *vma_create_file(struct process *proc, uint64_t start, uint64_t end,
                       uint64_t flags, struct file_descriptor *fd,
                       uint64_t file_offset) {
  vma_t *v = vma_create(proc, start, end, flags, VMA_FILE);
  if (!v)
    return NULL;
  v->file_fd = fd;
  v->file_offset = file_offset;
  if (fd)
    __atomic_fetch_add(&fd->ref_count, 1, __ATOMIC_ACQ_REL);
  return v;
}

void vma_destroy_all(struct process *proc) {
  if (!proc)
    return;
  vma_t *v = proc->vma_list;
  while (v) {
    vma_t *next = v->next;
    vma_release_fd(v);
    kfree(v);
    v = next;
  }
  proc->vma_list = NULL;
}

// ---------------------------------------------------------------------------
// [Fase C.2] Helper para asignar una página de usuario y ponerla a cero.
// Se usa en try_stack_growth, try_vma_demand, process_sbrk, process_spawn.
// ---------------------------------------------------------------------------
// [FIX] Sin reclaim inline. Antes, si el PMM estaba lleno, hacíamos
// swap_reclaim_one() para liberar 1 página. Bajo presión sostenida
// (memhog pidiendo más que RAM+swap), esto livelockeaba: cada PF
// liberaba 1 página haciendo ~50 ms de I/O, la mapeaba, el proceso
// tocaba la siguiente y volvía a empezar. Efectivo a 80 KB/s.
//
// Ahora el PF falla rápido. kswapd (que corre en su propio contexto
// y puede dormir) hace todo el reclaim en background. Si aún así no
// hay páginas, el proceso muere por OOM — es lo correcto cuando la
// demanda excede la memoria disponible.
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

  // [3.5] VMA_FILE: leer la página del fichero.
  if (vma->type == VMA_FILE && vma->file_fd) {
    file_descriptor_t *f = vma->file_fd;
    if (!f->node || !f->node->ops || !f->node->ops->read) {
      pmm_free_page(phys);
      return 0;
    }
    // file_offset ya está alineado a página (sys_mmap lo valida).
    // page >= vma->start, así que file_off >= file_offset.
    uint64_t file_off = vma->file_offset + (page - vma->start);
    int64_t r =
        f->node->ops->read(f->node, file_off, PAGE_SIZE, phys_to_virt(phys));
    if (r < 0) {
      pmm_free_page(phys);
      return 0;
    }
    // Si r < PAGE_SIZE, el resto queda a 0 (alloc_user_page_zeroed).
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
    // Log SIEMPRE para diagnóstico. Muestra present/write/fetch.
    LOG_DEBUG("[PF-USER] pid=%u rip=%p cr2=%p err=0x%lx (P=%d W=%d F=%d)",
              process_current() ? process_current()->pid : 0, (void *)regs->rip,
              (void *)cr2, err, (int)(err & 1), (int)((err >> 1) & 1),
              (int)((err >> 4) & 1));

    if (present) {
      // Solo aquí importa el bit "fetch": página presente + fetch = NX
      // violation.
      LOG_DEBUG("[PF-USER] pid=%u rip=%p cr2=%p err=0x%lx",
                process_current() ? process_current()->pid : 0,
                (void *)regs->rip, (void *)cr2, err);
      // [SWAP] ¿Violación de escritura sobre una página RW que kswapd
      // marcó temporalmente RO para reclaimarla?
      if ((err & 0x02) != 0) {
        struct process *proc = process_current();
        if (proc) {
          vma_t *vma = vma_find(proc, cr2);
          if (vma && (vma->flags & PTE_WRITABLE)) {
            uint64_t *pml4 = (uint64_t *)phys_to_virt(proc->pml4_phys);
            uint64_t page_va = cr2 & ~0xFFFULL;
            uint64_t pte;
            if (paging_get_pte_in(pml4, page_va, &pte) && (pte & PTE_PRESENT) &&
                !(pte & PTE_WRITABLE)) {
              uint64_t new_pte = (pte & PTE_FRAME) | vma->flags | PTE_PRESENT;
              paging_set_pte_in(pml4, page_va, new_pte);
              pf_resolved++;
              return 1;
            }
          }
        }
      }
      // Página presente + cualquier fallo = violación de permisos
      // (incluye NX: instruction fetch sobre página sin PTE_USER_X).
      signal_deliver_from_exception(SIGSEGV, regs);
      pf_resolved++;
      return 1;
    }

    // -----------------------------------------------------------------------
    // Página NO presente. Tanto un fetch como un data fault aquí son
    // demand-paging normal. El bit "fetch" SOLO importa si la página
    // está presente (NX). Hasta ahora este path no se alcanzaba para
    // fetches porque el `if (fetch)` de arriba enviaba SIGSEGV antes.
    // -----------------------------------------------------------------------

    struct process *proc = process_current();
    if (!proc) {
      kill_current_process(regs, "no process context");
      return 1;
    }

    // -----------------------------------------------------------------------
    // [SWAP] ¿La PTE es una swap entry? Si sí, hacer page-in.
    //
    // Se comprueba ANTES de try_stack_growth / try_vma_demand porque:
    //   - La VMA sigue existiendo (solo el PTE fue reemplazado).
    //   - try_vma_demand asignaría una página nueva cero y perdería los
    //     datos que están en swap.
    // -----------------------------------------------------------------------
    {
      uint64_t page_va = cr2 & ~0xFFFULL;
      mutex_lock(&proc->mm_lock);
      uint64_t *pml4 = (uint64_t *)phys_to_virt(proc->pml4_phys);
      uint64_t raw_pte;

      if (paging_get_pte_in(pml4, page_va, &raw_pte) && pte_is_swap(raw_pte)) {
        uint32_t sw_type = pte_swap_type(raw_pte);
        uint32_t sw_slot = pte_swap_offset(raw_pte);

        uint64_t new_phys = pmm_alloc_page();
        if (!new_phys) {
          mutex_unlock(&proc->mm_lock);
          kill_current_process(regs, "swap-in: sin memoria física");
          return 1;
        }

        if (swap_read_page(sw_type, sw_slot, new_phys) != 0) {
          pmm_free_page(new_phys);
          mutex_unlock(&proc->mm_lock);
          kill_current_process(regs, "swap-in: error de E/S");
          return 1;
        }

        vma_t *vma = vma_find(proc, page_va);
        if (!vma) {
          pmm_free_page(new_phys);
          mutex_unlock(&proc->mm_lock);
          kill_current_process(regs, "swap-in: VMA desaparecida");
          return 1;
        }
        uint64_t map_flags = vma->flags | PTE_PRESENT;

        if (paging_map_page_in(pml4, page_va, new_phys, map_flags) != 0) {
          pmm_free_page(new_phys);
          mutex_unlock(&proc->mm_lock);
          kill_current_process(regs, "swap-in: fallo al mapear");
          return 1;
        }

        swap_free_slot(sw_type, sw_slot);
        mutex_unlock(&proc->mm_lock);
        pf_resolved++;
        return 1;
      }
      mutex_unlock(&proc->mm_lock);
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
      // Distinguir OOM de permiso: si el PMM está a 0, es OOM.
      if (pmm_free_pages_count() < 64) {
        LOG_ERR("[PF] PID=%u addr=%p: OOM (sin páginas libres ni swap)",
                proc->pid, (void *)cr2);
        kill_current_process(regs, "out of memory");
      } else {
        kill_current_process(regs, "VMA permission violation");
      }
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
  vma_t *new_vmas = NULL;
  vma_t **new_vmas_tail = &new_vmas;
  for (vma_t *v = proc->vma_list; v; v = v->next) {
    if (v->end <= start || v->start >= end)
      continue;
    if (start > v->start && end < v->end) {
      // [3.5] kzalloc en vez de kmalloc para file_fd=NULL.
      vma_t *right = (vma_t *)kzalloc(sizeof(vma_t));
      if (!right) {
        while (new_vmas) {
          vma_t *next = new_vmas->next;
          vma_release_fd(new_vmas);
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
      // [3.5] Propagar file_fd si es VMA_FILE.
      if (v->type == VMA_FILE && v->file_fd) {
        right->file_fd = v->file_fd;
        __atomic_fetch_add(&right->file_fd->ref_count, 1, __ATOMIC_ACQ_REL);
        right->file_offset = v->file_offset + (end - v->start);
      }
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
      uint64_t *pml4 = (uint64_t *)phys_to_virt(proc->pml4_phys);

      // [FIX] Si la PTE es un swap entry, liberar el slot.
      uint64_t raw;
      if (paging_get_pte_in(pml4, p, &raw) && pte_is_swap(raw)) {
        swap_free_slot(pte_swap_type(raw), pte_swap_offset(raw));
        paging_set_pte_in(pml4, p, 0);
        continue;
      }

      uint64_t phys = paging_get_phys_in(pml4, p);
      if (phys) {
        paging_unmap_page_in(pml4, p);
        pmm_free_page(phys);
      }
    }

    if (start <= v->start && end >= v->end) {
      // VMA completamente cubierto: eliminar.
      *pp = v->next;
      vma_release_fd(v); // [3.5]
      kfree(v);
      touched++;
      continue;
    }
    if (start <= v->start) {
      // Recortar por la izquierda: queda [end, v->end).
      // [3.5] Ajustar file_offset.
      if (v->type == VMA_FILE && v->file_fd)
        v->file_offset += (end - v->start);
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
    vma_release_fd(new_vmas);
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
    // [FIX] El caso "cubre rango entero" necesita 2 VMAs nuevos
    // (mid + right), no 1. Sin esto, pool se agota antes de tiempo.
    if (addr > v->start && end < v->end)
      need += 2;
    else if (addr > v->start || end < v->end)
      need += 1;
  }

  vma_t *pool = NULL;
  for (int i = 0; i < need; i++) {
    vma_t *p = (vma_t *)kzalloc(sizeof(vma_t));
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

    int left_cov = (v->start >= addr);
    int right_cov = (v->end <= end);

    if (left_cov && right_cov) {
      // VMA completamente dentro: solo flags.
      v->flags = new_flags;
      pp = &v->next;
    } else if (left_cov) {
      // Split: [v->start, end) nuevos, [end, v->end) viejos.
      vma_t *right = pool;
      pool = pool->next;
      right->start = end;
      right->end = v->end;
      right->flags = v->flags;
      right->type = v->type;
      right->pad = 0;
      right->next = v->next;
      // [3.5] file_fd propagation
      if (v->type == VMA_FILE && v->file_fd) {
        right->file_fd = v->file_fd;
        __atomic_fetch_add(&right->file_fd->ref_count, 1, __ATOMIC_ACQ_REL);
        right->file_offset = v->file_offset + (end - v->start);
      }

      v->end = end;
      v->flags = new_flags;
      v->next = right;
      pp = &right->next;
    } else if (right_cov) {
      // Split: [v->start, addr) viejos, [addr, v->end) nuevos.
      vma_t *mid = pool;
      pool = pool->next;
      mid->start = addr;
      mid->end = v->end;
      mid->flags = new_flags;
      mid->type = v->type;
      mid->pad = 0;
      mid->next = v->next;
      // [3.5] file_fd propagation
      if (v->type == VMA_FILE && v->file_fd) {
        mid->file_fd = v->file_fd;
        __atomic_fetch_add(&mid->file_fd->ref_count, 1, __ATOMIC_ACQ_REL);
        mid->file_offset = v->file_offset + (addr - v->start);
      }

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
      // [3.5] file_fd propagation para ambos
      if (v->type == VMA_FILE && v->file_fd) {
        mid->file_fd = v->file_fd;
        __atomic_fetch_add(&mid->file_fd->ref_count, 1, __ATOMIC_ACQ_REL);
        mid->file_offset = v->file_offset + (addr - v->start);

        right->file_fd = v->file_fd;
        __atomic_fetch_add(&right->file_fd->ref_count, 1, __ATOMIC_ACQ_REL);
        right->file_offset = v->file_offset + (end - v->start);
      }

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
  int multi = (proc->team_size > 1);
  for (uint64_t p = addr; p < end; p += PAGE_SIZE) {
    uint64_t phys = paging_get_phys_in(pml4, p);
    if (phys) {
      paging_map_page_in(pml4, p, phys & PTE_FRAME, new_flags);
      if (multi)
        paging_invalidate_tlb_global(p);
      else
        paging_invalidate_tlb(p);
    }
  }

  LOG_TRACE("[MPROTECT] PID=%u [%p, %p) prot=%lx", proc->pid, (void *)addr,
            (void *)end, (unsigned long)prot);
  return 0;
}

// ---------------------------------------------------------------------------
// mmap(addr, length, prot, flags, fd, offset)
//
// Soporta mapeos anónimos y file-backed.
//
//   is_anon  = flags & MAP_ANONYMOUS != 0   → VMA_ANON
//   is_file  = !is_anon && fd >= 0          → VMA_FILE
//
// [3.5] File-backed: el VMA guarda el file_descriptor_t (con ref_count++)
// y el offset dentro del fichero. El demand pager lee la página del
// inodo cuando ocurre el #PF.
//
// MAP_SHARED file-backed requiere writeback; devolvemos -ENODEV hasta
// que exista esa infraestructura. Solo MAP_PRIVATE por ahora.
// ---------------------------------------------------------------------------
int64_t sys_mmap(struct process *proc, uint64_t addr, uint64_t length,
                 uint64_t prot, uint64_t flags, int fd, uint64_t offset) {
  if (!proc || length == 0)
    return -EINVAL;

  int is_anon = (flags & MMAP_MAP_ANONYMOUS) != 0;
  int is_file = !is_anon && fd >= 0;

  if (!is_anon && !is_file)
    return -EINVAL; // ni anónimo ni file-backed

  if (is_file && (flags & MMAP_MAP_SHARED)) {
    // MAP_SHARED file-backed: requiere writeback, no soportado todavía.
    return -ENODEV;
  }

  if (length > UINT64_MAX - 0xFFFULL)
    return -EINVAL;
  uint64_t len = (length + 0xFFFULL) & ~0xFFFULL;
  if (len == 0)
    return -EINVAL;

  uint64_t pte_flags = PTE_USER | PTE_PRESENT;
  if (prot & MMAP_PROT_WRITE)
    pte_flags |= PTE_WRITABLE;
  if (!(prot & MMAP_PROT_EXEC))
    pte_flags |= PTE_NX;

  uint64_t base;

  if (flags & MMAP_MAP_FIXED) {
    if ((addr & 0xFFFULL) != 0 || (length & 0xFFFULL) != 0)
      return -EINVAL;
    if (addr >= USER_LIMIT || len > USER_LIMIT - addr)
      return -EINVAL;
    base = addr;
    int64_t ur = vma_unmap_range(proc, base, base + len);
    if (ur < 0)
      return ur;
  } else {
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

  if (is_anon) {
    vma_t *v = vma_create(proc, base, base + len, pte_flags, VMA_ANON);
    if (!v)
      return -ENOMEM;
  } else {
    if (fd < 0 || fd >= MAX_PROCESS_FDS || !proc->fds[fd])
      return -EBADF;
    file_descriptor_t *f = proc->fds[fd];
    if (!f->node || !(f->node->flags & VFS_FILE))
      return -EINVAL;
    if (!f->node->ops || !f->node->ops->read) {
      return -EINVAL;
    }
    if (offset & 0xFFFULL) {
      return -EINVAL;
    }
    vma_t *v = vma_create_file(proc, base, base + len, pte_flags, f, offset);
    if (!v)
      return -ENOMEM;
  }

  return (int64_t)base;
}

// ---------------------------------------------------------------------------
// [B] mremap. Musl lo usa en realloc() de bloques mmap'd.
//
//   mremap(old_addr, old_size, new_size, flags, new_addr)
//
// Flags Linux: MREMAP_MAYMOVE=1, MREMAP_FIXED=2, MREMAP_DONTUNMAP=4.
// MREMAP_FIXED exige new_addr alineado y libre → lo rechazamos con
// EINVAL hasta que alguien lo necesite.
// ---------------------------------------------------------------------------
#define MREMAP_MAYMOVE 1
#define MREMAP_FIXED 2

int64_t sys_mremap(struct process *proc, uint64_t old_addr, uint64_t old_size,
                   uint64_t new_size, uint64_t flags, uint64_t new_addr) {
  if (!proc || old_size == 0)
    return -EINVAL;
  if (flags & MREMAP_FIXED)
    return -EINVAL; // no soportado
  if (old_addr & 0xFFF)
    return -EINVAL;

  old_size = (old_size + 0xFFF) & ~0xFFF;
  new_size = (new_size + 0xFFF) & ~0xFFF;

  // Linux: new_size == 0 equivale a munmap(old_addr, old_size).
  if (new_size == 0) {
    (void)sys_munmap(proc, old_addr, old_size);
    return -EINVAL;
  }
  if (old_addr >= USER_LIMIT || old_size > USER_LIMIT - old_addr)
    return -EINVAL;
  if (new_size > USER_LIMIT - old_addr)
    return -ENOMEM;

  vma_t *v = vma_find(proc, old_addr);
  if (!v || old_addr + old_size > v->end)
    return -EFAULT;

  if (new_size == old_size)
    return (int64_t)old_addr;

  // --- Shrink: recortar la cola del VMA ---
  if (new_size < old_size) {
    (void)vma_unmap_range(proc, old_addr + new_size, old_addr + old_size);
    return (int64_t)old_addr;
  }

  // --- Grow: ¿hay hueco contiguo detrás? ---
  uint64_t new_end = old_addr + new_size;
  int can_extend = 1;
  for (vma_t *q = proc->vma_list; q; q = q->next) {
    if (q == v)
      continue;
    // Colisión si [v->end, new_end) intersecta [q->start, q->end).
    if (q->end > v->end && q->start < new_end) {
      can_extend = 0;
      break;
    }
  }
  if (can_extend) {
    // Extender in-place. Los PTEs nuevos los demanda el PF handler.
    v->end = new_end;
    return (int64_t)old_addr;
  }

  // --- Move + copy: mmap anónimo nuevo, memcpy, munmap viejo ---
  if (!(flags & MREMAP_MAYMOVE))
    return -ENOMEM;

  int prot = 0;
  if (v->flags & PTE_WRITABLE)
    prot |= MMAP_PROT_WRITE;
  prot |= MMAP_PROT_READ;
  // Los VMAs siempre son legibles en Aurora, aunque no haya bit.

  int64_t dst = sys_mmap(proc, 0, new_size, prot,
                         MMAP_MAP_PRIVATE | MMAP_MAP_ANONYMOUS, -1, 0);
  if (dst < 0)
    return dst;

  // Copiar el contenido página a página. Las páginas no presentes se
  // quedan a cero en el destino (equivalente a "no estaban").
  uint64_t *pml4 = (uint64_t *)phys_to_virt(proc->pml4_phys);
  for (uint64_t off = 0; off < old_size; off += PAGE_SIZE) {
    uint64_t sp = paging_get_phys_in(pml4, old_addr + off);
    uint64_t dp = paging_get_phys_in(pml4, (uint64_t)dst + off);
    if (sp && dp)
      memcpy(phys_to_virt(dp), phys_to_virt(sp), PAGE_SIZE);
  }

  (void)sys_munmap(proc, old_addr, old_size);
  return dst;
}