// kernel/pidfd_inotify.c
//
// pidfd_open: fd que pasa a ser legible (POLLIN) cuando el proceso termina.
//   Guarda solo el PID (no un puntero: el process_t se libera al reapear).
//
// inotify_init/init1/add_watch/rm_watch: STUB. Crea fds validos y acepta
//   watches sobre rutas existentes, pero nunca entrega eventos (el VFS no
//   tiene hooks de notificacion todavia). Basta para que GLib/Qt/Mesa no
//   fallen al arrancar.

#include "pidfd_inotify.h"
#include "heap.h"
#include "klog.h"
#include "mutex.h"
#include "process.h"
#include "string.h"
#include "uaccess.h"
#include "vfs.h"
#include "wait.h"
#include <stddef.h>
#include <stdint.h>

#define F_NONBLOCK 0x800
#define F_CLOEXEC 0x80000

// Wait queue global de salidas de proceso (la despierta process_exit).
static wait_queue_t g_exit_wq;
static int g_exit_wq_init;

static void exit_wq_ensure(void) {
  if (!__atomic_exchange_n(&g_exit_wq_init, 1, __ATOMIC_ACQ_REL))
    wait_queue_init(&g_exit_wq);
}

void pidfd_notify_exit(void) {
  if (__atomic_load_n(&g_exit_wq_init, __ATOMIC_ACQUIRE))
    wake_up_all(&g_exit_wq);
}

static int64_t install_anon(process_t *proc, const char *name, vfs_ops_t *ops,
                            void *priv, int fdflags, bool cloexec) {
  vfs_node_t *n = (vfs_node_t *)kzalloc(sizeof(*n));
  file_descriptor_t *f = (file_descriptor_t *)kzalloc(sizeof(*f));
  if (!n || !f) {
    if (n)
      kfree(n);
    if (f)
      kfree(f);
    return -ENOMEM;
  }
  size_t nl = strlen(name);
  if (nl >= sizeof(n->name))
    nl = sizeof(n->name) - 1;
  memcpy(n->name, name, nl);
  n->name[nl] = '\0';
  n->flags = VFS_FILE;
  n->ops = ops;
  n->priv = priv;
  n->mode = S_IFREG | 0600;
  n->ref_count = 1;
  f->node = n;
  f->flags = O_RDONLY | fdflags;
  f->ref_count = 1;
  mutex_init(&f->lock);
  wait_queue_init(&f->read_wq);
  wait_queue_init(&f->write_wq);
  int fd = vfs_fd_install_ref(proc, f, cloexec);
  if (fd < 0) {
    kfree(n);
    kfree(f);
  }
  return fd;
}

// ---------------------------------------------------------------------------
// pidfd
// ---------------------------------------------------------------------------
typedef struct {
  uint32_t pid;
} pidfd_ctx_t;

static bool pid_gone(void *a) {
  return process_find_by_pid(((pidfd_ctx_t *)a)->pid) == NULL;
}

static int pidfd_close(vfs_node_t *node) {
  if (node->priv) {
    kfree(node->priv);
    node->priv = NULL;
  }
  return 0;
}

static int pidfd_poll(vfs_node_t *node, short events) {
  pidfd_ctx_t *c = (pidfd_ctx_t *)node->priv;
  if (!c)
    return 0;
  return ((events & 1) && pid_gone(c)) ? 1 : 0;
}

static wait_queue_t *pidfd_poll_wq(vfs_node_t *node) {
  (void)node;
  exit_wq_ensure();
  return &g_exit_wq;
}

static vfs_ops_t pidfd_ops = {
    .close = pidfd_close,
    .poll = pidfd_poll,
    .poll_wq = pidfd_poll_wq,
};

int64_t k_pidfd_open(uint64_t pid, uint64_t flags, uint64_t a3, uint64_t a4,
                     uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;
  process_t *proc = process_current();
  if (!proc)
    return -EFAULT;
  if (flags & ~(uint64_t)F_NONBLOCK)
    return -EINVAL;
  if (pid == 0 || pid > 0xFFFFFFFFULL)
    return -EINVAL;
  if (!process_find_by_pid((uint32_t)pid))
    return -ESRCH;
  exit_wq_ensure();
  pidfd_ctx_t *c = (pidfd_ctx_t *)kzalloc(sizeof(*c));
  if (!c)
    return -ENOMEM;
  c->pid = (uint32_t)pid;
  // pidfd_open siempre es close-on-exec.
  int64_t fd = install_anon(proc, "anon_inode:[pidfd]", &pidfd_ops, c, 0, true);
  if (fd < 0)
    kfree(c);
  return fd;
}

