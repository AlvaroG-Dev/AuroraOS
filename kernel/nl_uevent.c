// kernel/nl_uevent.c — ver nl_uevent.h

#include "nl_uevent.h"
#include "heap.h"
#include "klog.h"
#include "mutex.h"
#include "process.h"
#include "spinlock.h"
#include "string.h"
#include "uaccess.h"
#include "vfs.h"
#include "wait.h"
#include <stddef.h>
#include <stdint.h>

#define NETLINK_KOBJECT_UEVENT 15
#define NL_MAX_Q 64
#define NL_MSG_MAX 2048
#define U_O_NONBLOCK 0x800
#define SOCK_NONBLOCK_F 0x800
#define SOCK_CLOEXEC_F 0x80000
#define MSG_DONTWAIT_F 0x40
#define EPROTONOSUPPORT_N 93

struct sockaddr_nl_u {
  uint16_t nl_family;
  uint16_t nl_pad;
  uint32_t nl_pid;
  uint32_t nl_groups;
};

typedef struct nl_msg {
  struct nl_msg *next;
  size_t len;
  char data[];
} nl_msg_t;

typedef struct nl_sock {
  struct nl_sock *next;
  uint32_t pid;
  uint32_t groups;
  bool bound;
  bool nonblock;
  nl_msg_t *q_head, *q_tail;
  int q_count;
  wait_queue_t wq;
} nl_sock_t;

static spinlock_t g_lock;
static nl_sock_t *g_socks;
static uint32_t g_next_pid = 1000;
static vfs_ops_t nl_ops;

static void nl_free_queue(nl_msg_t *m) {
  while (m) {
    nl_msg_t *n = m->next;
    kfree(m);
    m = n;
  }
}

static int nl_close(vfs_node_t *node) {
  nl_sock_t *s = (nl_sock_t *)node->priv;
  if (!s)
    return 0;
  unsigned long fl = spin_lock_irqsave(&g_lock);
  nl_sock_t **pp = &g_socks;
  while (*pp && *pp != s)
    pp = &(*pp)->next;
  if (*pp)
    *pp = s->next;
  nl_msg_t *q = s->q_head;
  s->q_head = s->q_tail = NULL;
  spin_unlock_irqrestore(&g_lock, fl);
  nl_free_queue(q);
  node->priv = NULL;
  kfree(s);
  return 0;
}

static bool nl_readable(void *a) {
  return __atomic_load_n(&((nl_sock_t *)a)->q_head, __ATOMIC_ACQUIRE) != NULL;
}

// Saca un mensaje (bloquea si hace falta). Devuelve el nl_msg (kfree) o NULL
// con *err.
static nl_msg_t *nl_dequeue(nl_sock_t *s, bool nonblock, int *err) {
  for (;;) {
    unsigned long fl = spin_lock_irqsave(&g_lock);
    nl_msg_t *m = s->q_head;
    if (m) {
      s->q_head = m->next;
      if (!s->q_head)
        s->q_tail = NULL;
      s->q_count--;
      spin_unlock_irqrestore(&g_lock, fl);
      return m;
    }
    spin_unlock_irqrestore(&g_lock, fl);
    if (nonblock) {
      *err = -EAGAIN;
      return NULL;
    }
    int rc = wait_event_interruptible(&s->wq, nl_readable, s);
    if (rc < 0) {
      *err = rc;
      return NULL;
    }
  }
}

static int64_t nl_read(vfs_node_t *node, uint64_t off, size_t size, void *buf) {
  (void)off;
  nl_sock_t *s = (nl_sock_t *)node->priv;
  if (!s)
    return -EBADF;
  int err = 0;
  nl_msg_t *m = nl_dequeue(s, s->nonblock, &err);
  if (!m)
    return err;
  size_t n = m->len < size ? m->len : size;
  memcpy(buf, m->data, n);
  kfree(m);
  return (int64_t)n;
}

static int64_t nl_write(vfs_node_t *node, uint64_t off, size_t size,
                        const void *buf) {
  (void)node;
  (void)off;
  (void)buf;
  return (int64_t)size;
}

