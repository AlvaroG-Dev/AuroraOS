// kernel/unix_socket.c
//
// AF_UNIX: STREAM / DGRAM / SEQPACKET, nombres de ruta y abstractos,
// socketpair, SO_PEERCRED, SCM_RIGHTS y SCM_CREDENTIALS.
//
// Modelo:
//   - Un único mutex global (g_lock) protege todo el estado (colas, enlaces
//     entre pares, tabla de nombres). Nunca se duerme con él cogido ni se
//     copia a/desde userland con él cogido.
//   - Cada socket tiene una wait queue y un contador `gen`. Cualquier cambio
//     que pueda afectar a quien espera (datos nuevos, espacio liberado, el
//     par cerró) incrementa `gen` y despierta la wq. Los que esperan leen
//     `gen` bajo g_lock y duermen hasta que cambie: no hay despertares
//     perdidos.
//   - Los enlaces peer <-> peer son punteros sin refcount: close() los
//     rompe bajo g_lock antes de liberar. El socket vive mientras viva el
//     file_descriptor_t que lo contiene.
//   - SCM_RIGHTS: el mensaje retiene una referencia al file_descriptor_t
//     (vfs_fd_get_ref) hasta que el receptor lo instala en su tabla.
//   - Los nombres de ruta viven solo en una tabla interna (no se crea inodo
//     en el VFS todavía): connect() busca por ruta absoluta normalizada.

#include "unix_socket.h"
#include "signal.h"
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

#ifndef EISCONN
#define EISCONN 106
#endif
#ifndef EDESTADDRREQ
#define EDESTADDRREQ 89
#endif
#ifndef EPROTONOSUPPORT
#define EPROTONOSUPPORT 93
#endif
#ifndef EBADF
#define EBADF 9
#endif
#ifndef ENOMEM
#define ENOMEM 12
#endif
#ifndef ENOENT
#define ENOENT 2
#endif
#ifndef ESOCKTNOSUPPORT_U
#define ESOCKTNOSUPPORT_U 94
#endif
#ifndef ENOPROTOOPT_U
#define ENOPROTOOPT_U 92
#endif
#ifndef EACCES
#define EACCES 13
#endif

#define SOCK_STREAM_T 1
#define SOCK_DGRAM_T 2
#define SOCK_SEQPACKET_T 5

#define SOCK_NONBLOCK_F 0x800
#define SOCK_CLOEXEC_F 0x80000

#define U_O_NONBLOCK 0x800

#define MSG_PEEK_F 0x2
#define MSG_TRUNC_F 0x20
#define MSG_DONTWAIT_F 0x40
#define MSG_CTRUNC_F 0x8
#define MSG_CMSGCLOEXEC_F 0x40000000

#define SOL_SOCKET_L 1
#define SCM_RIGHTS_T 1
#define SCM_CREDENTIALS_T 2

#define SO_REUSEADDR_O 2
#define SO_TYPE_O 3
#define SO_ERROR_O 4
#define SO_SNDBUF_O 7
#define SO_RCVBUF_O 8
#define SO_PASSCRED_O 16
#define SO_PEERCRED_O 17
#define SO_PROTOCOL_O 38
#define SO_DOMAIN_O 39
#define SO_ACCEPTCONN_O 30

#define UNIX_BUF 212992
#define UNIX_DGRAM_MAX_Q 256
#define UNIX_MAX_FDS 64
#define UNIX_NAME_MAX 256
#define UNIX_SUN_PATH 108
#define UNIX_BACKLOG_MAX 128
#define UNIX_WAIT_POLL_TICKS 10
#define UNIX_IOV_MAX 1024

typedef struct {
  int32_t pid;
  uint32_t uid;
  uint32_t gid;
} ucred_t;

typedef struct umsg {
  struct umsg *next;
  uint8_t *data;
  size_t len;
  size_t off;
  file_descriptor_t **fds;
  int nfds;
  ucred_t cred;
  char from[UNIX_NAME_MAX];
  int from_len;
} umsg_t;

typedef struct usock {
  int type;
  bool nonblock;
  bool connected;
  bool listening;
  bool hup;      // el par cerró o hizo shutdown(WR): EOF tras vaciar la cola
  bool shut_rd;  // shutdown(RD) local
  bool shut_wr;  // shutdown(WR) local
  bool passcred;
  struct usock *peer;

  umsg_t *rq_head;
  umsg_t *rq_tail;
  size_t rq_bytes;
  int rq_count;

  // Listener
  struct usock *acc_head;
  struct usock *acc_tail;
  struct usock *acc_next;
  int acc_count;
  int backlog;

  // Nombre propio (bind) y destino por defecto de un DGRAM connect().
  char name[UNIX_NAME_MAX];
  int name_len;
  struct usock *bnext;
  bool registered;
  char dst[UNIX_NAME_MAX];
  int dst_len;

  ucred_t peercred;
  bool has_peercred;

  uint32_t gen;
  wait_queue_t wq;
} usock_t;

static mutex_t g_lock;
static usock_t *g_bound;
static uint32_t g_autobind;

#define LOCK() mutex_lock(&g_lock)
#define UNLOCK() mutex_unlock(&g_lock)

// ---------------------------------------------------------------------------
// Espera por generación
// ---------------------------------------------------------------------------
typedef struct {
  usock_t *u;
  uint32_t gen;
} waitarg_t;

static bool gen_changed(void *a) {
  waitarg_t *w = (waitarg_t *)a;
  return __atomic_load_n(&w->u->gen, __ATOMIC_ACQUIRE) != w->gen;
}

static void notify(usock_t *u) {
  __atomic_fetch_add(&u->gen, 1, __ATOMIC_ACQ_REL);
  wake_up_all(&u->wq);
}

// Duerme hasta que cambie `gen`. timeout 0 = indefinido. Devuelve 0 o -EINTR.
static int sleep_gen(usock_t *u, uint32_t gen, uint64_t timeout) {
  waitarg_t w = {u, gen};
  if (timeout) {
    long r = wait_event_interruptible_timeout(&u->wq, gen_changed, &w, timeout);
    return r < 0 ? -EINTR : 0;
  }
  int rc = wait_event_interruptible(&u->wq, gen_changed, &w);
  return rc < 0 ? -EINTR : 0;
}

// ---------------------------------------------------------------------------
// Mensajes
// ---------------------------------------------------------------------------
static void msg_free(umsg_t *m) {
  while (m) {
    umsg_t *n = m->next;
    for (int i = 0; i < m->nfds; i++)
      if (m->fds[i])
        vfs_fd_put_ref(m->fds[i]);
    if (m->fds)
      kfree(m->fds);
    if (m->data)
      kfree(m->data);
    kfree(m);
    m = n;
  }
}

static void rq_push(usock_t *u, umsg_t *m) {
  m->next = NULL;
  if (u->rq_tail)
    u->rq_tail->next = m;
  else
    u->rq_head = m;
  u->rq_tail = m;
  u->rq_bytes += m->len - m->off;
  u->rq_count++;
}

// Quita la cabeza (el llamante ya ha restado los bytes). Devuelve el mensaje.
static umsg_t *rq_pop(usock_t *u) {
  umsg_t *m = u->rq_head;
  if (!m)
    return NULL;
  u->rq_head = m->next;
  if (!u->rq_head)
    u->rq_tail = NULL;
  m->next = NULL;
  u->rq_count--;
  return m;
}

