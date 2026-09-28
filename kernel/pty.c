// kernel/pty.c
//
// [PTY Fase 2] Implementación del pool de PTYs y el par master/slave.
//
// Ver pty.h para la descripción del modelo.

#include "pty.h"
#include "heap.h"
#include "klog.h"
#include "process.h"
#include "string.h"
#include "uaccess.h"
#include "vfs.h"

static tty_pty_t g_ptys[PTY_MAX];
static spinlock_t g_pty_lock;
static int g_pty_ready = 0;

void pty_init(void) {
  memset(g_ptys, 0, sizeof(g_ptys));
  spin_init(&g_pty_lock);
  g_pty_ready = 1;
  LOG_INFO("[PTY] Pool inicializado (%d slots)", PTY_MAX);
}

// ---------------------------------------------------------------------------
// Asignación
// ---------------------------------------------------------------------------
tty_pty_t *pty_alloc(void) {
  if (!g_pty_ready)
    return NULL;

  unsigned long flags = spin_lock_irqsave(&g_pty_lock);
  tty_pty_t *found = NULL;
  for (int i = 0; i < PTY_MAX; i++) {
    if (!g_ptys[i].in_use) {
      found = &g_ptys[i];
      break;
    }
  }
  if (!found) {
    spin_unlock_irqrestore(&g_pty_lock, flags);
    return NULL;
  }

  // Reset completo del slot.
  memset(found, 0, sizeof(*found));
  found->in_use = 1;
  found->index = (uint32_t)(found - g_ptys);
  found->slave_locked = 0;
  found->master_open = 0;
  found->slave_open = 0;
  found->slave.fg_pgid = 0;
  found->slave.session_leader_pid = 0;

  spin_init(&found->slave.lock);
  spin_init(&found->m_lock);
  wait_queue_init(&found->slave.read_wq);
  wait_queue_init(&found->m_read_wq);

  tty_set_defaults(&found->slave);
  found->slave.pty = found;
  found->slave.winsize_cols = 80;
  found->slave.winsize_rows = 24;

  spin_unlock_irqrestore(&g_pty_lock, flags);
  LOG_DEBUG("[PTY] alloc -> /dev/pts/%u", found->index);
  return found;
}

void pty_free(tty_pty_t *pty) {
  if (!pty || !pty->in_use)
    return;
  unsigned long flags = spin_lock_irqsave(&g_pty_lock);
  LOG_DEBUG("[PTY] free /dev/pts/%u", pty->index);
  pty->in_use = 0;
  spin_unlock_irqrestore(&g_pty_lock, flags);

  // [CTTY] Cualquier proceso que tuviera este slave como terminal de
  // control queda huérfano.
  process_clear_ctty_for((struct tty *)&pty->slave);
}

void pty_release_master(void *pty_ptr) {
  tty_pty_t *pty = (tty_pty_t *)pty_ptr;
  if (!pty)
    return;
  unsigned long flags = spin_lock_irqsave(&g_pty_lock);
  if (pty->master_open > 0)
    pty->master_open--;
  int free_it = (pty->master_open == 0 && pty->slave_open == 0);
  spin_unlock_irqrestore(&g_pty_lock, flags);
  if (free_it)
    pty_free(pty);
}

void pty_release_slave(void *pty_ptr) {
  tty_pty_t *pty = (tty_pty_t *)pty_ptr;
  if (!pty)
    return;
  unsigned long flags = spin_lock_irqsave(&g_pty_lock);
  if (pty->slave_open > 0)
    pty->slave_open--;
  int free_it = (pty->master_open == 0 && pty->slave_open == 0);
  spin_unlock_irqrestore(&g_pty_lock, flags);
  if (free_it)
    pty_free(pty);
}

// ---------------------------------------------------------------------------
// Master → Slave
// ---------------------------------------------------------------------------
int64_t pty_master_write(tty_pty_t *pty, const void *buf, size_t size) {
  if (!pty || !buf || size == 0)
    return 0;
  const uint8_t *src = (const uint8_t *)buf;
  for (size_t i = 0; i < size; i++)
    tty_slave_receive(&pty->slave, src[i]);
  return (int64_t)size;
}

