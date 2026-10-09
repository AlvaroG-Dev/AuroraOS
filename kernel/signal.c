#include "signal.h"
#include "klog.h"
#include "process.h"
#include "sched.h"
#include "signalfd.h"
#include "syscall.h"
#include "uaccess.h"


extern registers_t *syscall_current_regs(void);

sig_action_t signal_default_action(int sig) {
  switch (sig) {
  case SIGCHLD:
  case SIGCONT:
  case SIGWINCH:
  case SIGURG:
    return SIG_ACT_IGN;
  case SIGSTOP:
  case SIGTSTP:
  case SIGTTIN:
  case SIGTTOU:
    return SIG_ACT_STOP;
  case SIGHUP:
  case SIGINT:
  case SIGKILL:
  case SIGTERM:
  case SIGPIPE:
  case SIGALRM:
    return SIG_ACT_TERM;
  case SIGQUIT:
  case SIGILL:
  case SIGABRT:
  case SIGFPE:
  case SIGSEGV:
  case SIGBUS:
  case SIGTRAP:
    return SIG_ACT_CORE;
  default:
    return SIG_ACT_IGN;
  }
}

// ---------------------------------------------------------------------------
// Layout del rt_sigframe (offsets desde frame_va).
//
//   +0x000  pretcode          (8)
//   +0x008  ucontext          (304)
//      +0x008  uc_flags       (8)
//      +0x010  uc_link        (8)
//      +0x018  uc_stack       (24)
//      +0x030  uc_mcontext    (256, = struct sigcontext)
//         +0x030  gregs[0]  r8
//         ...
//         +0x0a8  gregs[15] rsp
//         +0x0b0  gregs[16] rip
//         +0x0b8  gregs[17] eflags
//         +0x0c0  gregs[18] cs/gs/fs
//         +0x0c8  gregs[19] err
//         +0x0d0  gregs[20] trapno
//         +0x0d8  gregs[21] oldmask
//         +0x0e0  gregs[22] cr2
//         +0x0e8  fpstate ptr
//         +0x0f0  reserved1[8]
//      +0x130  uc_sigmask     (8)
//   +0x138  siginfo           (128)
//   +0x1b8  fpstate           (512)
//   total 0x3f8 = 1016  → alineado a 1024
// ---------------------------------------------------------------------------
#define SF_PRETCODE 0x000
#define SF_UCONTEXT 0x008
#define SF_UC_MCONTEXT (SF_UCONTEXT + 0x028)
#define SF_UC_SIGMASK (SF_UCONTEXT + 0x128)
#define SF_SIGINFO (SF_UCONTEXT + 304)
#define SF_SIZE 1024

// gregs relativos a uc_mcontext.
#define G_R8 0x00
#define G_R9 0x08
#define G_R10 0x10
#define G_R11 0x18
#define G_R12 0x20
#define G_R13 0x28
#define G_R14 0x30
#define G_R15 0x38
#define G_RDI 0x40
#define G_RSI 0x48
#define G_RBP 0x50
#define G_RBX 0x58
#define G_RDX 0x60
#define G_RAX 0x68
#define G_RCX 0x70
#define G_RSP 0x78
#define G_RIP 0x80
#define G_EFL 0x88
#define G_CSGSFS 0x90
#define G_ERR 0x98
#define G_TRAPNO 0xa0
#define G_OLDMASK 0xa8
#define G_CR2 0xb0

static int w_u64(uint64_t va, uint64_t v) {
  return copy_to_user((void *)va, &v, 8);
}
static int w_u32(uint64_t va, uint32_t v) {
  return copy_to_user((void *)va, &v, 4);
}

