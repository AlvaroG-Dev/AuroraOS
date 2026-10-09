// kernel/epoll.c
//
// epoll + eventfd con wake real vía wait_queue_t::subs.
//
// epoll subscribe su propia wq (ep->wq) a la wq natural de cada fd
// observado (node->ops->poll_wq, o fallback a fd->read_wq). Cuando
// la wq del fd se despierta (pipe, pty, tty, eventfd, signalfd), la
// cascada en wake_up_all_locked despierta ep->wq, y epoll_wait
// re-evalúa el estado de todos los fds.
//
// Semántica: level-triggered siempre (no EPOLLET). Wakeup inmediato,
// sin polling periódico.

#include "epoll.h"
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

// ============ constantes ============
#define EPOLLIN 0x001u
#define EPOLLPRI 0x002u
#define EPOLLOUT 0x004u
#define EPOLLERR 0x008u
#define EPOLLHUP 0x010u
#define EPOLLNVAL 0x020u

#define EPOLL_CTL_ADD 1
#define EPOLL_CTL_DEL 2
#define EPOLL_CTL_MOD 3

#define EPOLL_CLOEXEC 0x80000u

#define EFD_SEMAPHORE 1
#define EFD_NONBLOCK 0x800
#define EFD_CLOEXEC 0x80000

// Cap de espera por iteración: si un wake se pierde, re-poll a los
// 1000 ms. Con wakeups funcionando, es irrelevante.
#define EPOLL_WAIT_CAP_TICKS 1000

// ============ estructuras ============
typedef struct epoll_entry {
  int fd;
  uint32_t events;
  uint64_t data;
  file_descriptor_t *target;   // ref extra
  wait_queue_t *subscribed_wq; // wq a la que estamos suscritos
  struct epoll_entry *next;
} epoll_entry_t;

typedef struct epoll_instance {
  spinlock_t lock;
  wait_queue_t wq;
  epoll_entry_t *entries;
} epoll_t;

typedef struct eventfd_ctx {
  spinlock_t lock;
  wait_queue_t wq; // única wq (read+write); el poll_wq la expone
  uint64_t counter;
  int flags;
} eventfd_ctx_t;

// ============ helpers fd ============
static int fd_get_ref(process_t *proc, int fd, file_descriptor_t **out) {
  if (!proc || fd < 0 || fd >= MAX_PROCESS_FDS)
    return -EBADF;
  file_descriptor_t *f = proc->fds[fd];
  if (!f)
    return -EBADF;
  __atomic_fetch_add(&f->ref_count, 1, __ATOMIC_ACQ_REL);
  *out = f;
  return 0;
}

static void fd_put_ref(file_descriptor_t *f) {
  if (!f)
    return;
  int left = __atomic_sub_fetch(&f->ref_count, 1, __ATOMIC_ACQ_REL);
  if (left == 0) {
    if (f->node)
      vfs_node_free(f->node);
    kfree(f);
  }
}

// ============ epoll node ops ============
static int epoll_node_close(vfs_node_t *node) {
  epoll_t *ep = (epoll_t *)node->priv;
  if (!ep)
    return 0;
  epoll_entry_t *e = ep->entries;
  while (e) {
    epoll_entry_t *next = e->next;
    if (e->subscribed_wq)
      wait_queue_unsubscribe(e->subscribed_wq, &ep->wq);
    fd_put_ref(e->target);
    kfree(e);
    e = next;
  }
  kfree(ep);
  node->priv = NULL;
  return 0;
}

static vfs_ops_t epoll_node_ops = {
    .close = epoll_node_close,
};