// ---------------------------------------------------------------------------
// Slave → Master
// ---------------------------------------------------------------------------
void pty_slave_emit(struct tty_pty *pty_opaque, const void *buf, size_t size) {
  tty_pty_t *pty = (tty_pty_t *)pty_opaque;
  if (!pty || !buf || size == 0)
    return;

  const uint8_t *src = (const uint8_t *)buf;
  unsigned long flags = spin_lock_irqsave(&pty->m_lock);
  size_t space = PTY_M_BUF_SIZE - pty->m_count;
  size_t to_write = size;
  if (to_write > space) {
    LOG_WARN("[PTY] /dev/pts/%u master buffer lleno (%zu/%d). "
             "Descartando %zu bytes",
             pty->index, pty->m_count, PTY_M_BUF_SIZE, to_write - space);
    to_write = space;
  }
  for (size_t i = 0; i < to_write; i++) {
    pty->m_buf[pty->m_tail] = src[i];
    pty->m_tail = (pty->m_tail + 1) % PTY_M_BUF_SIZE;
  }
  pty->m_count += to_write;
  spin_unlock_irqrestore(&pty->m_lock, flags);

  if (to_write > 0)
    wake_up_all(&pty->m_read_wq);
}

// ---------------------------------------------------------------------------
// Read del master
// ---------------------------------------------------------------------------
static bool pty_master_has_data(void *arg) {
  tty_pty_t *pty = (tty_pty_t *)arg;
  unsigned long flags = spin_lock_irqsave(&pty->m_lock);
  bool ok = pty->m_count > 0;
  spin_unlock_irqrestore(&pty->m_lock, flags);
  return ok;
}

int64_t pty_master_read(tty_pty_t *pty, void *buf, size_t size) {
  if (!pty || !buf || size == 0)
    return 0;

  int rc = wait_event_interruptible(&pty->m_read_wq, pty_master_has_data, pty);
  if (rc < 0)
    return -EINTR;

  unsigned long flags = spin_lock_irqsave(&pty->m_lock);
  size_t n = pty->m_count < size ? pty->m_count : size;
  uint8_t *out = (uint8_t *)buf;
  for (size_t i = 0; i < n; i++) {
    out[i] = pty->m_buf[pty->m_head];
    pty->m_head = (pty->m_head + 1) % PTY_M_BUF_SIZE;
  }
  pty->m_count -= n;
  spin_unlock_irqrestore(&pty->m_lock, flags);
  return (int64_t)n;
}

// ---------------------------------------------------------------------------
// Condiciones para poll
// ---------------------------------------------------------------------------
bool pty_master_readable(tty_pty_t *pty) {
  if (!pty)
    return false;
  unsigned long flags = spin_lock_irqsave(&pty->m_lock);
  bool ok = pty->m_count > 0;
  spin_unlock_irqrestore(&pty->m_lock, flags);
  return ok;
}

bool pty_slave_readable(tty_pty_t *pty) {
  if (!pty)
    return false;
  unsigned long flags = spin_lock_irqsave(&pty->slave.lock);
  bool ok = pty->slave.count > 0 || pty->slave.eof_pending;
  spin_unlock_irqrestore(&pty->slave.lock, flags);
  return ok;
}

// ---------------------------------------------------------------------------
// [Fase 3] file_descriptor_t ops para master y slave.
//
// El nodo lleva priv = tty_pty_t* del par. Cada fd tiene su propio nodo
// (no compartido), así que node->priv es único por fd.
// ---------------------------------------------------------------------------

static int64_t pty_master_read_op(vfs_node_t *n, uint64_t off, size_t sz,
                                  void *buf) {
  (void)off;
  return pty_master_read((tty_pty_t *)n->priv, buf, sz);
}

static int64_t pty_master_write_op(vfs_node_t *n, uint64_t off, size_t sz,
                                   const void *buf) {
  (void)off;
  return pty_master_write((tty_pty_t *)n->priv, buf, sz);
}

static int pty_master_poll_op(vfs_node_t *n, short events) {
  tty_pty_t *p = (tty_pty_t *)n->priv;
  int rev = 0;
  if (events & 1 /*POLLIN*/) {
    if (pty_master_readable(p))
      rev |= 1;
  }
  if (events & 4 /*POLLOUT*/)
    rev |= 4;
  return rev;
}

