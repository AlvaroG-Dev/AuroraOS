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

static uint32_t next_pid = 1;
static process_t *process_list = NULL;
static spinlock_t process_lock;
static uint64_t next_user_vaddr = 0x0000000000400000ULL;

// Reader ELF sobre un vfs_node_t ya resuelto. No toca fds ni offset de
// fichero: usa directamente node->ops->read(node, offset, ...). El nodo
// se mantiene vivo por el llamante durante toda la carga.
struct elf_vfs_ctx {
  vfs_node_t *node;
};

void process_init(void) { spin_init(&process_lock); }

process_t *process_current(void) {
  task_t *t = sched_current();
  return t ? t->proc : NULL;
}

static int64_t elf_read_vfs(void *ctx, uint64_t offset, size_t size,
                            void *buf) {
  struct elf_vfs_ctx *c = (struct elf_vfs_ctx *)ctx;
  if (!c->node || !c->node->ops || !c->node->ops->read)
    return -1;
  return c->node->ops->read(c->node, offset, size, buf);
}

// ---------------------------------------------------------------------------
// [cwd] Inicializa proc->cwd a partir de `src`. Si `src` es NULL o vacío,
// usa "/". Trunca si hace falta.
// ---------------------------------------------------------------------------
static void process_set_cwd(process_t *proc, const char *src) {
  if (!proc)
    return;
  const char *s = (src && src[0]) ? src : "/";
  size_t n = strlen(s);
  if (n >= sizeof(proc->cwd))
    n = sizeof(proc->cwd) - 1;
  for (size_t i = 0; i < n; i++)
    proc->cwd[i] = s[i];
  proc->cwd[n] = '\0';
}

// ---------------------------------------------------------------------------
// [SIG] Inicializa el estado de señales de un proceso recién creado.
// ---------------------------------------------------------------------------
static void process_init_signals(process_t *proc) {
  if (!proc)
    return;
  proc->pending_signals = 0;
  proc->blocked_signals = 0;
}

// ---------------------------------------------------------------------------
// [Fase 3.2] Construcción del arg block en el stack del hijo.
//
//   [rsp + 0]              = argc
//   [rsp + 8]              = argv[0]
//   ...
//   [rsp + 8*argc]         = argv[argc-1]
//   [rsp + 8*(argc+1)]     = NULL        (terminador de argv)
//   [rsp + 8*(argc+2)]     = NULL        (terminador de envp)
//   [rsp + str_off]        = "arg0\0arg1\0..."
//
// Devuelve el nuevo RSP o 0 en error.
// ---------------------------------------------------------------------------
static uint64_t setup_arg_block(uint64_t *pml4, uint64_t stack_top, int argc,
                                const char *const *argv) {
  if (argc < 0 || argc > PROCESS_ARGV_MAX)
    return 0;
  if (argc > 0 && !argv)
    return 0;

  uint64_t strings_total = 0;
  for (int i = 0; i < argc; i++) {
    if (!argv[i])
      return 0;
    size_t n = strlen(argv[i]) + 1;
    if (n > 4096)
      return 0;
    strings_total += n;
  }
  strings_total = (strings_total + 7) & ~7ULL;

  uint64_t block = 8 + (uint64_t)argc * 8 + 8 + 8 + strings_total;
  block = (block + 15) & ~15ULL;

  if (block > stack_top)
    return 0;
  uint64_t new_rsp = (stack_top - block) & ~0xFULL;

  uint8_t *scratch = (uint8_t *)kmalloc(block);
  if (!scratch)
    return 0;
  memset(scratch, 0, block);

  *(uint64_t *)(scratch + 0) = (uint64_t)argc;

  uint64_t str_off = 8 + (uint64_t)argc * 8 + 8 + 8;
  for (int i = 0; i < argc; i++) {
    size_t n = strlen(argv[i]) + 1;
    memcpy(scratch + str_off, argv[i], n);
    *(uint64_t *)(scratch + 8 + i * 8) = new_rsp + str_off;
    str_off += n;
  }

  uint64_t va = new_rsp;
  uint64_t src = 0;
  while (src < block) {
    uint64_t page_va = va & ~0xFFFULL;
    uint64_t page_off = va & 0xFFFULL;
    uint64_t avail = PAGE_SIZE - page_off;
    uint64_t chunk = block - src;
    if (chunk > avail)
      chunk = avail;

    uint64_t phys = paging_get_phys_in(pml4, page_va);
    if (!phys) {
      LOG_ERR("[PROC] setup_arg_block: VA %p sin mapear", (void *)page_va);
      kfree(scratch);
      return 0;
    }
    memcpy((uint8_t *)phys_to_virt(phys) + page_off, scratch + src, chunk);
    va += chunk;
    src += chunk;
  }

  kfree(scratch);
  return new_rsp;
}