// ============ epoll_create / epoll_create1 ============
static int64_t epoll_create_internal(int flags) {
  process_t *proc = process_current();
  if (!proc)
    return -EFAULT;

  int free_fd = -1;
  for (int i = 0; i < MAX_PROCESS_FDS; i++) {
    if (!proc->fds[i]) {
      free_fd = i;
      break;
    }
  }
  if (free_fd < 0)
    return -EMFILE;

  epoll_t *ep = (epoll_t *)kzalloc(sizeof(epoll_t));
  if (!ep)
    return -ENOMEM;
  spin_init(&ep->lock);
  wait_queue_init(&ep->wq);
  ep->entries = NULL;

  vfs_node_t *n = (vfs_node_t *)kzalloc(sizeof(vfs_node_t));
  if (!n) {
    kfree(ep);
    return -ENOMEM;
  }
  const char *nm = "anon_inode:[eventpoll]";
  size_t nl = strlen(nm);
  if (nl >= sizeof(n->name))
    nl = sizeof(n->name) - 1;
  memcpy(n->name, nm, nl);
  n->name[nl] = '\0';
  n->flags = VFS_FILE;
  n->ops = &epoll_node_ops;
  n->priv = ep;
  n->mode = S_IFREG | 0600;
  n->ref_count = 1;

  file_descriptor_t *fd =
      (file_descriptor_t *)kzalloc(sizeof(file_descriptor_t));
  if (!fd) {
    vfs_node_free(n);
    return -ENOMEM;
  }
  fd->node = n;
  fd->flags = O_RDWR;
  fd->ref_count = 1;
  mutex_init(&fd->lock);
  wait_queue_init(&fd->read_wq);
  wait_queue_init(&fd->write_wq);

  proc->fds[free_fd] = fd;
  if (flags & EPOLL_CLOEXEC)
    proc->fd_cloexec_mask |= (1u << free_fd);
  return (int64_t)free_fd;
}

int64_t k_epoll_create(uint64_t size, uint64_t a2, uint64_t a3, uint64_t a4,
                       uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  if ((int32_t)size <= 0)
    return -EINVAL;
  return epoll_create_internal(0);
}

int64_t k_epoll_create1(uint64_t flags, uint64_t a2, uint64_t a3, uint64_t a4,
                        uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  if (flags & ~(uint64_t)EPOLL_CLOEXEC)
    return -EINVAL;
  return epoll_create_internal((int)flags);
}

// ============ epoll_ctl ============
static epoll_entry_t *epoll_find(epoll_t *ep, int fd) {
  for (epoll_entry_t *e = ep->entries; e; e = e->next) {
    if (e->fd == fd)
      return e;
  }
  return NULL;
}

static void epoll_remove_locked(epoll_t *ep, epoll_entry_t *prev,
                                epoll_entry_t *victim) {
  if (prev)
    prev->next = victim->next;
  else
    ep->entries = victim->next;
  if (victim->subscribed_wq)
    wait_queue_unsubscribe(victim->subscribed_wq, &ep->wq);
  fd_put_ref(victim->target);
  kfree(victim);
}

