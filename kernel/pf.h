// kernel/pf.h
#ifndef PF_H
#define PF_H

#include "idt.h"
#include <stdint.h>

// Tipos de VMA
#define VMA_ANON 0
#define VMA_STACK 1
#define VMA_ELF 2
#define VMA_FILE 3 // [3.5] mmap file-backed
// [FASE 4.3] mmap de MMIO (resourceN de sysfs, /dev/mem, DMA).
// Las páginas se mapean eagerly con ops->mmap, no por demand paging.
// vma_unmap_range NO las devuelve al PMM (son MMIO, no RAM).
#define VMA_MMIO 4
// mmap MAP_SHARED de un objeto con ops->get_page (memfd). Demand paging con
// paginas compartidas (PTE_SPECIAL|PTE_SHARED_OBJ): ni se liberan al
// desmapear ni se copian en fork.
#define VMA_SHARED 5
// [LinuxKPI L4] mmap de un fichero de dispositivo con vm_operations_struct
// (vfs_ops.mmap_dev). Las paginas son del driver (PTE_SPECIAL|PTE_SHARED_OBJ):
// ni se liberan al desmapear ni se copian en fork (se comparten). El VMA lleva
// referencia al fd (file_fd) y dev_ops: fault desde el #PF; open cuando nace
// otro VMA con el mismo dev_priv (fork, division por munmap/mprotect); close
// cuando desaparece (munmap, exit, exec). Recortar un VMA no llama a nada.
#define VMA_DEV 6

// Límites
#define MAX_STACK_GROWTH (8 * 1024 * 1024) // Límite RLIMIT_STACK predeterminado

struct process;
struct file_descriptor;
struct vfs_node;
struct vma;

typedef struct vma_dev_ops {
  // Fallo de pagina en `addr` (alineada) del VMA, en el contexto del proceso
  // dueño. Debe mapear la pagina (vma_dev_map_page) y devolver 0; -errno =
  // SIGBUS (el proceso muere).
  int (*fault)(struct vma *vma, uint64_t addr, int write);
  // `nv` es un VMA nuevo con el dev_priv de `old` (fork o division). Puede
  // cambiar nv->dev_priv. Si devuelve error en fork, el hijo no lo hereda.
  int (*open)(struct vma *nv, struct vma *old);
  void (*close)(struct vma *vma);
} vma_dev_ops_t;

#define VMA_DEV_DONTCOPY 0x1 // VM_DONTCOPY: fork no lo hereda

typedef struct vma {
  uint64_t start;
  uint64_t end;
  uint64_t flags;
  uint32_t type;
  uint32_t pad;
  struct vma *next;

  // [3.5] Solo VMA_FILE. Puntero al file_descriptor_t del fichero
  // backing. El VMA mantiene una referencia (ref_count++) para que
  // el fichero sobreviva al close() del usuario.
  struct file_descriptor *file_fd;

  // [3.5] Offset del byte 0 del VMA dentro del fichero.
  uint64_t file_offset;

  // [2.4] Nodo VFS que respalda este VMA, con su propia ref.
  //   VMA_ELF:  nodo del binario o del intérprete.
  //   VMA_FILE: == file_fd->node (redundante por claridad).
  //   VMA_ANON / VMA_STACK: NULL.
  struct vfs_node *file_node;

  // [LinuxKPI L4] Solo VMA_DEV.
  const vma_dev_ops_t *dev_ops;
  void *dev_priv;
  struct process *dev_proc; // proceso dueño (para fault/zap)
  uint32_t dev_flags;       // VMA_DEV_*
  uint32_t dev_cache;       // bits de cache de la PTE (PTE_NOCACHE/WRITETHRU)
} vma_t;

vma_t *vma_create_file(struct process *proc, uint64_t start, uint64_t end,
                       uint64_t flags, struct file_descriptor *fd,
                       uint64_t file_offset);

// [2.4] Como vma_create pero para VMA_ELF: toma ref extra al nodo del
// binario. `exe_node` puede ser NULL (tests con buffer contiguo o
// intérpretes sin nodo).
vma_t *vma_create_elf(struct process *proc, uint64_t start, uint64_t end,
                      uint64_t flags, struct vfs_node *exe_node);