// ---------------------------------------------------------------------------
// Tabla de nombres
// ---------------------------------------------------------------------------
static usock_t *find_bound(const char *key, int len) {
  for (usock_t *u = g_bound; u; u = u->bnext)
    if (u->name_len == len && memcmp(u->name, key, (size_t)len) == 0)
      return u;
  return NULL;
}

static void unregister_name(usock_t *u) {
  if (!u->registered)
    return;
  usock_t **pp = &g_bound;
  while (*pp) {
    if (*pp == u) {
      *pp = u->bnext;
      break;
    }
    pp = &(*pp)->bnext;
  }
  u->registered = false;
  u->bnext = NULL;
}

// El nombre de ruta de un socket vive en el VFS como un inodo S_IFSOCK (como
// en Linux: persiste al cerrar el socket y se quita con unlink). Un socket cuyo
// fichero ya no existe sigue vivo pero es inalcanzable.
static bool path_inode_is_sock(const char *key, int klen) {
  char path[UNIX_NAME_MAX + 1];
  if (klen <= 0 || klen > UNIX_NAME_MAX)
    return false;
  memcpy(path, key, (size_t)klen);
  path[klen] = '\0'; // las claves no llevan NUL final
  vfs_node_t *n = vfs_lookup(path);
  if (!n)
    return false;
  bool ok = (n->mode & S_IFMT) == S_IFSOCK;
  vfs_node_free(n);
  return ok;
}

static void cred_of(process_t *proc, ucred_t *c) {
  c->pid = (int32_t)proc->pid;
  c->uid = proc->euid;
  c->gid = proc->egid;
}

// ---------------------------------------------------------------------------
// Ciclo de vida
// ---------------------------------------------------------------------------
static usock_t *unix_new(int type) {
  usock_t *u = (usock_t *)kzalloc(sizeof(*u));
  if (!u)
    return NULL;
  u->type = type;
  wait_queue_init(&u->wq);
  return u;
}

static void unix_release(usock_t *u) {
  usock_t *kids = NULL;
  umsg_t *garbage;

  LOCK();
  unregister_name(u);
  usock_t *p = u->peer;
  if (p) {
    p->peer = NULL;
    p->hup = true;
    notify(p);
    u->peer = NULL;
  }
  while (u->acc_head) {
    usock_t *c = u->acc_head;
    u->acc_head = c->acc_next;
    c->acc_next = kids;
    kids = c;
    if (c->peer) {
      c->peer->peer = NULL;
      c->peer->hup = true;
      notify(c->peer);
      c->peer = NULL;
    }
  }
  u->acc_tail = NULL;
  u->acc_count = 0;
  garbage = u->rq_head;
  u->rq_head = u->rq_tail = NULL;
  u->rq_bytes = 0;
  u->rq_count = 0;
  UNLOCK();

  // Fuera del lock: soltar un fd puede cerrar otro socket (reentra en g_lock).
  msg_free(garbage);
  while (kids) {
    usock_t *c = kids;
    kids = c->acc_next;
    msg_free(c->rq_head);
    kfree(c);
  }
  kfree(u);
}

// ---------------------------------------------------------------------------
// Nodo VFS
// ---------------------------------------------------------------------------
typedef struct rxinfo {
  file_descriptor_t **fds;
  int nfds;
  ucred_t cred;
  bool has_cred;
  char from[UNIX_NAME_MAX];
  int from_len;
  size_t full_len; // longitud real del datagrama (para MSG_TRUNC)
  bool trunc;
} rxinfo_t;

static int64_t core_send(usock_t *u, bool nonblock, const void *data,
                         size_t len, const char *dkey, int dklen,
                         file_descriptor_t **fds, int nfds, process_t *proc);
static int64_t core_recv(usock_t *u, bool nonblock, int flags, void *buf,
                         size_t want, rxinfo_t *ri);

static void rx_drop(rxinfo_t *ri) {
  for (int i = 0; i < ri->nfds; i++)
    if (ri->fds[i])
      vfs_fd_put_ref(ri->fds[i]);
  if (ri->fds)
    kfree(ri->fds);
  ri->fds = NULL;
  ri->nfds = 0;
}

static int64_t uv_read(vfs_node_t *node, uint64_t off, size_t size,
                       void *buf) {
  (void)off;
  usock_t *u = (usock_t *)node->priv;
  if (!u)
    return -EBADF;
  rxinfo_t *ri = (rxinfo_t *)kzalloc(sizeof(*ri));
  if (!ri)
    return -ENOMEM;
  int64_t r = core_recv(u, u->nonblock, 0, buf, size, ri);
  rx_drop(ri);
  kfree(ri);
  return r;
}

static int64_t uv_write(vfs_node_t *node, uint64_t off, size_t size,
                        const void *buf) {
  (void)off;
  usock_t *u = (usock_t *)node->priv;
  if (!u)
    return -EBADF;
  process_t *proc = process_current();
  int64_t wr = core_send(u, u->nonblock, buf, size, NULL, 0, NULL, 0, proc);
  if (wr == -EPIPE)
    signal_raise_sigpipe(); // write() en un socket sin par: como Linux
  return wr;
}

static int uv_close(vfs_node_t *node) {
  usock_t *u = (usock_t *)node->priv;
  if (u) {
    node->priv = NULL;
    unix_release(u);
  }
  return 0;
}

// Lectura sin lock: el poll puede llamarse con spinlocks cogidos.
static int uv_poll(vfs_node_t *node, short events) {
  usock_t *u = (usock_t *)node->priv;
  if (!u)
    return 0;
  int rev = 0;
  bool hup = __atomic_load_n(&u->hup, __ATOMIC_ACQUIRE);
  bool rd = __atomic_load_n(&u->rq_head, __ATOMIC_ACQUIRE) != NULL ||
            __atomic_load_n(&u->acc_head, __ATOMIC_ACQUIRE) != NULL ||
            hup || u->shut_rd;
  if ((events & 1) && rd)
    rev |= 1; // POLLIN
  if (events & 4) {
    usock_t *p = __atomic_load_n(&u->peer, __ATOMIC_ACQUIRE);
    if (u->type == SOCK_DGRAM_T || hup || u->shut_wr ||
        (p && p->rq_bytes < UNIX_BUF))
      rev |= 4; // POLLOUT
  }
  if (hup && u->connected)
    rev |= 0x10 | 0x2000; // POLLHUP | POLLRDHUP
  return rev;
}

static wait_queue_t *uv_poll_wq(vfs_node_t *node) {
  usock_t *u = (usock_t *)node->priv;
  return u ? &u->wq : NULL;
}

static int64_t uv_ioctl(vfs_node_t *node, unsigned long req, uint64_t arg) {
  usock_t *u = (usock_t *)node->priv;
  if (!u)
    return -EBADF;
  if (req == 0x541B) { // FIONREAD
    int n = (int)u->rq_bytes;
    if (u->type != SOCK_STREAM_T)
      n = u->rq_head ? (int)(u->rq_head->len - u->rq_head->off) : 0;
    if (!access_ok((void *)arg, 4) || copy_to_user((void *)arg, &n, 4) < 0)
      return -EFAULT;
    return 0;
  }
  return -ENOTTY;
}

static vfs_ops_t unix_ops = {
    .read = uv_read,
    .write = uv_write,
    .close = uv_close,
    .poll = uv_poll,
    .poll_wq = uv_poll_wq,
    .ioctl = uv_ioctl,
};