int64_t k_epoll_ctl(uint64_t epfd, uint64_t op, uint64_t fd,
                    uint64_t event_uptr, uint64_t a5) {
  (void)a5;
  process_t *proc = process_current();
  if (!proc)
    return -EFAULT;

  int epfd_n = (int)epfd;
  if (epfd_n < 0 || epfd_n >= MAX_PROCESS_FDS || !proc->fds[epfd_n])
    return -EBADF;
  file_descriptor_t *epfd_fd = proc->fds[epfd_n];
  if (epfd_fd->node->ops != &epoll_node_ops)
    return -EINVAL;
  epoll_t *ep = (epoll_t *)epfd_fd->node->priv;

  int target_fd_num = (int)fd;
  if (target_fd_num < 0 || target_fd_num >= MAX_PROCESS_FDS)
    return -EBADF;
  // No permitir un self-loop (epoll observándose a sí mismo).
  if (target_fd_num == epfd_n)
    return -EINVAL;

  struct k_epoll_event uev;
  int has_ev = 0;
  if (event_uptr) {
    if (!access_ok((void *)event_uptr, sizeof(uev)))
      return -EFAULT;
    if (copy_from_user(&uev, (void *)event_uptr, sizeof(uev)) < 0)
      return -EFAULT;
    has_ev = 1;
  }

  int op32 = (int)op;

  if (op32 == EPOLL_CTL_ADD) {
    if (!has_ev)
      return -EFAULT;
    file_descriptor_t *target;
    int rc = fd_get_ref(proc, target_fd_num, &target);
    if (rc != 0)
      return rc;

    unsigned long flags = spin_lock_irqsave(&ep->lock);
    if (epoll_find(ep, target_fd_num)) {
      spin_unlock_irqrestore(&ep->lock, flags);
      fd_put_ref(target);
      return -EEXIST;
    }
    epoll_entry_t *e = (epoll_entry_t *)kzalloc(sizeof(*e));
    if (!e) {
      spin_unlock_irqrestore(&ep->lock, flags);
      fd_put_ref(target);
      return -ENOMEM;
    }
    e->fd = target_fd_num;
    e->events = uev.events;
    e->data = uev.data;
    e->target = target;

    // Suscribir ep->wq a la wq natural del fd. Preferimos poll_wq
    // (contrato explícito); si el nodo no la implementa, caemos al
    // read_wq del propio fd (mismo criterio que do_poll_common).
    wait_queue_t *wq = NULL;
    if (target->node && target->node->ops && target->node->ops->poll_wq)
      wq = target->node->ops->poll_wq(target->node);
    if (!wq)
      wq = &target->read_wq;
    e->subscribed_wq = wq;
    if (wq)
      wait_queue_subscribe(wq, &ep->wq);

    e->next = ep->entries;
    ep->entries = e;
    spin_unlock_irqrestore(&ep->lock, flags);
    wake_up_all(&ep->wq);
    return 0;
  }

  if (op32 == EPOLL_CTL_DEL) {
    unsigned long flags = spin_lock_irqsave(&ep->lock);
    epoll_entry_t *prev = NULL;
    epoll_entry_t *victim = NULL;
    for (epoll_entry_t *e = ep->entries; e; e = e->next) {
      if (e->fd == target_fd_num) {
        victim = e;
        break;
      }
      prev = e;
    }
    if (!victim) {
      spin_unlock_irqrestore(&ep->lock, flags);
      return -ENOENT;
    }
    epoll_remove_locked(ep, prev, victim);
    spin_unlock_irqrestore(&ep->lock, flags);
    return 0;
  }

  if (op32 == EPOLL_CTL_MOD) {
    if (!has_ev)
      return -EFAULT;
    unsigned long flags = spin_lock_irqsave(&ep->lock);
    epoll_entry_t *e = epoll_find(ep, target_fd_num);
    if (!e) {
      spin_unlock_irqrestore(&ep->lock, flags);
      return -ENOENT;
    }
    e->events = uev.events;
    e->data = uev.data;
    spin_unlock_irqrestore(&ep->lock, flags);
    wake_up_all(&ep->wq);
    return 0;
  }

  return -EINVAL;
}

// ============ epoll_wait ============
struct epoll_wait_ctx {
  epoll_t *ep;
};

static bool epoll_any_ready(void *arg) {
  struct epoll_wait_ctx *ctx = (struct epoll_wait_ctx *)arg;
  epoll_t *ep = ctx->ep;
  unsigned long flags = spin_lock_irqsave(&ep->lock);
  bool ready = false;
  for (epoll_entry_t *e = ep->entries; e; e = e->next) {
    if (!e->target || !e->target->node)
      continue;
    short req = 0;
    if (e->events & EPOLLIN)
      req |= 1;
    if (e->events & EPOLLOUT)
      req |= 4;
    if (e->events & EPOLLPRI)
      req |= 2;
    int rev = vfs_node_poll(e->target->node, req);
    if (rev != 0) {
      ready = true;
      break;
    }
  }
  spin_unlock_irqrestore(&ep->lock, flags);
  return ready;
}