static int nl_poll(vfs_node_t *node, short events) {
  nl_sock_t *s = (nl_sock_t *)node->priv;
  if (!s)
    return 0;
  int rev = 0;
  if ((events & 1) && nl_readable(s))
    rev |= 1;
  if (events & 4)
    rev |= 4;
  return rev;
}

static wait_queue_t *nl_poll_wq(vfs_node_t *node) {
  nl_sock_t *s = (nl_sock_t *)node->priv;
  return s ? &s->wq : NULL;
}

static vfs_ops_t nl_ops = {
    .read = nl_read,
    .write = nl_write,
    .close = nl_close,
    .poll = nl_poll,
    .poll_wq = nl_poll_wq,
};

static nl_sock_t *get_nl(process_t *proc, int fd, file_descriptor_t **fout) {
  file_descriptor_t *f = vfs_fd_get_ref(proc, fd);
  if (!f)
    return NULL;
  if (!f->node || f->node->ops != &nl_ops || !f->node->priv) {
    vfs_fd_put_ref(f);
    return NULL;
  }
  *fout = f;
  return (nl_sock_t *)f->node->priv;
}

bool nl_fd_is_socket(process_t *proc, int fd) {
  if (!proc || fd < 0 || fd >= MAX_PROCESS_FDS)
    return false;
  file_descriptor_t *f = proc->fds[fd];
  return f && f->node && f->node->ops == &nl_ops;
}

int64_t nl_sys_socket(process_t *proc, uint64_t type, uint64_t proto) {
  if (proto != NETLINK_KOBJECT_UEVENT)
    return -EPROTONOSUPPORT_N;
  int base = (int)(type & 0xF);
  if (base != 2 && base != 3) // SOCK_DGRAM / SOCK_RAW
    return -EINVAL;
  nl_sock_t *s = (nl_sock_t *)kzalloc(sizeof(*s));
  vfs_node_t *n = (vfs_node_t *)kzalloc(sizeof(*n));
  file_descriptor_t *f = (file_descriptor_t *)kzalloc(sizeof(*f));
  if (!s || !n || !f) {
    if (s)
      kfree(s);
    if (n)
      kfree(n);
    if (f)
      kfree(f);
    return -ENOMEM;
  }
  wait_queue_init(&s->wq);
  s->nonblock = (type & SOCK_NONBLOCK_F) != 0;
  const char *nm = "socket:[netlink]";
  memcpy(n->name, nm, strlen(nm) + 1);
  n->flags = VFS_FILE;
  n->ops = &nl_ops;
  n->priv = s;
  n->mode = S_IFSOCK | 0777;
  n->ref_count = 1;
  f->node = n;
  f->flags = O_RDWR | (s->nonblock ? U_O_NONBLOCK : 0);
  f->ref_count = 1;
  mutex_init(&f->lock);
  wait_queue_init(&f->read_wq);
  wait_queue_init(&f->write_wq);
  unsigned long fl = spin_lock_irqsave(&g_lock);
  s->next = g_socks;
  g_socks = s;
  spin_unlock_irqrestore(&g_lock, fl);
  int fd = vfs_fd_install_ref(proc, f, (type & SOCK_CLOEXEC_F) != 0);
  if (fd < 0) {
    vfs_node_free(n); // cierra y libera s
    kfree(f);
  }
  return fd;
}

int64_t nl_sys_bind(process_t *proc, int fd, uint64_t addr, uint64_t addrlen) {
  file_descriptor_t *f;
  nl_sock_t *s = get_nl(proc, fd, &f);
  if (!s)
    return -ENOTSOCK;
  struct sockaddr_nl_u sa;
  int64_t rc = 0;
  if (addrlen < sizeof(sa) || !access_ok((void *)addr, sizeof(sa)) ||
      copy_from_user(&sa, (void *)addr, sizeof(sa)) < 0) {
    rc = -EFAULT;
  } else if (sa.nl_family != AF_NETLINK_FAMILY) {
    rc = -EINVAL;
  } else {
    unsigned long fl = spin_lock_irqsave(&g_lock);
    s->pid = sa.nl_pid ? sa.nl_pid : __atomic_fetch_add(&g_next_pid, 1, __ATOMIC_ACQ_REL);
    s->groups = sa.nl_groups;
    s->bound = true;
    spin_unlock_irqrestore(&g_lock, fl);
  }
  vfs_fd_put_ref(f);
  return rc;
}

