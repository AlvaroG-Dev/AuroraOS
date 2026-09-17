// kernel/process.c
#include "process.h"
#include "cpu.h"
#include "elf.h"
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

static uint32_t next_pid = 0;
static process_t *process_list = NULL;
static uint64_t next_user_vaddr = 0x0000000000400000ULL;

process_t *process_current(void) {
  task_t *cur = sched_current();
  if (!cur)
    return NULL;
  process_t *p = process_list;
  while (p) {
    if (p->task == cur)
      return p;
    p = p->next;
  }
  return NULL;
}

process_t *process_get_by_pid(uint32_t pid) {
  process_t *p = process_list;
  while (p) {
    if (p->pid == pid)
      return p;
    p = p->next;
  }
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

  // 1. Clonar espacio de direcciones del kernel
  uint64_t pml4_phys = paging_clone_kernel_space();
  if (!pml4_phys) {
    LOG_ERR("[PROC] Fallo al clonar PML4");
    return NULL;
  }
  uint64_t *pml4 = (uint64_t *)phys_to_virt(pml4_phys);

  // 2. Cargar ELF (soporta auto-posicionamiento para ET_DYN)
  uint64_t load_base = next_user_vaddr;
  uint64_t entry = 0;
  if (elf_load(elf_data, elf_size, pml4, load_base, &entry) != 0) {
    LOG_ERR("[PROC] Fallo al cargar ELF");
    paging_free_user_space(pml4_phys);
    return NULL;
  }
  next_user_vaddr += 0x200000ULL;

  // 3. Mapear stack de usuario aislado para este proceso
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
    if (paging_map_page_in(pml4, stack_base + off, phys,
                           PTE_USER | PTE_WRITABLE | PTE_PRESENT | PTE_NX) !=
        0) {
      pmm_free_page(phys);
      LOG_ERR("[PROC] Fallo al mapear página de stack");
      paging_free_user_space(pml4_phys);
      return NULL;
    }
  }

  // 4. Crear la tarea en Ring 3 con el PML4 del proceso
  task_t *task =
      sched_create_user_task((void (*)(void))entry, stack_top, pml4_phys);
  if (!task) {
    LOG_ERR("[PROC] Fallo al crear tarea");
    paging_free_user_space(pml4_phys);
    return NULL;
  }

  // 5. Asignar estructura de proceso
  process_t *proc = (process_t *)kmalloc(sizeof(process_t));
  if (!proc) {
    task->state = TASK_DEAD;
    LOG_ERR("[PROC] Error al asignar process_t");
    return NULL;
  }
  proc->pid = next_pid++;
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
  proc->pml4_phys = pml4_phys;
  proc->load_base = load_base;
  proc->heap_start = USER_HEAP_BASE;
  proc->heap_end = USER_HEAP_BASE;
  proc->heap_max = USER_HEAP_MAX;

  // -------------------------------------------------------------------------
  // VMA y stack info
  // -------------------------------------------------------------------------
  proc->vma_list = NULL;
  proc->stack_base = stack_base;
  proc->stack_low = stack_base - MAX_STACK_GROWTH;
  proc->stack_top = stack_top;

  // Guard page: una página no mapeada justo debajo del stack permitido.
  // Cualquier acceso a esta página dispara #PF fuera de VMA → kill.
  proc->stack_guard = proc->stack_low;
  proc->stack_low += PAGE_SIZE; // el stack real empieza una página más arriba

  // VMA del ELF (rango aproximado de 2 MB por proceso)
  vma_create(proc, load_base, load_base + 0x200000, PTE_USER | PTE_NX, VMA_ELF);

  // VMA del stack (empieza en stack_low + guard page, termina en stack_top)
  vma_create(proc, proc->stack_low, proc->stack_top,
             PTE_USER | PTE_WRITABLE | PTE_NX, VMA_STACK);

  // Inicializar tabla de descriptores de archivos con stdin, stdout, stderr
  for (int f = 0; f < MAX_PROCESS_FDS; f++) {
    proc->fds[f] = NULL;
  }
  proc->fds[0] = vfs_create_stdio_fd(0); // stdin
  proc->fds[1] = vfs_create_stdio_fd(1); // stdout
  proc->fds[2] = vfs_create_stdio_fd(2); // stderr

  // Init wait queue de hijos ANTES de enlazar en la lista, para que
  // cualquier process_exit que corra después pueda despertar con seguridad.
  wait_queue_init(&proc->child_wq);

  // [PREEMPT] Publicación atómica en process_list. Cualquier lector
  // que corra concurrentemente verá la lista consistente.
  preempt_disable();
  proc->next = process_list;
  process_list = proc;
  preempt_enable();

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

  process_t *p = process_list;
  while (p) {
    if ((pid == -1 && p->ppid == parent->pid) ||
        (pid >= 0 && (uint32_t)pid == p->pid)) {
      if (p->is_zombie)
        return true;
    }
    p = p->next;
  }
  return false;
}

int process_waitpid(process_t *parent, int32_t pid, int *status_out,
                    int options) {
  if (!parent)
    return -1;

  waitpid_ctx_t ctx = {.parent = parent, .pid = pid};

  while (1) {
    // ¿Hay ya un hijo zombie que encaje?
    process_t *found_zombie = NULL;
    int has_matching_child = 0;

    process_t *p = process_list;
    while (p) {
      if ((pid == -1 && p->ppid == parent->pid) ||
          (pid >= 0 && (uint32_t)pid == p->pid)) {
        has_matching_child = 1;
        if (p->is_zombie) {
          found_zombie = p;
          break;
        }
      }
      p = p->next;
    }

    if (found_zombie) {
      uint32_t zpid = found_zombie->pid;
      if (status_out) {
        stac();
        *status_out = found_zombie->exit_code;
        clac();
      }
      process_terminate(found_zombie);
      return (int)zpid;
    }

    if (!has_matching_child) {
      return -1; // No existe tal hijo
    }

    if (options & WNOHANG) {
      return 0; // Hijo en ejecución, no bloquear
    }

    // Esperar en la wq del padre. La condición se re-evalúa bajo el
    // lock de la wq cada vez que process_exit hace wake_up_all.
    // No hay race: si el hijo muere justo entre el escaneo de arriba
    // y el wait_event, la re-evaluación de la condición lo detecta
    // (o el wake_up ya nos encontrará en la wq).
    int rc =
        wait_event_interruptible(&parent->child_wq, has_matching_zombie, &ctx);
    if (rc < 0)
      return -1;
    // Al volver, re-escaneamos desde el principio del while(1).
  }
}

void process_exit(process_t *proc, int exit_code) {
  if (!proc)
    return;
  proc->exit_code = exit_code;
  proc->is_zombie = 1;

  for (int f = 0; f < MAX_PROCESS_FDS; f++) {
    if (proc->fds[f]) {
      vfs_close_for_proc(proc, f);
    }
  }

  // Despertar al padre (si existe) para que su waitpid re-evalúe
  // la condición. Aunque el padre esté esperando por un pid
  // distinto, has_matching_zombie lo filtrará correctamente y el
  // padre volverá a dormir.
  process_t *parent = process_get_by_pid(proc->ppid);
  if (parent) {
    wake_up_all(&parent->child_wq);
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

  vma_destroy_all(proc);

  // [PREEMPT] Unlink atómico.
  preempt_disable();
  process_t **p = &process_list;
  while (*p) {
    if (*p == proc) {
      *p = proc->next;
      break;
    }
    p = &(*p)->next;
  }
  preempt_enable();

  kfree(proc);
}