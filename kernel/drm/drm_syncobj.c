// kernel/drm/drm_syncobj.c
//
// drm_syncobj (binario y de linea temporal) y sync_file, con los ioctls y
// estructuras binarias de Linux (DRM_CAP_SYNCOBJ / DRM_CAP_SYNCOBJ_TIMELINE).
// Es la base de sincronizacion que Mesa (virgl/venus, radv, iris...), Vulkan y
// los compositores usan para coordinar GPU y CPU; los drivers de GPU de
// Aurora senalan fences con drm_syncobj_signal_point().
//
// Modelo: cada syncobj tiene un valor de linea temporal (`value`). Uno binario
// esta senalado si value >= 1 (el punto 0 se trata como 1). Un syncobj
// importado desde un sync_file aun sin senalar es un alias del origen.
// Todos comparten una unica cola de espera (las esperas son poco frecuentes).

#include "../heap.h"
#include "../klog.h"
#include "../process.h"
#include "../string.h"
#include "../uaccess.h"
#include "../vfs.h"
#include "drm.h"

extern uint64_t sched_get_ticks(void);

#define DRM_CLOEXEC 0x80000
#ifndef ETIME
#define ETIME 62 // Linux: timeout de DRM_IOCTL_SYNCOBJ_WAIT
#endif
#define SYNCOBJ_CREATE_SIGNALED (1u << 0)
#define SYNCOBJ_FD_SYNC_FILE (1u << 0) // export/import sync_file
#define WAIT_ALL (1u << 0)
#define WAIT_FOR_SUBMIT (1u << 1)
#define WAIT_AVAILABLE (1u << 2)
#define WAIT_DEADLINE (1u << 3)
#define MAX_WAIT_HANDLES 64

struct drm_syncobj {
  int refcnt;
  uint64_t value;            // linea temporal; binario: 0 = sin senalar
  struct drm_syncobj *alias; // import de un sync_file pendiente (con alias_pt)
  uint64_t alias_pt;
};

static spinlock_t g_so_lock = {.locked = 0};
static wait_queue_t g_so_wq;
static bool g_so_init;

static void so_init_once(void) {
  if (!g_so_init) {
    wait_queue_init(&g_so_wq);
    g_so_init = true;
  }
}

static uint64_t norm_pt(uint64_t pt) { return pt ? pt : 1; }

static bool so_signaled_locked(struct drm_syncobj *o, uint64_t pt) {
  pt = norm_pt(pt);
  for (int depth = 0; o && depth < 8; depth++) {
    if (o->value >= pt)
      return true;
    pt = norm_pt(o->alias_pt);
    o = o->alias;
  }
  return false;
}

static bool so_signaled(struct drm_syncobj *o, uint64_t pt) {
  unsigned long fl = spin_lock_irqsave(&g_so_lock);
  bool r = so_signaled_locked(o, pt);
  spin_unlock_irqrestore(&g_so_lock, fl);
  return r;
}

static struct drm_syncobj *so_new(void) {
  struct drm_syncobj *o = (struct drm_syncobj *)kzalloc(sizeof(*o));
  if (o)
    o->refcnt = 1;
  return o;
}
static void so_get(struct drm_syncobj *o) {
  unsigned long fl = spin_lock_irqsave(&g_so_lock);
  o->refcnt++;
  spin_unlock_irqrestore(&g_so_lock, fl);
}
static void so_put(struct drm_syncobj *o) {
  unsigned long fl = spin_lock_irqsave(&g_so_lock);
  int r = --o->refcnt;
  spin_unlock_irqrestore(&g_so_lock, fl);
  if (r == 0) {
    if (o->alias)
      so_put(o->alias);
    kfree(o);
  }
}

// API para drivers de GPU ----------------------------------------------------
void drm_syncobj_signal_point(struct drm_syncobj *o, uint64_t point) {
  so_init_once();
  unsigned long fl = spin_lock_irqsave(&g_so_lock);
  point = norm_pt(point);
  if (point > o->value)
    o->value = point;
  spin_unlock_irqrestore(&g_so_lock, fl);
  wake_up_all(&g_so_wq);
}