// [2.4] Copia el backing (file_fd + file_node) de src a dst, con los
// refs correctos, ajustando file_offset por off_delta. Exportada para
// que process.c (clone_vmas) la use sin duplicar lógica.
void vma_inherit_backing(vma_t *dst, const vma_t *src, uint64_t off_delta);

vma_t *vma_create_file(struct process *proc, uint64_t start, uint64_t end,
                       uint64_t flags, struct file_descriptor *fd,
                       uint64_t file_offset);

// Inicialización
void pf_init(void);

// Handler principal. Devuelve 1 si resolvió el fallo, 0 si no.
int handle_page_fault(registers_t *regs);

// Mata el proceso actual y salta a task_die_hlt.
void kill_current_process(registers_t *regs, const char *reason);

// VMA helpers
vma_t *vma_find(struct process *proc, uint64_t addr);
vma_t *vma_create(struct process *proc, uint64_t start, uint64_t end,
                  uint64_t flags, uint32_t type);
void vma_destroy_all(struct process *proc);
// Libera un VMA que no esta en ninguna lista (refs y, si es VMA_DEV, close).
void vma_free_one(vma_t *v);

// [MMAP] Desmapea todos los VMAs que se solapen con [start, end) y
// libera sus páginas físicas. start y end deben estar alineados a
// página. Es la primitiva común de sys_munmap y sys_mmap(MAP_FIXED).
//
// Devuelve el número de VMAs tocados (>=0) o un errno negativo (solo
// -ENOMEM, si falla el kmalloc del split de un VMA).
int64_t vma_unmap_range(struct process *proc, uint64_t start, uint64_t end);

// [LinuxKPI L4] VMA_DEV `dst` nacido de `src` (division o fork, con `owner`
// como proceso dueño): copia dev_ops/dev_priv y llama a dev_ops->open.
int vma_dev_inherit(vma_t *dst, const vma_t *src, struct process *owner);
// [LinuxKPI L4] VMA_DEV: mapea `phys` en `addr` del VMA (flags del VMA +
// dev_cache + PTE_SPECIAL|PTE_SHARED_OBJ). Reemplaza lo que hubiera. 0/-errno.
int vma_dev_map_page(vma_t *vma, uint64_t addr, uint64_t phys);
// Quita las PTE de [start, end) del VMA (zap_vma_ptes, unmap_mapping_range)
// con shootdown de TLB. Las paginas no se liberan.
void vma_dev_zap(vma_t *vma, uint64_t start, uint64_t end);

// Syscalls
int64_t sys_mmap(struct process *proc, uint64_t addr, uint64_t length,
                 uint64_t prot, uint64_t flags, int fd, uint64_t offset);
int64_t sys_munmap(struct process *proc, uint64_t addr, uint64_t length);

// [4.3] mprotect(addr, len, prot). Actualiza los VMAs y las PTEs.
int64_t sys_mprotect(struct process *proc, uint64_t addr, uint64_t length,
                     uint64_t prot);

// [B] mremap. Crece/encoge/realoca un rango mmap'd existente.
// Soporta crecimiento in-place si hay hueco contiguo, o move+copy si
// MREMAP_MAYMOVE. No soporta MREMAP_FIXED.
int64_t sys_mremap(struct process *proc, uint64_t old_addr, uint64_t old_size,
                   uint64_t new_size, uint64_t flags, uint64_t new_addr);

// Estadísticas
uint64_t pf_stats_resolved(void);
uint64_t pf_stats_killed(void);
void pf_dump_stats(void);

// [A.2] madvise(2) — MADV_* de Linux x86_64.
#define MADV_NORMAL 0
#define MADV_RANDOM 1
#define MADV_SEQUENTIAL 2
#define MADV_WILLNEED 3
#define MADV_DONTNEED 4
#define MADV_FREE 8
#define MADV_DONTDUMP 16
#define MADV_DODUMP 17

// [A.2] Libera las páginas físicas del rango [addr, addr+len) que
// estén dentro de un VMA. Las páginas sin VMA (p.ej. la región brk,
// que glibc gestiona directo con sbrk) se dejan intactas, porque un
// fault posterior no tendría VMA y mataría al proceso.
//
// Para VMA_FILE: la próxima falta relee del fichero.
// Para VMA_ANON / VMA_STACK: la próxima falta cero-rellena.
int64_t sys_madvise(struct process *proc, uint64_t addr, uint64_t len,
                    uint64_t advice);

#endif