// Envuelve u en un fd nuevo. En error libera u.
static int64_t unix_wrap(process_t *proc, usock_t *u, bool cloexec) {
  vfs_node_t *node = (vfs_node_t *)kzalloc(sizeof(*node));
  if (!node) {
    unix_release(u);
    return -ENOMEM;
  }
  const char *nm = "socket:[unix]";
  size_t nl = strlen(nm);
  if (nl >= sizeof(node->name))
    nl = sizeof(node->name) - 1;
  memcpy(node->name, nm, nl);
  node->name[nl] = '\0';
  node->flags = VFS_FILE;
  node->ops = &unix_ops;
  node->priv = u;
  node->mode = S_IFSOCK | 0777;
  node->ref_count = 1;

  file_descriptor_t *fd = (file_descriptor_t *)kzalloc(sizeof(*fd));
  if (!fd) {
    kfree(node);
    unix_release(u);
    return -ENOMEM;
  }
  fd->node = node;
  fd->flags = O_RDWR | (u->nonblock ? U_O_NONBLOCK : 0);
  fd->ref_count = 1;
  mutex_init(&fd->lock);
  wait_queue_init(&fd->read_wq);
  wait_queue_init(&fd->write_wq);

  int r = vfs_fd_install_ref(proc, fd, cloexec);
  if (r < 0) {
    vfs_node_free(node); // cierra -> unix_release
    kfree(fd);
    return r;
  }
  return r;
}

// Obtiene el socket de un fd con una referencia al fd (hay que soltarla).
static usock_t *get_unix(process_t *proc, int fd, file_descriptor_t **fout) {
  file_descriptor_t *f = vfs_fd_get_ref(proc, fd);
  if (!f)
    return NULL;
  if (!f->node || f->node->ops != &unix_ops || !f->node->priv) {
    vfs_fd_put_ref(f);
    return NULL;
  }
  *fout = f;
  return (usock_t *)f->node->priv;
}

bool unix_fd_is_socket(process_t *proc, int fd) {
  if (!proc || fd < 0 || fd >= MAX_PROCESS_FDS)
    return false;
  file_descriptor_t *f = proc->fds[fd];
  return f && f->node && f->node->ops == &unix_ops;
}

static bool is_nonblock(usock_t *u, file_descriptor_t *f, uint64_t flags) {
  return u->nonblock || (f->flags & U_O_NONBLOCK) ||
         (flags & MSG_DONTWAIT_F);
}

// ---------------------------------------------------------------------------
// Direcciones
// ---------------------------------------------------------------------------
// Devuelve 0 con key/keylen; keylen==0 => autobind (solo family). Errno si no.
static int parse_addr(process_t *proc, uint64_t uaddr, uint64_t addrlen,
                      char *key, int *keylen) {
  if (addrlen < 2 || addrlen > sizeof(uint16_t) + UNIX_SUN_PATH + 8)
    return -EINVAL;
  uint8_t raw[2 + UNIX_SUN_PATH + 8];
  if (!access_ok((void *)uaddr, addrlen) ||
      copy_from_user(raw, (void *)uaddr, addrlen) < 0)
    return -EFAULT;
  if ((raw[0] | (raw[1] << 8)) != AF_UNIX_FAMILY)
    return -EAFNOSUPPORT;
  size_t n = addrlen - 2;
  if (n > UNIX_SUN_PATH)
    n = UNIX_SUN_PATH;
  if (n == 0) {
    *keylen = 0;
    return 0;
  }
  const uint8_t *p = raw + 2;
  if (p[0] == 0) { // abstracto: nombre exacto, incluido el NUL inicial
    memcpy(key, p, n);
    *keylen = (int)n;
    return 0;
  }
  char path[UNIX_SUN_PATH + 1];
  size_t pl = 0;
  while (pl < n && p[pl])
    pl++;
  memcpy(path, p, pl);
  path[pl] = '\0';
  char abs[UNIX_NAME_MAX];
  const char *cwd = (proc && proc->cwd[0]) ? proc->cwd : "/";
  int rc = vfs_resolve_path(cwd, path, abs, sizeof(abs));
  if (rc < 0)
    return rc;
  size_t al = strlen(abs);
  memcpy(key, abs, al);
  *keylen = (int)al;
  return 0;
}

// Escribe sockaddr_un (family + nombre) y el addrlen real.
static void put_addr(uint64_t uaddr, uint64_t addrlen_ptr, const char *name,
                     int name_len) {
  if (!uaddr || !addrlen_ptr)
    return;
  uint32_t ulen = 0;
  if (!access_ok((void *)addrlen_ptr, 4) ||
      copy_from_user(&ulen, (void *)addrlen_ptr, 4) < 0)
    return;
  uint8_t out[2 + UNIX_NAME_MAX + 1];
  memset(out, 0, sizeof(out));
  out[0] = AF_UNIX_FAMILY;
  size_t total = 2;
  if (name_len > 0) {
    memcpy(out + 2, name, (size_t)name_len);
    total = 2 + (size_t)name_len + (name[0] == '/' ? 1 : 0);
  }
  size_t ncopy = total < ulen ? total : ulen;
  if (ncopy && access_ok((void *)uaddr, ncopy))
    (void)copy_to_user((void *)uaddr, out, ncopy);
  uint32_t real = (uint32_t)total;
  (void)copy_to_user((void *)addrlen_ptr, &real, 4);
}

// ---------------------------------------------------------------------------
// Envío / recepción (sobre buffers del kernel)
// ---------------------------------------------------------------------------
// Si devuelve >= 0 los fds pasan a ser del mensaje; si < 0 siguen siendo del
// llamante (que debe soltarlos).
static int64_t core_send(usock_t *u, bool nonblock, const void *data,
                         size_t len, const char *dkey, int dklen,
                         file_descriptor_t **fds, int nfds, process_t *proc) {
  bool dgram = (u->type == SOCK_DGRAM_T);
  if (u->type == SOCK_STREAM_T && len == 0)
    return 0;
  if (len > UNIX_BUF && u->type != SOCK_STREAM_T)
    return -EMSGSIZE;

  LOCK();
  for (;;) {
    usock_t *p = NULL;
    if (u->shut_wr) {
      UNLOCK();
      return -EPIPE;
    }
    if (dgram) {
      if (dkey && dklen > 0) {
        p = find_bound(dkey, dklen);
        if (!p || p->type != SOCK_DGRAM_T) {
          UNLOCK();
          return -ECONNREFUSED;
        }
      } else if (u->peer) {
        p = u->peer;
      } else if (u->dst_len > 0) {
        p = find_bound(u->dst, u->dst_len);
        if (!p || p->type != SOCK_DGRAM_T) {
          UNLOCK();
          return -ECONNREFUSED;
        }
      } else if (u->connected) {
        UNLOCK();
        return -ECONNREFUSED;
      } else {
        UNLOCK();
        return -EDESTADDRREQ;
      }
    } else {
      p = u->peer;
      if (!p) {
        UNLOCK();
        return u->connected ? -EPIPE : -ENOTCONN;
      }
    }
    if (p->shut_rd) {
      UNLOCK();
      return -EPIPE;
    }

    size_t n = len;
    bool room;
    if (u->type == SOCK_STREAM_T) {
      size_t space = p->rq_bytes < UNIX_BUF ? UNIX_BUF - p->rq_bytes : 0;
      room = space > 0;
      if (n > space)
        n = space;
    } else if (dgram) {
      room = p->rq_count < UNIX_DGRAM_MAX_Q;
    } else {
      room = (p->rq_bytes == 0 || p->rq_bytes + len <= UNIX_BUF) &&
             p->rq_count < UNIX_DGRAM_MAX_Q;
    }

    if (room) {
      umsg_t *m = (umsg_t *)kzalloc(sizeof(*m));
      uint8_t *copy = NULL;
      if (m && n) {
        copy = (uint8_t *)kmalloc(n);
        if (!copy) {
          kfree(m);
          m = NULL;
        }
      }
      if (!m) {
        UNLOCK();
        return -ENOMEM;
      }
      if (n)
        memcpy(copy, data, n);
      m->data = copy;
      m->len = n;
      if (proc)
        cred_of(proc, &m->cred);
      if (dgram && u->name_len > 0) {
        memcpy(m->from, u->name, (size_t)u->name_len);
        m->from_len = u->name_len;
      }
      if (nfds > 0) {
        m->fds = fds;
        m->nfds = nfds;
      }
      rq_push(p, m);
      notify(p);
      UNLOCK();
      return (int64_t)n;
    }

    if (nonblock) {
      UNLOCK();
      return -EAGAIN;
    }
    uint32_t gen = __atomic_load_n(&u->gen, __ATOMIC_ACQUIRE);
    UNLOCK();
    // DGRAM sin par enlazado: el receptor no puede avisarnos -> polling.
    int rc = sleep_gen(u, gen, (dgram && !u->peer) ? UNIX_WAIT_POLL_TICKS : 0);
    if (rc < 0)
      return rc;
    LOCK();
  }
}