static int64_t pty_master_ioctl_op(vfs_node_t *n, unsigned long req,
                                   uint64_t arg) {
  tty_pty_t *p = (tty_pty_t *)n->priv;

  switch (req) {
  case TIOCGPTN: { // Devolver el índice N (int).
    if (!access_ok((void *)arg, sizeof(int32_t)))
      return -EFAULT;
    int32_t num = (int32_t)p->index;
    if (copy_to_user((void *)arg, &num, sizeof(num)) < 0)
      return -EFAULT;
    return 0;
  }
  case TIOCSPTLCK: { // Bloquear/desbloquear el slave.
    if (!access_ok((void *)arg, sizeof(int32_t)))
      return -EFAULT;
    int32_t val;
    if (copy_from_user(&val, (void *)arg, sizeof(val)) < 0)
      return -EFAULT;
    unsigned long flags = spin_lock_irqsave(&g_pty_lock);
    p->slave_locked = (val != 0) ? 1 : 0;
    spin_unlock_irqrestore(&g_pty_lock, flags);
    return 0;
  }
  // Resto de ioctls (TIOCGWINSZ, TIOCSWINSZ, TCGETS, TCSETS, ...) van
  // al slave. Es lo que hace Linux: el master actúa como proxy del
  // termios/winsize del slave.
  default:
    return tty_ioctl(&p->slave, req, arg);
  }
}

static int pty_master_close_op(vfs_node_t *n) {
  pty_release_master(n->priv);
  return 0;
}

static wait_queue_t *pty_master_poll_wq_op(vfs_node_t *n) {
  return &((tty_pty_t *)n->priv)->m_read_wq;
}

static wait_queue_t *pty_slave_poll_wq_op(vfs_node_t *n) {
  return &((tty_pty_t *)n->priv)->slave.read_wq;
}

static vfs_ops_t pty_master_ops = {
    .read = pty_master_read_op,
    .write = pty_master_write_op,
    .poll = pty_master_poll_op,
    .poll_wq = pty_master_poll_wq_op,
    .ioctl = pty_master_ioctl_op,
    .close = pty_master_close_op,
};

// ---------------------------------------------------------------------------
// Ops del slave: delegan directamente en el tty_t embebido del par.
// ---------------------------------------------------------------------------

static int64_t pty_slave_read_op(vfs_node_t *n, uint64_t off, size_t sz,
                                 void *buf) {
  return tty_read(&((tty_pty_t *)n->priv)->slave, off, sz, buf);
}

static int64_t pty_slave_write_op(vfs_node_t *n, uint64_t off, size_t sz,
                                  const void *buf) {
  return tty_write(&((tty_pty_t *)n->priv)->slave, off, sz, buf);
}

static int pty_slave_poll_op(vfs_node_t *n, short events) {
  return tty_poll(&((tty_pty_t *)n->priv)->slave, events);
}

static int64_t pty_slave_ioctl_op(vfs_node_t *n, unsigned long req,
                                  uint64_t arg) {
  return tty_ioctl(&((tty_pty_t *)n->priv)->slave, req, arg);
}

static int pty_slave_close_op(vfs_node_t *n) {
  pty_release_slave(n->priv);
  return 0;
}

static vfs_ops_t pty_slave_ops = {
    .read = pty_slave_read_op,
    .write = pty_slave_write_op,
    .poll = pty_slave_poll_op,
    .poll_wq = pty_slave_poll_wq_op,
    .ioctl = pty_slave_ioctl_op,
    .close = pty_slave_close_op,
};

