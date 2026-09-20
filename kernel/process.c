// kernel/process.c
#include "process.h"
#include "cpu.h"
#include "elf.h"
#include "gfx/winsrv.h"
#include "heap.h"
#include "klog.h"
#include "paging.h"
#include "pf.h"
#include "pmm.h"
#include "sched.h"
#include "serial.h"
#include "string.h"
#include "tarfs.h"
#include "vfs.h"
#include "wait.h"
#include <stddef.h>

#define USER_STACK_BASE 0x00007FFFF0000000ULL
#define USER_STACK_PAGES 4
#define USER_STACK_SIZE (USER_STACK_PAGES * PAGE_SIZE)

#define USER_HEAP_BASE 0x0000000040000000ULL
#define USER_HEAP_MAX 0x0000000048000000ULL // 128 MB max

static uint32_t next_pid = 1; // ← [P0.6] empieza en 1
static process_t *process_list = NULL;
static spinlock_t process_lock; // ← [P0.1] nuevo
static uint64_t next_user_vaddr = 0x0000000000400000ULL;

void process_init(void) { // ← [P0.1] llamar desde el boot
  spin_init(&process_lock);
}

process_t *process_current(void) {
  // [Fase A] Enlace directo, sin recorrer la lista.
  task_t *t = sched_current();
  return t ? t->proc : NULL;
}

process_t *process_get_by_pid(uint32_t pid) {
  unsigned long flags = spin_lock_irqsave(&process_lock);
  process_t *p = process_list;
  while (p) {
    if (p->pid == pid) {
      spin_unlock_irqrestore(&process_lock, flags);
      return p;
    }
    p = p->next;
  }
  spin_unlock_irqrestore(&process_lock, flags);
  return NULL;
}

