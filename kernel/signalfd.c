// kernel/signalfd.c
//
// signalfd4 / signalfd. Crea un fd del que se pueden leer señales
// pendientes como struct signalfd_siginfo (128 bytes).
//
// Semántica Linux:
//   - Al crear el signalfd con un mask, esas señales se bloquean en el
//     proceso (blocked_signals |= mask). Así quedan pending_signals
//     pero no se entregan por handler.
//   - read() consume la primera señal pendiente del mask y la
//     convierte a siginfo.
//   - signalfd4(fd >= 0, mask, ...) modifica el mask del signalfd
//     existente (no crea uno nuevo).
//   - Al cerrar el fd, se desbloquea el mask.

#include "signalfd.h"
#include "heap.h"
#include "klog.h"
#include "mutex.h"
#include "process.h"
#include "sched.h"
#include "signal.h"
#include "spinlock.h"
#include "string.h"
#include "uaccess.h"
#include "vfs.h"
#include "wait.h"
#include <stddef.h>
#include <stdint.h>

#define SIGNALFD_MAX_PER_PROC 8

#define SFD_NONBLOCK 0x800
#define SFD_CLOEXEC 0x80000

// Linux x86_64 layout, 128 bytes.
struct signalfd_siginfo {
  uint32_t ssi_signo;
  int32_t ssi_errno;
  int32_t ssi_code;
  uint32_t ssi_pid;
  uint32_t ssi_uid;
  int32_t ssi_fd;
  uint32_t ssi_tid;
  uint32_t ssi_band;
  uint32_t ssi_overrun;
  uint32_t ssi_trapno;
  int32_t ssi_status;
  int32_t ssi_int;
  uint64_t ssi_ptr;
  uint64_t ssi_utime;
  uint64_t ssi_stime;
  uint64_t ssi_addr;
  uint16_t ssi_addr_lsb;
  uint16_t __pad2;
  int32_t ssi_syscall;
  uint64_t ssi_call_addr;
  uint32_t ssi_arch;
  uint8_t __pad[28];
};
_Static_assert(sizeof(struct signalfd_siginfo) == 128, "signalfd_siginfo");

typedef struct signalfd_ctx {
  uint64_t mask;
  int flags;
  wait_queue_t wq;
  struct signalfd_ctx *next;
} signalfd_ctx_t;

// ---------------------------------------------------------------------------
// ops del nodo
// ---------------------------------------------------------------------------
static int signalfd_close_op(vfs_node_t *node) {
  signalfd_ctx_t *ctx = (signalfd_ctx_t *)node->priv;
  if (!ctx)
    return 0;

  // Desbloquear las señales del mask (como Linux al cerrar).
  process_t *proc = process_current();
  if (proc)
    proc->blocked_signals &= ~ctx->mask;

  kfree(ctx);
  node->priv = NULL;
  return 0;
}

static bool signalfd_readable_cond(void *arg) {
  signalfd_ctx_t *ctx = (signalfd_ctx_t *)arg;
  process_t *proc = process_current();
  if (!proc)
    return true;
  uint64_t pend = __atomic_load_n(&proc->pending_signals, __ATOMIC_ACQUIRE);
  return (pend & ctx->mask) != 0;
}

static int64_t signalfd_read_op(vfs_node_t *node, uint64_t off, size_t sz,
                                void *buf) {
  (void)off;
  if (sz < sizeof(struct signalfd_siginfo))
    return -EINVAL;
  signalfd_ctx_t *ctx = (signalfd_ctx_t *)node->priv;
  if (!ctx)
    return -EINVAL;
  process_t *proc = process_current();
  if (!proc)
    return -EFAULT;

  for (;;) {
    uint64_t pend =
        __atomic_load_n(&proc->pending_signals, __ATOMIC_ACQUIRE) & ctx->mask;
    if (pend != 0) {
      int sig = __builtin_ctzll(pend);
      // Consumir del pending.
      __atomic_fetch_and(&proc->pending_signals, ~(1ULL << sig),
                         __ATOMIC_ACQ_REL);

      struct signalfd_siginfo si;
      memset(&si, 0, sizeof(si));
      si.ssi_signo = (uint32_t)sig;
      si.ssi_code = 0; // SI_USER
      si.ssi_pid = proc->pid;
      si.ssi_uid = proc->uid;
      // buf ya viene del kernel.
      memcpy(buf, &si, sizeof(si));
      return (int64_t)sizeof(si);
    }

    if (ctx->flags & SFD_NONBLOCK)
      return -EAGAIN;

    int rc = wait_event_interruptible(&ctx->wq, signalfd_readable_cond, ctx);
    if (rc < 0)
      return rc;
  }
}

static int signalfd_poll_op(vfs_node_t *node, short events) {
  signalfd_ctx_t *ctx = (signalfd_ctx_t *)node->priv;
  if (!ctx)
    return 0;
  process_t *proc = process_current();
  if (!proc)
    return 0;
  int rev = 0;
  uint64_t pend =
      __atomic_load_n(&proc->pending_signals, __ATOMIC_ACQUIRE) & ctx->mask;
  if ((events & 1) && pend)
    rev |= 1; // POLLIN
  return rev;
}

static wait_queue_t *signalfd_poll_wq_op(vfs_node_t *node) {
  signalfd_ctx_t *ctx = (signalfd_ctx_t *)node->priv;
  return ctx ? &ctx->wq : NULL;
}

static vfs_ops_t signalfd_ops = {
    .read = signalfd_read_op,
    .poll = signalfd_poll_op,
    .poll_wq = signalfd_poll_wq_op, // ← AÑADIR
    .close = signalfd_close_op,
};