// ---------------------------------------------------------------------------
// Helper: construir (nodo, fd) y publicar en proc->fds[fd_num].
// ---------------------------------------------------------------------------
static int pty_install_fd(struct process *proc, int fd_num, tty_pty_t *p,
                          vfs_ops_t *ops, const char *name) {
  vfs_node_t *n = (vfs_node_t *)kzalloc(sizeof(vfs_node_t));
  if (!n)
    return -ENOMEM;
  size_t nl = strlen(name);
  if (nl >= sizeof(n->name))
    nl = sizeof(n->name) - 1;
  memcpy(n->name, name, nl);
  n->name[nl] = '\0';
  n->flags = VFS_CHARDEVICE;
  n->ops = ops;
  n->priv = p;

  file_descriptor_t *fd =
      (file_descriptor_t *)kzalloc(sizeof(file_descriptor_t));
  if (!fd) {
    kfree(n);
    return -ENOMEM;
  }
  fd->node = n;
  fd->offset = 0;
  fd->flags = O_RDWR;
  fd->ref_count = 1;
  wait_queue_init(&fd->read_wq);
  wait_queue_init(&fd->write_wq);

  proc->fds[fd_num] = fd;
  return 0;
}

// ---------------------------------------------------------------------------
// Open del master: /dev/ptmx
// ---------------------------------------------------------------------------
int pty_open_master_fd(struct process *proc, int fd_num) {
  if (!proc || fd_num < 0 || fd_num >= MAX_PROCESS_FDS)
    return -EINVAL;
  if (proc->fds[fd_num])
    return -EBUSY;

  tty_pty_t *p = pty_alloc();
  if (!p)
    return -ENOMEM;

  unsigned long flags = spin_lock_irqsave(&g_pty_lock);
  if (p->master_open > 0) {
    spin_unlock_irqrestore(&g_pty_lock, flags);
    pty_free(p);
    return -EBUSY;
  }
  p->master_open = 1;
  spin_unlock_irqrestore(&g_pty_lock, flags);

  int rc = pty_install_fd(proc, fd_num, p, &pty_master_ops, "ptmx");
  if (rc != 0) {
    pty_release_master(p);
    return rc;
  }
  LOG_DEBUG("[PTY] open master -> /dev/pts/%u", p->index);
  return 0;
}

// ---------------------------------------------------------------------------
// Open del slave: /dev/pts/N
// ---------------------------------------------------------------------------
int pty_open_slave_fd(struct process *proc, int fd_num, int index) {
  if (!proc || fd_num < 0 || fd_num >= MAX_PROCESS_FDS)
    return -EINVAL;
  if (index < 0 || index >= PTY_MAX)
    return -EINVAL;
  if (proc->fds[fd_num])
    return -EBUSY;

  tty_pty_t *p = &g_ptys[index];

  unsigned long flags = spin_lock_irqsave(&g_pty_lock);
  if (!p->in_use) {
    spin_unlock_irqrestore(&g_pty_lock, flags);
    return -ENOENT;
  }
  if (p->slave_locked) {
    spin_unlock_irqrestore(&g_pty_lock, flags);
    return -EIO;
  }
  if (p->master_open == 0) {
    // El slave solo existe mientras haya master. Sin master, no hay
    // quién reciba el output ni quién envíe el input.
    spin_unlock_irqrestore(&g_pty_lock, flags);
    return -ENXIO;
  }
  p->slave_open++;
  spin_unlock_irqrestore(&g_pty_lock, flags);

  int rc = pty_install_fd(proc, fd_num, p, &pty_slave_ops, "pts");
  if (rc != 0) {
    pty_release_slave(p);
    return rc;
  }
  LOG_DEBUG("[PTY] open slave /dev/pts/%d", index);
  return 0;
}

int pty_is_in_use(int idx) {
  if (idx < 0 || idx >= PTY_MAX)
    return 0;
  return g_ptys[idx].in_use;
}

struct tty_pty *pty_slave_from_fd(struct file_descriptor *fd) {
  if (!fd || !fd->node)
    return NULL;
  if (fd->node->ops != &pty_slave_ops)
    return NULL;
  return (struct tty_pty *)fd->node->priv;
}

void pty_set_fg_pgid(struct tty_pty *pty_opaque, uint32_t pgid) {
  if (!pty_opaque)
    return;
  tty_pty_t *pty = (tty_pty_t *)pty_opaque;
  unsigned long flags = spin_lock_irqsave(&pty->slave.lock);
  pty->slave.fg_pgid = pgid;
  spin_unlock_irqrestore(&pty->slave.lock, flags);
}