process_t *process_spawn(const char *name, const void *elf_data,
                         size_t elf_size) {
  if (!elf_data || elf_size < sizeof(Elf64_Ehdr)) {
    LOG_ERR("[PROC] Datos ELF inválidos");
    return NULL;
  }
  if (elf_validate(elf_data, elf_size) != 0) {
    LOG_ERR("[PROC] ELF no válido");
    return NULL;
  }

  // 1. Clonar PML4
  uint64_t pml4_phys = paging_clone_kernel_space();
  if (!pml4_phys) {
    LOG_ERR("[PROC] Fallo al clonar PML4");
    return NULL;
  }
  uint64_t *pml4 = (uint64_t *)phys_to_virt(pml4_phys);

  // 2. Reservar un rango de direcciones virtuales para el ELF.
  //
  // __sync_fetch_and_add es atómico: dos process_spawn concurrentes
  // en distintas CPUs obtienen rangos disjuntos. Si elf_load falla
  // después, perdemos 2 MB de espacio virtual (fuga despreciable
  // frente a los 128 TB disponibles). No merece la pena complicar
  // el código para recuperarlo.
  uint64_t load_base = __sync_fetch_and_add(&next_user_vaddr, 0x200000ULL);

  // 3. Cargar ELF
  uint64_t entry = 0;
  if (elf_load(elf_data, elf_size, pml4, load_base, &entry) != 0) {
    LOG_ERR("[PROC] Fallo al cargar ELF");
    paging_free_user_space(pml4_phys);
    return NULL;
  }

  // 4. Mapear stack de usuario (zeroed)
  uint64_t stack_base = USER_STACK_BASE;
  uint64_t stack_top = stack_base + USER_STACK_SIZE;
  LOG_INFO("[PROC] Mapeando stack de usuario en %p - %p", (void *)stack_base,
           (void *)stack_top);

  for (uint64_t off = 0; off < USER_STACK_SIZE; off += PAGE_SIZE) {
    uint64_t phys = pmm_alloc_page();
    if (!phys) {
      LOG_ERR("[PROC] Fallo al asignar página física para stack");
      paging_free_user_space(pml4_phys);
      return NULL;
    }
    memset(phys_to_virt(phys), 0, PAGE_SIZE);
    if (paging_map_page_in(pml4, stack_base + off, phys,
                           PTE_USER | PTE_WRITABLE | PTE_PRESENT | PTE_NX) !=
        0) {
      pmm_free_page(phys);
      LOG_ERR("[PROC] Fallo al mapear página de stack");
      paging_free_user_space(pml4_phys);
      return NULL;
    }
  }

  // 5. [Fase A] Crear la tarea PARADA (no runnable todavía)
  task_t *task = sched_create_user_task_stopped((void (*)(void))entry,
                                                stack_top, pml4_phys);
  if (!task) {
    LOG_ERR("[PROC] Fallo al crear tarea");
    paging_free_user_space(pml4_phys);
    return NULL;
  }

  // 6. Asignar process_t
  process_t *proc = (process_t *)kzalloc(sizeof(process_t));
  if (!proc) {
    task_put(task);
    paging_free_user_space(pml4_phys);
    LOG_ERR("[PROC] Error al asignar process_t");
    return NULL;
  }

  proc->pid = __sync_add_and_fetch(&next_pid, 1) - 1;
  proc->ppid = 0;
  proc->exit_code = 0;
  proc->is_zombie = 0;

  size_t i = 0;
  while (name[i] && i < sizeof(proc->name) - 1) {
    proc->name[i] = name[i];
    i++;
  }
  proc->name[i] = '\0';
  proc->task = task;
  // process_t mantiene una referencia propia al task_t. El scheduler posee
  // la referencia inicial; esta segunda referencia impide que el reaper
  // pueda liberar task_t mientras process_t siga exponiendo proc->task.
  task_get(task);
  proc->pml4_phys = pml4_phys;
  proc->load_base = load_base;
  proc->heap_start = USER_HEAP_BASE;
  proc->heap_end = USER_HEAP_BASE;
  proc->heap_max = USER_HEAP_MAX;

  proc->vma_list = NULL;
  proc->stack_base = stack_base;
  proc->stack_low = stack_base - MAX_STACK_GROWTH;
  proc->stack_top = stack_top;
  proc->stack_guard = proc->stack_low;
  proc->stack_low += PAGE_SIZE;

  vma_create(proc, load_base, load_base + 0x200000, PTE_USER | PTE_NX, VMA_ELF);
  vma_create(proc, proc->stack_low, proc->stack_top,
             PTE_USER | PTE_WRITABLE | PTE_NX, VMA_STACK);

  for (int f = 0; f < MAX_PROCESS_FDS; f++)
    proc->fds[f] = NULL;
  proc->fds[0] = vfs_create_stdio_fd(0);
  proc->fds[1] = vfs_create_stdio_fd(1);
  proc->fds[2] = vfs_create_stdio_fd(2);

  wait_queue_init(&proc->child_wq);

  // 7. Enlazar tarea ↔ proceso ANTES de publicar la tarea.
  task->proc = proc;

  // 8. Publicar en process_list con lock.
  unsigned long flags = spin_lock_irqsave(&process_lock);
  proc->next = process_list;
  process_list = proc;
  spin_unlock_irqrestore(&process_lock, flags);

  // 9. Lo ÚLTIMO: hacer la tarea runnable.
  sched_publish_task(task);

  LOG_INFO("[PROC] Proceso '%s' creado (PID=%u)", name, proc->pid);
  return proc;
}

process_t *process_spawn_child(process_t *parent, const char *path) {
  process_t *child = process_load(path);
  if (child) {
    if (parent) {
      child->ppid = parent->pid;
    }
  }
  return child;
}

process_t *process_load(const char *path) {
  tar_node_t *node = tarfs_open(path);
  if (!node || node->is_dir) {
    LOG_ERR("[PROC] No se encontró el archivo: %s", path);
    return NULL;
  }
  LOG_INFO("[PROC] Cargando archivo: %s (tamaño=%lu)", path,
           (unsigned long)node->size);
  return process_spawn(path, node->data, node->size);
}