static int64_t core_recv(usock_t *u, bool nonblock, int flags, void *buf,
                         size_t want, rxinfo_t *ri) {
  bool peek = (flags & MSG_PEEK_F) != 0;
  umsg_t *garbage = NULL;
  umsg_t *gtail = NULL;

  LOCK();
  for (;;) {
    if (u->rq_head)
      break;
    if (u->type != SOCK_DGRAM_T && !u->connected && !u->hup) {
      UNLOCK();
      return -ENOTCONN;
    }
    if (u->hup || u->shut_rd) {
      UNLOCK();
      return 0;
    }
    if (nonblock) {
      UNLOCK();
      return -EAGAIN;
    }
    uint32_t gen = __atomic_load_n(&u->gen, __ATOMIC_ACQUIRE);
    UNLOCK();
    int rc = sleep_gen(u, gen, 0);
    if (rc < 0)
      return rc;
    LOCK();
  }

  size_t got = 0;
  umsg_t *first = u->rq_head;
  ri->cred = first->cred;
  ri->has_cred = true;
  memcpy(ri->from, first->from, (size_t)first->from_len);
  ri->from_len = first->from_len;

  if (u->type != SOCK_STREAM_T) {
    // Datagrama / paquete: un mensaje entero, se trunca lo que no cabe.
    umsg_t *m = first;
    size_t avail = m->len - m->off;
    size_t n = avail < want ? avail : want;
    if (n)
      memcpy(buf, m->data + m->off, n);
    got = n;
    ri->full_len = avail;
    ri->trunc = n < avail;
    if (!peek) {
      rq_pop(u);
      u->rq_bytes -= avail;
      ri->fds = m->fds;
      ri->nfds = m->nfds;
      m->fds = NULL;
      m->nfds = 0;
      garbage = m;
    }
  } else {
    while (u->rq_head && got < want) {
      umsg_t *m = u->rq_head;
      size_t avail = m->len - m->off;
      size_t n = avail < want - got ? avail : want - got;
      if (n)
        memcpy((uint8_t *)buf + got, m->data + m->off, n);
      got += n;
      bool had_fds = (m->nfds > 0);
      if (peek) {
        // Sin consumir: solo recorre la cabeza (basta para MSG_PEEK).
        break;
      }
      m->off += n;
      u->rq_bytes -= n;
      if (m->nfds > 0 && !ri->fds) {
        ri->fds = m->fds;
        ri->nfds = m->nfds;
        m->fds = NULL;
        m->nfds = 0;
      }
      if (m->off >= m->len) {
        rq_pop(u);
        m->next = NULL;
        if (gtail)
          gtail->next = m;
        else
          garbage = m;
        gtail = m;
      }
      if (had_fds)
        break; // como Linux: un skb con fds cierra la lectura
    }
  }

  if (!peek && u->peer)
    notify(u->peer); // hay espacio para quien envía
  UNLOCK();
  msg_free(garbage);
  return (int64_t)got;
}

// ---------------------------------------------------------------------------
// iovec / msghdr
// ---------------------------------------------------------------------------
struct u_iovec {
  uint64_t base;
  uint64_t len;
};

struct u_msghdr {
  uint64_t msg_name;
  uint32_t msg_namelen;
  uint32_t _pad0;
  uint64_t msg_iov;
  uint64_t msg_iovlen;
  uint64_t msg_control;
  uint64_t msg_controllen;
  uint32_t msg_flags;
  uint32_t _pad1;
};

static struct u_iovec *load_iov(const struct u_msghdr *mh, size_t *total) {
  if (mh->msg_iovlen > UNIX_IOV_MAX)
    return NULL;
  size_t cnt = (size_t)mh->msg_iovlen;
  *total = 0;
  if (cnt == 0)
    return (struct u_iovec *)kzalloc(sizeof(struct u_iovec));
  struct u_iovec *iov = (struct u_iovec *)kmalloc(cnt * sizeof(*iov));
  if (!iov)
    return NULL;
  if (!access_ok((void *)mh->msg_iov, cnt * sizeof(*iov)) ||
      copy_from_user(iov, (void *)mh->msg_iov, cnt * sizeof(*iov)) < 0) {
    kfree(iov);
    return NULL;
  }
  for (size_t i = 0; i < cnt; i++) {
    if (iov[i].len > (1ULL << 30) || *total + iov[i].len > (1ULL << 30)) {
      kfree(iov);
      return NULL;
    }
    *total += (size_t)iov[i].len;
  }
  return iov;
}

#define CMSG_HDR 16
#define CMSG_ALIGN8(x) (((x) + 7) & ~(size_t)7)

// Procesa el buffer de control del emisor. Devuelve fds retenidos (ref) en
// *fds_out / *nfds_out. 0 o errno.
static int parse_control(process_t *proc, const struct u_msghdr *mh,
                         file_descriptor_t ***fds_out, int *nfds_out) {
  *fds_out = NULL;
  *nfds_out = 0;
  if (!mh->msg_control || mh->msg_controllen < CMSG_HDR)
    return 0;
  size_t clen = (size_t)mh->msg_controllen;
  if (clen > 4096)
    return -ENOBUFS;
  uint8_t *cb = (uint8_t *)kmalloc(clen);
  if (!cb)
    return -ENOMEM;
  if (!access_ok((void *)mh->msg_control, clen) ||
      copy_from_user(cb, (void *)mh->msg_control, clen) < 0) {
    kfree(cb);
    return -EFAULT;
  }
  file_descriptor_t **fds = NULL;
  int nfds = 0;
  size_t off = 0;
  int rc = 0;
  while (off + CMSG_HDR <= clen) {
    uint64_t l = *(uint64_t *)(cb + off);
    int32_t level = *(int32_t *)(cb + off + 8);
    int32_t type = *(int32_t *)(cb + off + 12);
    if (l < CMSG_HDR || off + l > clen) {
      rc = -EINVAL;
      break;
    }
    if (level == SOL_SOCKET_L && type == SCM_RIGHTS_T) {
      int n = (int)((l - CMSG_HDR) / 4);
      if (nfds + n > UNIX_MAX_FDS) {
        rc = -EINVAL;
        break;
      }
      if (!fds) {
        fds = (file_descriptor_t **)kzalloc(UNIX_MAX_FDS * sizeof(*fds));
        if (!fds) {
          rc = -ENOMEM;
          break;
        }
      }
      for (int i = 0; i < n; i++) {
        int32_t fd = *(int32_t *)(cb + off + CMSG_HDR + (size_t)i * 4);
        file_descriptor_t *f = vfs_fd_get_ref(proc, fd);
        if (!f) {
          rc = -EBADF;
          break;
        }
        fds[nfds++] = f;
      }
      if (rc)
        break;
    }
    // SCM_CREDENTIALS: las credenciales siempre son las reales del emisor.
    off += CMSG_ALIGN8(l);
  }
  kfree(cb);
  if (rc) {
    for (int i = 0; i < nfds; i++)
      vfs_fd_put_ref(fds[i]);
    if (fds)
      kfree(fds);
    return rc;
  }
  if (nfds == 0 && fds) {
    kfree(fds);
    fds = NULL;
  }
  *fds_out = fds;
  *nfds_out = nfds;
  return 0;
}