int64_t k_epoll_wait(uint64_t epfd, uint64_t events_uptr, uint64_t maxevents,
                     uint64_t timeout, uint64_t a5) {
  (void)a5;
  process_t *proc = process_current();
  if (!proc)
    return -EFAULT;

  int epfd_n = (int)epfd;
  if (epfd_n < 0 || epfd_n >= MAX_PROCESS_FDS || !proc->fds[epfd_n])
    return -EBADF;
  file_descriptor_t *epfd_fd = proc->fds[epfd_n];
  if (epfd_fd->node->ops != &epoll_node_ops)
    return -EINVAL;
  epoll_t *ep = (epoll_t *)epfd_fd->node->priv;

  int maxev = (int)maxevents;
  if (maxev <= 0)
    return -EINVAL;
  if (maxev > 1024)
    maxev = 1024;
  if (!access_ok((void *)events_uptr,
                 (size_t)maxev * sizeof(struct k_epoll_event)))
    return -EFAULT;

  int32_t timeout_ms = (int32_t)timeout;
  uint64_t deadline = 0;
  if (timeout_ms >= 0)
    deadline = sched_get_ticks() + (uint64_t)timeout_ms;

  struct k_epoll_event *kout = (struct k_epoll_event *)kmalloc(
      (size_t)maxev * sizeof(struct k_epoll_event));
  if (!kout)
    return -ENOMEM;

  struct epoll_wait_ctx wctx = {.ep = ep};

  for (;;) {
    int ready = 0;
    unsigned long flags = spin_lock_irqsave(&ep->lock);
    for (epoll_entry_t *e = ep->entries; e && ready < maxev; e = e->next) {
      if (!e->target || !e->target->node)
        continue;
      short req = 0;
      if (e->events & EPOLLIN)
        req |= 1;
      if (e->events & EPOLLOUT)
        req |= 4;
      if (e->events & EPOLLPRI)
        req |= 2;
      int rev = vfs_node_poll(e->target->node, req);
      if (rev == 0)
        continue;
      uint32_t reported = 0;
      if ((rev & 1) && (e->events & EPOLLIN))
        reported |= EPOLLIN;
      if ((rev & 4) && (e->events & EPOLLOUT))
        reported |= EPOLLOUT;
      if ((rev & 2) && (e->events & EPOLLPRI))
        reported |= EPOLLPRI;
      if (rev & 8)
        reported |= EPOLLERR;
      if (rev & 16)
        reported |= EPOLLHUP;
      if (reported == 0)
        continue;
      kout[ready].events = reported;
      kout[ready].data = e->data;
      ready++;
    }
    spin_unlock_irqrestore(&ep->lock, flags);

    if (ready > 0) {
      if (copy_to_user((void *)events_uptr, kout,
                       (size_t)ready * sizeof(struct k_epoll_event)) < 0) {
        kfree(kout);
        return -EFAULT;
      }
      kfree(kout);
      return ready;
    }

    if (timeout_ms == 0) {
      kfree(kout);
      return 0;
    }

    uint64_t wait_ticks;
    if (timeout_ms < 0) {
      wait_ticks = EPOLL_WAIT_CAP_TICKS;
    } else {
      uint64_t now = sched_get_ticks();
      if (now >= deadline) {
        kfree(kout);
        return 0;
      }
      wait_ticks = deadline - now;
      if (wait_ticks > EPOLL_WAIT_CAP_TICKS)
        wait_ticks = EPOLL_WAIT_CAP_TICKS;
    }

    // Bloquea en ep->wq hasta que un fd observado se vuelva listo
    // (cascada desde su poll_wq) o hasta el timeout.
    long rc = wait_event_interruptible_timeout(&ep->wq, epoll_any_ready, &wctx,
                                               wait_ticks);
    if (rc < 0) {
      kfree(kout);
      return -EINTR;
    }
    // rc >= 0 → re-poll y devolver si hay algo.
  }
}

