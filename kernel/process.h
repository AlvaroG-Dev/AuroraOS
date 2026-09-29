// kernel/process.h
#ifndef PROCESS_H
#define PROCESS_H

#include "sched.h"
#include "signal.h"
#include "vfs.h"
#include "wait.h"
#include <stddef.h>
#include <stdint.h>

#define WNOHANG 1
#define WUNTRACED 2
#define WCONTINUED 8

#define PROCESS_ARGV_MAX 16

#define PROCESS_ENVP_MAX 32
#define PROCESS_ENV_STR_MAX 256

struct tty;

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

  // [JOB CONTROL] Process group ID. Un proceso con pgid == pid es
  // líder de su grupo. Los hijos heredan el pgid del padre y pueden
  // cambiarlo con setpgid() mientras el padre lo permita (misma sesión).
  uint32_t pgid;

  // [JOB CONTROL] Session ID. Un proceso con sid == pid es líder de
  // sesión. Un terminal tiene una sesión asociada (fg_pgid la gestiona
  // en tty.c), y los procesos que abren un terminal pasan a pertenecer
  // a su sesión vía setsid() + TIOCSCTTY.
  uint32_t sid;

  // [CTTY] Terminal de control del proceso. NULL si no tiene.
  // Lo setea TIOCSCTTY, lo limpia setsid/TIOCNOTTY, y process_clear_ctty_for
  // cuando el tty subyacente se libera. Todos los procesos de una misma
  // sesión comparten ctty.
  struct tty *ctty;

  int exit_code;
  int is_zombie;

  // [JOB CONTROL] Estado de parada por SIGSTOP/SIGTSTP/SIGTTIN/SIGTTOU.
  //   stopped            1 si el proceso está parado ahora mismo.
  //   stop_signal        señal que lo paró (para WIFSTOPPED).
  //   stop_event_pending 1 si hay un evento "stopped" aún no reportado
  //                      al padre vía waitpid(WUNTRACED).
  //   cont_event_pending 1 si hay un evento "continued" aún no reportado
  //                      al padre vía waitpid(WCONTINUED).
  int stopped;
  int stop_signal;
  int stop_event_pending;
  int cont_event_pending;

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

  uint64_t pending_signals;
  uint64_t blocked_signals;
  k_sigaction_t sigactions[SIG_MAX];

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

  // [3.3.d] argv del proceso. Array de argc strings kmalloc'd. Los llena
  // process_set_argv() en spawn y execve; los libera process_clear_argv()
  // en exit y antes de sobrescribirlos en execve. El cmdline de
  // /proc/<pid> los lee tal cual (formato Linux: strings NUL-separated,
  // terminados en NUL extra).
  char *argv[PROCESS_ARGV_MAX];
  int argc;

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

// [JOB CONTROL] Para el proceso actual: marca stopped=1, notifica al
// padre (child_wq), y cede la CPU en bucle hasta que SIGCONT la
// reactive (stopped=0). Se llama desde signal.c (deliver) cuando
// SIGTSTP/SIGSTOP/SIGTTIN/SIGTTOU tienen acción por defecto.
// No es noreturn: retorna cuando el proceso es continuado.
void process_stop_current(int sig);

// [JOB CONTROL] Despierta al padre (por PID) para que su child_wq
// re-evalúe la condición (stop/cont nuevos). No-op si no espera.
void process_wake_parent(uint32_t parent_pid);

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

// [JOB CONTROL] Devuelve 1 si el proceso `pid` pertenece al grupo
// `pgid`. Un pid 0 o pgid 0 se interpretan como "el proceso actual".
// No toma locks internamente: el llamante debe tener cuidado si los
// campos pueden cambiar concurrentemente.
int process_is_in_pgrp(process_t *p, uint32_t pgid);

// [JOB CONTROL] Envía señal a todos los procesos vivos (no zombie) con
// pgid == pgid_arg. Ignora a zombies. Devuelve el número de procesos
// señalados, o -ESRCH si no existe ningún proceso en ese grupo.
int process_signal_pgrp(uint32_t pgid_arg, uint64_t signal_mask);

task_t *process_signal_pid_ex(uint32_t pid, uint64_t signal_mask,
                              int *need_interrupt);

// Devuelve 1 si existe algún proceso vivo en el grupo `pgid`. 0 si no.
int process_pgrp_exists(uint32_t pgid);

// [CTTY] Recorre process_list y pone a NULL el ctty de cualquier proceso
// que apunte a `t`. Se llama desde pty_free cuando un PTY se libera.
void process_clear_ctty_for(struct tty *t);

// [RUSAGE] Contabiliza un tick de CPU (1 ms) al proceso de la tarea
// indicada. La llama sched_tick() en cada tick del LAPIC. No hace nada
// si `t` es NULL o si la tarea no pertenece a un proceso (idle, kmain).
void process_account_tick(task_t *t);

// [3.3.d] argv del proceso.
int process_set_argv(process_t *proc, int argc, const char *const *argv);
void process_clear_argv(process_t *proc);
int process_inherit_argv(const process_t *parent, process_t *child);

// [3.3.d] Iterador de process_list. cb corre bajo process_lock: no
// debe dormir, ni llamar a ninguna función que tome process_lock.
// Devolver != 0 desde cb detiene la iteración.
typedef int (*process_iter_cb_t)(process_t *p, void *arg);
void process_for_each(process_iter_cb_t cb, void *arg);

// Variantes con envp explícito.
process_t *process_spawn_child_args_env(process_t *parent, const char *path,
                                        int argc, const char *const *argv,
                                        int envc, const char *const *envp);
process_t *process_spawn_child_args_fds_env(process_t *parent, const char *path,
                                            int argc, const char *const *argv,
                                            int envc, const char *const *envp,
                                            const spawn_fds_t *fds);
#endif // PROCESS_H