static void release_fds(file_descriptor_t **fds, int nfds) {
  for (int i = 0; i < nfds; i++)
    vfs_fd_put_ref(fds[i]);
  if (fds)
    kfree(fds);
}

// ---------------------------------------------------------------------------
// Syscalls
// ---------------------------------------------------------------------------
int64_t unix_sys_socket(process_t *proc, uint64_t type, uint64_t proto) {
  if (proto != 0)
    return -EPROTONOSUPPORT;
  int base = (int)(type & 0xF);
  if (base != SOCK_STREAM_T && base != SOCK_DGRAM_T &&
      base != SOCK_SEQPACKET_T)
    return -ESOCKTNOSUPPORT_U;
  usock_t *u = unix_new(base);
  if (!u)
    return -ENOMEM;
  u->nonblock = (type & SOCK_NONBLOCK_F) != 0;
  return unix_wrap(proc, u, (type & SOCK_CLOEXEC_F) != 0);
}

int64_t unix_sys_socketpair(process_t *proc, uint64_t type, uint64_t proto,
                            uint64_t sv_uptr) {
  if (proto != 0)
    return -EPROTONOSUPPORT;
  int base = (int)(type & 0xF);
  if (base != SOCK_STREAM_T && base != SOCK_DGRAM_T &&
      base != SOCK_SEQPACKET_T)
    return -ESOCKTNOSUPPORT_U;
  if (!access_ok((void *)sv_uptr, 8))
    return -EFAULT;
  usock_t *a = unix_new(base);
  usock_t *b = unix_new(base);
  if (!a || !b) {
    if (a)
      kfree(a);
    if (b)
      kfree(b);
    return -ENOMEM;
  }
  a->nonblock = b->nonblock = (type & SOCK_NONBLOCK_F) != 0;
  a->connected = b->connected = true;
  a->peer = b;
  b->peer = a;
  cred_of(proc, &a->peercred);
  b->peercred = a->peercred;
  a->has_peercred = b->has_peercred = true;

  bool cx = (type & SOCK_CLOEXEC_F) != 0;
  int64_t fa = unix_wrap(proc, a, cx);
  if (fa < 0) {
    // a ya se liberó (con b como par: unix_release lo desenlaza).
    unix_release(b);
    return fa;
  }
  int64_t fb = unix_wrap(proc, b, cx);
  if (fb < 0) {
    vfs_close_for_proc(proc, (int)fa);
    return fb;
  }
  int sv[2] = {(int)fa, (int)fb};
  if (copy_to_user((void *)sv_uptr, sv, sizeof(sv)) < 0) {
    vfs_close_for_proc(proc, (int)fa);
    vfs_close_for_proc(proc, (int)fb);
    return -EFAULT;
  }
  return 0;
}

int64_t unix_sys_bind(process_t *proc, int fd, uint64_t addr,
                      uint64_t addrlen) {
  file_descriptor_t *f;
  usock_t *u = get_unix(proc, fd, &f);
  if (!u)
    return -ENOTSOCK;
  char key[UNIX_NAME_MAX];
  int klen = 0;
  int64_t rc = parse_addr(proc, addr, addrlen, key, &klen);
  if (rc < 0) {
    vfs_fd_put_ref(f);
    return rc;
  }
  LOCK();
  if (u->registered) {
    rc = -EINVAL;
  } else if (klen == 0) {
    // autobind: nombre abstracto "\0xxxxx"
    for (int tries = 0; tries < 0x100000; tries++) {
      uint32_t id = (g_autobind++) & 0xFFFFF;
      key[0] = 0;
      for (int i = 0; i < 5; i++)
        key[1 + i] = "0123456789abcdef"[(id >> (16 - 4 * i)) & 0xF];
      klen = 6;
      if (!find_bound(key, klen))
        break;
    }
    memcpy(u->name, key, (size_t)klen);
    u->name_len = klen;
    u->bnext = g_bound;
    g_bound = u;
    u->registered = true;
    rc = 0;
  } else if (key[0] == '/' && find_bound(key, klen) &&
             !path_inode_is_sock(key, klen)) {
    // Nombre retenido por un socket cuyo fichero fue borrado: se libera.
    unregister_name(find_bound(key, klen));
    goto bind_new;
  } else if (find_bound(key, klen)) {
    rc = -EADDRINUSE;
  } else {
bind_new:
    if (key[0] == '/') {
      // Inodo S_IFSOCK en el VFS. Si el FS no admite mknod (initrd de solo
      // lectura...) el nombre solo existe en la tabla interna.
      char kpath[UNIX_NAME_MAX + 1];
      memcpy(kpath, key, (size_t)klen);
      kpath[klen] = '\0';
      int mrc = vfs_mknod_raw(kpath, S_IFSOCK | 0777, 0);
      if (mrc == -EEXIST) {
        rc = -EADDRINUSE;
        goto bind_done;
      }
    }
    memcpy(u->name, key, (size_t)klen);
    u->name_len = klen;
    u->bnext = g_bound;
    g_bound = u;
    u->registered = true;
    rc = 0;
  }
bind_done:
  UNLOCK();
  vfs_fd_put_ref(f);
  return rc;
}

int64_t unix_sys_listen(process_t *proc, int fd, int backlog) {
  file_descriptor_t *f;
  usock_t *u = get_unix(proc, fd, &f);
  if (!u)
    return -ENOTSOCK;
  int64_t rc = 0;
  LOCK();
  if (u->type == SOCK_DGRAM_T)
    rc = -EOPNOTSUPP;
  else if (u->connected)
    rc = -EINVAL;
  else if (!u->registered)
    rc = -EINVAL; // Linux autobinds; los clientes reales siempre hacen bind
  else {
    if (backlog < 1)
      backlog = 1;
    if (backlog > UNIX_BACKLOG_MAX)
      backlog = UNIX_BACKLOG_MAX;
    u->listening = true;
    u->backlog = backlog;
    cred_of(proc, &u->peercred);
    u->has_peercred = true;
  }
  UNLOCK();
  vfs_fd_put_ref(f);
  return rc;
}