// ---------------------------------------------------------------------------
// Spawn desde un buffer ELF contiguo (path clásico, sin argv ni cwd
// heredado). Se mantiene para tests y para futuros exec().
// ---------------------------------------------------------------------------
static process_t *process_spawn_with_ppid(const char *name,
                                          const void *elf_data, size_t elf_size,
                                          uint32_t ppid) {
  if (!elf_data || elf_size < sizeof(Elf64_Ehdr)) {
    LOG_ERR("[PROC] Datos ELF inválidos");
    return NULL;
  }
  if (elf_validate(elf_data, elf_size) != 0) {
    LOG_ERR("[PROC] ELF no válido");
    return NULL;
  }

  uint64_t pml4_phys = paging_clone_kernel_space();
  if (!pml4_phys) {
    LOG_ERR("[PROC] Fallo al clonar PML4");
    return NULL;
  }
  uint64_t *pml4 = (uint64_t *)phys_to_virt(pml4_phys);

  uint64_t load_base = __sync_fetch_and_add(&next_user_vaddr, 0x200000ULL);

  uint64_t entry = 0;
  if (elf_load(elf_data, elf_size, pml4, load_base, &entry) != 0) {
    LOG_ERR("[PROC] Fallo al cargar ELF");
    paging_free_user_space(pml4_phys);
    return NULL;
  }

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

  task_t *task = sched_create_user_task_stopped((void (*)(void))entry,
                                                stack_top, pml4_phys);
  if (!task) {
    LOG_ERR("[PROC] Fallo al crear tarea");
    paging_free_user_space(pml4_phys);
    return NULL;
  }

  process_t *proc = (process_t *)kzalloc(sizeof(process_t));
  if (!proc) {
    task_put(task);
    paging_free_user_space(pml4_phys);
    LOG_ERR("[PROC] Error al asignar process_t");
    return NULL;
  }

  proc->pid = __sync_add_and_fetch(&next_pid, 1) - 1;
  proc->ppid = ppid;
  proc->exit_code = 0;
  proc->is_zombie = 0;

  size_t i = 0;
  while (name[i] && i < sizeof(proc->name) - 1) {
    proc->name[i] = name[i];
    i++;
  }
  proc->name[i] = '\0';
  proc->task = task;
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

  // [cwd] Este path no hereda cwd. Arranca en "/".
  process_set_cwd(proc, "/");
  // [SIG] Estado de señales inicial.
  process_init_signals(proc);

  const Elf64_Ehdr *ehdr = (const Elf64_Ehdr *)elf_data;
  uint64_t elf_vma_start = ~0ULL;
  uint64_t elf_vma_end = 0;
  const Elf64_Phdr *phdrs =
      (const Elf64_Phdr *)((const uint8_t *)elf_data + ehdr->e_phoff);

  for (uint16_t pi = 0; pi < ehdr->e_phnum; pi++) {
    const Elf64_Phdr *ph = &phdrs[pi];
    if (ph->p_type != PT_LOAD || ph->p_memsz == 0)
      continue;

    uint64_t seg_start = ph->p_vaddr;
    uint64_t seg_end = ph->p_vaddr + ph->p_memsz;
    if (seg_end < seg_start)
      continue;

    if (ehdr->e_type == ET_DYN) {
      uint64_t relocated_start = seg_start + load_base;
      uint64_t relocated_end = seg_end + load_base;
      if (relocated_start < seg_start || relocated_end < seg_end)
        continue;
      seg_start = relocated_start;
      seg_end = relocated_end;
    }

    seg_start &= ~0xFFFULL;
    if (seg_end > ~0xFFFULL)
      seg_end = ~0ULL;
    else
      seg_end = (seg_end + 0xFFFULL) & ~0xFFFULL;

    if (seg_end <= seg_start)
      continue;

    if (seg_start < elf_vma_start)
      elf_vma_start = seg_start;
    if (seg_end > elf_vma_end)
      elf_vma_end = seg_end;
  }

  if (elf_vma_start == ~0ULL || elf_vma_start >= elf_vma_end ||
      !vma_create(proc, elf_vma_start, elf_vma_end, PTE_USER | PTE_NX,
                  VMA_ELF)) {
    LOG_ERR("[PROC] No se pudo crear VMA ELF");
    task_put(task);
    task_put(task);
    paging_free_user_space(pml4_phys);
    kfree(proc);
    return NULL;
  }

  if (!vma_create(proc, proc->stack_low, proc->stack_top,
                  PTE_USER | PTE_WRITABLE | PTE_NX, VMA_STACK)) {
    LOG_ERR("[PROC] No se pudo crear VMA stack");
    vma_destroy_all(proc);
    task_put(task);
    task_put(task);
    paging_free_user_space(pml4_phys);
    kfree(proc);
    return NULL;
  }

  for (int f = 0; f < MAX_PROCESS_FDS; f++)
    proc->fds[f] = NULL;
  proc->fds[0] = vfs_create_stdio_fd(0);
  proc->fds[1] = vfs_create_stdio_fd(1);
  proc->fds[2] = vfs_create_stdio_fd(2);

  wait_queue_init(&proc->child_wq);

  // Enlazar tarea ↔ proceso ANTES de publicar la tarea.
  task->proc = proc;

  // Publicar en process_list con lock.
  unsigned long flags = spin_lock_irqsave(&process_lock);
  proc->next = process_list;
  process_list = proc;
  spin_unlock_irqrestore(&process_lock, flags);

  // Lo ÚLTIMO: hacer la tarea runnable.
  sched_publish_task(task);

  LOG_INFO("[PROC] Proceso '%s' creado (PID=%u)", name, proc->pid);
  return proc;
}