// Construye el rt_sigframe en `frame_va`. Devuelve 0 o -1.
static int build_frame(uint64_t frame_va, int sig, const k_sigaction_t *act,
                       registers_t *regs, uint64_t old_mask) {
  uint64_t mc = frame_va + SF_UC_MCONTEXT;

  // pretcode
  if (w_u64(frame_va + SF_PRETCODE, (uint64_t)act->restorer) < 0)
    return -1;

  // ucontext header
  if (w_u64(frame_va + SF_UCONTEXT + 0x00, 0) < 0)
    return -1; // uc_flags
  if (w_u64(frame_va + SF_UCONTEXT + 0x08, 0) < 0)
    return -1; // uc_link
  if (w_u64(frame_va + SF_UCONTEXT + 0x10, 0) < 0)
    return -1; // ss_sp
  if (w_u64(frame_va + SF_UCONTEXT + 0x18, 0) < 0)
    return -1; // ss_flags+pad
  if (w_u64(frame_va + SF_UCONTEXT + 0x20, 0) < 0)
    return -1; // ss_size

  // gregs
  if (w_u64(mc + G_R8, regs->r8) < 0)
    return -1;
  if (w_u64(mc + G_R9, regs->r9) < 0)
    return -1;
  if (w_u64(mc + G_R10, regs->r10) < 0)
    return -1;
  if (w_u64(mc + G_R11, regs->r11) < 0)
    return -1;
  if (w_u64(mc + G_R12, regs->r12) < 0)
    return -1;
  if (w_u64(mc + G_R13, regs->r13) < 0)
    return -1;
  if (w_u64(mc + G_R14, regs->r14) < 0)
    return -1;
  if (w_u64(mc + G_R15, regs->r15) < 0)
    return -1;
  if (w_u64(mc + G_RDI, regs->rdi) < 0)
    return -1;
  if (w_u64(mc + G_RSI, regs->rsi) < 0)
    return -1;
  if (w_u64(mc + G_RBP, regs->rbp) < 0)
    return -1;
  if (w_u64(mc + G_RBX, regs->rbx) < 0)
    return -1;
  if (w_u64(mc + G_RDX, regs->rdx) < 0)
    return -1;
  if (w_u64(mc + G_RAX, regs->rax) < 0)
    return -1;
  if (w_u64(mc + G_RCX, regs->rcx) < 0)
    return -1;
  if (w_u64(mc + G_RSP, regs->rsp) < 0)
    return -1;
  if (w_u64(mc + G_RIP, regs->rip) < 0)
    return -1;
  if (w_u64(mc + G_EFL, regs->rflags) < 0)
    return -1;
  if (w_u64(mc + G_CSGSFS, (uint64_t)regs->cs) < 0)
    return -1;
  if (w_u64(mc + G_ERR, 0) < 0)
    return -1;
  if (w_u64(mc + G_TRAPNO, (uint64_t)sig) < 0)
    return -1;
  if (w_u64(mc + G_OLDMASK, old_mask) < 0)
    return -1;
  if (w_u64(mc + G_CR2, 0) < 0)
    return -1;

  // uc_sigmask
  if (w_u64(frame_va + SF_UC_SIGMASK, old_mask) < 0)
    return -1;

  // siginfo (128 bytes). Solo los 3 primeros campos importan.
  uint64_t si = frame_va + SF_SIGINFO;
  if (w_u32(si + 0x00, (uint32_t)sig) < 0)
    return -1; // si_signo
  if (w_u32(si + 0x04, 0) < 0)
    return -1; // si_errno
  if (w_u32(si + 0x08, 0) < 0)
    return -1; // si_code = SI_USER

  return 0;
}

// Entrega `sig` al proceso actual, si tiene handler. Devuelve 1 si
// entregada, 0 si ignorada, y no retorna si es fatal.
static int deliver(process_t *proc, int sig, registers_t *regs) {
  k_sigaction_t *act = &proc->sigactions[sig];

  if (act->handler == SIG_DFL) {
    sig_action_t def = signal_default_action(sig);
    if (def == SIG_ACT_TERM || def == SIG_ACT_CORE) {
      LOG_INFO("[SIG] PID=%u SIG %d -> terminar", proc->pid, sig);
      process_exit_current(128 + sig);
    }
    if (def == SIG_ACT_STOP) {
      LOG_INFO("[SIG] PID=%u SIG %d -> stop", proc->pid, sig);
      process_stop_current(sig); // retorna cuando llega SIGCONT
    }
    return 1;
  }
  if (act->handler == SIG_IGN) {
    return 1;
  }
  if (!act->restorer) {
    // Sin trampoline no podemos entregar. Consumir la señal.
    LOG_WARN("[SIG] PID=%u SIG %d sin restorer, ignorado", proc->pid, sig);
    return 1;
  }

  uint64_t frame_va = (regs->rsp - SF_SIZE) & ~0xFULL;
  uint64_t old_mask = proc->blocked_signals;

  if (build_frame(frame_va, sig, act, regs, old_mask) < 0) {
    LOG_ERR("[SIG] PID=%u SIG %d: fallo al construir frame", proc->pid, sig);
    process_exit_current(128 + sig);
  }

  proc->blocked_signals = old_mask | act->mask | (1ULL << sig);

  // Redirigir el trap frame al handler.
  regs->rip = (uint64_t)act->handler;
  regs->rsp = frame_va;
  regs->rdi = (uint64_t)sig;
  regs->rsi = frame_va + SF_SIGINFO;
  regs->rdx = frame_va + SF_UCONTEXT;
  // rax preservado (valor de retorno de la syscall).

  return 1;
}

