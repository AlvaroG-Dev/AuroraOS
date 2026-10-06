// kernel/process.c
#include "process.h"
#include "cpu.h"
#include "elf.h"
#include "futex.h"
#include "gfx/winsrv.h"
#include "heap.h"
#include "klog.h"
#include "mutex.h"
#include "paging.h"
#include "pf.h"
#include "pmm.h"
#include "pty.h"
#include "sched.h"
#include "swap.h"
#include "serial.h"
#include "string.h"
#include "tarfs.h"
#include "uaccess.h"
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

// [4.5] Defaults POSIX. NOFILE limitado a MAX_PROCESS_FDS; NPROC
// acotado para que un fork bomb no llene la tabla. El resto infinito.
static void process_init_rlimits(process_t *proc) {
  if (!proc)
    return;
  for (int i = 0; i < RLIM_NLIMITS; i++) {
    proc->rlimits[i].rlim_cur = RLIM_INFINITY;
    proc->rlimits[i].rlim_max = RLIM_INFINITY;
  }
  proc->rlimits[RLIMIT_NOFILE].rlim_cur = MAX_PROCESS_FDS;
  proc->rlimits[RLIMIT_NOFILE].rlim_max = MAX_PROCESS_FDS;
  proc->rlimits[RLIMIT_NPROC].rlim_cur = 256;
  proc->rlimits[RLIMIT_NPROC].rlim_max = 256;
  proc->rlimits[RLIMIT_STACK].rlim_cur = 8 * 1024 * 1024;
  proc->rlimits[RLIMIT_STACK].rlim_max = RLIM_INFINITY;
}

static void set_proc_name_from_path(process_t *proc, const char *path) {
  const char *base = path;
  for (const char *p = path; *p; p++)
    if (*p == '/')
      base = p + 1;
  size_t n = 0;
  while (base[n] && n < sizeof(proc->name) - 1) {
    proc->name[n] = base[n];
    n++;
  }
  proc->name[n] = '\0';
}

void process_init(void) { spin_init(&process_lock); }

// ---------------------------------------------------------------------------
// [ENV] Helpers de entorno por proceso.
// ---------------------------------------------------------------------------
void process_clear_envp(process_t *proc) {
  if (!proc)
    return;
  for (int i = 0; i < PROCESS_ENVP_MAX; i++) {
    if (proc->envp[i]) {
      kfree(proc->envp[i]);
      proc->envp[i] = NULL;
    }
  }
}

int process_set_envp(process_t *proc, int envc, const char *const *envp) {
  if (!proc)
    return -EINVAL;
  if (envc < 0 || envc >= PROCESS_ENVP_MAX)
    return -EINVAL;

  process_clear_envp(proc);
  if (!envp || envc == 0)
    return 0;

  for (int i = 0; i < envc; i++) {
    if (!envp[i]) {
      process_clear_envp(proc);
      return -EINVAL;
    }
    size_t n = strlen(envp[i]);
    if (n == 0 || n >= PROCESS_ENV_STR_MAX) {
      process_clear_envp(proc);
      return -E2BIG;
    }
    char *copy = (char *)kmalloc(n + 1);
    if (!copy) {
      process_clear_envp(proc);
      return -ENOMEM;
    }
    memcpy(copy, envp[i], n + 1);
    proc->envp[i] = copy;
  }
  return 0;
}

int process_inherit_envp(const process_t *parent, process_t *child) {
  if (!parent || !child)
    return -EINVAL;
  process_clear_envp(child);
  for (int i = 0; i < PROCESS_ENVP_MAX && parent->envp[i]; i++) {
    size_t n = strlen(parent->envp[i]);
    char *copy = (char *)kmalloc(n + 1);
    if (!copy) {
      process_clear_envp(child);
      return -ENOMEM;
    }
    memcpy(copy, parent->envp[i], n + 1);
    child->envp[i] = copy;
  }
  return 0;
}

// ---------------------------------------------------------------------------
// [3.3.d] argv del proceso.
// ---------------------------------------------------------------------------
void process_clear_argv(process_t *proc) {
  if (!proc)
    return;
  for (int i = 0; i < PROCESS_ARGV_MAX; i++) {
    if (proc->argv[i]) {
      kfree(proc->argv[i]);
      proc->argv[i] = NULL;
    }
  }
  proc->argc = 0;
}

int process_set_argv(process_t *proc, int argc, const char *const *argv) {
  if (!proc)
    return -EINVAL;
  if (argc < 0 || argc > PROCESS_ARGV_MAX)
    return -EINVAL;

  process_clear_argv(proc);
  if (!argv || argc == 0)
    return 0;

  for (int i = 0; i < argc; i++) {
    if (!argv[i]) {
      process_clear_argv(proc);
      return -EINVAL;
    }
    // [FIX] argv[i] puede ser "" (string vacío). Linux lo permite:
    // `grep "" fichero`, `awk '...' ""`, etc. Solo rechazamos si
    // excede el límite o es NULL (ya filtrado arriba).
    size_t n = strlen(argv[i]);
    if (n >= PROCESS_ENV_STR_MAX) {
      process_clear_argv(proc);
      return -E2BIG;
    }
    char *copy = (char *)kmalloc(n + 1);
    if (!copy) {
      process_clear_argv(proc);
      return -ENOMEM;
    }
    memcpy(copy, argv[i], n + 1);
    proc->argv[i] = copy;
  }
  proc->argc = argc;
  return 0;
}

int process_inherit_argv(const process_t *parent, process_t *child) {
  if (!parent || !child)
    return -EINVAL;
  process_clear_argv(child);
  for (int i = 0; i < parent->argc && i < PROCESS_ARGV_MAX; i++) {
    if (!parent->argv[i])
      break;
    size_t n = strlen(parent->argv[i]);
    char *copy = (char *)kmalloc(n + 1);
    if (!copy) {
      process_clear_argv(child);
      return -ENOMEM;
    }
    memcpy(copy, parent->argv[i], n + 1);
    child->argv[i] = copy;
  }
  child->argc = parent->argc;
  return 0;
}

// [3.3.d] Iterador de process_list. cb bajo process_lock.
void process_for_each(process_iter_cb_t cb, void *arg) {
  if (!cb)
    return;
  unsigned long flags = spin_lock_irqsave(&process_lock);
  process_t *p = process_list;
  while (p) {
    process_t *next = p->next; // por si cb alterase algo (no debería)
    if (cb(p, arg) != 0)
      break;
    p = next;
  }
  spin_unlock_irqrestore(&process_lock, flags);
}