process_t *process_spawn(const char *name, const void *elf_data,
                         size_t elf_size) {
  return process_spawn_with_ppid(name, elf_data, elf_size, 0);
}

// ---------------------------------------------------------------------------
// [Fase 3.2] Spawn con reader streaming + argv + cwd heredado.
// ---------------------------------------------------------------------------
static process_t *process_spawn_streaming_with_ppid_args_cwd(
    const char *name, elf_read_fn read, void *read_ctx, uint64_t file_size,
    int argc, const char *const *argv, const char *inherited_cwd,
    uint32_t ppid) {
  if (!read) {
    LOG_ERR("[PROC] reader ELF nulo");
    return NULL;
  }

  uint64_t pml4_phys = paging_clone_kernel_space();
  if (!pml4_phys) {
    LOG_ERR("[PROC] Fallo al clonar PML4");
    return NULL;
  }
  uint64_t *pml4 = (uint64_t *)phys_to_virt(pml4_phys);

  uint64_t load_base = __sync_fetch_and_add(&next_user_vaddr, 0x200000ULL);

  uint64_t entry = 0;
  uint64_t elf_vma_start = ~0ULL;
  uint64_t elf_vma_end = 0;
  if (elf_load_streaming(read, read_ctx, file_size, pml4, load_base, &entry,
                         &elf_vma_start, &elf_vma_end) != 0) {
    LOG_ERR("[PROC] Fallo al cargar ELF (streaming)");
    paging_free_user_space(pml4_phys);
    return NULL;
  }

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

  uint64_t user_rsp = setup_arg_block(pml4, stack_top, argc, argv);
  if (!user_rsp) {
    LOG_ERR("[PROC] setup_arg_block falló");
    paging_free_user_space(pml4_phys);
    return NULL;
  }

  task_t *task = sched_create_user_task_stopped((void (*)(void))entry, user_rsp,
                                                pml4_phys);
  if (!task) {
    LOG_ERR("[PROC] Fallo al crear tarea");
    paging_free_user_space(pml4_phys);
    return NULL;
  }

  process_t *proc = (process_t *)kzalloc(sizeof(process_t));
  if (!proc) {
    task_put(task);
    paging_free_user_space(pml4_phys);
    LOG_ERR("[PROC] Error al asignar process_t");
    return NULL;
  }

  proc->pid = __sync_add_and_fetch(&next_pid, 1) - 1;
  proc->ppid = ppid;
  proc->exit_code = 0;
  proc->is_zombie = 0;

  size_t i = 0;
  while (name[i] && i < sizeof(proc->name) - 1) {
    proc->name[i] = name[i];
    i++;
  }
  proc->name[i] = '\0';
  proc->task = task;
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

  // [cwd] Heredar del padre o "/".
  process_set_cwd(proc, inherited_cwd);
  // [SIG] Estado de señales inicial.
  process_init_signals(proc);

  if (elf_vma_start == ~0ULL || elf_vma_start >= elf_vma_end ||
      !vma_create(proc, elf_vma_start, elf_vma_end, PTE_USER | PTE_NX,
                  VMA_ELF)) {
    LOG_ERR("[PROC] No se pudo crear VMA ELF (start=%p end=%p)",
            (void *)elf_vma_start, (void *)elf_vma_end);
    task_put(task);
    task_put(task);
    paging_free_user_space(pml4_phys);
    kfree(proc);
    return NULL;
  }

  if (!vma_create(proc, proc->stack_low, proc->stack_top,
                  PTE_USER | PTE_WRITABLE | PTE_NX, VMA_STACK)) {
    LOG_ERR("[PROC] No se pudo crear VMA stack");
    vma_destroy_all(proc);
    task_put(task);
    task_put(task);
    paging_free_user_space(pml4_phys);
    kfree(proc);
    return NULL;
  }

  for (int f = 0; f < MAX_PROCESS_FDS; f++)
    proc->fds[f] = NULL;
  proc->fds[0] = vfs_create_stdio_fd(0);
  proc->fds[1] = vfs_create_stdio_fd(1);
  proc->fds[2] = vfs_create_stdio_fd(2);

  wait_queue_init(&proc->child_wq);

  task->proc = proc;

  unsigned long flags = spin_lock_irqsave(&process_lock);
  proc->next = process_list;
  process_list = proc;
  spin_unlock_irqrestore(&process_lock, flags);

  sched_publish_task(task);

  LOG_INFO("[PROC] Proceso '%s' creado (PID=%u, argc=%d, cwd='%s', streaming)",
           name, proc->pid, argc, proc->cwd);
  return proc;
}