int64_t nl_sys_getsockname(process_t *proc, int fd, uint64_t addr,
                           uint64_t addrlen_ptr) {
  file_descriptor_t *f;
  nl_sock_t *s = get_nl(proc, fd, &f);
  if (!s)
    return -ENOTSOCK;
  struct sockaddr_nl_u sa = {AF_NETLINK_FAMILY, 0, s->pid, s->groups};
  uint32_t ulen = 0;
  if (addr && addrlen_ptr && access_ok((void *)addrlen_ptr, 4) &&
      copy_from_user(&ulen, (void *)addrlen_ptr, 4) == 0) {
    uint32_t n = ulen < sizeof(sa) ? ulen : (uint32_t)sizeof(sa);
    if (n && access_ok((void *)addr, n))
      (void)copy_to_user((void *)addr, &sa, n);
    uint32_t real = sizeof(sa);
    (void)copy_to_user((void *)addrlen_ptr, &real, 4);
  }
  vfs_fd_put_ref(f);
  return 0;
}

int64_t nl_sys_setsockopt(process_t *proc, int fd, uint64_t level,
                          uint64_t optname, uint64_t optval, uint64_t optlen) {
  (void)level;
  (void)optname;
  (void)optval;
  (void)optlen;
  file_descriptor_t *f;
  if (!get_nl(proc, fd, &f))
    return -ENOTSOCK;
  vfs_fd_put_ref(f); // SO_ATTACH_FILTER, SO_RCVBUFFORCE, SO_PASSCRED...: ok
  return 0;
}

int64_t nl_sys_getsockopt(process_t *proc, int fd, uint64_t level,
                          uint64_t optname, uint64_t optval,
                          uint64_t optlen_ptr) {
  (void)level;
  (void)optname;
  file_descriptor_t *f;
  if (!get_nl(proc, fd, &f))
    return -ENOTSOCK;
  vfs_fd_put_ref(f);
  int v = 0;
  uint32_t ulen = 0;
  if (optval && optlen_ptr && access_ok((void *)optlen_ptr, 4) &&
      copy_from_user(&ulen, (void *)optlen_ptr, 4) == 0 && ulen >= 4 &&
      access_ok((void *)optval, 4)) {
    (void)copy_to_user((void *)optval, &v, 4);
    uint32_t n = 4;
    (void)copy_to_user((void *)optlen_ptr, &n, 4);
  }
  return 0;
}

struct u_iovec {
  uint64_t base, len;
};
struct u_msghdr {
  uint64_t msg_name;
  uint32_t msg_namelen, _pad0;
  uint64_t msg_iov, msg_iovlen, msg_control, msg_controllen;
  uint32_t msg_flags, _pad1;
};

int64_t nl_sys_sendmsg(process_t *proc, int fd, uint64_t msg_uptr,
                       uint64_t flags) {
  (void)flags;
  file_descriptor_t *f;
  if (!get_nl(proc, fd, &f))
    return -ENOTSOCK;
  vfs_fd_put_ref(f);
  struct u_msghdr mh;
  if (!access_ok((void *)msg_uptr, sizeof(mh)) ||
      copy_from_user(&mh, (void *)msg_uptr, sizeof(mh)) < 0)
    return -EFAULT;
  uint64_t total = 0; // se descarta: nadie escucha, pero el envio "funciona"
  struct u_iovec iov;
  for (uint64_t i = 0; i < mh.msg_iovlen && i < 64; i++) {
    if (copy_from_user(&iov, (void *)(mh.msg_iov + i * sizeof(iov)),
                       sizeof(iov)) < 0)
      return -EFAULT;
    total += iov.len;
  }
  return (int64_t)total;
}

int64_t nl_sys_sendto(process_t *proc, int fd, uint64_t len) {
  file_descriptor_t *f;
  if (!get_nl(proc, fd, &f))
    return -ENOTSOCK;
  vfs_fd_put_ref(f);
  return (int64_t)len;
}