// ---------------------------------------------------------------------------
// inotify (stub)
// ---------------------------------------------------------------------------
typedef struct {
  int flags;
  int next_wd;
  wait_queue_t wq;
} inotify_ctx_t;

static bool never(void *a) {
  (void)a;
  return false;
}

static int64_t ino_read(vfs_node_t *node, uint64_t off, size_t sz, void *buf) {
  (void)off;
  (void)buf;
  inotify_ctx_t *c = (inotify_ctx_t *)node->priv;
  if (!c)
    return -EINVAL;
  if (sz < 16)
    return -EINVAL;
  if (c->flags & F_NONBLOCK)
    return -EAGAIN;
  for (;;) {
    int rc = wait_event_interruptible(&c->wq, never, NULL);
    if (rc < 0)
      return rc;
  }
}

static int ino_close(vfs_node_t *node) {
  if (node->priv) {
    kfree(node->priv);
    node->priv = NULL;
  }
  return 0;
}

static int ino_poll(vfs_node_t *node, short events) {
  (void)node;
  (void)events;
  return 0;
}

static wait_queue_t *ino_poll_wq(vfs_node_t *node) {
  inotify_ctx_t *c = (inotify_ctx_t *)node->priv;
  return c ? &c->wq : NULL;
}

static vfs_ops_t inotify_ops = {
    .read = ino_read,
    .close = ino_close,
    .poll = ino_poll,
    .poll_wq = ino_poll_wq,
};

static int64_t inotify_new(uint64_t flags) {
  process_t *proc = process_current();
  if (!proc)
    return -EFAULT;
  if (flags & ~(uint64_t)(F_NONBLOCK | F_CLOEXEC))
    return -EINVAL;
  inotify_ctx_t *c = (inotify_ctx_t *)kzalloc(sizeof(*c));
  if (!c)
    return -ENOMEM;
  wait_queue_init(&c->wq);
  c->flags = (int)flags;
  c->next_wd = 1;
  int64_t fd = install_anon(proc, "anon_inode:inotify", &inotify_ops, c,
                            (flags & F_NONBLOCK) ? 0x800 : 0,
                            (flags & F_CLOEXEC) != 0);
  if (fd < 0)
    kfree(c);
  return fd;
}

int64_t k_inotify_init(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                       uint64_t a5) {
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  return inotify_new(0);
}

int64_t k_inotify_init1(uint64_t flags, uint64_t a2, uint64_t a3, uint64_t a4,
                        uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  return inotify_new(flags);
}

int64_t k_inotify_add_watch(uint64_t fd, uint64_t path_uptr, uint64_t mask,
                            uint64_t a4, uint64_t a5) {
  (void)a4;
  (void)a5;
  process_t *proc = process_current();
  if (!proc)
    return -EFAULT;
  if (mask == 0)
    return -EINVAL;
  file_descriptor_t *f = vfs_fd_get_ref(proc, (int)fd);
  if (!f)
    return -EBADF;
  if (!f->node || f->node->ops != &inotify_ops || !f->node->priv) {
    vfs_fd_put_ref(f);
    return -EINVAL;
  }
  inotify_ctx_t *c = (inotify_ctx_t *)f->node->priv;
  char raw[VFS_PATH_MAX], abs[VFS_PATH_MAX];
  long n = strncpy_from_user(raw, (const char *)path_uptr, sizeof(raw));
  if (n < 0) {
    vfs_fd_put_ref(f);
    return -EFAULT;
  }
  const char *cwd = proc->cwd[0] ? proc->cwd : "/";
  int rc = vfs_resolve_path(cwd, raw, abs, sizeof(abs));
  int64_t ret;
  if (rc != 0) {
    ret = rc;
  } else {
    vfs_node_t *node = vfs_lookup(abs);
    if (!node) {
      ret = -ENOENT;
    } else {
      vfs_node_free(node);
      ret = __atomic_fetch_add(&c->next_wd, 1, __ATOMIC_ACQ_REL);
    }
  }
  vfs_fd_put_ref(f);
  return ret;
}

int64_t k_inotify_rm_watch(uint64_t fd, uint64_t wd, uint64_t a3, uint64_t a4,
                           uint64_t a5) {
  (void)wd;
  (void)a3;
  (void)a4;
  (void)a5;
  process_t *proc = process_current();
  if (!proc)
    return -EFAULT;
  file_descriptor_t *f = vfs_fd_get_ref(proc, (int)fd);
  if (!f)
    return -EBADF;
  bool ok = f->node && f->node->ops == &inotify_ops;
  vfs_fd_put_ref(f);
  return ok ? 0 : -EINVAL;
}