// ---------------------------------------------------------------------------
// Carga desde VFS con/sin argv, y con/sin cwd heredado.
// ---------------------------------------------------------------------------
static process_t *process_load_with_ppid_args_cwd(const char *path, int argc,
                                                  const char *const *argv,
                                                  const char *inherited_cwd,
                                                  uint32_t ppid) {
  vfs_node_t *node = vfs_lookup(path);
  if (!node) {
    LOG_ERR("[PROC] No se pudo abrir '%s'", path);
    return NULL;
  }
  if (node->flags & VFS_DIRECTORY) {
    vfs_node_free(node);
    LOG_ERR("[PROC] '%s' es un directorio", path);
    return NULL;
  }
  if (!node->ops || !node->ops->read) {
    vfs_node_free(node);
    LOG_ERR("[PROC] '%s' no es legible", path);
    return NULL;
  }

  struct elf_vfs_ctx ctx = {.node = node};
  uint64_t file_size = node->size;

  LOG_INFO("[PROC] Cargando '%s' (%llu bytes, argc=%d, streaming)", path,
           (unsigned long long)file_size, argc);

  process_t *p = process_spawn_streaming_with_ppid_args_cwd(
      path, elf_read_vfs, &ctx, file_size, argc, argv, inherited_cwd, ppid);

  vfs_node_free(node);
  return p;
}

static process_t *process_load_with_ppid_args(const char *path, int argc,
                                              const char *const *argv,
                                              uint32_t ppid) {
  return process_load_with_ppid_args_cwd(path, argc, argv, "/", ppid);
}

static process_t *process_load_with_ppid(const char *path, uint32_t ppid) {
  return process_load_with_ppid_args(path, 0, NULL, ppid);
}

