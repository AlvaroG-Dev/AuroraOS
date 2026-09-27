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
// [musl] Info necesaria para construir el auxv. La rellena
// process_spawn_streaming_* justo después de elf_load_streaming.
// ---------------------------------------------------------------------------
typedef struct {
  uint64_t phdr_vaddr; // AT_PHDR
  uint64_t entry;      // AT_ENTRY
  uint16_t phnum;      // AT_PHNUM
  uint16_t phent;      // AT_PHENT
} proc_auxv_info_t;

// ---------------------------------------------------------------------------
// [Fase 3.2 / musl] Construcción del arg block + envp + auxv en el stack.
//
// Layout al entrar al entry point (x86_64 SysV, Linux ABI):
//
//   [rsp + 0]                   = argc
//   [rsp + 8]                   = argv[0]
//   ...
//   [rsp + 8*(argc+1)]          = NULL                (terminador argv)
//   [rsp + 8*(argc+2)]          = envp[0]
//   ...
//   [rsp + 8*(argc+2+envc)]     = NULL                (terminador envp)
//   [rsp + 8*(argc+3+envc)]     = auxv[0].type
//   [rsp + 8*(argc+3+envc)+8]   = auxv[0].val
//   ...
//   [rsp + ...]                 = AT_NULL (0), 0
//   [rsp + ...]                 = "arg0\0arg1\0...\0execfn\0" + 16 bytes random
//
// El puntero de AT_RANDOM apunta a 16 bytes aleatorios en la zona de strings.
// El puntero de AT_EXECFN apunta al path del binario.
//
// Devuelve el nuevo RSP (16-byte aligned) o 0 en error.
// ---------------------------------------------------------------------------
static uint64_t setup_arg_block(uint64_t *pml4, uint64_t stack_top, int argc,
                                const char *const *argv, int envc,
                                const char *const *envp,
                                const proc_auxv_info_t *ai,
                                const char *execfn) {
  if (argc < 0 || argc > PROCESS_ARGV_MAX)
    return 0;
  if (argc > 0 && !argv)
    return 0;
  if (envc < 0 || envc > 32)
    return 0;
  if (envc > 0 && !envp)
    return 0;

  // --- Medir strings ---
  uint64_t argv_strings = 0;
  for (int i = 0; i < argc; i++) {
    if (!argv[i])
      return 0;
    size_t n = strlen(argv[i]) + 1;
    if (n > 4096)
      return 0;
    argv_strings += n;
  }
  uint64_t envp_strings = 0;
  for (int i = 0; i < envc; i++) {
    if (!envp[i])
      return 0;
    size_t n = strlen(envp[i]) + 1;
    if (n > 4096)
      return 0;
    envp_strings += n;
  }
  uint64_t execfn_len = execfn ? (strlen(execfn) + 1) : 1;
  if (execfn_len > 4096)
    execfn_len = 4096;

  // --- Tamaños de cada zona ---
  const int N_AUXV_TOTAL = 18; // 17 entradas reales + AT_NULL

  uint64_t argc_sz = 8;
  uint64_t argv_sz = ((uint64_t)argc + 1) * 8;
  uint64_t envp_sz = ((uint64_t)envc + 1) * 8;
  uint64_t auxv_sz = (uint64_t)N_AUXV_TOTAL * 16;
  uint64_t strings_sz = argv_strings + envp_strings + execfn_len + 16;
  strings_sz = (strings_sz + 7) & ~7ULL;

  uint64_t total = argc_sz + argv_sz + envp_sz + auxv_sz + strings_sz;
  total = (total + 15) & ~15ULL;
  if (total > stack_top)
    return 0;
  uint64_t new_rsp = (stack_top - total) & ~0xFULL;

  uint8_t *scratch = (uint8_t *)kmalloc(total);
  if (!scratch)
    return 0;
  memset(scratch, 0, total);

  uint64_t off = 0;
  *(uint64_t *)(scratch + off) = (uint64_t)argc;
  off += 8;

  // La zona de strings empieza justo después del auxv.
  uint64_t str_off = argc_sz + argv_sz + envp_sz + auxv_sz;

  // --- argv[] ---
  for (int i = 0; i < argc; i++) {
    size_t n = strlen(argv[i]) + 1;
    memcpy(scratch + str_off, argv[i], n);
    *(uint64_t *)(scratch + off) = new_rsp + str_off;
    off += 8;
    str_off += n;
  }
  *(uint64_t *)(scratch + off) = 0; // argv terminator
  off += 8;

  // --- envp[] ---
  for (int i = 0; i < envc; i++) {
    size_t n = strlen(envp[i]) + 1;
    memcpy(scratch + str_off, envp[i], n);
    *(uint64_t *)(scratch + off) = new_rsp + str_off;
    off += 8;
    str_off += n;
  }
  *(uint64_t *)(scratch + off) = 0; // envp terminator
  off += 8;

  // --- 16 bytes random para AT_RANDOM ---
  uint64_t random_va = new_rsp + str_off;
  {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    uint64_t r = ((uint64_t)hi << 32) | lo;
    r ^= 0x9E3779B97F4A7C15ULL;
    r ^= (r << 13);
    r ^= (r >> 7);
    r ^= (r << 17);
    memcpy(scratch + str_off, &r, 8);
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    uint64_t r2 = ((uint64_t)hi << 32) | lo;
    r2 ^= 0xBF58476D1CE4E5B9ULL;
    memcpy(scratch + str_off + 8, &r2, 8);
  }
  str_off += 16;

  // --- AT_EXECFN string ---
  uint64_t execfn_va = new_rsp + str_off;
  if (execfn) {
    size_t n = strlen(execfn) + 1;
    memcpy(scratch + str_off, execfn, n);
    str_off += n;
  } else {
    scratch[str_off] = '\0';
    str_off += 1;
  }

// --- auxv ---
#define PUSH_AUXV(t, v)                                                        \
  do {                                                                         \
    *(uint64_t *)(scratch + off) = (uint64_t)(t);                              \
    off += 8;                                                                  \
    *(uint64_t *)(scratch + off) = (uint64_t)(v);                              \
    off += 8;                                                                  \
  } while (0)

  PUSH_AUXV(3, ai ? ai->phdr_vaddr : 0);      // AT_PHDR
  PUSH_AUXV(4, ai ? (uint64_t)ai->phent : 0); // AT_PHENT
  PUSH_AUXV(5, ai ? (uint64_t)ai->phnum : 0); // AT_PHNUM
  PUSH_AUXV(6, PAGE_SIZE);                    // AT_PAGESZ
  PUSH_AUXV(7, 0);                            // AT_BASE (no ld.so)
  PUSH_AUXV(8, 0);                            // AT_FLAGS
  PUSH_AUXV(9, ai ? ai->entry : 0);           // AT_ENTRY
  PUSH_AUXV(11, 0);                           // AT_UID
  PUSH_AUXV(12, 0);                           // AT_EUID
  PUSH_AUXV(13, 0);                           // AT_GID
  PUSH_AUXV(14, 0);                           // AT_EGID
  PUSH_AUXV(16, 0);                           // AT_HWCAP
  PUSH_AUXV(17, 100);                         // AT_CLKTCK
  PUSH_AUXV(23, 0);                           // AT_SECURE
  PUSH_AUXV(25, random_va);                   // AT_RANDOM
  PUSH_AUXV(26, 0);                           // AT_HWCAP2
  PUSH_AUXV(31, execfn_va);                   // AT_EXECFN
  PUSH_AUXV(0, 0);                            // AT_NULL
#undef PUSH_AUXV

  // Los dos "carriles" (off = punteros, str_off = strings) son
  // independientes. off termina exactamente en
  // argc_sz+argv_sz+envp_sz+auxv_sz. str_off termina en eso + los bytes
  // reales de strings, que puede ser < strings_sz por el redondeo a 8.
  // Lo único que hay que garantizar es que str_off no se salga del buffer.
  if (off > total || str_off > total) {
    LOG_ERR("[PROC] setup_arg_block: overflow off=%llu str_off=%llu total=%llu",
            (unsigned long long)off, (unsigned long long)str_off,
            (unsigned long long)total);
    kfree(scratch);
    return 0;
  }

  // --- Copiar al stack del usuario ---
  uint64_t va = new_rsp;
  uint64_t src = 0;
  while (src < total) {
    uint64_t page_va = va & ~0xFFFULL;
    uint64_t page_off = va & 0xFFFULL;
    uint64_t avail = PAGE_SIZE - page_off;
    uint64_t chunk = total - src;
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

  proc->fs_base = 0;

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
static process_t *process_spawn_streaming_with_ppid_args_cwd_fds(
    const char *name, elf_read_fn read, void *read_ctx, uint64_t file_size,
    int argc, const char *const *argv, const char *inherited_cwd,
    const spawn_fds_t *fds, process_t *parent, uint32_t ppid) {
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
  uint64_t phdr_vaddr = 0;
  uint16_t phnum = 0;
  uint16_t phent = 0;
  if (elf_load_streaming(read, read_ctx, file_size, pml4, load_base, &entry,
                         &elf_vma_start, &elf_vma_end, &phdr_vaddr, &phnum,
                         &phent) != 0) {
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

  proc_auxv_info_t ai = {
      .phdr_vaddr = phdr_vaddr,
      .entry = entry,
      .phnum = phnum,
      .phent = phent,
  };
  uint64_t user_rsp =
      setup_arg_block(pml4, stack_top, argc, argv, 0, NULL, &ai, name);
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

  proc->fs_base = 0;
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

  // [pipe] Stdio del hijo. Si el llamante pasó fds concretos, los
  // compartimos con el padre (mismo file_descriptor_t, ref_count++).
  // Si no, creamos stdio nuevo apuntando a los nodos globales.
  if (fds && parent) {
    if (fds->fd_in != -1) {
      file_descriptor_t *fd = parent->fds[fds->fd_in];
      fd->ref_count++;
      proc->fds[0] = fd;
    } else {
      proc->fds[0] = vfs_create_stdio_fd(0);
    }
    if (fds->fd_out != -1) {
      file_descriptor_t *fd = parent->fds[fds->fd_out];
      fd->ref_count++;
      proc->fds[1] = fd;
    } else {
      proc->fds[1] = vfs_create_stdio_fd(1);
    }
    if (fds->fd_err != -1) {
      file_descriptor_t *fd = parent->fds[fds->fd_err];
      fd->ref_count++;
      proc->fds[2] = fd;
    } else {
      proc->fds[2] = vfs_create_stdio_fd(2);
    }
  } else {
    proc->fds[0] = vfs_create_stdio_fd(0);
    proc->fds[1] = vfs_create_stdio_fd(1);
    proc->fds[2] = vfs_create_stdio_fd(2);
  }

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

static process_t *process_spawn_streaming_with_ppid_args_cwd(
    const char *name, elf_read_fn read, void *read_ctx, uint64_t file_size,
    int argc, const char *const *argv, const char *inherited_cwd,
    uint32_t ppid) {
  return process_spawn_streaming_with_ppid_args_cwd_fds(
      name, read, read_ctx, file_size, argc, argv, inherited_cwd, NULL, NULL,
      ppid);
}

// ---------------------------------------------------------------------------
// Carga desde VFS con/sin argv, y con/sin cwd heredado.
// ---------------------------------------------------------------------------
static process_t *process_load_with_ppid_args_cwd_fds(
    const char *path, int argc, const char *const *argv,
    const char *inherited_cwd, const spawn_fds_t *fds, process_t *parent,
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

  process_t *p = process_spawn_streaming_with_ppid_args_cwd_fds(
      path, elf_read_vfs, &ctx, file_size, argc, argv, inherited_cwd, fds,
      parent, ppid);

  vfs_node_free(node);
  return p;
}

static process_t *process_load_with_ppid_args_cwd(const char *path, int argc,
                                                  const char *const *argv,
                                                  const char *inherited_cwd,
                                                  uint32_t ppid) {
  return process_load_with_ppid_args_cwd_fds(path, argc, argv, inherited_cwd,
                                             NULL, NULL, ppid);
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

process_t *process_spawn_child_args_fds(process_t *parent, const char *path,
                                        int argc, const char *const *argv,
                                        const spawn_fds_t *fds) {
  const char *cwd = (parent && parent->cwd[0]) ? parent->cwd : "/";
  return process_load_with_ppid_args_cwd_fds(path, argc, argv, cwd, fds, parent,
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

// Publica una señal bajo process_lock y toma una referencia de la tarea
// antes de soltarlo. Así waitpid() no puede liberar el process_t entre la
// búsqueda del PID y el acceso a pending_signals/task.
task_t *process_signal_pid(uint32_t pid, uint64_t signal_mask) {
  unsigned long flags = spin_lock_irqsave(&process_lock);
  process_t *p = process_list;
  while (p) {
    if (p->pid == pid && !p->is_zombie) {
      task_t *task = p->task;
      if (task)
        task_get(task);
      __atomic_fetch_or(&p->pending_signals, signal_mask, __ATOMIC_RELEASE);
      spin_unlock_irqrestore(&process_lock, flags);
      return task;
    }
    p = p->next;
  }
  spin_unlock_irqrestore(&process_lock, flags);
  return NULL;
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

  task_t *parent_task = NULL;
  unsigned long flags = spin_lock_irqsave(&process_lock);
  proc->exit_code = exit_code;
  proc->is_zombie = 1;

  process_t *p = process_list;
  while (p) {
    if (p->pid == proc->ppid) {
      // Mantener viva la task del padre permite despertar su wait queue
      // después de soltar process_lock sin conservar un process_t que
      // pueda ser liberado por el reaper concurrentemente.
      parent_task = p->task;
      if (parent_task)
        task_get(parent_task);
      break;
    }
    p = p->next;
  }
  spin_unlock_irqrestore(&process_lock, flags);

  if (parent_task) {
    wait_queue_wake_task(parent_task);
    task_put(parent_task);
  }
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