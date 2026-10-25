// kernel/timerfd.c
//
// timerfd_create / timerfd_settime / timerfd_gettime. Resolucion de 1 tick
// (1 ms). Un kthread unico duerme hasta el siguiente vencimiento y despierta
// las wait queues de los timerfd vencidos (lo que necesitan poll/epoll).

#include "timerfd.h"
#include "heap.h"
#include "klog.h"
#include "kthread.h"
#include "mutex.h"
#include "process.h"
#include "sched.h"
#include "spinlock.h"
#include "string.h"
#include "uaccess.h"
#include "vfs.h"
#include "wait.h"
#include <stddef.h>
#include <stdint.h>

#define TFD_NONBLOCK 0x800
#define TFD_CLOEXEC 0x80000
#define TFD_TIMER_ABSTIME 1
#define TFD_TIMER_CANCEL_ON_SET 2
#define TFD_MAX 256

struct k_ts {
  int64_t sec;
  int64_t nsec;
};
struct k_its {
  struct k_ts interval;
  struct k_ts value;
};

typedef struct tfd {
  int clockid;
  int flags;
  bool armed;
  uint64_t expiry;   // tick absoluto
  uint64_t interval; // ticks, 0 = one-shot
  uint64_t count;    // expiraciones sin leer
  wait_queue_t wq;
} tfd_t;

static spinlock_t g_lock;
static tfd_t *g_list[TFD_MAX];
static wait_queue_t g_kick_wq;
static uint32_t g_kick_gen;
static int g_started;

static vfs_ops_t tfd_ops;

// Con g_lock cogido.
static void tfd_update(tfd_t *t, uint64_t now) {
  if (!t->armed || now < t->expiry)
    return;
  if (t->interval) {
    uint64_t n = 1 + (now - t->expiry) / t->interval;
    t->count += n;
    t->expiry += n * t->interval;
  } else {
    t->count += 1;
    t->armed = false;
  }
  wake_up_all(&t->wq);
}

typedef struct {
  uint32_t gen;
} kick_arg_t;

static bool kick_changed(void *a) {
  return __atomic_load_n(&g_kick_gen, __ATOMIC_ACQUIRE) !=
         ((kick_arg_t *)a)->gen;
}

static void kick(void) {
  __atomic_fetch_add(&g_kick_gen, 1, __ATOMIC_ACQ_REL);
  wake_up_all(&g_kick_wq);
}

static int tfd_thread(void *arg) {
  (void)arg;
  for (;;) {
    kick_arg_t ka;
    ka.gen = __atomic_load_n(&g_kick_gen, __ATOMIC_ACQUIRE);
    uint64_t now = sched_get_ticks();
    uint64_t next = 0;
    unsigned long fl = spin_lock_irqsave(&g_lock);
    for (int i = 0; i < TFD_MAX; i++) {
      tfd_t *t = g_list[i];
      if (!t)
        continue;
      tfd_update(t, now);
      if (t->armed && (next == 0 || t->expiry < next))
        next = t->expiry;
    }
    spin_unlock_irqrestore(&g_lock, fl);
    uint64_t timeout = 0; // 0 = indefinido
    if (next) {
      now = sched_get_ticks();
      timeout = next > now ? next - now : 1;
    }
    wait_event_timeout(&g_kick_wq, kick_changed, &ka, timeout);
  }
  return 0;
}

static uint64_t ts_to_ticks(const struct k_ts *ts) {
  if (ts->sec < 0 || ts->nsec < 0 || ts->nsec >= 1000000000LL)
    return (uint64_t)-1;
  return (uint64_t)ts->sec * 1000ULL + ((uint64_t)ts->nsec + 999999) / 1000000;
}

static void ticks_to_ts(uint64_t t, struct k_ts *ts) {
  ts->sec = (int64_t)(t / 1000);
  ts->nsec = (int64_t)(t % 1000) * 1000000;
}

// ---- ops -------------------------------------------------------------
static int tfd_close(vfs_node_t *node) {
  tfd_t *t = (tfd_t *)node->priv;
  if (!t)
    return 0;
  unsigned long fl = spin_lock_irqsave(&g_lock);
  for (int i = 0; i < TFD_MAX; i++)
    if (g_list[i] == t)
      g_list[i] = NULL;
  spin_unlock_irqrestore(&g_lock, fl);
  node->priv = NULL;
  kfree(t);
  return 0;
}