int64_t unix_sys_connect(process_t *proc, int fd, uint64_t addr,
                         uint64_t addrlen) {
  file_descriptor_t *f;
  usock_t *u = get_unix(proc, fd, &f);
  if (!u)
    return -ENOTSOCK;
  char key[UNIX_NAME_MAX];
  int klen = 0;
  int64_t rc = parse_addr(proc, addr, addrlen, key, &klen);
  if (rc < 0 || klen == 0) {
    vfs_fd_put_ref(f);
    return rc < 0 ? rc : -EINVAL;
  }
  bool nb = is_nonblock(u, f, 0);

  LOCK();
  for (;;) {
    usock_t *l = find_bound(key, klen);
    if (l && key[0] == '/' && !path_inode_is_sock(key, klen))
      l = NULL; // fichero borrado: el socket ya no es alcanzable
    if (u->type == SOCK_DGRAM_T) {
      if (!l || l->type != SOCK_DGRAM_T) {
        rc = (key[0] == '/') ? -ENOENT : -ECONNREFUSED;
      } else {
        memcpy(u->dst, key, (size_t)klen);
        u->dst_len = klen;
        rc = 0;
      }
      break;
    }
    if (u->connected || u->listening) {
      rc = u->connected ? -EISCONN : -EINVAL;
      break;
    }
    if (!l) {
      rc = (key[0] == '/') ? -ENOENT : -ECONNREFUSED;
      break;
    }
    if (!l->listening || l->type != u->type) {
      rc = -ECONNREFUSED;
      break;
    }
    if (l->acc_count >= l->backlog) {
      if (nb) {
        rc = -EAGAIN;
        break;
      }
      uint32_t gen = __atomic_load_n(&u->gen, __ATOMIC_ACQUIRE);
      UNLOCK();
      int w = sleep_gen(u, gen, UNIX_WAIT_POLL_TICKS);
      if (w < 0) {
        vfs_fd_put_ref(f);
        return w;
      }
      LOCK();
      continue;
    }
    usock_t *c = unix_new(u->type);
    if (!c) {
      rc = -ENOMEM;
      break;
    }
    c->connected = true;
    memcpy(c->name, l->name, (size_t)l->name_len);
    c->name_len = l->name_len; // getsockname del hijo = nombre del listener
    c->peer = u;
    u->peer = c;
    u->connected = true;
    cred_of(proc, &c->peercred); // el servidor ve las credenciales del cliente
    c->has_peercred = true;
    u->peercred = l->peercred; // el cliente ve las del listener
    u->has_peercred = l->has_peercred;
    c->acc_next = NULL;
    if (l->acc_tail)
      l->acc_tail->acc_next = c;
    else
      l->acc_head = c;
    l->acc_tail = c;
    l->acc_count++;
    notify(l);
    rc = 0;
    break;
  }
  UNLOCK();
  vfs_fd_put_ref(f);
  return rc;
}

int64_t unix_sys_accept(process_t *proc, int fd, uint64_t addr,
                        uint64_t addrlen_ptr, uint64_t flags) {
  file_descriptor_t *f;
  usock_t *u = get_unix(proc, fd, &f);
  if (!u)
    return -ENOTSOCK;
  bool nb = is_nonblock(u, f, 0);
  usock_t *c = NULL;

  LOCK();
  for (;;) {
    if (!u->listening) {
      UNLOCK();
      vfs_fd_put_ref(f);
      return -EINVAL;
    }
    if (u->acc_head) {
      c = u->acc_head;
      u->acc_head = c->acc_next;
      if (!u->acc_head)
        u->acc_tail = NULL;
      u->acc_count--;
      c->acc_next = NULL;
      // Hueco en el backlog: avisar a connect() bloqueados (polling) y epoll.
      notify(u);
      break;
    }
    if (nb) {
      UNLOCK();
      vfs_fd_put_ref(f);
      return -EAGAIN;
    }
    uint32_t gen = __atomic_load_n(&u->gen, __ATOMIC_ACQUIRE);
    UNLOCK();
    int rc = sleep_gen(u, gen, 0);
    if (rc < 0) {
      vfs_fd_put_ref(f);
      return rc;
    }
    LOCK();
  }
  char pname[UNIX_NAME_MAX];
  int plen = 0;
  if (c->peer && c->peer->name_len > 0) {
    plen = c->peer->name_len;
    memcpy(pname, c->peer->name, (size_t)plen);
  }
  UNLOCK();
  vfs_fd_put_ref(f);

  c->nonblock = (flags & SOCK_NONBLOCK_F) != 0;
  int64_t nfd = unix_wrap(proc, c, (flags & SOCK_CLOEXEC_F) != 0);
  if (nfd < 0)
    return nfd;
  put_addr(addr, addrlen_ptr, pname, plen);
  return nfd;
}

int64_t unix_sys_sendmsg(process_t *proc, int fd, uint64_t msg_uptr,
                         uint64_t flags) {
  file_descriptor_t *f;
  usock_t *u = get_unix(proc, fd, &f);
  if (!u)
    return -ENOTSOCK;
  struct u_msghdr mh;
  if (!msg_uptr || !access_ok((void *)msg_uptr, sizeof(mh)) ||
      copy_from_user(&mh, (void *)msg_uptr, sizeof(mh)) < 0) {
    vfs_fd_put_ref(f);
    return -EFAULT;
  }
  size_t total = 0;
  struct u_iovec *iov = load_iov(&mh, &total);
  if (!iov) {
    vfs_fd_put_ref(f);
    return -EMSGSIZE;
  }
  if (u->type == SOCK_STREAM_T && total > UNIX_BUF)
    total = UNIX_BUF; // envío parcial permitido en STREAM
  uint8_t *kbuf = NULL;
  if (total) {
    kbuf = (uint8_t *)kmalloc(total);
    if (!kbuf) {
      kfree(iov);
      vfs_fd_put_ref(f);
      return -ENOMEM;
    }
    size_t off = 0;
    for (size_t i = 0; i < mh.msg_iovlen && off < total; i++) {
      size_t n = (size_t)iov[i].len;
      if (n > total - off)
        n = total - off;
      if (n == 0)
        continue;
      if (!access_ok((void *)iov[i].base, n) ||
          copy_from_user(kbuf + off, (void *)iov[i].base, n) < 0) {
        kfree(kbuf);
        kfree(iov);
        vfs_fd_put_ref(f);
        return -EFAULT;
      }
      off += n;
    }
  }
  kfree(iov);

  char dkey[UNIX_NAME_MAX];
  int dklen = 0;
  int64_t rc = 0;
  if (mh.msg_name && mh.msg_namelen >= 2) {
    rc = parse_addr(proc, mh.msg_name, mh.msg_namelen, dkey, &dklen);
    if (rc == 0 && u->type != SOCK_DGRAM_T)
      dklen = 0; // en STREAM/SEQPACKET el destino se ignora
  }
  file_descriptor_t **fds = NULL;
  int nfds = 0;
  if (rc == 0)
    rc = parse_control(proc, &mh, &fds, &nfds);
  if (rc < 0) {
    if (kbuf)
      kfree(kbuf);
    vfs_fd_put_ref(f);
    return rc;
  }

  // Un mensaje sin datos pero con fds: Linux lo permite en DGRAM/SEQPACKET.
  if (total == 0 && u->type == SOCK_STREAM_T && nfds) {
    release_fds(fds, nfds);
    if (kbuf)
      kfree(kbuf);
    vfs_fd_put_ref(f);
    return 0;
  }

  int64_t r = core_send(u, is_nonblock(u, f, flags), kbuf, total,
                        dklen ? dkey : NULL, dklen, fds, nfds, proc);
  if (r == -EPIPE && !(flags & 0x4000)) // MSG_NOSIGNAL
    signal_raise_sigpipe();
  if (r < 0 && nfds)
    release_fds(fds, nfds);
  if (kbuf)
    kfree(kbuf);
  vfs_fd_put_ref(f);
  return r;
}