// ---------------------------------------------------------------------------
// [RUSAGE] Contabilidad de CPU por proceso.
//
// La tarea solo corre en una CPU a la vez (garantizado por on_cpu y
// el scheduler), así que un `++` sin lock sobre proc->cpu_ticks_user
// es correcto: nadie más escribe este campo concurrentemente.
//
// Solo cuentan tareas con proc != NULL. Las idle y kmain_task no son
// procesos y no aparecen en wait4.
// ---------------------------------------------------------------------------
void process_account_tick(task_t *t) {
  if (t && t->proc)
    t->proc->cpu_ticks_user++;
}

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
  for (int i = 0; i < SIG_MAX; i++) {
    proc->sigactions[i].handler = SIG_DFL;
    proc->sigactions[i].flags = 0;
    proc->sigactions[i].restorer = NULL;
    proc->sigactions[i].mask = 0;
  }
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
                                const proc_auxv_info_t *ai, const char *execfn,
                                uint64_t at_base) {
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
  PUSH_AUXV(7, at_base);                      // AT_BASE ← CAMBIO
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
// [3.4.b] Inicializa las credenciales de un proceso recién creado.
// Todo arranca como root con umask 022 (default POSIX).
// ---------------------------------------------------------------------------
static void process_init_creds(process_t *proc) {
  if (!proc)
    return;
  proc->uid = 0;
  proc->euid = 0;
  proc->suid = 0;
  proc->fsuid = 0;
  proc->gid = 0;
  proc->egid = 0;
  proc->sgid = 0;
  proc->fsgid = 0;
  proc->umask = 022;
  // [5.1] Coherente con Linux: el proceso inicial arranca con el grupo
  // root como suplementario. Sin esto, `id` omite `groups=` y algunos
  // applets que iteran getgroups() ven una lista vacía.
  proc->ngroups = 1;
  proc->groups[0] = 0;
  for (int i = 1; i < NGROUPS_MAX; i++)
    proc->groups[i] = 0;

  process_init_rlimits(proc);
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
  proc->team_size = 1;
  proc->ctty = NULL;

  set_proc_name_from_path(proc, name);
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

  process_init_creds(proc);

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

  proc->fd_cloexec_mask = 0;

  mutex_init(&proc->mm_lock);
  mutex_init(&proc->fd_lock);
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
    int argc, const char *const *argv, int envc, const char *const *envp,
    const char *inherited_cwd, const spawn_fds_t *fds, process_t *parent,
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
  uint64_t phdr_vaddr = 0;
  uint16_t phnum = 0, phent = 0;
  char interp_path[VFS_PATH_MAX];
  size_t interp_len = 0;

  if (elf_load_streaming(read, read_ctx, file_size, pml4, load_base, &entry,
                         &elf_vma_start, &elf_vma_end, &phdr_vaddr, &phnum,
                         &phent, interp_path, sizeof(interp_path),
                         &interp_len) != 0) {
    LOG_ERR("[PROC] Fallo al cargar ELF (streaming)");
    paging_free_user_space(pml4_phys);
    return NULL;
  }

  // --- Cargar intérprete si PT_INTERP ---
  uint64_t at_base = 0;
  uint64_t final_entry = entry;
  uint64_t interp_vma_start = 0, interp_vma_end = 0;

  if (interp_len > 0) {
    LOG_INFO("[PROC] Cargando intérprete '%s'", interp_path);

    vfs_node_t *inode = vfs_lookup(interp_path);
    if (!inode || !inode->ops || !inode->ops->read) {
      if (inode)
        vfs_node_free(inode);
      LOG_ERR("[PROC] Intérprete no accesible");
      paging_free_user_space(pml4_phys);
      return NULL;
    }

    struct elf_vfs_ctx ictx = {.node = inode};
    uint64_t interp_load_base = 0x00007f0000000000ULL;
    uint64_t interp_entry = 0, interp_phdr = 0;
    uint16_t interp_phnum = 0, interp_phent = 0;

    int irc = elf_load_streaming(
        elf_read_vfs, &ictx, inode->size, pml4, interp_load_base, &interp_entry,
        &interp_vma_start, &interp_vma_end, &interp_phdr, &interp_phnum,
        &interp_phent, NULL, 0, NULL);
    vfs_node_free(inode);
    if (irc != 0) {
      LOG_ERR("[PROC] Fallo al cargar intérprete");
      paging_free_user_space(pml4_phys);
      return NULL;
    }

    at_base = interp_load_base;
    final_entry = interp_entry;
    LOG_INFO("[PROC] AT_BASE=%p, entry final=%p", (void *)at_base,
             (void *)final_entry);
  }

  // --- Stack ---
  uint64_t stack_base = USER_STACK_BASE;
  uint64_t stack_top = stack_base + USER_STACK_SIZE;
  for (uint64_t off = 0; off < USER_STACK_SIZE; off += PAGE_SIZE) {
    uint64_t phys = pmm_alloc_page();
    if (!phys) {
      paging_free_user_space(pml4_phys);
      return NULL;
    }
    memset(phys_to_virt(phys), 0, PAGE_SIZE);
    if (paging_map_page_in(pml4, stack_base + off, phys,
                           PTE_USER | PTE_WRITABLE | PTE_PRESENT | PTE_NX) !=
        0) {
      pmm_free_page(phys);
      paging_free_user_space(pml4_phys);
      return NULL;
    }
  }

  // --- auxv ---
  proc_auxv_info_t ai = {
      .phdr_vaddr = phdr_vaddr,
      .entry = entry,
      .phnum = phnum,
      .phent = phent,
  };
  uint64_t user_rsp = setup_arg_block(pml4, stack_top, argc, argv, envc, envp,
                                      &ai, name, at_base);
  if (!user_rsp) {
    paging_free_user_space(pml4_phys);
    return NULL;
  }

  // --- Tarea ---
  task_t *task = sched_create_user_task_stopped((void (*)(void))final_entry,
                                                user_rsp, pml4_phys);
  if (!task) {
    paging_free_user_space(pml4_phys);
    return NULL;
  }

  process_t *proc = (process_t *)kzalloc(sizeof(process_t));
  if (!proc) {
    task_put(task);
    paging_free_user_space(pml4_phys);
    return NULL;
  }

  proc->pid = __sync_add_and_fetch(&next_pid, 1) - 1;
  proc->ppid = ppid;
  proc->pgid = proc->pid;
  proc->sid = proc->pid;
  proc->team_size = 1;
  proc->ctty = NULL;
  set_proc_name_from_path(proc, name);
  proc->task = task;
  task_get(task);
  proc->pml4_phys = pml4_phys;
  proc->load_base = load_base;
  proc->heap_start = USER_HEAP_BASE;
  proc->heap_end = USER_HEAP_BASE;
  proc->heap_max = USER_HEAP_MAX;
  proc->next_mmap_addr = 0x0000000060000000ULL;

  proc->vma_list = NULL;
  proc->stack_base = stack_base;
  proc->stack_low = stack_base - MAX_STACK_GROWTH;
  proc->stack_top = stack_top;
  proc->stack_guard = proc->stack_low;
  proc->stack_low += PAGE_SIZE;
  proc->fs_base = 0;
  process_set_cwd(proc, inherited_cwd);
  process_init_signals(proc);

  process_init_creds(proc);

  if (process_set_envp(proc, envc, envp) != 0 ||
      process_set_argv(proc, argc, argv) != 0) {
    process_clear_envp(proc);
    process_clear_argv(proc);
    task_put(task);
    task_put(task);
    paging_free_user_space(pml4_phys);
    kfree(proc);
    return NULL;
  }

  if (elf_vma_start == ~0ULL || elf_vma_start >= elf_vma_end ||
      !vma_create(proc, elf_vma_start, elf_vma_end,
                  PTE_USER | PTE_WRITABLE | PTE_NX, VMA_ELF)) {
    LOG_ERR("[PROC] No se pudo crear VMA ELF");
    process_clear_envp(proc);
    process_clear_argv(proc);
    task_put(task);
    task_put(task);
    paging_free_user_space(pml4_phys);
    kfree(proc);
    return NULL;
  }

  if (interp_len > 0 &&
      !vma_create(proc, interp_vma_start, interp_vma_end,
                  PTE_USER | PTE_WRITABLE | PTE_NX, VMA_ELF)) {
    LOG_ERR("[PROC] No se pudo crear VMA intérprete");
    vma_destroy_all(proc);
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

  proc->fd_cloexec_mask = 0;

  if (fds && parent) {
    if (fds->fd_in != -1) {
      file_descriptor_t *fd = parent->fds[fds->fd_in];
      __atomic_fetch_add(&fd->ref_count, 1, __ATOMIC_ACQ_REL);
      proc->fds[0] = fd;
      struct tty_pty *pty = pty_slave_from_fd(fd);
      if (pty) {
        pty_set_fg_pgid(pty, proc->pid);
        if (pty->slave.session_leader_pid == 0) {
          pty->slave.session_leader_pid = proc->pid;
          proc->ctty = &pty->slave;
        }
      }
    } else {
      proc->fds[0] = vfs_create_stdio_fd(0);
    }
    if (fds->fd_out != -1) {
      file_descriptor_t *fd = parent->fds[fds->fd_out];
      __atomic_fetch_add(&fd->ref_count, 1, __ATOMIC_ACQ_REL);
      proc->fds[1] = fd;
    } else {
      proc->fds[1] = vfs_create_stdio_fd(1);
    }
    if (fds->fd_err != -1) {
      file_descriptor_t *fd = parent->fds[fds->fd_err];
      __atomic_fetch_add(&fd->ref_count, 1, __ATOMIC_ACQ_REL);
      proc->fds[2] = fd;
    } else {
      proc->fds[2] = vfs_create_stdio_fd(2);
    }
  } else {
    proc->fds[0] = vfs_create_stdio_fd(0);
    proc->fds[1] = vfs_create_stdio_fd(1);
    proc->fds[2] = vfs_create_stdio_fd(2);
  }

  mutex_init(&proc->mm_lock);
  mutex_init(&proc->fd_lock);
  wait_queue_init(&proc->child_wq);
  task->proc = proc;

  unsigned long flags = spin_lock_irqsave(&process_lock);
  proc->next = process_list;
  process_list = proc;
  spin_unlock_irqrestore(&process_lock, flags);

  sched_publish_task(task);

  LOG_INFO("[PROC] Proceso '%s' creado (PID=%u, at_base=%p)", name, proc->pid,
           (void *)at_base);
  return proc;
}

static process_t *process_spawn_streaming_with_ppid_args_cwd(
    const char *name, elf_read_fn read, void *read_ctx, uint64_t file_size,
    int argc, const char *const *argv, int envc, const char *const *envp,
    const char *inherited_cwd, uint32_t ppid) {
  return process_spawn_streaming_with_ppid_args_cwd_fds(
      name, read, read_ctx, file_size, argc, argv, envc, envp, inherited_cwd,
      NULL, NULL, ppid);
}

// ---------------------------------------------------------------------------
// Carga desde VFS con/sin argv, y con/sin cwd heredado.
// ---------------------------------------------------------------------------
static process_t *process_load_with_ppid_args_cwd_fds(
    const char *path, int argc, const char *const *argv, int envc,
    const char *const *envp, const char *inherited_cwd, const spawn_fds_t *fds,
    process_t *parent, uint32_t ppid) {
  vfs_node_t *node = vfs_lookup(path);
  if (!node) {
    LOG_TRACE("[PROC] No se pudo abrir '%s'", path);
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

  LOG_INFO("[PROC] Cargando '%s' (%llu bytes, argc=%d, envc=%d, streaming)",
           path, (unsigned long long)file_size, argc, envc);

  process_t *p = process_spawn_streaming_with_ppid_args_cwd_fds(
      path, elf_read_vfs, &ctx, file_size, argc, argv, envc, envp,
      inherited_cwd, fds, parent, ppid);

  vfs_node_free(node);
  return p;
}

static process_t *process_load_with_ppid_args_cwd(
    const char *path, int argc, const char *const *argv, int envc,
    const char *const *envp, const char *inherited_cwd, uint32_t ppid) {
  return process_load_with_ppid_args_cwd_fds(path, argc, argv, envc, envp,
                                             inherited_cwd, NULL, NULL, ppid);
}

static process_t *process_load_with_ppid_args(const char *path, int argc,
                                              const char *const *argv, int envc,
                                              const char *const *envp,
                                              uint32_t ppid) {
  return process_load_with_ppid_args_cwd(path, argc, argv, envc, envp, "/",
                                         ppid);
}

static process_t *process_load_with_ppid(const char *path, uint32_t ppid) {
  return process_load_with_ppid_args(path, 0, NULL, 0, NULL, ppid);
}

process_t *process_spawn_child(process_t *parent, const char *path) {
  const char *cwd = (parent && parent->cwd[0]) ? parent->cwd : "/";
  return process_load_with_ppid_args_cwd(path, 0, NULL, 0, NULL, cwd,
                                         parent ? parent->pid : 0);
}

process_t *process_spawn_child_args(process_t *parent, const char *path,
                                    int argc, const char *const *argv) {
  return process_spawn_child_args_env(parent, path, argc, argv, 0, NULL);
}

process_t *process_spawn_child_args_env(process_t *parent, const char *path,
                                        int argc, const char *const *argv,
                                        int envc, const char *const *envp) {
  const char *cwd = (parent && parent->cwd[0]) ? parent->cwd : "/";
  return process_load_with_ppid_args_cwd(path, argc, argv, envc, envp, cwd,
                                         parent ? parent->pid : 0);
}

process_t *process_spawn_child_args_fds(process_t *parent, const char *path,
                                        int argc, const char *const *argv,
                                        const spawn_fds_t *fds) {
  return process_spawn_child_args_fds_env(parent, path, argc, argv, 0, NULL,
                                          fds);
}

process_t *process_spawn_child_args_fds_env(process_t *parent, const char *path,
                                            int argc, const char *const *argv,
                                            int envc, const char *const *envp,
                                            const spawn_fds_t *fds) {
  const char *cwd = (parent && parent->cwd[0]) ? parent->cwd : "/";
  return process_load_with_ppid_args_cwd_fds(
      path, argc, argv, envc, envp, cwd, fds, parent, parent ? parent->pid : 0);
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
// antes de soltarlo. `*need_interrupt` (si no es NULL) vale 1 solo si quedó
// alguna señal realmente entregable, es decir, si hay que sacar a la tarea
// de un wait interrumpible.
task_t *process_signal_pid_ex(uint32_t pid, uint64_t signal_mask,
                              int *need_interrupt) {
  if (need_interrupt)
    *need_interrupt = 0;

  unsigned long flags = spin_lock_irqsave(&process_lock);
  process_t *p = process_list;
  while (p) {
    if (p->pid == pid && !p->is_zombie) {
      task_t *task = p->task;
      if (task)
        task_get(task);

      // [JOB] SIGCONT despierta a un proceso parado. Consumimos el
      // SIGCONT de la máscara: su efecto (reanudar) se aplica aquí.
      int notify_parent = 0;
      uint32_t ppid = p->ppid;
      if ((signal_mask & (1ULL << SIGCONT)) && p->stopped) {
        p->stopped = 0;
        p->stop_event_pending = 0;
        p->cont_event_pending = 1;
        notify_parent = 1;
        signal_mask &= ~(1ULL << SIGCONT);
      } else if ((signal_mask & (1ULL << SIGKILL)) && p->stopped) {
        // SIGKILL a un proceso parado lo despierta para que muera en
        // su próximo syscall. SIGKILL sigue pendiente.
        p->stopped = 0;
        p->stop_event_pending = 0;
        notify_parent = 1;
      }

      // [FIX] Las señales ignoradas se descartan aquí (como en Linux).
      uint64_t before = signal_mask;
      signal_mask = signal_filter_ignored(p, signal_mask);
      if (before != signal_mask)
        LOG_DEBUG("[SIG] pid=%u: descartadas señales ignoradas 0x%lx", p->pid,
                  (unsigned long)(before & ~signal_mask));

      if (signal_mask)
        __atomic_fetch_or(&p->pending_signals, signal_mask, __ATOMIC_RELEASE);
      spin_unlock_irqrestore(&process_lock, flags);

      if (need_interrupt)
        *need_interrupt = (signal_mask != 0);

      if (notify_parent) {
        if (task)
          sched_cont_task(task);
        process_wake_parent(ppid);
      }
      return task;
    }
    p = p->next;
  }
  spin_unlock_irqrestore(&process_lock, flags);
  return NULL;
}

// Wrapper de compatibilidad (misma firma que antes).
task_t *process_signal_pid(uint32_t pid, uint64_t signal_mask) {
  return process_signal_pid_ex(pid, signal_mask, NULL);
}

// ---------------------------------------------------------------------------
// Condición de despertar: ¿existe algún hijo zombie que satisfaga el filtro?
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// [JOB] Condición de despertar del padre: ¿hay un evento listo?
//   - zombie (siempre)
//   - stop    (solo si options & WUNTRACED)
//   - cont    (solo si options & WCONTINUED)
// ---------------------------------------------------------------------------
typedef struct {
  process_t *parent;
  int32_t pid;
  int options;
} waitpid_ctx_t;

// [JOB] Filter extendido: pid>0, pid==0, pid==-1, pid<-1.
static bool wait_pid_matches(process_t *p, process_t *parent, int32_t pid) {
  if (p->ppid != parent->pid)
    return false;
  if (pid > 0)
    return (uint32_t)pid == p->pid;
  if (pid == -1)
    return true;
  if (pid == 0)
    return p->pgid == parent->pgid;
  return p->pgid == (uint32_t)(-pid);
}

static bool has_matching_event(void *arg) {
  waitpid_ctx_t *ctx = (waitpid_ctx_t *)arg;
  process_t *parent = ctx->parent;
  int32_t pid = ctx->pid;
  int options = ctx->options;

  unsigned long flags = spin_lock_irqsave(&process_lock);
  process_t *p = process_list;
  bool found = false;
  while (p) {
    if (wait_pid_matches(p, parent, pid)) {
      if (p->is_zombie) {
        found = true;
        break;
      }
      if ((options & WUNTRACED) && p->stop_event_pending) {
        found = true;
        break;
      }
      if ((options & WCONTINUED) && p->cont_event_pending) {
        found = true;
        break;
      }
    }
    p = p->next;
  }
  spin_unlock_irqrestore(&process_lock, flags);
  return found;
}

// ---------------------------------------------------------------------------
// [JOB] process_waitpid.
//
// Devuelve:
//   >0  PID del hijo cuyo evento se reporta. status_out contiene el
//       código de estado YA en formato Linux:
//         exited:    (code & 0xff) << 8
//         stopped:   ((sig & 0xff) << 8) | 0x7f
//         continued: 0xffff
//    0  WNOHANG sin eventos listos.
//   -1  no hay hijos que cumplan el filtro.
// ---------------------------------------------------------------------------
int process_waitpid(process_t *parent, int32_t pid, int *status_out,
                    proc_rusage_t *rusage_out, int options) {
  if (!parent)
    return -EINVAL;

  waitpid_ctx_t ctx = {.parent = parent, .pid = pid, .options = options};

  while (1) {
    process_t *found_zombie = NULL;
    process_t *found_stopped = NULL;
    process_t *found_continued = NULL;
    task_t *zombie_task = NULL;
    int has_matching_child = 0;
    int stopped_sig = 0;

    unsigned long flags = spin_lock_irqsave(&process_lock);
    process_t *p = process_list;
    while (p) {
      // [JOB] Antes: (pid == -1 && ...) || (pid >= 0 && ...) — no cubría
      // pid < -1. BusyBox ash usa wait4(-pgid, WUNTRACED) en fg, así que
      // ese caso es la norma, no la excepción.
      if (wait_pid_matches(p, parent, pid)) {
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

        if (!found_stopped && (options & WUNTRACED) && p->stop_event_pending) {
          found_stopped = p;
          stopped_sig = p->stop_signal;
          p->stop_event_pending = 0;
        }
        if (!found_stopped && !found_continued && (options & WCONTINUED) &&
            p->cont_event_pending) {
          found_continued = p;
          p->cont_event_pending = 0;
        }
      }
      p = p->next;
    }
    spin_unlock_irqrestore(&process_lock, flags);

    if (found_zombie) {
      uint32_t zpid = found_zombie->pid;
      int exit_code = found_zombie->exit_code;
      LOG_TRACE("[WAITPID] reaping pid=%u exit_code=%d", zpid, exit_code);

      if (status_out) {
        stac();
        *status_out = (exit_code & 0xff) << 8; // WIFEXITED
        clac();
      }

      if (rusage_out) {
        rusage_out->utime_ticks = found_zombie->cpu_ticks_user;
        rusage_out->stime_ticks = 0;
        rusage_out->minflt = 0;
        rusage_out->majflt = 0;
        rusage_out->nvcsw = 0;
        rusage_out->nivcsw = 0;
        rusage_out->maxrss_kb = 0;
      }

      if (zombie_task) {
        // [CONCURRENCIA] Esperar a que la tarea no esté en ningún CPU.
        // on_cpu se limpia en task_switch, así que esto es la barrera
        // mínima antes de soltar la referencia.
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

    if (found_stopped) {
      uint32_t spid = found_stopped->pid;
      if (status_out) {
        stac();
        *status_out = ((stopped_sig & 0xff) << 8) | 0x7f; // WIFSTOPPED
        clac();
      }
      if (rusage_out)
        memset(rusage_out, 0, sizeof(*rusage_out));
      return (int)spid;
    }

    if (found_continued) {
      uint32_t cpid = found_continued->pid;
      if (status_out) {
        stac();
        *status_out = 0xffff; // WIFCONTINUED
        clac();
      }
      if (rusage_out)
        memset(rusage_out, 0, sizeof(*rusage_out));
      return (int)cpid;
    }

    if (!has_matching_child) {
      // [FIX ECHILD] Linux: wait4(-1) sin hijos vivos -> ECHILD sin
      // bloquear. Antes devolvíamos -1 (= EPERM), y init entraba en un
      // bucle de "waitpid: Operation not permitted" cada segundo.
      // El dump de process_list que había aquí era útil durante el
      // desarrollo de D; ahora solo genera ruido.
      return -ECHILD;
    }
    if (options & WNOHANG)
      return 0;

    int rc =
        wait_event_interruptible(&parent->child_wq, has_matching_event, &ctx);
    if (rc < 0)
      return -EINTR;
  }
}

void process_exit(process_t *proc, int exit_code) {
  if (!proc)
    return;

  if (proc->is_zombie)
    return;

  LOG_INFO(
      "[EXIT] pid=%u ppid=%u pgid=%u name='%s' code=%d pending=0x%lx "
      "blocked=0x%lx",
      proc->pid, proc->ppid, proc->pgid, proc->name, exit_code,
      (unsigned long)__atomic_load_n(&proc->pending_signals, __ATOMIC_ACQUIRE),
      (unsigned long)proc->blocked_signals);

  process_clear_envp(proc);
  process_clear_argv(proc);

  for (int f = 0; f < MAX_PROCESS_FDS; f++) {
    if (proc->fds[f])
      vfs_close_for_proc(proc, f);
  }

  // [CLONE] Limpiar ventanas del thread ACTUAL, no de proc->task.
  // Si el líder murió antes que otros threads del team, proc->task
  // apunta a una task ya DEAD; limpiar ahí sería un doble cleanup.
  // El último thread en salir es el que tiene ventanas vivas.
  task_t *cur = sched_current();
  if (cur && cur->proc == proc) {
    winsrv_cleanup_task(cur);
    cur->state = TASK_DEAD;
  }

  task_t *parent_task = NULL;
  process_t *parent_proc = NULL;
  task_t *init_task = NULL;

  unsigned long flags = spin_lock_irqsave(&process_lock);
  proc->exit_code = exit_code;
  proc->is_zombie = 1;

  const uint32_t my_pid = proc->pid;
  const uint32_t my_ppid = proc->ppid;

  int reparented = 0;
  process_t *p = process_list;
  while (p) {
    if (p->pid == my_ppid) {
      parent_task = p->task;
      parent_proc = p;
      if (parent_task)
        task_get(parent_task);
    }
    if (my_pid != 1 && p->ppid == my_pid && p->pid != my_pid) {
      p->ppid = 1;
      reparented = 1;
    }
    p = p->next;
  }

  if (my_pid == 1) {
    LOG_ERR("[INIT] PID 1 ha salido. El sistema queda sin reaper.");
  }

  if (reparented) {
    p = process_list;
    while (p) {
      if (p->pid == 1 && p->task) {
        init_task = p->task;
        task_get(init_task);
        break;
      }
      p = p->next;
    }
  }

  spin_unlock_irqrestore(&process_lock, flags);

  if (parent_task) {
    if (parent_proc) {
      __atomic_fetch_or(&parent_proc->pending_signals, 1ULL << SIGCHLD,
                        __ATOMIC_RELEASE);
    }
    wait_queue_wake_task(parent_task);
    task_put(parent_task);
  }
  if (init_task) {
    wait_queue_wake_task(init_task);
    task_put(init_task);
  }
}

__attribute__((noreturn)) void process_exit_current(int exit_code) {
  process_t *proc = process_current();
  task_t *cur = sched_current();

  // [CLONE_CHILD_CLEARTID] Escribir 0 en *clear_child_tid y futex_wake
  // a quien espere (pthread_join). Esto va ANTES de cualquier otra
  // cosa: el joiner puede estar bloqueado en futex_wait esperando
  // justo esta escritura.
  if (cur && cur->clear_child_tid) {
    put_user_u32(cur->clear_child_tid, 0);
    if (proc && proc->pml4_phys)
      futex_wake_user((uint64_t)cur->clear_child_tid, 1, proc->pml4_phys);
    cur->clear_child_tid = NULL;
  }

  if (proc) {
    // [CLONE_THREAD] ¿Somos el último thread del team?
    // Si no, decrementar y limpiar solo lo nuestro. Si sí, hacer
    // el exit "real" del proceso (fds, zombie, SIGCHLD al padre).
    int last;
    unsigned long flags = spin_lock_irqsave(&process_lock);
    if (proc->team_size > 1) {
      proc->team_size--;
      last = 0;
    } else {
      proc->team_size = 0;
      last = 1;
    }
    spin_unlock_irqrestore(&process_lock, flags);

    if (last) {
      process_exit(proc, exit_code);
    } else {
      // Salida de un thread no-líder. Limpiar sus ventanas (su task
      // muere, sus ventanas también) pero NO tocar fds/argv/envp.
      if (cur)
        winsrv_cleanup_task(cur);
    }
  }

  // Releer cur — process_exit puede haber cambiado el current_task si
  // la tarea estaba siendo reemplazada por el scheduler.
  cur = sched_current();
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

// ---------------------------------------------------------------------------
// [fork] Clonar el espacio de usuario: copia cada PTE presente del padre
// a páginas físicas nuevas en el hijo. Sin COW — lento pero simple.
//
// Se hace con walk manual (no paging_get_phys_in por VA, que sería
// 256*512*512 iteraciones): recorremos las tablas directamente y
// saltamos ramas ausentes en un solo test.
// ---------------------------------------------------------------------------
static int clone_user_space(uint64_t *parent_pml4, uint64_t *child_pml4) {
  for (int i = 0; i < 256; i++) {
    uint64_t pml4e = parent_pml4[i];
    if (!(pml4e & PTE_PRESENT))
      continue;
    if (pml4e & PTE_HUGE) {
      LOG_WARN("[FORK] huge page en PML4 (ignorada)");
      continue;
    }
    uint64_t *pdpt_parent = (uint64_t *)phys_to_virt(pml4e & PTE_FRAME);

    for (int j = 0; j < 512; j++) {
      uint64_t pdpte = pdpt_parent[j];
      if (!(pdpte & PTE_PRESENT))
        continue;
      if (pdpte & PTE_HUGE) {
        LOG_WARN("[FORK] 1GB huge page en user space (ignorada)");
        continue;
      }
      uint64_t *pd_parent = (uint64_t *)phys_to_virt(pdpte & PTE_FRAME);

      for (int k = 0; k < 512; k++) {
        uint64_t pde = pd_parent[k];
        if (!(pde & PTE_PRESENT))
          continue;
        if (pde & PTE_HUGE) {
          LOG_WARN("[FORK] 2MB huge page en user space (ignorada)");
          continue;
        }
        uint64_t *pt_parent = (uint64_t *)phys_to_virt(pde & PTE_FRAME);

        for (int l = 0; l < 512; l++) {
          uint64_t pte = pt_parent[l];
          if (pte_is_swap(pte)) {
            uint64_t new_phys = pmm_alloc_page();
            if (!new_phys)
              return -1;
            if (swap_read_page(pte_swap_type(pte), pte_swap_offset(pte),
                               new_phys) != 0) {
              pmm_free_page(new_phys);
              return -1;
            }
            uint64_t virt = ((uint64_t)i << 39) | ((uint64_t)j << 30) |
                            ((uint64_t)k << 21) | ((uint64_t)l << 12);
            if (paging_map_page_in(child_pml4, virt, new_phys,
                                   PTE_USER | PTE_WRITABLE | PTE_PRESENT |
                                       PTE_NX) != 0) {
              pmm_free_page(new_phys);
              return -1;
            }
            continue;
          }
          if (!(pte & PTE_PRESENT))
            continue;

          uint64_t old_phys = pte & PTE_FRAME;
          uint64_t new_phys = pmm_alloc_page();
          if (!new_phys) {
            LOG_ERR("[FORK] sin memoria clonando página");
            return -1;
          }
          memcpy(phys_to_virt(new_phys), phys_to_virt(old_phys), PAGE_SIZE);

          uint64_t virt = ((uint64_t)i << 39) | ((uint64_t)j << 30) |
                          ((uint64_t)k << 21) | ((uint64_t)l << 12);
          // paging_map_page_in enmascara flags con (0xFFF | PTE_NX). Le
          // pasamos el pte entero (con el frame) y ya lo filtra.
          if (paging_map_page_in(child_pml4, virt, new_phys, pte) != 0) {
            pmm_free_page(new_phys);
            LOG_ERR("[FORK] paging_map_page_in falló en %p", (void *)virt);
            return -1;
          }
        }
      }
    }
  }
  return 0;
}

// ---------------------------------------------------------------------------
// [fork] Clonar la lista de VMAs del padre al hijo.
// ---------------------------------------------------------------------------
static int clone_vmas(process_t *parent, process_t *child) {
  child->vma_list = NULL;
  vma_t **tail = &child->vma_list;

  for (vma_t *v = parent->vma_list; v; v = v->next) {
    vma_t *nv = (vma_t *)kzalloc(sizeof(vma_t));
    if (!nv) {
      vma_destroy_all(child);
      return -1;
    }
    nv->start = v->start;
    nv->end = v->end;
    nv->flags = v->flags;
    nv->type = v->type;
    nv->pad = 0;
    nv->next = NULL;

    // [3.5] Compartir el file_descriptor_t con el padre: tomar ref.
    if (v->type == VMA_FILE && v->file_fd) {
      nv->file_fd = v->file_fd;
      __atomic_fetch_add(&nv->file_fd->ref_count, 1, __ATOMIC_ACQ_REL);
      nv->file_offset = v->file_offset;
    }

    *tail = nv;
    tail = &nv->next;
  }
  return 0;
}

// ---------------------------------------------------------------------------
// [fork] sys_fork: implementación.
//
// Requiere que el dispatcher haya dejado los regs del syscall accesibles
// vía syscall_current_regs(). Ver syscall.c.
// ---------------------------------------------------------------------------
extern registers_t *syscall_current_regs(void);

int64_t sys_fork(void) {
  process_t *parent = process_current();
  if (!parent) {
    LOG_ERR("[FORK] sin proceso actual");
    return -EINVAL;
  }

  registers_t *regs = syscall_current_regs();
  if (!regs) {
    LOG_ERR("[FORK] sin registers del syscall");
    return -EINVAL;
  }

  // 1. Freeze the parent's address space while its page tables and VMA list
  // are cloned. The lock is sleepable because swapped pages may need I/O.
  mutex_lock(&parent->mm_lock);

  // 2. Clonar PML4 (kernel half compartido).
  uint64_t child_pml4_phys = paging_clone_kernel_space();
  if (!child_pml4_phys) {
    LOG_ERR("[FORK] paging_clone_kernel_space falló");
    mutex_unlock(&parent->mm_lock);
    return -ENOMEM;
  }
  uint64_t *parent_pml4 = (uint64_t *)phys_to_virt(parent->pml4_phys);
  uint64_t *child_pml4 = (uint64_t *)phys_to_virt(child_pml4_phys);

  // 2. Copiar user space.
  if (clone_user_space(parent_pml4, child_pml4) != 0) {
    paging_free_user_space(child_pml4_phys);
    mutex_unlock(&parent->mm_lock);
    return -ENOMEM;
  }

  // 3. Crear tarea del hijo con los regs del padre.
  task_t *child_task = sched_create_forked_user_task(regs, child_pml4_phys);
  if (!child_task) {
    LOG_ERR("[FORK] sched_create_forked_user_task falló");
    paging_free_user_space(child_pml4_phys);
    mutex_unlock(&parent->mm_lock);
    return -ENOMEM;
  }
  child_task->fs_base = parent->fs_base;

  // 4. process_t del hijo.
  process_t *child = (process_t *)kzalloc(sizeof(process_t));
  if (!child) {
    LOG_ERR("[FORK] sin memoria para process_t");
    paging_free_user_space(child_pml4_phys);
    if (child_task->stack)
      kfree(child_task->stack);
    kfree(child_task);
    mutex_unlock(&parent->mm_lock);
    return -ENOMEM;
  }

  child->pid = __sync_add_and_fetch(&next_pid, 1) - 1;
  child->ppid = parent->pid;
  child->pgid = parent->pgid;
  child->sid = parent->sid;
  child->exit_code = 0;
  child->is_zombie = 0;
  child->team_size = 1; // ← AÑADIR
  child->ctty = parent->ctty;

  // Nombre: prefijo con el del padre.
  {
    size_t i = 0;
    while (parent->name[i] && i < sizeof(child->name) - 1) {
      child->name[i] = parent->name[i];
      i++;
    }
    child->name[i] = '\0';
  }

  child->task = child_task;
  task_get(child_task);
  child->pml4_phys = child_pml4_phys;
  child->load_base = parent->load_base;
  child->heap_start = parent->heap_start;
  child->heap_end = parent->heap_end;
  child->heap_max = parent->heap_max;
  child->next_mmap_addr = parent->next_mmap_addr;
  child->stack_base = parent->stack_base;
  child->stack_low = parent->stack_low;
  child->stack_top = parent->stack_top;
  child->stack_guard = parent->stack_guard;

  child->fs_base = parent->fs_base;

  process_set_cwd(child, parent->cwd);
  process_init_signals(child);
  for (int i = 0; i < SIG_MAX; i++)
    child->sigactions[i] = parent->sigactions[i];
  child->blocked_signals = parent->blocked_signals;
  child->pending_signals = 0;

  child->uid = parent->uid;
  child->euid = parent->euid;
  child->suid = parent->suid;
  child->fsuid = parent->fsuid;
  child->gid = parent->gid;
  child->egid = parent->egid;
  child->sgid = parent->sgid;
  child->fsgid = parent->fsgid;
  child->umask = parent->umask;
  child->ngroups = parent->ngroups;
  for (int i = 0; i < NGROUPS_MAX; i++)
    child->groups[i] = parent->groups[i];

  if (process_inherit_envp(parent, child) != 0) {
    LOG_ERR("[FORK] process_inherit_envp falló");
    paging_free_user_space(child_pml4_phys);
    if (child_task->stack)
      kfree(child_task->stack);
    kfree(child_task);
    kfree(child);
    mutex_unlock(&parent->mm_lock);
    return -ENOMEM;
  }

  if (process_inherit_argv(parent, child) != 0) {
    LOG_ERR("[FORK] process_inherit_argv falló");
    process_clear_envp(child);
    paging_free_user_space(child_pml4_phys);
    if (child_task->stack)
      kfree(child_task->stack);
    kfree(child_task);
    kfree(child);
    mutex_unlock(&parent->mm_lock);
    return -ENOMEM;
  }

  for (int i = 0; i < RLIM_NLIMITS; i++)
    child->rlimits[i] = parent->rlimits[i];

  if (clone_vmas(parent, child) != 0) {
    LOG_ERR("[FORK] clone_vmas falló");
    paging_free_user_space(child_pml4_phys);
    if (child_task->stack)
      kfree(child_task->stack);
    kfree(child_task);
    kfree(child);
    mutex_unlock(&parent->mm_lock);
    return -ENOMEM;
  }

  mutex_unlock(&parent->mm_lock);

  for (int f = 0; f < MAX_PROCESS_FDS; f++) {
    child->fds[f] = parent->fds[f];
    if (child->fds[f])
      __atomic_fetch_add(&child->fds[f]->ref_count, 1, __ATOMIC_ACQ_REL);
  }
  child->fd_cloexec_mask = parent->fd_cloexec_mask;

  mutex_init(&child->mm_lock);
  mutex_init(&child->fd_lock);
  wait_queue_init(&child->child_wq);
  child_task->proc = child;

  unsigned long flags = spin_lock_irqsave(&process_lock);
  child->next = process_list;
  process_list = child;
  spin_unlock_irqrestore(&process_lock, flags);

  sched_publish_task(child_task);

  LOG_INFO("[FORK] '%s' (PID=%u) clonado a PID=%u", parent->name, parent->pid,
           child->pid);

  return (int64_t)child->pid;
}

task_t *process_clone_thread(process_t *parent, registers_t *regs,
                             uint64_t stack, uint64_t tls, uint64_t *ptid_out,
                             uint64_t *ctid_set_out, uint64_t *ctid_clear_out) {
  if (!parent || !regs)
    return NULL;

  task_t *child_task =
      sched_create_forked_user_task_ex(regs, parent->pml4_phys, stack);
  if (!child_task) {
    LOG_ERR("[CLONE] sched_create_forked_user_task_ex falló");
    return NULL;
  }

  child_task->fs_base = tls ? tls : parent->fs_base;
  child_task->cpu_affinity = smp_processor_id();

  task_get(child_task);
  child_task->proc = parent;

  unsigned long flags = spin_lock_irqsave(&process_lock);
  parent->team_size++;
  int new_size = parent->team_size;
  spin_unlock_irqrestore(&process_lock, flags);

  uint32_t tid = child_task->id;

  // CLONE_PARENT_SETTID: escribe el tid en el padre (musl usa &self->tid).
  if (ptid_out) {
    if (put_user_u32((uint32_t *)ptid_out, tid) < 0)
      LOG_WARN("[CLONE] PARENT_SETTID falló (tid=%u)", tid);
  }

  // CLONE_CHILD_SETTID: escribe el tid en el hijo. musl 1.2.x NO usa
  // este flag (solo CLEARTID), pero otros libcs sí.
  if (ctid_set_out) {
    if (put_user_u32((uint32_t *)ctid_set_out, tid) < 0)
      LOG_WARN("[CLONE] CHILD_SETTID falló (tid=%u)", tid);
  }

  // CLONE_CHILD_CLEARTID: registra la dirección donde el kernel
  // escribirá 0 al morir el hilo (con futex-wake). musl usa
  // &__thread_list_lock. NO escribir nada aquí — solo guardar el
  // puntero para process_exit_current.
  if (ctid_clear_out) {
    child_task->clear_child_tid = (uint32_t *)ctid_clear_out;
  }

  sched_publish_task(child_task);

  LOG_INFO("[CLONE] thread tid=%u team pid=%u cpu=%d (team_size=%d)", tid,
           parent->pid, child_task->cpu_affinity, new_size);
  return child_task;
}

// ---------------------------------------------------------------------------
// [execve] Reemplazo in-place del espacio de usuario.
//
// No crea proceso nuevo: muta el actual. Conserva PID, fds, cwd.
// En éxito deja *new_entry / *new_rsp listos para que k_execve escriba
// el trap frame del syscall (rip/rsp) y el iretq aterrice en el nuevo
// entry point con el CR3 ya cargado.
//
// En error devuelve un errno negativo y NO toca el proceso.
// ---------------------------------------------------------------------------
int process_execve_prepare(const char *path, int argc, const char *const *argv,
                           int envc, const char *const *envp,
                           uint64_t *new_entry, uint64_t *new_rsp) {
  process_t *proc = process_current();
  if (!proc || !path || !new_entry || !new_rsp)
    return -EINVAL;
  if (argc < 0 || argc > PROCESS_ARGV_MAX)
    return -EINVAL;
  if (envc < 0 || envc > PROCESS_ENVP_MAX)
    return -EINVAL;

  // execve replaces the entire address space. Aurora does not yet have the
  // Linux-style mechanism that terminates/synchronizes sibling threads, so
  // allowing execve() with team_size > 1 would leave sibling tasks running
  // on a freed PML4. Reject it until that machinery exists.
  unsigned long exec_flags = spin_lock_irqsave(&process_lock);
  int has_siblings = (proc->team_size > 1);
  spin_unlock_irqrestore(&process_lock, exec_flags);
  if (has_siblings)
    return -EAGAIN;

  uint32_t tpid = proc->pid;

  vfs_node_t *node = vfs_lookup(path);
  if (!node)
    return -ENOENT;
  if (node->flags & VFS_DIRECTORY) {
    vfs_node_free(node);
    return -EISDIR;
  }
  if (!node->ops || !node->ops->read) {
    vfs_node_free(node);
    return -EACCES;
  }

  if ((node->mode & 0111) == 0) {
    vfs_node_free(node);
    return -EACCES;
  }

  uint32_t new_mode = node->mode;
  uint32_t new_uid = node->uid;
  uint32_t new_gid = node->gid;

  uint64_t new_pml4_phys = paging_clone_kernel_space();
  if (!new_pml4_phys) {
    vfs_node_free(node);
    return -ENOMEM;
  }
  uint64_t *new_pml4 = (uint64_t *)phys_to_virt(new_pml4_phys);

  uint64_t load_base = 0x555555554000ULL;

  struct elf_vfs_ctx ctx = {.node = node};
  uint64_t entry = 0, vma_start = ~0ULL, vma_end = 0;
  uint64_t phdr_vaddr = 0;
  uint16_t phnum = 0, phent = 0;
  char interp_path[VFS_PATH_MAX];
  size_t interp_len = 0;

  int rc =
      elf_load_streaming(elf_read_vfs, &ctx, node->size, new_pml4, load_base,
                         &entry, &vma_start, &vma_end, &phdr_vaddr, &phnum,
                         &phent, interp_path, sizeof(interp_path), &interp_len);
  vfs_node_free(node);
  if (rc != 0) {
    LOG_WARN("[EXECVE-TRACE] pid=%u elf_load_streaming failed rc=%d", tpid, rc);
    paging_free_user_space(new_pml4_phys);
    return rc == -ENOMEM ? -ENOMEM : -ENOEXEC;
  }

  uint64_t at_base = 0;
  uint64_t final_entry = entry;
  uint64_t interp_vma_start = 0, interp_vma_end = 0;

  if (interp_len > 0) {

    vfs_node_t *inode = vfs_lookup(interp_path);
    if (!inode) {
      LOG_ERR("[EXECVE] Intérprete no encontrado: %s", interp_path);
      paging_free_user_space(new_pml4_phys);
      return -ENOENT;
    }
    if (!inode->ops || !inode->ops->read) {
      vfs_node_free(inode);
      paging_free_user_space(new_pml4_phys);
      return -EACCES;
    }

    struct elf_vfs_ctx ictx = {.node = inode};
    uint64_t interp_load_base = 0x00007f0000000000ULL;
    uint64_t interp_entry = 0;
    uint64_t interp_phdr = 0;
    uint16_t interp_phnum = 0, interp_phent = 0;

    int irc = elf_load_streaming(
        elf_read_vfs, &ictx, inode->size, new_pml4, interp_load_base,
        &interp_entry, &interp_vma_start, &interp_vma_end, &interp_phdr,
        &interp_phnum, &interp_phent, NULL, 0, NULL);
    vfs_node_free(inode);
    if (irc != 0) {
      LOG_ERR("[EXECVE] Fallo al cargar intérprete");
      paging_free_user_space(new_pml4_phys);
      return irc == -ENOMEM ? -ENOMEM : -ENOEXEC;
    }

    at_base = interp_load_base;
    final_entry = interp_entry;
  }

  uint64_t stack_base = USER_STACK_BASE;
  uint64_t stack_top = stack_base + USER_STACK_SIZE;
  for (uint64_t off = 0; off < USER_STACK_SIZE; off += PAGE_SIZE) {
    uint64_t phys = pmm_alloc_page();
    if (!phys) {
      paging_free_user_space(new_pml4_phys);
      return -ENOMEM;
    }
    memset(phys_to_virt(phys), 0, PAGE_SIZE);
    if (paging_map_page_in(new_pml4, stack_base + off, phys,
                           PTE_USER | PTE_WRITABLE | PTE_PRESENT | PTE_NX) !=
        0) {
      pmm_free_page(phys);
      paging_free_user_space(new_pml4_phys);
      return -ENOMEM;
    }
  }

  proc_auxv_info_t ai = {
      .phdr_vaddr = phdr_vaddr,
      .entry = entry,
      .phnum = phnum,
      .phent = phent,
  };
  uint64_t user_rsp = setup_arg_block(new_pml4, stack_top, argc, argv, envc,
                                      envp, &ai, path, at_base);
  if (!user_rsp) {
    LOG_WARN("[EXECVE-TRACE] pid=%u setup_arg_block failed", tpid);
    paging_free_user_space(new_pml4_phys);
    return -ENOMEM;
  }

  // ===== COMMIT =====
  uint64_t old_pml4_phys = proc->pml4_phys;

  vma_destroy_all(proc);

  proc->pml4_phys = new_pml4_phys;
  if (proc->task)
    proc->task->cr3 = new_pml4_phys;

  proc->load_base = load_base;
  proc->heap_start = USER_HEAP_BASE;
  proc->heap_end = USER_HEAP_BASE;
  proc->heap_max = USER_HEAP_MAX;
  proc->next_mmap_addr = 0x0000000060000000ULL;
  proc->stack_base = stack_base;
  proc->stack_low = stack_base - MAX_STACK_GROWTH;
  proc->stack_top = stack_top;
  proc->stack_guard = proc->stack_low;
  proc->stack_low += PAGE_SIZE;

  for (int i = 0; i < SIG_MAX; i++) {
    if (proc->sigactions[i].handler != SIG_IGN)
      proc->sigactions[i].handler = SIG_DFL;
    proc->sigactions[i].flags = 0;
    proc->sigactions[i].restorer = NULL;
    proc->sigactions[i].mask = 0;
  }

  int was_root = (proc->euid == 0);
  if (new_mode & S_ISUID) {
    proc->euid = new_uid;
    proc->fsuid = new_uid;
    if (was_root) {
      proc->uid = new_uid;
      proc->suid = new_uid;
    }
  }
  if (new_mode & S_ISGID) {
    proc->egid = new_gid;
    proc->fsgid = new_gid;
    if (was_root) {
      proc->gid = new_gid;
      proc->sgid = new_gid;
    }
  }

  proc->fs_base = 0;
  if (proc->task)
    proc->task->fs_base = 0;

  {
    const char *base = path;
    for (const char *p = path; *p; p++)
      if (*p == '/')
        base = p + 1;
    size_t n = 0;
    while (base[n] && n < sizeof(proc->name) - 1) {
      proc->name[n] = base[n];
      n++;
    }
    proc->name[n] = '\0';
  }

  if (!vma_create(proc, vma_start, vma_end, PTE_USER | PTE_WRITABLE | PTE_NX,
                  VMA_ELF))
    LOG_ERR("[EXECVE] vma_create ELF falló");
  if (interp_len > 0 && !vma_create(proc, interp_vma_start, interp_vma_end,
                                    PTE_USER | PTE_WRITABLE | PTE_NX, VMA_ELF))
    LOG_ERR("[EXECVE] vma_create intérprete falló");
  if (!vma_create(proc, proc->stack_low, proc->stack_top,
                  PTE_USER | PTE_WRITABLE | PTE_NX, VMA_STACK))
    LOG_ERR("[EXECVE] vma_create stack falló");

  if (process_set_envp(proc, envc, envp) != 0)
    LOG_WARN("[EXECVE] process_set_envp falló, entorno conservado");
  if (process_set_argv(proc, argc, argv) != 0)
    LOG_WARN("[EXECVE] process_set_argv falló, argv conservado");

  write_cr3(new_pml4_phys);

  wrmsr(0xC0000100, 0);
  paging_free_user_space(old_pml4_phys);

  for (int f = 0; f < MAX_PROCESS_FDS; f++) {
    if (proc->fd_cloexec_mask & (1u << f)) {
      proc->fd_cloexec_mask &= ~(1u << f);
      if (proc->fds[f])
        vfs_close_for_proc(proc, f);
    }
  }

  *new_entry = final_entry;
  *new_rsp = user_rsp;

  LOG_INFO("[EXECVE] '%s' cargado (entry=%p rsp=%p argc=%d envc=%d at_base=%p)",
           path, (void *)final_entry, (void *)user_rsp, argc, envc,
           (void *)at_base);
  return 0;
}

// ---------------------------------------------------------------------------
// [JOB CONTROL] Helpers
// ---------------------------------------------------------------------------
int process_is_in_pgrp(process_t *p, uint32_t pgid) {
  if (!p)
    return 0;
  return p->pgid == pgid;
}

int process_signal_pgrp(uint32_t pgid_arg, uint64_t signal_mask) {
  if (pgid_arg == 0)
    return -ESRCH;

#define SIGPGRP_MAX 32
  task_t *tasks[SIGPGRP_MAX];
  uint32_t ppids[SIGPGRP_MAX];
  int resume_flags[SIGPGRP_MAX];
  int intr_flags[SIGPGRP_MAX];
  int n = 0;

  unsigned long flags = spin_lock_irqsave(&process_lock);
  process_t *p = process_list;
  while (p && n < SIGPGRP_MAX) {
    if (p->pgid == pgid_arg && !p->is_zombie && p->task) {
      task_t *t = p->task;
      task_get(t);

      int resume = 0;
      uint64_t mask = signal_mask;
      if ((mask & (1ULL << SIGCONT)) && p->stopped) {
        p->stopped = 0;
        p->stop_event_pending = 0;
        p->cont_event_pending = 1;
        resume = 1;
        mask &= ~(1ULL << SIGCONT);
      } else if ((mask & (1ULL << SIGKILL)) && p->stopped) {
        p->stopped = 0;
        p->stop_event_pending = 0;
        resume = 1;
      }

      // [FIX] Descartar señales ignoradas: no se encolan ni interrumpen.
      uint64_t before = mask;
      mask = signal_filter_ignored(p, mask);
      if (before != mask)
        LOG_DEBUG("[SIG] pgrp=%u pid=%u: descartadas señales ignoradas 0x%lx",
                  pgid_arg, p->pid, (unsigned long)(before & ~mask));

      if (mask)
        __atomic_fetch_or(&p->pending_signals, mask, __ATOMIC_RELEASE);

      tasks[n] = t;
      ppids[n] = p->ppid;
      resume_flags[n] = resume;
      intr_flags[n] = (mask != 0);
      n++;
    }
    p = p->next;
  }
  spin_unlock_irqrestore(&process_lock, flags);

  for (int i = 0; i < n; i++) {
    if (resume_flags[i]) {
      sched_cont_task(tasks[i]);
      process_wake_parent(ppids[i]);
    }
    // [FIX] Solo interrumpir si quedó una señal entregable.
    if (intr_flags[i])
      wait_queue_interrupt_task(tasks[i]);
    task_put(tasks[i]);
  }
  return n > 0 ? n : -ESRCH;
}

int process_pgrp_exists(uint32_t pgid) {
  if (pgid == 0)
    return 0;
  unsigned long flags = spin_lock_irqsave(&process_lock);
  process_t *p = process_list;
  int found = 0;
  while (p) {
    // [3.3] Un pgrp existe mientras haya CUALQUIER proceso (vivo o
    // zombie) con ese pgid. Linux usa find_vpid(), que respeta zombies.
    // Filtrar zombies aquí hacía fallar tcsetpgrp() en fg sobre jobs
    // recién muertos.
    if (p->pgid == pgid) {
      found = 1;
      break;
    }
    p = p->next;
  }
  spin_unlock_irqrestore(&process_lock, flags);
  return found;
}

void process_clear_ctty_for(struct tty *t) {
  if (!t)
    return;
  unsigned long flags = spin_lock_irqsave(&process_lock);
  process_t *p = process_list;
  while (p) {
    if (p->ctty == t)
      p->ctty = NULL;
    p = p->next;
  }
  spin_unlock_irqrestore(&process_lock, flags);
}

// ---------------------------------------------------------------------------
// [JOB CONTROL] Parar el proceso actual hasta que llegue SIGCONT.
//
// El proceso queda en TASK_STOPPED en la runqueue. El selector del
// scheduler lo ignora. Solo sched_cont_task lo devuelve a READY.
//
// El bucle `while (proc->stopped)` es la clave: cuando SIGCONT llega
// desde otro proceso (process_signal_pid/pgrp), se pone stopped=0 y se
// llama a sched_cont_task. Al despertar, salimos del bucle.
// ---------------------------------------------------------------------------
void process_stop_current(int sig) {
  process_t *proc = process_current();
  task_t *cur = sched_current();
  if (!proc || !cur)
    return;

  proc->stopped = 1;
  proc->stop_signal = sig;
  proc->stop_event_pending = 1;

  // Avisar al padre para que su waitpid(WUNTRACED) vea el evento.
  process_wake_parent(proc->ppid);

  while (proc->stopped) {
    cur->state = TASK_STOPPED;
    sched_yield();
  }
}

// ---------------------------------------------------------------------------
// [JOB CONTROL] Despertar al padre (por PID) para que su child_wq
// re-evalúe la condición. Si el padre no está en waitpid, la llamada a
// wait_queue_wake_task es no-op.
// ---------------------------------------------------------------------------
void process_wake_parent(uint32_t parent_pid) {
  if (parent_pid == 0)
    return;
  unsigned long flags = spin_lock_irqsave(&process_lock);
  process_t *p = process_list;
  task_t *parent_task = NULL;
  while (p) {
    if (p->pid == parent_pid && !p->is_zombie) {
      if (p->task) {
        parent_task = p->task;
        task_get(parent_task);
      }
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