// Sin g_lock: puede evaluarse con el lock de una wait queue cogido y
// tfd_update() despierta wait queues con g_lock cogido (orden inverso).
static bool tfd_ready(void *a) {
  tfd_t *t = (tfd_t *)a;
  return __atomic_load_n(&t->count, __ATOMIC_ACQUIRE) != 0 ||
         (__atomic_load_n(&t->armed, __ATOMIC_ACQUIRE) &&
          sched_get_ticks() >= __atomic_load_n(&t->expiry, __ATOMIC_ACQUIRE));
}

static int64_t tfd_read(vfs_node_t *node, uint64_t off, size_t sz, void *buf) {
  (void)off;
  tfd_t *t = (tfd_t *)node->priv;
  if (!t || sz < 8)
    return -EINVAL;
  for (;;) {
    unsigned long fl = spin_lock_irqsave(&g_lock);
    tfd_update(t, sched_get_ticks());
    if (t->count) {
      uint64_t c = t->count;
      t->count = 0;
      spin_unlock_irqrestore(&g_lock, fl);
      memcpy(buf, &c, 8);
      return 8;
    }
    spin_unlock_irqrestore(&g_lock, fl);
    if (t->flags & TFD_NONBLOCK)
      return -EAGAIN;
    int rc = wait_event_interruptible(&t->wq, tfd_ready, t);
    if (rc < 0)
      return rc;
  }
}

static int tfd_poll(vfs_node_t *node, short events) {
  tfd_t *t = (tfd_t *)node->priv;
  if (!t)
    return 0;
  return ((events & 1) && tfd_ready(t)) ? 1 : 0;
}

static wait_queue_t *tfd_poll_wq(vfs_node_t *node) {
  tfd_t *t = (tfd_t *)node->priv;
  return t ? &t->wq : NULL;
}

static vfs_ops_t tfd_ops = {
    .read = tfd_read,
    .close = tfd_close,
    .poll = tfd_poll,
    .poll_wq = tfd_poll_wq,
};

// ---- syscalls --------------------------------------------------------
static tfd_t *tfd_from_fd(process_t *proc, int fd, file_descriptor_t **fout) {
  file_descriptor_t *f = vfs_fd_get_ref(proc, fd);
  if (!f)
    return NULL;
  if (!f->node || f->node->ops != &tfd_ops || !f->node->priv) {
    vfs_fd_put_ref(f);
    return NULL;
  }
  *fout = f;
  return (tfd_t *)f->node->priv;
}

int64_t k_timerfd_create(uint64_t clockid, uint64_t flags, uint64_t a3,
                         uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;
  process_t *proc = process_current();
  if (!proc)
    return -EFAULT;
  if (clockid != 0 && clockid != 1 && clockid != 7)
    return -EINVAL;
  if (flags & ~(uint64_t)(TFD_NONBLOCK | TFD_CLOEXEC))
    return -EINVAL;

  if (!__atomic_exchange_n(&g_started, 1, __ATOMIC_ACQ_REL)) {
    wait_queue_init(&g_kick_wq);
    kthread_run(tfd_thread, NULL, "timerfd");
  }

  tfd_t *t = (tfd_t *)kzalloc(sizeof(*t));
  vfs_node_t *n = (vfs_node_t *)kzalloc(sizeof(*n));
  file_descriptor_t *f = (file_descriptor_t *)kzalloc(sizeof(*f));
  if (!t || !n || !f) {
    if (t)
      kfree(t);
    if (n)
      kfree(n);
    if (f)
      kfree(f);
    return -ENOMEM;
  }
  wait_queue_init(&t->wq);
  t->clockid = (int)clockid;
  t->flags = (int)flags;

  const char *nm = "anon_inode:[timerfd]";
  size_t nl = strlen(nm);
  memcpy(n->name, nm, nl + 1);
  n->flags = VFS_FILE;
  n->ops = &tfd_ops;
  n->priv = t;
  n->mode = S_IFREG | 0600;
  n->ref_count = 1;
  f->node = n;
  f->flags = O_RDONLY | ((flags & TFD_NONBLOCK) ? 0x800 : 0);
  f->ref_count = 1;
  mutex_init(&f->lock);
  wait_queue_init(&f->read_wq);
  wait_queue_init(&f->write_wq);

  // Registrar antes de instalar para que el kthread lo vea.
  int slot = -1;
  unsigned long fl = spin_lock_irqsave(&g_lock);
  for (int i = 0; i < TFD_MAX; i++)
    if (!g_list[i]) {
      g_list[i] = t;
      slot = i;
      break;
    }
  spin_unlock_irqrestore(&g_lock, fl);
  if (slot < 0) {
    kfree(t);
    kfree(n);
    kfree(f);
    return -EMFILE;
  }
  int fd = vfs_fd_install_ref(proc, f, (flags & TFD_CLOEXEC) != 0);
  if (fd < 0) {
    fl = spin_lock_irqsave(&g_lock);
    g_list[slot] = NULL;
    spin_unlock_irqrestore(&g_lock, fl);
    kfree(t);
    kfree(n);
    kfree(f);
    return fd;
  }
  return fd;
}