struct drm_syncobj *drm_syncobj_find(drm_file_t *f, uint32_t handle) {
  if (handle == 0 || handle > DRM_MAX_SYNCOBJS)
    return NULL;
  return f->syncobjs[handle - 1];
}

static int so_handle_alloc(drm_file_t *f, struct drm_syncobj *o, uint32_t *h) {
  for (int i = 0; i < DRM_MAX_SYNCOBJS; i++)
    if (!f->syncobjs[i]) {
      f->syncobjs[i] = o;
      *h = (uint32_t)i + 1;
      return 0;
    }
  return -ENOSPC;
}

void drm_file_release_syncobjs(drm_file_t *f) {
  for (int i = 0; i < DRM_MAX_SYNCOBJS; i++)
    if (f->syncobjs[i]) {
      so_put(f->syncobjs[i]);
      f->syncobjs[i] = NULL;
    }
}

// Ficheros anonimos (syncobj fd y sync_file) -----------------------------------
typedef struct {
  struct drm_syncobj *o;
  uint64_t pt;
} so_fd_t;

static int so_fd_poll(vfs_node_t *node, short events) {
  so_fd_t *s = (so_fd_t *)node->priv;
  if (!s)
    return 0;
  return so_signaled(s->o, s->pt) ? (events & (1 | 4)) : 0; // POLLIN | POLLOUT
}
static wait_queue_t *so_fd_poll_wq(vfs_node_t *node) {
  (void)node;
  so_init_once();
  return &g_so_wq;
}
static int so_fd_close(vfs_node_t *node) {
  so_fd_t *s = (so_fd_t *)node->priv;
  if (s) {
    so_put(s->o);
    kfree(s);
    node->priv = NULL;
  }
  return 0;
}
static vfs_ops_t so_fd_ops = {
    .close = so_fd_close,
    .poll = so_fd_poll,
    .poll_wq = so_fd_poll_wq,
};

static so_fd_t *so_fd_of(process_t *proc, int fd) {
  if (fd < 0 || fd >= MAX_PROCESS_FDS || !proc->fds[fd] || !proc->fds[fd]->node)
    return NULL;
  vfs_node_t *n = proc->fds[fd]->node;
  return n->ops == &so_fd_ops ? (so_fd_t *)n->priv : NULL;
}

// Crea un fd que referencia (o, pt); toma una referencia sobre o.
static int so_make_fd(struct drm_syncobj *o, uint64_t pt, const char *name,
                      uint32_t cloexec, int *out) {
  process_t *proc = process_current();
  if (!proc)
    return -EFAULT;
  int fdn = -1;
  for (int i = 0; i < MAX_PROCESS_FDS; i++)
    if (!proc->fds[i]) {
      fdn = i;
      break;
    }
  if (fdn < 0)
    return -EMFILE;
  so_fd_t *s = (so_fd_t *)kzalloc(sizeof(*s));
  vfs_node_t *vn = (vfs_node_t *)kzalloc(sizeof(*vn));
  file_descriptor_t *fd = (file_descriptor_t *)kzalloc(sizeof(*fd));
  if (!s || !vn || !fd) {
    kfree(s);
    kfree(vn);
    kfree(fd);
    return -ENOMEM;
  }
  so_get(o);
  s->o = o;
  s->pt = pt;
  strncpy(vn->name, name, sizeof(vn->name) - 1);
  vn->flags = VFS_FILE;
  vn->ops = &so_fd_ops;
  vn->priv = s;
  vn->mode = S_IFREG | 0600;
  vn->ref_count = 1;
  fd->node = vn;
  fd->flags = O_RDWR;
  fd->ref_count = 1;
  mutex_init(&fd->lock);
  wait_queue_init(&fd->read_wq);
  wait_queue_init(&fd->write_wq);
  proc->fds[fdn] = fd;
  if (cloexec & DRM_CLOEXEC)
    fd_cloexec_set(proc, fdn);
  *out = fdn;
  return 0;
}