int64_t unix_sys_recvmsg(process_t *proc, int fd, uint64_t msg_uptr,
                         uint64_t flags) {
  file_descriptor_t *f;
  usock_t *u = get_unix(proc, fd, &f);
  if (!u)
    return -ENOTSOCK;
  struct u_msghdr mh;
  if (!msg_uptr || !access_ok((void *)msg_uptr, sizeof(mh)) ||
      copy_from_user(&mh, (void *)msg_uptr, sizeof(mh)) < 0) {
    vfs_fd_put_ref(f);
    return -EFAULT;
  }
  size_t total = 0;
  struct u_iovec *iov = load_iov(&mh, &total);
  if (!iov) {
    vfs_fd_put_ref(f);
    return -EMSGSIZE;
  }
  size_t want = total;
  if (want > UNIX_BUF)
    want = UNIX_BUF;
  uint8_t *kbuf = (uint8_t *)kmalloc(want ? want : 1);
  rxinfo_t *ri = (rxinfo_t *)kzalloc(sizeof(*ri));
  if (!kbuf || !ri) {
    if (kbuf)
      kfree(kbuf);
    if (ri)
      kfree(ri);
    kfree(iov);
    vfs_fd_put_ref(f);
    return -ENOMEM;
  }

  int64_t n = core_recv(u, is_nonblock(u, f, flags), (int)flags, kbuf, want, ri);
  if (n < 0) {
    kfree(kbuf);
    kfree(ri);
    kfree(iov);
    vfs_fd_put_ref(f);
    return n;
  }

  // Datos -> iovecs
  size_t left = (size_t)n, off = 0;
  for (size_t i = 0; i < mh.msg_iovlen && left; i++) {
    size_t c = (size_t)iov[i].len;
    if (c > left)
      c = left;
    if (!c)
      continue;
    if (!access_ok((void *)iov[i].base, c) ||
        copy_to_user((void *)iov[i].base, kbuf + off, c) < 0) {
      rx_drop(ri);
      kfree(kbuf);
      kfree(ri);
      kfree(iov);
      vfs_fd_put_ref(f);
      return -EFAULT;
    }
    off += c;
    left -= c;
  }
  kfree(kbuf);
  kfree(iov);

  uint32_t out_flags = ri->trunc ? MSG_TRUNC_F : 0;
  size_t ctl_used = 0;

  // Control: SCM_RIGHTS (+ SCM_CREDENTIALS si SO_PASSCRED).
  if (mh.msg_control && mh.msg_controllen >= CMSG_HDR) {
    uint8_t cb[CMSG_HDR + UNIX_MAX_FDS * 4 + 32];
    size_t cap = mh.msg_controllen;
    if (cap > sizeof(cb))
      cap = sizeof(cb);
    if (ri->nfds > 0) {
      int fit = (int)((cap - CMSG_HDR) / 4);
      if (fit > ri->nfds)
        fit = ri->nfds;
      if (fit < ri->nfds)
        out_flags |= MSG_CTRUNC_F;
      int installed = 0;
      int32_t newfds[UNIX_MAX_FDS];
      bool cx = (flags & MSG_CMSGCLOEXEC_F) != 0;
      for (int i = 0; i < ri->nfds; i++) {
        if (i < fit) {
          int nfd = vfs_fd_install_ref(proc, ri->fds[i], cx);
          if (nfd < 0) {
            vfs_fd_put_ref(ri->fds[i]);
            out_flags |= MSG_CTRUNC_F;
          } else {
            newfds[installed++] = nfd;
          }
        } else {
          vfs_fd_put_ref(ri->fds[i]);
        }
        ri->fds[i] = NULL;
      }
      if (installed) {
        *(uint64_t *)(cb) = CMSG_HDR + (uint64_t)installed * 4;
        *(int32_t *)(cb + 8) = SOL_SOCKET_L;
        *(int32_t *)(cb + 12) = SCM_RIGHTS_T;
        memcpy(cb + CMSG_HDR, newfds, (size_t)installed * 4);
        ctl_used = CMSG_ALIGN8(CMSG_HDR + (size_t)installed * 4);
      }
      kfree(ri->fds);
      ri->fds = NULL;
      ri->nfds = 0;
    }
    if (u->passcred && ctl_used + CMSG_HDR + 16 <= cap) {
      memset(cb + ctl_used, 0, CMSG_HDR + 16);
      *(uint64_t *)(cb + ctl_used) = CMSG_HDR + 12;
      *(int32_t *)(cb + ctl_used + 8) = SOL_SOCKET_L;
      *(int32_t *)(cb + ctl_used + 12) = SCM_CREDENTIALS_T;
      memcpy(cb + ctl_used + CMSG_HDR, &ri->cred, 12);
      ctl_used += CMSG_HDR + 16;
    }
    if (ctl_used &&
        (!access_ok((void *)mh.msg_control, ctl_used) ||
         copy_to_user((void *)mh.msg_control, cb, ctl_used) < 0))
      ctl_used = 0;
  } else if (ri->nfds > 0) {
    out_flags |= MSG_CTRUNC_F;
    rx_drop(ri);
  }

  // Dirección del emisor (DGRAM)
  uint32_t namelen_out = 0;
  if (mh.msg_name && u->type == SOCK_DGRAM_T) {
    uint8_t sa[2 + UNIX_NAME_MAX + 1];
    memset(sa, 0, sizeof(sa));
    sa[0] = AF_UNIX_FAMILY;
    size_t tot = 2;
    if (ri->from_len > 0) {
      memcpy(sa + 2, ri->from, (size_t)ri->from_len);
      tot = 2 + (size_t)ri->from_len + (ri->from[0] == '/' ? 1 : 0);
    }
    size_t c = tot < mh.msg_namelen ? tot : mh.msg_namelen;
    if (c && access_ok((void *)mh.msg_name, c))
      (void)copy_to_user((void *)mh.msg_name, sa, c);
    namelen_out = (uint32_t)tot;
  }

  struct u_msghdr out = mh;
  out.msg_namelen = namelen_out;
  out.msg_controllen = ctl_used;
  out.msg_flags = out_flags;
  (void)copy_to_user((void *)msg_uptr, &out, sizeof(out));

  int64_t ret = n;
  if ((flags & MSG_TRUNC_F) && u->type != SOCK_STREAM_T)
    ret = (int64_t)ri->full_len;
  kfree(ri);
  vfs_fd_put_ref(f);
  return ret;
}

int64_t unix_sys_sendto(process_t *proc, int fd, uint64_t buf, uint64_t len,
                        uint64_t flags, uint64_t addr, uint64_t addrlen) {
  file_descriptor_t *f;
  usock_t *u = get_unix(proc, fd, &f);
  if (!u)
    return -ENOTSOCK;
  size_t n = (size_t)len;
  if (n > (1ULL << 30))
    n = 1 << 30;
  if (u->type == SOCK_STREAM_T && n > UNIX_BUF)
    n = UNIX_BUF;
  uint8_t *kbuf = n ? (uint8_t *)kmalloc(n) : NULL;
  if (n && !kbuf) {
    vfs_fd_put_ref(f);
    return -ENOMEM;
  }
  if (n && (!access_ok((void *)buf, n) ||
            copy_from_user(kbuf, (void *)buf, n) < 0)) {
    kfree(kbuf);
    vfs_fd_put_ref(f);
    return -EFAULT;
  }
  char dkey[UNIX_NAME_MAX];
  int dklen = 0;
  int64_t rc = 0;
  if (addr && addrlen >= 2 && u->type == SOCK_DGRAM_T)
    rc = parse_addr(proc, addr, addrlen, dkey, &dklen);
  if (rc < 0) {
    if (kbuf)
      kfree(kbuf);
    vfs_fd_put_ref(f);
    return rc;
  }
  int64_t r = core_send(u, is_nonblock(u, f, flags), kbuf, n,
                        dklen ? dkey : NULL, dklen, NULL, 0, proc);
  if (r == -EPIPE && !(flags & 0x4000)) // MSG_NOSIGNAL
    signal_raise_sigpipe();
  if (kbuf)
    kfree(kbuf);
  vfs_fd_put_ref(f);
  return r;
}