// ---------------------------------------------------------------------------
// Condición de despertar: ¿existe algún hijo zombie que satisfaga el filtro?
// ---------------------------------------------------------------------------
typedef struct {
  process_t *parent;
  int32_t pid;
} waitpid_ctx_t;

static bool has_matching_zombie(void *arg) {
  waitpid_ctx_t *ctx = (waitpid_ctx_t *)arg;
  process_t *parent = ctx->parent;
  int32_t pid = ctx->pid;

  unsigned long flags = spin_lock_irqsave(&process_lock);
  process_t *p = process_list;
  bool found = false;
  while (p) {
    if ((pid == -1 && p->ppid == parent->pid) ||
        (pid >= 0 && (uint32_t)pid == p->pid && p->ppid == parent->pid)) {
      // [P0.6] Solo cuentan hijos MÍOS.
      if (p->is_zombie) {
        found = true;
        break;
      }
    }
    p = p->next;
  }
  spin_unlock_irqrestore(&process_lock, flags);
  return found;
}

int process_waitpid(process_t *parent, int32_t pid, int *status_out,
                    int options) {
  if (!parent)
    return -1;

  waitpid_ctx_t ctx = {.parent = parent, .pid = pid};

  while (1) {
    process_t *found_zombie = NULL;
    task_t *zombie_task = NULL;
    int has_matching_child = 0;

    unsigned long flags = spin_lock_irqsave(&process_lock);
    process_t *p = process_list;
    while (p) {
      if ((pid == -1 && p->ppid == parent->pid) ||
          (pid >= 0 && (uint32_t)pid == p->pid && p->ppid == parent->pid)) {
        has_matching_child = 1;
        if (p->is_zombie) {
          // Reap atómico: sacar el proceso de process_list y adquirir la
          // referencia del task mientras aún tenemos process_lock. Esto
          // evita que otro waitpid() recoja el mismo zombie y garantiza que
          // task_t siga vivo después de soltar el lock.
          found_zombie = p;
          zombie_task = p->task;
          if (zombie_task)
            task_get(zombie_task);

          process_t **pp = &process_list;
          while (*pp && *pp != found_zombie)
            pp = &(*pp)->next;
          if (*pp == found_zombie)
            *pp = found_zombie->next;
          break;
        }
      }
      p = p->next;
    }
    spin_unlock_irqrestore(&process_lock, flags);

    if (found_zombie) {
      // found_zombie ya no está publicado en process_list. El proceso
      // pertenece exclusivamente a este waitpid(), y zombie_task mantiene
      // vivo el task_t aunque el scheduler haya retirado su referencia.
      uint32_t zpid = found_zombie->pid;
      int exit_code = found_zombie->exit_code;

      if (status_out) {
        stac();
        *status_out = exit_code;
        clac();
      }

      // Cleanup fuera del lock. No se toca ningún task_t después de
      // liberar la referencia adquirida arriba.
      if (zombie_task) {
        zombie_task->proc = NULL;
        // Liberar la referencia temporal de waitpid. La referencia propia
        // del process_t se libera inmediatamente antes de destruir proc.
        task_put(zombie_task);
        zombie_task = NULL;
      }
      if (found_zombie->pml4_phys)
        paging_free_user_space(found_zombie->pml4_phys);
      vma_destroy_all(found_zombie);
      if (found_zombie->task) {
        task_put(found_zombie->task);
        found_zombie->task = NULL;
      }
      kfree(found_zombie);

      return (int)zpid;
    }

    if (!has_matching_child)
      return -1;
    if (options & WNOHANG)
      return 0;

    int rc =
        wait_event_interruptible(&parent->child_wq, has_matching_zombie, &ctx);
    if (rc < 0)
      return -1;
  }
}

