// kernel/net/inet_socket.c

#include "inet_socket.h"
#include "../heap.h"
#include "../klog.h"
#include "../mutex.h"
#include "../process.h"
#include "../string.h"
#include "../uaccess.h"
#include "../vfs.h"
#include "socket.h"

// Estructuras de userland (Linux x86_64).
struct user_sockaddr_in {
  uint16_t sin_family;
  uint16_t sin_port; // big endian
  uint32_t sin_addr; // big endian
  uint8_t sin_zero[8];
};

// ---------------------------------------------------------------------------
// Vnode ops
// ---------------------------------------------------------------------------

static int64_t sk_read_op(vfs_node_t *node, uint64_t off, size_t size,
                          void *buf) {
  (void)off;
  socket_t *s = (socket_t *)node->priv;
  int wb = 0;
  int rc = socket_recv(s, buf, size, &wb);
  if (wb)
    return -EAGAIN;
  return rc;
}

static int64_t sk_write_op(vfs_node_t *node, uint64_t off, size_t size,
                           const void *buf) {
  (void)off;
  socket_t *s = (socket_t *)node->priv;
  return socket_send(s, buf, size);
}

static int sk_close_op(vfs_node_t *node) {
  socket_t *s = (socket_t *)node->priv;
  if (s) {
    socket_put(s);
    node->priv = NULL;
  }
  return 0;
}

static int sk_poll_op(vfs_node_t *node, short events) {
  socket_t *s = (socket_t *)node->priv;
  if (!s)
    return 0;
  int rev = 0;
  if ((events & 1) && socket_is_readable(s))
    rev |= 1;
  if (events & 4) // POLLOUT
    rev |= 4;
  return rev;
}

static wait_queue_t *sk_poll_wq_op(vfs_node_t *node) {
  socket_t *s = (socket_t *)node->priv;
  return socket_rx_wq(s);
}

static vfs_ops_t inet_socket_ops = {
    .read = sk_read_op,
    .write = sk_write_op,
    .close = sk_close_op,
    .poll = sk_poll_op,
    .poll_wq = sk_poll_wq_op,
};

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------
static int64_t wrap_socket(struct process *proc, socket_t *s, int flags) {
  vfs_node_t *node = (vfs_node_t *)kzalloc(sizeof(*node));
  if (!node) {
    socket_put(s);
    return -ENOMEM;
  }
  const char *nm =
      (socket_type(s) == SOCK_STREAM) ? "socket:[tcp]" : "socket:[udp]";
  size_t nl = strlen(nm);
  if (nl >= sizeof(node->name))
    nl = sizeof(node->name) - 1;
  memcpy(node->name, nm, nl);
  node->name[nl] = '\0';
  node->flags = VFS_FILE;
  node->ops = &inet_socket_ops;
  node->priv = s;
  node->mode = S_IFREG | 0600;
  node->ref_count = 1;

  file_descriptor_t *fd = (file_descriptor_t *)kzalloc(sizeof(*fd));
  if (!fd) {
    kfree(node);
    socket_put(s);
    return -ENOMEM;
  }
  fd->node = node;
  fd->offset = 0;
  fd->flags = O_RDWR;
  fd->ref_count = 1;
  mutex_init(&fd->lock);
  wait_queue_init(&fd->read_wq);
  wait_queue_init(&fd->write_wq);

  int free_fd = -1;
  for (int i = 0; i < MAX_PROCESS_FDS; i++) {
    if (!proc->fds[i]) {
      free_fd = i;
      break;
    }
  }
  if (free_fd < 0) {
    kfree(fd);
    kfree(node);
    socket_put(s);
    return -EMFILE;
  }
  proc->fds[free_fd] = fd;
  if (flags & SOCK_CLOEXEC)
    proc->fd_cloexec_mask |= (1u << free_fd);
  return free_fd;
}

int64_t inet_socket_create(struct process *proc, int type, int protocol) {
  if (!proc)
    return -EFAULT;

  int flags = type & ~0xF;
  int base_type = type & 0xF;
  if (base_type != SOCK_DGRAM && base_type != SOCK_STREAM)
    return -EOPNOTSUPP;

  socket_t *s = socket_create(base_type, protocol);
  if (!s)
    return -EOPNOTSUPP;
  if (flags & SOCK_NONBLOCK)
    socket_set_nonblock(s, 1);

  int64_t r = wrap_socket(proc, s, flags);
  if (r >= 0)
    LOG_DEBUG("[SOCK] fd=%d creado (type=%d proto=%d)", (int)r, base_type,
              protocol);
  return r;
}

int64_t inet_socket_wrap(struct process *proc, struct socket *s) {
  if (!proc || !s)
    return -EINVAL;
  return wrap_socket(proc, s, 0);
}

// ---------------------------------------------------------------------------
// Helpers para syscall.c
// ---------------------------------------------------------------------------
socket_t *inet_socket_from_fd(struct process *proc, int fd) {
  if (!proc || fd < 0 || fd >= MAX_PROCESS_FDS)
    return NULL;
  file_descriptor_t *f = proc->fds[fd];
  if (!f || !f->node || f->node->ops != &inet_socket_ops)
    return NULL;
  return (socket_t *)f->node->priv;
}