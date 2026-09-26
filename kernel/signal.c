#include "signal.h"
#include "klog.h"
#include "process.h"
#include "sched.h"

sig_action_t signal_default_action(int sig) {
  switch (sig) {
  case SIGCHLD:
  case SIGCONT:
    return SIG_ACT_IGN;

  case SIGSTOP:
  case SIGTSTP:
    return SIG_ACT_STOP;

  case SIGHUP:
  case SIGINT:
  case SIGKILL:
  case SIGTERM:
    return SIG_ACT_TERM;

  case SIGQUIT:
  case SIGILL:
  case SIGABRT:
  case SIGFPE:
  case SIGSEGV:
    return SIG_ACT_CORE;

  default:
    return SIG_ACT_IGN;
  }
}

void signal_check_pending(void) {
  process_t *proc = process_current();
  if (!proc)
    return;

  uint64_t pending = __atomic_load_n(&proc->pending_signals, __ATOMIC_ACQUIRE);
  if (pending == 0)
    return;

  // SIGKILL ignora máscara: siempre mata.
  if (pending & (1ULL << SIGKILL)) {
    LOG_INFO("[SIG] PID=%u recibió SIGKILL, terminando", proc->pid);
    process_exit_current(128 + SIGKILL);
  }

  uint64_t deliverable = pending & ~proc->blocked_signals;
  if (deliverable == 0)
    return;

  for (int sig = 1; sig < SIG_MAX; sig++) {
    if (!(deliverable & (1ULL << sig)))
      continue;

    // Consumir la señal antes de actuar.
    __atomic_fetch_and(&proc->pending_signals, ~(1ULL << sig),
                       __ATOMIC_ACQ_REL);

    sig_action_t act = signal_default_action(sig);
    if (act == SIG_ACT_TERM || act == SIG_ACT_CORE) {
      LOG_INFO("[SIG] PID=%u recibió señal %d, terminando", proc->pid, sig);
      process_exit_current(128 + sig);
    }
    // IGN / STOP / CONT: por ahora solo consumir.
  }
}