int64_t nl_sys_recvmsg(process_t *proc, int fd, uint64_t msg_uptr,
                       uint64_t flags) {
  file_descriptor_t *f;
  nl_sock_t *s = get_nl(proc, fd, &f);
  if (!s)
    return -ENOTSOCK;
  struct u_msghdr mh;
  if (!access_ok((void *)msg_uptr, sizeof(mh)) ||
      copy_from_user(&mh, (void *)msg_uptr, sizeof(mh)) < 0) {
    vfs_fd_put_ref(f);
    return -EFAULT;
  }
  bool nb = s->nonblock || (f->flags & U_O_NONBLOCK) || (flags & MSG_DONTWAIT_F);
  int err = 0;
  nl_msg_t *m = nl_dequeue(s, nb, &err);
  if (!m) {
    vfs_fd_put_ref(f);
    return err;
  }
  size_t left = m->len, off = 0;
  struct u_iovec iov;
  for (uint64_t i = 0; i < mh.msg_iovlen && i < 64 && left; i++) {
    if (copy_from_user(&iov, (void *)(mh.msg_iov + i * sizeof(iov)),
                       sizeof(iov)) < 0)
      break;
    size_t c = iov.len < left ? (size_t)iov.len : left;
    if (c && access_ok((void *)iov.base, c) &&
        copy_to_user((void *)iov.base, m->data + off, c) == 0) {
      off += c;
      left -= c;
    } else if (c) {
      break;
    }
  }
  int64_t ret = (int64_t)off;
  uint32_t mflags = left ? 0x20 : 0; // MSG_TRUNC
  if (mh.msg_name && mh.msg_namelen >= sizeof(struct sockaddr_nl_u)) {
    struct sockaddr_nl_u from = {AF_NETLINK_FAMILY, 0, 0, 1};
    if (access_ok((void *)mh.msg_name, sizeof(from)))
      (void)copy_to_user((void *)mh.msg_name, &from, sizeof(from));
    mh.msg_namelen = sizeof(from);
  }
  mh.msg_controllen = 0;
  mh.msg_flags = mflags;
  (void)copy_to_user((void *)msg_uptr, &mh, sizeof(mh));
  kfree(m);
  vfs_fd_put_ref(f);
  return ret;
}

int64_t nl_sys_recvfrom(process_t *proc, int fd, uint64_t buf, uint64_t len,
                        uint64_t flags) {
  file_descriptor_t *f;
  nl_sock_t *s = get_nl(proc, fd, &f);
  if (!s)
    return -ENOTSOCK;
  bool nb = s->nonblock || (f->flags & U_O_NONBLOCK) || (flags & MSG_DONTWAIT_F);
  int err = 0;
  nl_msg_t *m = nl_dequeue(s, nb, &err);
  if (!m) {
    vfs_fd_put_ref(f);
    return err;
  }
  size_t n = m->len < len ? m->len : (size_t)len;
  int64_t ret = (int64_t)n;
  if (n && (!access_ok((void *)buf, n) || copy_to_user((void *)buf, m->data, n) < 0))
    ret = -EFAULT;
  kfree(m);
  vfs_fd_put_ref(f);
  return ret;
}

void nl_uevent_broadcast(const char *msg, size_t len) {
  if (!msg || !len || len > NL_MSG_MAX)
    return;
  unsigned long fl = spin_lock_irqsave(&g_lock);
  for (nl_sock_t *s = g_socks; s; s = s->next) {
    if (!s->bound || !(s->groups & 1) || s->q_count >= NL_MAX_Q)
      continue;
    nl_msg_t *m = (nl_msg_t *)kmalloc(sizeof(nl_msg_t) + len);
    if (!m)
      continue;
    m->next = NULL;
    m->len = len;
    memcpy(m->data, msg, len);
    if (s->q_tail)
      s->q_tail->next = m;
    else
      s->q_head = m;
    s->q_tail = m;
    s->q_count++;
    wake_up_all(&s->wq);
  }
  spin_unlock_irqrestore(&g_lock, fl);
}