// Estructuras uapi --------------------------------------------------------------
struct sync_create { uint32_t handle, flags; };
struct sync_destroy { uint32_t handle, pad; };
struct sync_handle { uint32_t handle, flags; int32_t fd; uint32_t pad; uint64_t point; };
struct sync_wait {
  uint64_t handles;
  int64_t timeout_nsec;
  uint32_t count_handles, flags, first_signaled, pad;
  uint64_t deadline_nsec;
};
struct sync_tl_wait {
  uint64_t handles, points;
  int64_t timeout_nsec;
  uint32_t count_handles, flags, first_signaled, pad;
  uint64_t deadline_nsec;
};
struct sync_array { uint64_t handles; uint32_t count_handles, pad; };
struct sync_tl_array { uint64_t handles, points; uint32_t count_handles, flags; };
struct sync_transfer {
  uint32_t src_handle, dst_handle;
  uint64_t src_point, dst_point;
  uint32_t flags, pad;
};

static int get_arg(uint64_t arg, unsigned long req, void *dst, size_t full) {
  size_t n = (req >> 16) & 0x3FFF;
  if (n > full)
    n = full;
  memset(dst, 0, full);
  return drm_get_user(arg, dst, n);
}
static int put_arg(uint64_t arg, unsigned long req, const void *src, size_t full) {
  size_t n = (req >> 16) & 0x3FFF;
  if (n > full)
    n = full;
  return drm_put_user(arg, src, n);
}

static int copy_handles(uint64_t uptr, uint32_t n, uint32_t *h, uint64_t *pts,
                        uint64_t uptr_pts) {
  if (n == 0 || n > MAX_WAIT_HANDLES)
    return -EINVAL;
  int rc = drm_get_user(uptr, h, n * sizeof(uint32_t));
  if (rc)
    return rc;
  if (pts) {
    if (uptr_pts)
      rc = drm_get_user(uptr_pts, pts, n * sizeof(uint64_t));
    else
      memset(pts, 0, n * sizeof(uint64_t));
  }
  return rc;
}

// Espera ----------------------------------------------------------------------
struct wait_ctx {
  struct drm_syncobj *o[MAX_WAIT_HANDLES];
  uint64_t pt[MAX_WAIT_HANDLES];
  uint32_t n, flags;
};

// Cuantos estan senalados y cual fue el primero.
static uint32_t wc_count(const struct wait_ctx *w, uint32_t *first) {
  uint32_t c = 0;
  *first = 0;
  for (uint32_t i = 0; i < w->n; i++)
    if (so_signaled(w->o[i], w->pt[i])) {
      if (!c)
        *first = i;
      c++;
    }
  return c;
}
static bool wc_done(void *a) {
  struct wait_ctx *w = (struct wait_ctx *)a;
  uint32_t first;
  uint32_t c = wc_count(w, &first);
  return (w->flags & WAIT_ALL) ? c == w->n : c > 0;
}

// timeout_abs_ns: CLOCK_MONOTONIC absoluto (como Linux).
static int do_wait(drm_file_t *f, uint64_t uh, uint64_t upts, int64_t timeout_abs_ns,
                   uint32_t n, uint32_t flags, uint32_t *first_out) {
  uint32_t h[MAX_WAIT_HANDLES];
  struct wait_ctx w;
  uint64_t pts[MAX_WAIT_HANDLES];
  int rc = copy_handles(uh, n, h, pts, upts);
  if (rc)
    return rc;
  w.n = n;
  w.flags = flags;
  for (uint32_t i = 0; i < n; i++) {
    w.o[i] = drm_syncobj_find(f, h[i]);
    if (!w.o[i])
      return -ENOENT;
    w.pt[i] = pts[i];
  }
  so_init_once();
  // Se toman referencias: otro hilo podria destruir el handle mientras dormimos.
  for (uint32_t i = 0; i < n; i++)
    so_get(w.o[i]);
  int64_t now_ms = (int64_t)sched_get_ticks();
  int64_t abs_ms = timeout_abs_ns / 1000000;
  int64_t left = abs_ms - now_ms;
  rc = 0;
  if (!wc_done(&w)) {
    if (left <= 0) {
      rc = -ETIME;
    } else {
      long r = wait_event_interruptible_timeout(&g_so_wq, wc_done, &w, (uint64_t)left);
      if (r < 0)
        rc = (int)r;
      else if (!wc_done(&w))
        rc = -ETIME;
    }
  }
  uint32_t first = 0;
  wc_count(&w, &first);
  *first_out = first;
  for (uint32_t i = 0; i < n; i++)
    so_put(w.o[i]);
  return rc;
}