// ============ epoll_pwait / epoll_pwait2 ============
int64_t k_epoll_pwait(uint64_t epfd, uint64_t events_uptr, uint64_t maxevents,
                      uint64_t timeout, uint64_t sigmask_uptr) {
  process_t *proc = process_current();
  if (!proc)
    return -EFAULT;
  uint64_t saved_mask = proc->blocked_signals;
  int changed = 0;
  if (sigmask_uptr) {
    if (!access_ok((void *)sigmask_uptr, 8))
      return -EFAULT;
    uint64_t m = 0;
    if (copy_from_user(&m, (void *)sigmask_uptr, 8) < 0)
      return -EFAULT;
    m &= ~((1ULL << SIGKILL) | (1ULL << SIGSTOP));
    proc->blocked_signals = m;
    changed = 1;
  }
  int64_t rc = k_epoll_wait(epfd, events_uptr, maxevents, timeout, 0);
  if (changed)
    proc->blocked_signals = saved_mask;
  return rc;
}

int64_t k_epoll_pwait2(uint64_t epfd, uint64_t events_uptr, uint64_t maxevents,
                       uint64_t ts_ptr, uint64_t sigmask_uptr) {
  uint64_t timeout_ms = 0;
  int infinite = 0;
  if (ts_ptr) {
    if (!access_ok((void *)ts_ptr, 16))
      return -EFAULT;
    struct {
      int64_t sec;
      int64_t nsec;
    } ts;
    if (copy_from_user(&ts, (void *)ts_ptr, 16) < 0)
      return -EFAULT;
    if (ts.sec < 0 || ts.nsec < 0)
      return -EINVAL;
    int64_t ms = ts.sec * 1000 + ts.nsec / 1000000;
    if (ms > 0x7FFFFFFF)
      ms = 0x7FFFFFFF;
    timeout_ms = (uint64_t)ms;
  } else {
    infinite = 1;
  }
  uint64_t t = infinite ? (uint64_t)(int64_t)-1 : timeout_ms;
  return k_epoll_pwait(epfd, events_uptr, maxevents, t, sigmask_uptr);
}

// ============ eventfd ============
static bool eventfd_has_data(void *arg) {
  eventfd_ctx_t *ctx = (eventfd_ctx_t *)arg;
  return __atomic_load_n(&ctx->counter, __ATOMIC_ACQUIRE) > 0;
}

static bool eventfd_has_space(void *arg) {
  eventfd_ctx_t *ctx = (eventfd_ctx_t *)arg;
  return __atomic_load_n(&ctx->counter, __ATOMIC_ACQUIRE) <
         0xfffffffffffffffeULL;
}

static int64_t eventfd_read_op(vfs_node_t *node, uint64_t off, size_t sz,
                               void *buf) {
  (void)off;
  if (sz < 8)
    return -EINVAL;
  eventfd_ctx_t *ctx = (eventfd_ctx_t *)node->priv;
  if (!ctx)
    return -EINVAL;

  if (ctx->flags & EFD_NONBLOCK) {
    if (__atomic_load_n(&ctx->counter, __ATOMIC_ACQUIRE) == 0)
      return -EAGAIN;
  } else {
    int rc = wait_event_interruptible(&ctx->wq, eventfd_has_data, ctx);
    if (rc < 0)
      return rc;
  }

  unsigned long flags = spin_lock_irqsave(&ctx->lock);
  uint64_t v;
  if (ctx->flags & EFD_SEMAPHORE) {
    v = 1;
    ctx->counter -= 1;
  } else {
    v = ctx->counter;
    ctx->counter = 0;
  }
  spin_unlock_irqrestore(&ctx->lock, flags);

  // buf ya viene del kernel (vfs_read_for_proc copió el chunk). No
  // hacemos copy_to_user aquí: escribimos directo al buffer de kernel.
  memcpy(buf, &v, 8);
  wake_up_all(&ctx->wq);
  return 8;
}