void process_exit(process_t *proc, int exit_code) {
  if (!proc)
    return;
  proc->exit_code = exit_code;
  proc->is_zombie = 1;

  for (int f = 0; f < MAX_PROCESS_FDS; f++) {
    if (proc->fds[f])
      vfs_close_for_proc(proc, f);
  }

  if (proc->task)
    winsrv_cleanup_task(proc->task);

  process_t *parent = process_get_by_pid(proc->ppid);
  if (parent)
    wake_up_all(&parent->child_wq);
}

__attribute__((noreturn)) void process_exit_current(int exit_code) {
  process_t *proc = process_current();
  task_t *cur = sched_current();

  if (proc)
    process_exit(proc, exit_code);

  if (cur) {
    // Marcar DEAD. on_cpu se pondrá a 0 cuando el scheduler cambie
    // a otra tarea (en sched_tick).
    cur->state = TASK_DEAD;
  }

  // Ceder y esperar a que el reaper nos libere.
  while (1) {
    sched_yield();
    __asm__ volatile("sti; hlt");
  }
}

void *process_sbrk(process_t *proc, int64_t increment) {
  if (!proc)
    return (void *)-1;

  uint64_t old_brk = proc->heap_end;
  if (increment == 0) {
    return (void *)old_brk;
  }

  if (increment > 0) {
    uint64_t new_brk = old_brk + (uint64_t)increment;
    if (new_brk > proc->heap_max || new_brk < old_brk) {
      return (void *)-1;
    }

    uint64_t start_page = (old_brk + PAGE_SIZE - 1) & ~0xFFFULL;
    uint64_t end_page = (new_brk + PAGE_SIZE - 1) & ~0xFFFULL;

    uint64_t *pml4 = (uint64_t *)phys_to_virt(proc->pml4_phys);

    for (uint64_t page = start_page; page < end_page; page += PAGE_SIZE) {
      if (!paging_get_phys_in(pml4, page)) {
        uint64_t phys = pmm_alloc_page();
        if (!phys) {
          LOG_ERR("[PROC] sbrk: sin memoria física libre");
          return (void *)-1;
        }
        if (paging_map_page_in(pml4, page, phys,
                               PTE_USER | PTE_WRITABLE | PTE_PRESENT |
                                   PTE_NX) != 0) {
          pmm_free_page(phys);
          LOG_ERR("[PROC] sbrk: error mapeando página");
          return (void *)-1;
        }
        memset(phys_to_virt(phys), 0, PAGE_SIZE);
      }
    }

    proc->heap_end = new_brk;
    return (void *)old_brk;
  } else {
    uint64_t decr = (uint64_t)(-increment);
    if (decr > (old_brk - proc->heap_start)) {
      return (void *)-1;
    }
    uint64_t new_brk = old_brk - decr;
    proc->heap_end = new_brk;
    return (void *)old_brk;
  }
}

void process_terminate(process_t *proc) {
  if (!proc)
    return;
  LOG_INFO("[PROC] Limpiando process_t '%s' (PID=%u)", proc->name, proc->pid);

  // 1. Desvincular tarea ↔ proceso y liberar la referencia que
  // process_t adquirió en process_spawn(). La referencia del scheduler
  // es independiente y la libera reap_dead_tasks().
  if (proc->task) {
    task_t *task = proc->task;
    proc->task = NULL;
    task->proc = NULL;
    task_put(task);
  }

  // 2. [P1.3] Liberar tablas de página. Solo cuando la tarea está
  // DEAD y on_cpu == 0, garantizado porque process_terminate se llama
  // desde waitpid() sobre un hijo ya reapeado.
  if (proc->pml4_phys) {
    paging_free_user_space(proc->pml4_phys);
    proc->pml4_phys = 0;
  }

  // 3. VMA list
  vma_destroy_all(proc);

  // 4. Desvincular de process_list con lock
  unsigned long flags = spin_lock_irqsave(&process_lock);
  process_t **p = &process_list;
  while (*p) {
    if (*p == proc) {
      *p = proc->next;
      break;
    }
    p = &(*p)->next;
  }
  spin_unlock_irqrestore(&process_lock, flags);

  kfree(proc);
}