void signal_check_pending(void) {
  process_t *proc = process_current();
  if (!proc)
    return;

  registers_t *regs = syscall_current_regs();
  if (!regs)
    return;

  uint64_t pending = __atomic_load_n(&proc->pending_signals, __ATOMIC_ACQUIRE);
  if (pending == 0)
    return;

  // TEMPORAL: identificar el bucle.
  LOG_INFO("[SIG-DBG] pid=%u pending=0x%lx blocked=0x%lx", proc->pid,
           (unsigned long)pending, (unsigned long)proc->blocked_signals);

  if (pending & (1ULL << SIGKILL)) {
    __atomic_fetch_and(&proc->pending_signals, ~(1ULL << SIGKILL),
                       __ATOMIC_ACQ_REL);
    LOG_INFO("[SIG] PID=%u SIGKILL", proc->pid);
    process_exit_current(128 + SIGKILL);
  }

  uint64_t deliverable = pending & ~proc->blocked_signals;
  if (deliverable == 0)
    return;

  for (int sig = 1; sig < SIG_MAX; sig++) {
    if (!(deliverable & (1ULL << sig)))
      continue;
    __atomic_fetch_and(&proc->pending_signals, ~(1ULL << sig),
                       __ATOMIC_ACQ_REL);
    deliver(proc, sig, regs);
    return;
  }
}

// ---------------------------------------------------------------------------
// [4.3] Entrega inmediata de una señal usando los registers del fault.
// La usa pf.c cuando una excepción (#PF, #GP, #UD...) en userland debe
// convertirse en señal para el proceso actual.
//
// Si el handler está instalado, redirige el trap frame al handler y
// retorna 1 (el fault ya está resuelto).
// Si es SIG_DFL, actúa según la acción por defecto (típicamente matar)
// y NO retorna.
// ---------------------------------------------------------------------------
int signal_deliver_from_exception(int sig, registers_t *regs) {
  process_t *proc = process_current();
  if (!proc || !regs)
    return 0;
  return deliver(proc, sig, regs);
}

// ---------------------------------------------------------------------------
// Syscalls
// ---------------------------------------------------------------------------
int64_t k_rt_sigaction(uint64_t sig, uint64_t act, uint64_t oact,
                       uint64_t sigsetsize, uint64_t _) {
  (void)_;
  if (sig < 1 || sig >= SIG_MAX)
    return -EINVAL;
  if (sig == SIGKILL || sig == SIGSTOP)
    return -EINVAL;
  if (sigsetsize != 8)
    return -EINVAL;

  process_t *proc = process_current();
  if (!proc)
    return -EFAULT;

  if (oact) {
    if (!access_ok((void *)oact, sizeof(k_sigaction_t)))
      return -EFAULT;
    if (copy_to_user((void *)oact, &proc->sigactions[sig],
                     sizeof(k_sigaction_t)) < 0)
      return -EFAULT;
  }
  if (act) {
    if (!access_ok((void *)act, sizeof(k_sigaction_t)))
      return -EFAULT;
    k_sigaction_t na;
    if (copy_from_user(&na, (void *)act, sizeof(k_sigaction_t)) < 0)
      return -EFAULT;
    proc->sigactions[sig] = na;
  }
  return 0;
}