static int64_t eventfd_write_op(vfs_node_t *node, uint64_t off, size_t sz,
                                const void *buf) {
  (void)off;
  if (sz < 8)
    return -EINVAL;
  eventfd_ctx_t *ctx = (eventfd_ctx_t *)node->priv;
  if (!ctx)
    return -EINVAL;

  uint64_t v;
  // buf ya viene del kernel (vfs_write_for_proc copió el chunk).
  memcpy(&v, buf, 8);
  if (v == 0xffffffffffffffffULL)
    return -EINVAL;

  for (;;) {
    unsigned long flags = spin_lock_irqsave(&ctx->lock);
    uint64_t c = ctx->counter;
    if (c <= 0xfffffffffffffffeULL - v) {
      ctx->counter = c + v;
      spin_unlock_irqrestore(&ctx->lock, flags);
      break;
    }
    spin_unlock_irqrestore(&ctx->lock, flags);
    if (ctx->flags & EFD_NONBLOCK)
      return -EAGAIN;
    int rc = wait_event_interruptible(&ctx->wq, eventfd_has_space, ctx);
    if (rc < 0)
      return rc;
  }
  wake_up_all(&ctx->wq);
  return 8;
}

static int eventfd_poll_op(vfs_node_t *node, short events) {
  eventfd_ctx_t *ctx = (eventfd_ctx_t *)node->priv;
  int rev = 0;
  uint64_t c = __atomic_load_n(&ctx->counter, __ATOMIC_ACQUIRE);
  if ((events & 1) && c > 0)
    rev |= 1;
  if ((events & 4) && c < 0xfffffffffffffffeULL)
    rev |= 4;
  return rev;
}

static wait_queue_t *eventfd_poll_wq_op(vfs_node_t *node) {
  eventfd_ctx_t *ctx = (eventfd_ctx_t *)node->priv;
  return ctx ? &ctx->wq : NULL;
}

static int eventfd_close_op(vfs_node_t *node) {
  eventfd_ctx_t *ctx = (eventfd_ctx_t *)node->priv;
  if (ctx)
    kfree(ctx);
  node->priv = NULL;
  return 0;
}

static vfs_ops_t eventfd_node_ops = {
    .read = eventfd_read_op,
    .write = eventfd_write_op,
    .poll = eventfd_poll_op,
    .poll_wq = eventfd_poll_wq_op,
    .close = eventfd_close_op,
};

static int64_t eventfd_create_internal(uint64_t initval, int flags) {
  if (flags & ~(EFD_SEMAPHORE | EFD_NONBLOCK | EFD_CLOEXEC))
    return -EINVAL;
  process_t *proc = process_current();
  if (!proc)
    return -EFAULT;

  int free_fd = -1;
  for (int i = 0; i < MAX_PROCESS_FDS; i++) {
    if (!proc->fds[i]) {
      free_fd = i;
      break;
    }
  }
  if (free_fd < 0)
    return -EMFILE;

  eventfd_ctx_t *ctx = (eventfd_ctx_t *)kzalloc(sizeof(*ctx));
  if (!ctx)
    return -ENOMEM;
  spin_init(&ctx->lock);
  wait_queue_init(&ctx->wq);
  ctx->counter = initval;
  ctx->flags = flags;

  vfs_node_t *n = (vfs_node_t *)kzalloc(sizeof(vfs_node_t));
  if (!n) {
    kfree(ctx);
    return -ENOMEM;
  }
  const char *nm = "anon_inode:[eventfd]";
  size_t nl = strlen(nm);
  if (nl >= sizeof(n->name))
    nl = sizeof(n->name) - 1;
  memcpy(n->name, nm, nl);
  n->name[nl] = '\0';
  n->flags = VFS_FILE;
  n->ops = &eventfd_node_ops;
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
  fd->flags = O_RDWR;
  fd->ref_count = 1;
  mutex_init(&fd->lock);
  wait_queue_init(&fd->read_wq);
  wait_queue_init(&fd->write_wq);

  proc->fds[free_fd] = fd;
  if (flags & EFD_CLOEXEC)
    proc->fd_cloexec_mask |= (1u << free_fd);
  return (int64_t)free_fd;
}

int64_t k_eventfd(uint64_t initval, uint64_t a2, uint64_t a3, uint64_t a4,
                  uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  return eventfd_create_internal(initval, 0);
}

int64_t k_eventfd2(uint64_t initval, uint64_t flags, uint64_t a3, uint64_t a4,
                   uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;
  return eventfd_create_internal(initval, (int)flags);
}