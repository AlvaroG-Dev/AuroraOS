// kernel/process.h
#ifndef PROCESS_H
#define PROCESS_H

#include "sched.h"
#include "vfs.h"
#include "wait.h"
#include <stddef.h>
#include <stdint.h>

#define WNOHANG 1

#define PROCESS_ARGV_MAX 16

// [pipe] Fds que el shell quiere asignar al hijo. -1 significa
// "usar el stdio por defecto" (stdin/stdout/stderr del kernel).
typedef struct {
  int fd_in;
  int fd_out;
  int fd_err;
} spawn_fds_t;

typedef struct process {
  uint32_t pid;
  uint32_t ppid;
  int exit_code;
  int is_zombie;
  char name[64];
  task_t *task;
  uint64_t pml4_phys;
  uint64_t load_base;
  uint64_t heap_start;
  uint64_t heap_end;
  uint64_t heap_max;
  struct vma *vma_list;
  uint64_t stack_base;
  uint64_t stack_low;
  uint64_t stack_top;
  uint64_t stack_guard;
  file_descriptor_t *fds[MAX_PROCESS_FDS];
  struct process *next;
  wait_queue_t child_wq;
  char cwd[VFS_PATH_MAX];

  // [SIG] Bitmask de señales pendientes y bloqueadas. Bit N = señal N.
  // pendiente:  aún no entregada.
  // bloqueada:  el proceso pidió posponerla (sigprocmask futuro).
  // Solo se entregan en signal_check_pending(), llamada desde el
  // retorno a userland de cada syscall.
  uint64_t pending_signals;
  uint64_t blocked_signals;

  // [musl] FS segment base (TLS). Se guarda también en task_t para que
  // switch.asm lo restaure al cambiar de tarea.
  uint64_t fs_base;
} process_t;

process_t *process_spawn(const char *name, const void *elf_data,
                         size_t elf_size);
process_t *process_load(const char *path);
process_t *process_spawn_child(process_t *parent, const char *path);

int process_waitpid(process_t *parent, int32_t pid, int *status_out,
                    int options);

// [Fase A] Marca el proceso como zombie, cierra fds y ventanas, y
// despierta al padre. NO mata la tarea. Es la parte "lógica".
void process_exit(process_t *proc, int exit_code);

// [Fase A] Marca el proceso como zombie, cierra fds/ventanas, despierta
// al padre, mata la tarea actual y NO retorna. Unifica SYS_EXIT y
// kill_current_process.
__attribute__((noreturn)) void process_exit_current(int exit_code);

process_t *process_current(void);
void *process_sbrk(process_t *proc, int64_t increment);

process_t *process_spawn_child_args(process_t *parent, const char *path,
                                    int argc, const char *const *argv);

// [SIG] Busca un proceso por PID. Devuelve NULL si no existe o es zombie.
// El puntero devuelto NO tiene refcount propio: el llamante debe usarlo
// con cuidado (procesos solo se liberan desde waitpid).
process_t *process_find_by_pid(uint32_t pid);

// [SIG] Envía una señal a un PID manteniendo process_lock durante la
// publicación y devuelve una referencia propia a la tarea destino.
task_t *process_signal_pid(uint32_t pid, uint64_t signal_mask);

// [pipe] Variante de process_spawn_child_args que asigna fds concretos
// (heredados del padre, compartiendo file_descriptor_t) a stdin/stdout/
// stderr del hijo. Los fds del struct deben ser válidos en `parent`.
process_t *process_spawn_child_args_fds(process_t *parent, const char *path,
                                        int argc, const char *const *argv,
                                        const spawn_fds_t *fds);

#endif // PROCESS_H