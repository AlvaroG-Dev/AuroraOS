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

#define PROCESS_ENVP_MAX 32
#define PROCESS_ENV_STR_MAX 256

// [pipe] Fds que el shell quiere asignar al hijo. -1 significa
// "usar el stdio por defecto" (stdin/stdout/stderr del kernel).
typedef struct {
  int fd_in;
  int fd_out;
  int fd_err;
} spawn_fds_t;

// ---------------------------------------------------------------------------
// [RUSAGE] Contadores de recursos por proceso. Los llena process_waitpid
// al reapear un zombie; k_wait4 los traduce al struct rusage de Linux.
// Los que no contabilizamos todavía van a 0.
// ---------------------------------------------------------------------------
typedef struct {
  uint64_t utime_ticks; // tiempo CPU en userland (ticks de 1 ms)
  uint64_t stime_ticks; // tiempo CPU en kernel (siempre 0 por ahora)
  uint64_t minflt;      // page faults resueltos sin I/O (0)
  uint64_t majflt;      // page faults con I/O (0)
  uint64_t nvcsw;       // context switches voluntarios (0)
  uint64_t nivcsw;      // context switches involuntarios (0)
  int64_t maxrss_kb;    // RSS máximo en KB (0)
} proc_rusage_t;

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

  // [MMAP] Próxima dirección sugerida para mmap(NULL, ...) y para el
  // fallback cuando un hint no se puede satisfacer. Per-process: el
  // espacio virtual es privado, así que no tiene sentido un contador
  // global.
  uint64_t next_mmap_addr;

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
  // [RUSAGE] Tiempo de CPU acumulado en ticks (1000 Hz). Lo incrementa
  // sched_tick() cada vez que la tarea de este proceso corre. Al reapear
  // el proceso, process_waitpid lo vuelca a proc_rusage_t.
  uint64_t cpu_ticks_user;

  // [ENV] Variables de entorno. Array NULL-terminated de strings
  // "KEY=VALUE" kmalloc'd. Cada uno liberado por process_clear_envp.
  // Los hereda fork y los reemplaza execve.
  char *envp[PROCESS_ENVP_MAX];
} process_t;

process_t *process_spawn(const char *name, const void *elf_data,
                         size_t elf_size);
process_t *process_load(const char *path);
process_t *process_spawn_child(process_t *parent, const char *path);

int process_waitpid(process_t *parent, int32_t pid, int *status_out,
                    proc_rusage_t *rusage_out, int options);

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

// [fork] Duplica el proceso actual. Devuelve el pid del hijo al padre,
// 0 al hijo (vía frame de iretq), o negativo en error.
int64_t sys_fork(void);

// [execve] Reemplaza el espacio de usuario del proceso actual con el
// binario `path`. En éxito devuelve 0 y rellena *new_entry / *new_rsp
// con el punto de entrada del nuevo ELF y el RSP inicial con argv
// ya construido. El llamante (syscall.c) es quien reescribe los regs
// del trap frame para aterrizar en ese (rip, rsp).
//
// NO crea proceso nuevo: muta el actual in-place. Conserva PID, fds,
// cwd, y cierra... (FD_CLOEXEC no soportado aún; todos los fds
// sobreviven, que es lo correcto para busybox sh → applet).
//
// En error devuelve un errno negativo y NO toca el proceso.
int process_execve_prepare(const char *path, int argc, const char *const *argv,
                           int envc, const char *const *envp,
                           uint64_t *new_entry, uint64_t *new_rsp);

// [RUSAGE] Contabiliza un tick de CPU (1 ms) al proceso de la tarea
// indicada. La llama sched_tick() en cada tick del LAPIC. No hace nada
// si `t` es NULL o si la tarea no pertenece a un proceso (idle, kmain).
void process_account_tick(task_t *t);

int process_set_envp(process_t *proc, int envc, const char *const *envp);
void process_clear_envp(process_t *proc);
int process_inherit_envp(const process_t *parent, process_t *child);

// Variantes con envp explícito.
process_t *process_spawn_child_args_env(process_t *parent, const char *path,
                                        int argc, const char *const *argv,
                                        int envc, const char *const *envp);
process_t *process_spawn_child_args_fds_env(process_t *parent, const char *path,
                                            int argc, const char *const *argv,
                                            int envc, const char *const *envp,
                                            const spawn_fds_t *fds);
#endif // PROCESS_H