int64_t k_timerfd_settime(uint64_t fd, uint64_t flags, uint64_t new_uptr,
                          uint64_t old_uptr, uint64_t a5) {
  (void)a5;
  process_t *proc = process_current();
  if (!proc)
    return -EFAULT;
  if (flags & ~(uint64_t)(TFD_TIMER_ABSTIME | TFD_TIMER_CANCEL_ON_SET))
    return -EINVAL;
  struct k_its nw;
  if (!new_uptr || !access_ok((void *)new_uptr, sizeof(nw)) ||
      copy_from_user(&nw, (void *)new_uptr, sizeof(nw)) < 0)
    return -EFAULT;
  uint64_t val = ts_to_ticks(&nw.value);
  uint64_t itv = ts_to_ticks(&nw.interval);
  if (val == (uint64_t)-1 || itv == (uint64_t)-1)
    return -EINVAL;

  file_descriptor_t *f;
  tfd_t *t = tfd_from_fd(proc, (int)fd, &f);
  if (!t)
    return -EBADF;

  uint64_t now = sched_get_ticks();
  uint64_t expiry = 0;
  bool arm = (nw.value.sec != 0 || nw.value.nsec != 0);
  if (arm) {
    if (flags & TFD_TIMER_ABSTIME) {
      int64_t delta;
      if (t->clockid == 0)
        delta = (int64_t)val - kernel_realtime_ms();
      else
        delta = (int64_t)val - (int64_t)now;
      expiry = delta > 0 ? now + (uint64_t)delta : now;
    } else {
      expiry = now + (val ? val : 1);
    }
  }

  struct k_its old;
  memset(&old, 0, sizeof(old));
  unsigned long fl = spin_lock_irqsave(&g_lock);
  tfd_update(t, now);
  if (t->armed) {
    ticks_to_ts(t->expiry > now ? t->expiry - now : 0, &old.value);
    ticks_to_ts(t->interval, &old.interval);
  }
  t->armed = arm;
  t->expiry = expiry;
  t->interval = arm ? itv : 0;
  t->count = 0;
  if (arm && expiry <= now) // absoluto en el pasado: vence ya
    tfd_update(t, now);
  spin_unlock_irqrestore(&g_lock, fl);
  kick();
  vfs_fd_put_ref(f);

  if (old_uptr && access_ok((void *)old_uptr, sizeof(old)))
    (void)copy_to_user((void *)old_uptr, &old, sizeof(old));
  return 0;
}

int64_t k_timerfd_gettime(uint64_t fd, uint64_t cur_uptr, uint64_t a3,
                          uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;
  process_t *proc = process_current();
  if (!proc)
    return -EFAULT;
  file_descriptor_t *f;
  tfd_t *t = tfd_from_fd(proc, (int)fd, &f);
  if (!t)
    return -EBADF;
  struct k_its cur;
  memset(&cur, 0, sizeof(cur));
  uint64_t now = sched_get_ticks();
  unsigned long fl = spin_lock_irqsave(&g_lock);
  tfd_update(t, now);
  if (t->armed) {
    ticks_to_ts(t->expiry > now ? t->expiry - now : 0, &cur.value);
    ticks_to_ts(t->interval, &cur.interval);
  }
  spin_unlock_irqrestore(&g_lock, fl);
  vfs_fd_put_ref(f);
  if (!cur_uptr || !access_ok((void *)cur_uptr, sizeof(cur)) ||
      copy_to_user((void *)cur_uptr, &cur, sizeof(cur)) < 0)
    return -EFAULT;
  return 0;
}