int64_t unix_sys_recvfrom(process_t *proc, int fd, uint64_t buf, uint64_t len,
                          uint64_t flags, uint64_t addr, uint64_t addrlen_ptr) {
  file_descriptor_t *f;
  usock_t *u = get_unix(proc, fd, &f);
  if (!u)
    return -ENOTSOCK;
  size_t want = (size_t)len;
  if (want > UNIX_BUF)
    want = UNIX_BUF;
  uint8_t *kbuf = (uint8_t *)kmalloc(want ? want : 1);
  rxinfo_t *ri = (rxinfo_t *)kzalloc(sizeof(*ri));
  if (!kbuf || !ri) {
    if (kbuf)
      kfree(kbuf);
    if (ri)
      kfree(ri);
    vfs_fd_put_ref(f);
    return -ENOMEM;
  }
  int64_t n = core_recv(u, is_nonblock(u, f, flags), (int)flags, kbuf, want, ri);
  if (n >= 0) {
    if (n && (!access_ok((void *)buf, (size_t)n) ||
              copy_to_user((void *)buf, kbuf, (size_t)n) < 0))
      n = -EFAULT;
    else if (u->type == SOCK_DGRAM_T)
      put_addr(addr, addrlen_ptr, ri->from, ri->from_len);
  }
  rx_drop(ri);
  kfree(ri);
  kfree(kbuf);
  vfs_fd_put_ref(f);
  return n;
}

int64_t unix_sys_getsockname(process_t *proc, int fd, uint64_t addr,
                             uint64_t addrlen_ptr, bool peer) {
  file_descriptor_t *f;
  usock_t *u = get_unix(proc, fd, &f);
  if (!u)
    return -ENOTSOCK;
  char name[UNIX_NAME_MAX];
  int nl = 0;
  int64_t rc = 0;
  LOCK();
  if (peer) {
    if (!u->peer && !(u->type == SOCK_DGRAM_T && u->dst_len > 0))
      rc = -ENOTCONN;
    else if (u->peer) {
      nl = u->peer->name_len;
      memcpy(name, u->peer->name, (size_t)nl);
    } else {
      nl = u->dst_len;
      memcpy(name, u->dst, (size_t)nl);
    }
  } else {
    nl = u->name_len;
    memcpy(name, u->name, (size_t)nl);
  }
  UNLOCK();
  if (rc == 0)
    put_addr(addr, addrlen_ptr, name, nl);
  vfs_fd_put_ref(f);
  return rc;
}

int64_t unix_sys_shutdown(process_t *proc, int fd, int how) {
  file_descriptor_t *f;
  usock_t *u = get_unix(proc, fd, &f);
  if (!u)
    return -ENOTSOCK;
  if (how < 0 || how > 2) {
    vfs_fd_put_ref(f);
    return -EINVAL;
  }
  int64_t rc = 0;
  LOCK();
  if (!u->connected && !u->peer && u->type != SOCK_DGRAM_T) {
    rc = -ENOTCONN;
  } else {
    if (how == 0 || how == 2) {
      u->shut_rd = true;
      if (u->peer)
        notify(u->peer); // sus envíos ahora darán EPIPE
      notify(u);
    }
    if (how == 1 || how == 2) {
      u->shut_wr = true;
      if (u->peer) {
        u->peer->hup = true; // el par ve EOF tras vaciar la cola
        notify(u->peer);
      }
    }
  }
  UNLOCK();
  vfs_fd_put_ref(f);
  return rc;
}

int64_t unix_sys_setsockopt(process_t *proc, int fd, uint64_t level,
                            uint64_t optname, uint64_t optval,
                            uint64_t optlen) {
  file_descriptor_t *f;
  usock_t *u = get_unix(proc, fd, &f);
  if (!u)
    return -ENOTSOCK;
  if (level == SOL_SOCKET_L && optname == SO_PASSCRED_O) {
    int v = 0;
    if (optlen < 4 || !access_ok((void *)optval, 4) ||
        copy_from_user(&v, (void *)optval, 4) < 0) {
      vfs_fd_put_ref(f);
      return -EFAULT;
    }
    u->passcred = v != 0;
  }
  // Resto de opciones (REUSEADDR, buffers, timeouts...): se aceptan e ignoran.
  vfs_fd_put_ref(f);
  return 0;
}

int64_t unix_sys_getsockopt(process_t *proc, int fd, uint64_t level,
                            uint64_t optname, uint64_t optval,
                            uint64_t optlen_ptr) {
  file_descriptor_t *f;
  usock_t *u = get_unix(proc, fd, &f);
  if (!u)
    return -ENOTSOCK;
  int64_t rc = 0;
  uint8_t out[16];
  uint32_t outlen = 4;
  memset(out, 0, sizeof(out));
  if (level != SOL_SOCKET_L) {
    rc = -ENOPROTOOPT_U;
  } else {
    int v = 0;
    switch (optname) {
    case SO_TYPE_O:
      v = u->type;
      break;
    case SO_ERROR_O:
      v = 0;
      break;
    case SO_SNDBUF_O:
    case SO_RCVBUF_O:
      v = UNIX_BUF;
      break;
    case SO_PASSCRED_O:
      v = u->passcred;
      break;
    case SO_ACCEPTCONN_O:
      v = u->listening;
      break;
    case SO_DOMAIN_O:
      v = AF_UNIX_FAMILY;
      break;
    case SO_PROTOCOL_O:
    case SO_REUSEADDR_O:
      v = 0;
      break;
    case SO_PEERCRED_O: {
      LOCK();
      bool ok = u->has_peercred;
      ucred_t c = u->peercred;
      UNLOCK();
      if (!ok) {
        rc = -ENOTCONN;
        break;
      }
      memcpy(out, &c, 12);
      outlen = 12;
      goto done;
    }
    default:
      rc = -ENOPROTOOPT_U;
      break;
    }
    if (rc == 0)
      memcpy(out, &v, 4);
  }
done:
  if (rc == 0) {
    uint32_t ulen = 0;
    if (!access_ok((void *)optlen_ptr, 4) ||
        copy_from_user(&ulen, (void *)optlen_ptr, 4) < 0) {
      rc = -EFAULT;
    } else {
      uint32_t n = ulen < outlen ? ulen : outlen;
      if (n && (!access_ok((void *)optval, n) ||
                copy_to_user((void *)optval, out, n) < 0))
        rc = -EFAULT;
      else
        (void)copy_to_user((void *)optlen_ptr, &n, 4);
    }
  }
  vfs_fd_put_ref(f);
  return rc;
}

void unix_set_nonblock(process_t *proc, int fd, bool nb) {
  file_descriptor_t *f;
  usock_t *u = get_unix(proc, fd, &f);
  if (!u)
    return;
  u->nonblock = nb;
  vfs_fd_put_ref(f);
}