// Dispatcher ------------------------------------------------------------------
int drm_syncobj_ioctl(drm_file_t *f, unsigned long req, uint64_t arg) {
  unsigned nr = req & 0xFF;
  so_init_once();
  switch (nr) {
  case 0xBF: { // CREATE
    struct sync_create a;
    int rc = get_arg(arg, req, &a, sizeof(a));
    if (rc)
      return rc;
    if (a.flags & ~SYNCOBJ_CREATE_SIGNALED)
      return -EINVAL;
    struct drm_syncobj *o = so_new();
    if (!o)
      return -ENOMEM;
    if (a.flags & SYNCOBJ_CREATE_SIGNALED)
      o->value = 1;
    rc = so_handle_alloc(f, o, &a.handle);
    if (rc) {
      so_put(o);
      return rc;
    }
    return put_arg(arg, req, &a, sizeof(a));
  }
  case 0xC0: { // DESTROY
    struct sync_destroy a;
    int rc = get_arg(arg, req, &a, sizeof(a));
    if (rc)
      return rc;
    struct drm_syncobj *o = drm_syncobj_find(f, a.handle);
    if (!o)
      return -EINVAL;
    f->syncobjs[a.handle - 1] = NULL;
    so_put(o);
    return 0;
  }
  case 0xC1: { // HANDLE_TO_FD
    struct sync_handle a;
    int rc = get_arg(arg, req, &a, sizeof(a));
    if (rc)
      return rc;
    if (a.flags & ~(SYNCOBJ_FD_SYNC_FILE | DRM_CLOEXEC))
      return -EINVAL;
    struct drm_syncobj *o = drm_syncobj_find(f, a.handle);
    if (!o)
      return -ENOENT;
    uint64_t pt = 0;
    int fdn;
    if (a.flags & SYNCOBJ_FD_SYNC_FILE) {
      // sync_file: el fence actual (el punto pedido; 0 = el valor presente).
      unsigned long fl = spin_lock_irqsave(&g_so_lock);
      pt = a.point ? a.point : norm_pt(o->value);
      spin_unlock_irqrestore(&g_so_lock, fl);
    } else {
      pt = 0; // fd del syncobj completo
    }
    rc = so_make_fd(o, pt, (a.flags & SYNCOBJ_FD_SYNC_FILE) ? "sync_file" : "syncobj",
                    a.flags, &fdn);
    if (rc)
      return rc;
    a.fd = fdn;
    return put_arg(arg, req, &a, sizeof(a));
  }
  case 0xC2: { // FD_TO_HANDLE
    struct sync_handle a;
    int rc = get_arg(arg, req, &a, sizeof(a));
    if (rc)
      return rc;
    if (a.flags & ~SYNCOBJ_FD_SYNC_FILE)
      return -EINVAL;
    process_t *proc = process_current();
    so_fd_t *s = proc ? so_fd_of(proc, a.fd) : NULL;
    if (!s)
      return -EINVAL;
    struct drm_syncobj *o;
    if (a.flags & SYNCOBJ_FD_SYNC_FILE) {
      o = so_new(); // alias del fence del sync_file
      if (!o)
        return -ENOMEM;
      so_get(s->o);
      o->alias = s->o;
      o->alias_pt = s->pt;
      if (so_signaled(s->o, s->pt))
        o->value = 1;
    } else {
      o = s->o; // mismo objeto compartido
      so_get(o);
    }
    rc = so_handle_alloc(f, o, &a.handle);
    if (rc) {
      so_put(o);
      return rc;
    }
    return put_arg(arg, req, &a, sizeof(a));
  }
  case 0xC3: { // WAIT
    struct sync_wait a;
    int rc = get_arg(arg, req, &a, sizeof(a));
    if (rc)
      return rc;
    if (a.flags & ~(WAIT_ALL | WAIT_FOR_SUBMIT | WAIT_AVAILABLE | WAIT_DEADLINE))
      return -EINVAL;
    uint32_t first;
    rc = do_wait(f, a.handles, 0, a.timeout_nsec, a.count_handles, a.flags, &first);
    if (rc == 0 || rc == -ETIME) {
      a.first_signaled = first;
      int rc2 = put_arg(arg, req, &a, sizeof(a));
      if (rc2)
        return rc2;
    }
    return rc;
  }
  case 0xCA: { // TIMELINE_WAIT
    struct sync_tl_wait a;
    int rc = get_arg(arg, req, &a, sizeof(a));
    if (rc)
      return rc;
    if (a.flags & ~(WAIT_ALL | WAIT_FOR_SUBMIT | WAIT_AVAILABLE | WAIT_DEADLINE))
      return -EINVAL;
    uint32_t first;
    rc = do_wait(f, a.handles, a.points, a.timeout_nsec, a.count_handles, a.flags, &first);
    if (rc == 0 || rc == -ETIME) {
      a.first_signaled = first;
      int rc2 = put_arg(arg, req, &a, sizeof(a));
      if (rc2)
        return rc2;
    }
    return rc;
  }
  case 0xC4:   // RESET
  case 0xC5: { // SIGNAL
    struct sync_array a;
    int rc = get_arg(arg, req, &a, sizeof(a));
    if (rc)
      return rc;
    uint32_t h[MAX_WAIT_HANDLES];
    rc = copy_handles(a.handles, a.count_handles, h, NULL, 0);
    if (rc)
      return rc;
    for (uint32_t i = 0; i < a.count_handles; i++)
      if (!drm_syncobj_find(f, h[i]))
        return -ENOENT;
    for (uint32_t i = 0; i < a.count_handles; i++) {
      struct drm_syncobj *o = drm_syncobj_find(f, h[i]);
      if (nr == 0xC5) {
        drm_syncobj_signal_point(o, 1);
      } else {
        unsigned long fl = spin_lock_irqsave(&g_so_lock);
        o->value = 0;
        spin_unlock_irqrestore(&g_so_lock, fl);
      }
    }
    return 0;
  }
  case 0xCD: { // TIMELINE_SIGNAL
    struct sync_tl_array a;
    int rc = get_arg(arg, req, &a, sizeof(a));
    if (rc)
      return rc;
    uint32_t h[MAX_WAIT_HANDLES];
    uint64_t pts[MAX_WAIT_HANDLES];
    rc = copy_handles(a.handles, a.count_handles, h, pts, a.points);
    if (rc)
      return rc;
    for (uint32_t i = 0; i < a.count_handles; i++)
      if (!drm_syncobj_find(f, h[i]))
        return -ENOENT;
    for (uint32_t i = 0; i < a.count_handles; i++)
      drm_syncobj_signal_point(drm_syncobj_find(f, h[i]), pts[i]);
    return 0;
  }
  case 0xCB: { // QUERY: ultimo punto senalado de cada syncobj
    struct sync_tl_array a;
    int rc = get_arg(arg, req, &a, sizeof(a));
    if (rc)
      return rc;
    uint32_t h[MAX_WAIT_HANDLES];
    uint64_t pts[MAX_WAIT_HANDLES];
    rc = copy_handles(a.handles, a.count_handles, h, NULL, 0);
    if (rc)
      return rc;
    for (uint32_t i = 0; i < a.count_handles; i++) {
      struct drm_syncobj *o = drm_syncobj_find(f, h[i]);
      if (!o)
        return -ENOENT;
      unsigned long fl = spin_lock_irqsave(&g_so_lock);
      pts[i] = o->value;
      spin_unlock_irqrestore(&g_so_lock, fl);
    }
    return drm_put_user(a.points, pts, a.count_handles * sizeof(uint64_t));
  }
  case 0xCC: { // TRANSFER: copia el fence src_point de src a dst_point de dst
    struct sync_transfer a;
    int rc = get_arg(arg, req, &a, sizeof(a));
    if (rc)
      return rc;
    struct drm_syncobj *s = drm_syncobj_find(f, a.src_handle);
    struct drm_syncobj *d = drm_syncobj_find(f, a.dst_handle);
    if (!s || !d)
      return -ENOENT;
    if (a.flags)
      return -EINVAL;
    if (!so_signaled(s, a.src_point))
      return -EAGAIN; // sin soporte de fences futuras compartidas entre objetos
    drm_syncobj_signal_point(d, a.dst_point);
    return 0;
  }
  }
  return -ENOTTY;
}