process_t *process_spawn_child(process_t *parent, const char *path) {
  const char *cwd = (parent && parent->cwd[0]) ? parent->cwd : "/";
  return process_load_with_ppid_args_cwd(path, 0, NULL, cwd,
                                         parent ? parent->pid : 0);
}

process_t *process_spawn_child_args(process_t *parent, const char *path,
                                    int argc, const char *const *argv) {
  const char *cwd = (parent && parent->cwd[0]) ? parent->cwd : "/";
  return process_load_with_ppid_args_cwd(path, argc, argv, cwd,
                                         parent ? parent->pid : 0);
}

process_t *process_load(const char *path) {
  return process_load_with_ppid(path, 0);
}

// ---------------------------------------------------------------------------
// [SIG] Búsqueda de proceso por PID. Solo devuelve procesos vivos (no
// zombies). Se usa en sys_kill_k. El llamante no tiene refcount propio;
// el PID solo se recicla tras reaper, así que mientras exista este
// proceso en process_list, es válido.
// ---------------------------------------------------------------------------
process_t *process_find_by_pid(uint32_t pid) {
  unsigned long flags = spin_lock_irqsave(&process_lock);
  process_t *p = process_list;
  process_t *found = NULL;
  while (p) {
    if (p->pid == pid && !p->is_zombie) {
      found = p;
      break;
    }
    p = p->next;
  }
  spin_unlock_irqrestore(&process_lock, flags);
  return found;
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
      uint32_t zpid = found_zombie->pid;
      int exit_code = found_zombie->exit_code;

      if (status_out) {
        stac();
        *status_out = exit_code;
        clac();
      }

      if (zombie_task) {
        while (__atomic_load_n(&zombie_task->on_cpu, __ATOMIC_ACQUIRE))
          __asm__ volatile("pause");

        zombie_task->proc = NULL;
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

  for (int f = 0; f < MAX_PROCESS_FDS; f++) {
    if (proc->fds[f])
      vfs_close_for_proc(proc, f);
  }

  if (proc->task)
    winsrv_cleanup_task(proc->task);

  task_t *cur = sched_current();
  if (cur && cur == proc->task)
    cur->state = TASK_DEAD;

  process_t *parent = NULL;
  unsigned long flags = spin_lock_irqsave(&process_lock);
  proc->exit_code = exit_code;
  proc->is_zombie = 1;

  process_t *p = process_list;
  while (p) {
    if (p->pid == proc->ppid) {
      parent = p;
      break;
    }
    p = p->next;
  }
  spin_unlock_irqrestore(&process_lock, flags);

  if (parent)
    wake_up_all(&parent->child_wq);
}

__attribute__((noreturn)) void process_exit_current(int exit_code) {
  process_t *proc = process_current();
  task_t *cur = sched_current();

  if (proc)
    process_exit(proc, exit_code);

  if (cur) {
    cur->state = TASK_DEAD;
  }

  while (1) {
    sched_yield();
    __asm__ volatile("sti; hlt");
  }
}

void *process_sbrk(process_t *proc, int64_t increment) {
  if (!proc)
    return (void *)-1;

  uint64_t old_brk = proc->heap_end;
  if (increment == 0)
    return (void *)old_brk;

  if (increment > 0) {
    uint64_t new_brk = old_brk + (uint64_t)increment;
    if (new_brk > proc->heap_max || new_brk < old_brk)
      return (void *)-1;

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
  }

  uint64_t decr = (uint64_t)(-(increment + 1)) + 1;
  if (decr > (old_brk - proc->heap_start))
    return (void *)-1;

  uint64_t new_brk = old_brk - decr;
  uint64_t old_end_page = (old_brk + PAGE_SIZE - 1) & ~0xFFFULL;
  uint64_t new_end_page = (new_brk + PAGE_SIZE - 1) & ~0xFFFULL;

  uint64_t *pml4 = (uint64_t *)phys_to_virt(proc->pml4_phys);
  for (uint64_t page = new_end_page; page < old_end_page; page += PAGE_SIZE) {
    uint64_t phys = paging_get_phys_in(pml4, page);
    if (phys) {
      if (paging_unmap_page_in(pml4, page) == 0)
        pmm_free_page(phys);
    }
  }

  proc->heap_end = new_brk;
  return (void *)old_brk;
}