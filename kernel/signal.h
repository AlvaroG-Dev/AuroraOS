#ifndef KERNEL_SIGNAL_H
#define KERNEL_SIGNAL_H

#include "idt.h"
#include <stdint.h>

#define SIGHUP 1
#define SIGINT 2
#define SIGQUIT 3
#define SIGILL 4
#define SIGTRAP 5
#define SIGABRT 6
#define SIGBUS 7
#define SIGFPE 8
#define SIGKILL 9
#define SIGUSR1 10
#define SIGSEGV 11
#define SIGUSR2 12
#define SIGPIPE 13
#define SIGALRM 14
#define SIGTERM 15
#define SIGSTKFLT 16
#define SIGCHLD 17
#define SIGCONT 18
#define SIGSTOP 19
#define SIGTSTP 20
#define SIGTTIN 21
#define SIGTTOU 22
#define SIGURG 23
#define SIGXCPU 24
#define SIGXFSZ 25
#define SIGVTALRM 26
#define SIGPROF 27
#define SIGWINCH 28
#define SIGIO 29
#define SIGPWR 30
#define SIGSYS 31

#define SIG_MAX 64

#define SIG_DFL ((void (*)(int))0)
#define SIG_IGN ((void (*)(int))1)

#define SA_NOCLDSTOP 0x00000001
#define SA_NOCLDWAIT 0x00000002
#define SA_SIGINFO 0x00000004
#define SA_ONSTACK 0x08000000
#define SA_RESTART 0x10000000
#define SA_NODEFER 0x40000000
#define SA_RESETHAND 0x80000000
#define SA_RESTORER 0x04000000

// Layout compatible con el syscall rt_sigaction de Linux x86_64 (32 bytes).
typedef struct {
  void (*handler)(int);
  uint64_t flags;
  void (*restorer)(void);
  uint64_t mask;
} k_sigaction_t;

typedef enum {
  SIG_ACT_TERM = 0,
  SIG_ACT_IGN = 1,
  SIG_ACT_CORE = 2,
  SIG_ACT_STOP = 3,
  SIG_ACT_CONT = 4,
} sig_action_t;

sig_action_t signal_default_action(int sig);

// Se llama al final de syscall_handler_c, después de que el handler
// haya escrito su valor de retorno en el trap frame.
void signal_check_pending(void);

struct process;
// Devuelve `mask` sin los bits de señales ignoradas (SIG_IGN, o SIG_DFL con
// acción por defecto IGN). SIGKILL/SIGSTOP nunca se filtran.
uint64_t signal_filter_ignored(struct process *p, uint64_t mask);

// [syscall.c] Handlers de rt_sigaction/rt_sigreturn. Se definen en
// signal.c pero se registran en la tabla de syscalls de syscall.c.
// La firma coincide con syscall_fn_t: 5 argumentos uint64_t.
int64_t k_rt_sigaction(uint64_t sig, uint64_t act, uint64_t oact,
                       uint64_t sigsetsize, uint64_t a5);
int64_t k_rt_sigreturn(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                       uint64_t a5);
int64_t k_rt_sigprocmask(uint64_t how, uint64_t set, uint64_t oldset,
                         uint64_t sigsetsize, uint64_t a5);

// [4.3] Entrega una señal al proceso actual usando los regs del fault.
int signal_deliver_from_exception(int sig, registers_t *regs);

#endif