// ---------------------------------------------------------------------------
// Install
// ---------------------------------------------------------------------------
static int signalfd_install_fd(process_t *proc, int fd_num,
                               signalfd_ctx_t *ctx) {
  vfs_node_t *n = (vfs_node_t *)kzalloc(sizeof(vfs_node_t));
  if (!n)
    return -ENOMEM;
  const char *nm = "anon_inode:[signalfd]";
  size_t nl = strlen(nm);
  if (nl >= sizeof(n->name))
    nl = sizeof(n->name) - 1;
  memcpy(n->name, nm, nl);
  n->name[nl] = '\0';
  n->flags = VFS_FILE;
  n->ops = &signalfd_ops;
  n->priv = ctx;
  n->mode = S_IFREG | 0600;
  n->ref_count = 1;

  file_descriptor_t *fd =
      (file_descriptor_t *)kzalloc(sizeof(file_descriptor_t));
  if (!fd) {
    vfs_node_free(n);
    return -ENOMEM;
  }
  fd->node = n;
  fd->offset = 0;
  fd->flags = O_RDONLY;
  fd->ref_count = 1;
  fd->flock_mode = 0;
  mutex_init(&fd->lock);
  wait_queue_init(&fd->read_wq);
  wait_queue_init(&fd->write_wq);

  proc->fds[fd_num] = fd;
  return 0;
}

// ---------------------------------------------------------------------------
// Crear / modificar
// ---------------------------------------------------------------------------
static int64_t signalfd_common(uint64_t fd_arg, uint64_t mask_uptr,
                               uint64_t sizemask, uint64_t flags) {
  if (sizemask != 8)
    return -EINVAL;
  if (flags & ~(uint64_t)(SFD_NONBLOCK | SFD_CLOEXEC))
    return -EINVAL;

  uint64_t mask = 0;
  if (!access_ok((void *)mask_uptr, 8))
    return -EFAULT;
  if (copy_from_user(&mask, (void *)mask_uptr, 8) < 0)
    return -EFAULT;

  // SIGKILL y SIGSTOP no son bloqueables.
  mask &= ~((1ULL << SIGKILL) | (1ULL << SIGSTOP));

  process_t *proc = process_current();
  if (!proc)
    return -EFAULT;

  int32_t kfd = (int32_t)fd_arg;

  // Modificar signalfd existente.
  if (kfd != -1) {
    if (kfd < 0 || kfd >= MAX_PROCESS_FDS || !proc->fds[kfd])
      return -EBADF;
    file_descriptor_t *f = proc->fds[kfd];
    if (f->node->ops != &signalfd_ops)
      return -EINVAL;
    signalfd_ctx_t *ctx = (signalfd_ctx_t *)f->node->priv;
    if (!ctx)
      return -EINVAL;

    // Desbloquear el mask anterior, bloquear el nuevo.
    proc->blocked_signals &= ~ctx->mask;
    ctx->mask = mask;
    proc->blocked_signals |= mask;
    wake_up_all(&ctx->wq);
    return (int64_t)kfd;
  }

  // Crear nuevo.
  int free_fd = -1;
  for (int i = 0; i < MAX_PROCESS_FDS; i++) {
    if (!proc->fds[i]) {
      free_fd = i;
      break;
    }
  }
  if (free_fd < 0)
    return -EMFILE;

  signalfd_ctx_t *ctx = (signalfd_ctx_t *)kzalloc(sizeof(*ctx));
  if (!ctx)
    return -ENOMEM;
  wait_queue_init(&ctx->wq);
  ctx->mask = mask;
  ctx->flags = (int)flags;

  int rc = signalfd_install_fd(proc, free_fd, ctx);
  if (rc != 0) {
    kfree(ctx);
    return rc;
  }

  proc->blocked_signals |= mask;
  if (flags & SFD_CLOEXEC)
    proc->fd_cloexec_mask |= (1u << free_fd);
  return (int64_t)free_fd;
}

int64_t k_signalfd(uint64_t fd, uint64_t mask_uptr, uint64_t sizemask,
                   uint64_t a4, uint64_t a5) {
  (void)a4;
  (void)a5;
  return signalfd_common(fd, mask_uptr, sizemask, 0);
}

int64_t k_signalfd4(uint64_t fd, uint64_t mask_uptr, uint64_t sizemask,
                    uint64_t flags, uint64_t a5) {
  (void)a5;
  return signalfd_common(fd, mask_uptr, sizemask, flags);
}

// ---------------------------------------------------------------------------
// Notify / cleanup
// ---------------------------------------------------------------------------
void signalfd_notify(struct process *proc, uint64_t sig_mask) {
  if (!proc)
    return;
  for (int i = 0; i < MAX_PROCESS_FDS; i++) {
    file_descriptor_t *f = proc->fds[i];
    if (!f || !f->node || f->node->ops != &signalfd_ops)
      continue;
    signalfd_ctx_t *ctx = (signalfd_ctx_t *)f->node->priv;
    if (!ctx)
      continue;
    if (ctx->mask & sig_mask)
      wake_up_all(&ctx->wq);
  }
}

void signalfd_cleanup(struct process *proc) {
  if (!proc)
    return;
  for (int i = 0; i < MAX_PROCESS_FDS; i++) {
    file_descriptor_t *f = proc->fds[i];
    if (!f || !f->node || f->node->ops != &signalfd_ops)
      continue;
    signalfd_ctx_t *ctx = (signalfd_ctx_t *)f->node->priv;
    if (ctx)
      proc->blocked_signals &= ~ctx->mask;
  }
}