int64_t k_rt_sigreturn(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                       uint64_t a5) {
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  process_t *proc = process_current();
  registers_t *regs = syscall_current_regs();
  if (!proc || !regs)
    return -EFAULT;

  // Al entrar en rt_sigreturn, RSP = frame_va + 8 (porque el `ret`
  // del handler ya ha popeado pretcode). El ucontext empieza justo
  // en RSP. uc_mcontext está a offset 0x28 desde ahí.
  uint64_t rsp = regs->rsp;
  uint64_t mc = rsp + 0x28;
  uint64_t g[23];
  if (!access_ok((void *)mc, 23 * 8))
    return -EFAULT;
  if (copy_from_user(g, (void *)mc, 23 * 8) < 0)
    return -EFAULT;

  regs->r8 = g[0];
  regs->r9 = g[1];
  regs->r10 = g[2];
  regs->r11 = g[3];
  regs->r12 = g[4];
  regs->r13 = g[5];
  regs->r14 = g[6];
  regs->r15 = g[7];
  regs->rdi = g[8];
  regs->rsi = g[9];
  regs->rbp = g[10];
  regs->rbx = g[11];
  regs->rdx = g[12];
  regs->rax = g[13];
  regs->rcx = g[14];
  regs->rsp = g[15];
  regs->rip = g[16];
  regs->rflags = g[17];
  // g[18..22] = cs/gs/fs, err, trapno, oldmask, cr2 → ignorar
  regs->cs = 0x1B;
  regs->ss = 0x23;

  uint64_t mask = 0;
  if (copy_from_user(&mask, (void *)(rsp + 0x128), 8) < 0)
    return -EFAULT;
  proc->blocked_signals = mask;

  return (int64_t)regs->rax;
}

// SIG_BLOCK=0, SIG_UNBLOCK=1, SIG_SETMASK=2.
int64_t k_rt_sigprocmask(uint64_t how, uint64_t set, uint64_t oldset,
                         uint64_t sigsetsize, uint64_t a5) {
  (void)a5;
  if (sigsetsize != 8)
    return -EINVAL;

  process_t *proc = process_current();
  if (!proc)
    return -EFAULT;

  if (oldset) {
    if (!access_ok((void *)oldset, 8))
      return -EFAULT;
    uint64_t old = proc->blocked_signals;
    if (copy_to_user((void *)oldset, &old, 8) < 0)
      return -EFAULT;
  }
  if (set) {
    if (!access_ok((void *)set, 8))
      return -EFAULT;
    uint64_t s = 0;
    if (copy_from_user(&s, (void *)set, 8) < 0)
      return -EFAULT;

    if (how == 0)
      proc->blocked_signals |= s;
    else if (how == 1)
      proc->blocked_signals &= ~s;
    else if (how == 2)
      proc->blocked_signals = s;
    else
      return -EINVAL;

    // SIGKILL y SIGSTOP no son bloqueables.
    proc->blocked_signals &= ~((1ULL << SIGKILL) | (1ULL << SIGSTOP));
  }
  return 0;
}

uint64_t signal_filter_ignored(process_t *p, uint64_t mask) {
  if (!p)
    return mask;
  for (int sig = 1; sig < SIG_MAX; sig++) {
    if (!(mask & (1ULL << sig)))
      continue;
    if (sig == SIGKILL || sig == SIGSTOP)
      continue;

    // [FIX JOB] SIGCHLD nunca se filtra. Aunque su acción por defecto
    // sea IGN, el shell necesita que le llegue la interrupción para
    // despertar de read() y llamar a wait4(WNOHANG). Linux hace lo
    // mismo: do_notify_parent() envía SIGCHLD al padre y despierta
    // su wait queue incondicionalmente.
    //
    // Sin este caso especial, `kill %1` sobre un job stopped dejaba
    // el zombie vivo en process_list y `jobs` seguía mostrando el
    // job como Running.
    if (sig == SIGCHLD)
      continue;

    // [FIX JOB] SIGCONT tampoco. Si el proceso estaba stopped,
    // process_signal_pid_ex ya lo reanudó antes de llegar aquí; si
    // no, es un no-op que Linux descarta sin encolarlo. Filtrarlo
    // aquí evita un EINTR espurio en wait_common (bug original de
    // nanosleep + bg).
    if (sig == SIGCONT)
      continue;

    void (*h)(int) = p->sigactions[sig].handler;
    if (h == SIG_IGN ||
        (h == SIG_DFL && signal_default_action(sig) == SIG_ACT_IGN))
      mask &= ~(1ULL << sig);
  }
  return mask;
}