#ifndef KERNEL_SIGNAL_H
#define KERNEL_SIGNAL_H

#define SIGHUP 1
#define SIGINT 2
#define SIGQUIT 3
#define SIGILL 4
#define SIGABRT 6
#define SIGFPE 8
#define SIGKILL 9
#define SIGSEGV 11
#define SIGTERM 15
#define SIGCHLD 17
#define SIGCONT 18
#define SIGSTOP 19
#define SIGTSTP 20

#define SIG_MAX 32

typedef enum {
  SIG_ACT_TERM = 0, // terminar (128 + sig en exit code)
  SIG_ACT_IGN = 1,  // ignorar
  SIG_ACT_CORE = 2, // terminar (sin core dump por ahora)
  SIG_ACT_STOP = 3, // detener (aún no implementado)
  SIG_ACT_CONT = 4, // continuar (aún no implementado)
} sig_action_t;

sig_action_t signal_default_action(int sig);

// Se llama en el camino de retorno a userland tras cada syscall.
// Si hay señales pendientes y su acción por defecto es fatal, no retorna
// (llama a process_exit_current).
void signal_check_pending(void);

#endif