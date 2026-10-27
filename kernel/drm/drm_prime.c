// kernel/drm/drm_prime.c
//
// PRIME / dma-buf (DRM_CAP_PRIME = import | export): un objeto GEM se exporta
// como un fichero anonimo ("dmabuf") que mantiene viva una referencia al GEM.
// El fd se puede pasar por SCM_RIGHTS (Wayland linux-dmabuf, Mesa/GBM), mapear
// con mmap (vfs_ops.get_page) y volver a importar en otro fichero DRM con
// PRIME_FD_TO_HANDLE: ambos manejan las mismas paginas fisicas (sin copia).

#include "../heap.h"
#include "../klog.h"
#include "../pmm.h"
#include "../process.h"
#include "../string.h"
#include "../uaccess.h"
#include "../vfs.h"
#include "drm.h"

#define DRM_CLOEXEC 0x80000 // O_CLOEXEC
#define DRM_RDWR 0x2        // O_RDWR

struct drm_prime_handle {
  uint32_t handle;
  uint32_t flags;
  int32_t fd;
};

typedef struct dmabuf {
  drm_gem_t *gem;
  struct drm_syncobj *resv; // fence implicito (senalado = sin trabajo pendiente)
} dmabuf_t;

static int dmabuf_get_page(vfs_node_t *node, uint64_t pgoff, uint64_t *phys) {
  dmabuf_t *d = (dmabuf_t *)node->priv;
  if (!d || !phys || pgoff >= d->gem->npages)
    return -EFAULT;
  *phys = d->gem->pages[pgoff];
  return 0;
}

static int dmabuf_close(vfs_node_t *node) {
  dmabuf_t *d = (dmabuf_t *)node->priv;
  if (!d)
    return 0;
  drm_gem_put(d->gem);
  drm_resv_put(d->resv);
  kfree(d);
  node->priv = NULL;
  return 0;
}

// Listo para leer/escribir cuando el fence implicito esta senalado.
static int dmabuf_poll(vfs_node_t *node, short events) {
  dmabuf_t *d = (dmabuf_t *)node->priv;
  if (d && !drm_resv_signaled(d->resv))
    return 0;
  return events & (1 | 4);
}

static wait_queue_t *dmabuf_poll_wq(vfs_node_t *node) {
  (void)node;
  return drm_syncobj_wq();
}

// DMA_BUF_IOCTL_SYNC / SET_NAME: el acceso a la memoria es coherente.
struct dmabuf_sync_file { uint32_t flags; int32_t fd; };
#define DMA_BUF_SYNC_READ (1u << 0)
#define DMA_BUF_SYNC_WRITE (2u << 0)

static int64_t dmabuf_ioctl(vfs_node_t *node, unsigned long req, uint64_t arg) {
  dmabuf_t *d = (dmabuf_t *)node->priv;
  if (((req >> 8) & 0xFF) != 'b')
    return -ENOTTY;
  switch (req & 0xFF) {
  case 0: // DMA_BUF_IOCTL_SYNC: el acceso a la memoria es coherente
  case 1: // SET_NAME_A / SET_NAME_B
    return 0;
  case 2: { // EXPORT_SYNC_FILE: sync_file del fence implicito
    struct dmabuf_sync_file a;
    int rc = drm_get_user(arg, &a, sizeof(a));
    if (rc)
      return rc;
    if (!d || !a.flags || (a.flags & ~(DMA_BUF_SYNC_READ | DMA_BUF_SYNC_WRITE)))
      return -EINVAL;
    int fdn;
    rc = drm_resv_export_sync_file(d->resv, &fdn);
    if (rc)
      return rc;
    a.fd = fdn;
    return drm_put_user(arg, &a, sizeof(a));
  }
  case 3: { // IMPORT_SYNC_FILE: el fence del sync_file pasa a ser el implicito
    struct dmabuf_sync_file a;
    int rc = drm_get_user(arg, &a, sizeof(a));
    if (rc)
      return rc;
    if (!d || !a.flags || (a.flags & ~(DMA_BUF_SYNC_READ | DMA_BUF_SYNC_WRITE)))
      return -EINVAL;
    rc = drm_resv_import_sync_file(&d->resv, a.fd);
    if (rc == 0)
      wake_up_all(drm_syncobj_wq());
    return rc;
  }
  }
  return -ENOTTY;
}

static vfs_ops_t dmabuf_ops = {
    .get_page = dmabuf_get_page,
    .close = dmabuf_close,
    .poll = dmabuf_poll,
    .poll_wq = dmabuf_poll_wq,
    .ioctl = dmabuf_ioctl,
};

static dmabuf_t *dmabuf_of_fd(process_t *proc, int fd) {
  if (fd < 0 || fd >= MAX_PROCESS_FDS || !proc->fds[fd] || !proc->fds[fd]->node)
    return NULL;
  vfs_node_t *n = proc->fds[fd]->node;
  return n->ops == &dmabuf_ops ? (dmabuf_t *)n->priv : NULL;
}

int drm_ioctl_prime_handle_to_fd(drm_file_t *f, uint64_t arg) {
  struct drm_prime_handle a;
  int rc = drm_get_user(arg, &a, sizeof(a));
  if (rc)
    return rc;
  if (a.flags & ~(uint32_t)(DRM_CLOEXEC | DRM_RDWR))
    return -EINVAL;
  drm_gem_t *g = drm_handle_lookup(f, a.handle);
  if (!g)
    return -ENOENT;
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

  dmabuf_t *d = (dmabuf_t *)kzalloc(sizeof(*d));
  vfs_node_t *vn = (vfs_node_t *)kzalloc(sizeof(*vn));
  file_descriptor_t *fd = (file_descriptor_t *)kzalloc(sizeof(*fd));
  if (!d || !vn || !fd) {
    kfree(d);
    kfree(vn);
    kfree(fd);
    return -ENOMEM;
  }
  d->resv = drm_resv_new();
  if (!d->resv) {
    kfree(d);
    kfree(vn);
    kfree(fd);
    return -ENOMEM;
  }
  drm_gem_get(g);
  g->mapped = true; // las paginas no se liberan hasta soltar todas las refs
  d->gem = g;
  strncpy(vn->name, "dmabuf", sizeof(vn->name) - 1);
  vn->flags = VFS_FILE;
  vn->ops = &dmabuf_ops;
  vn->priv = d;
  vn->size = g->size;
  vn->mode = S_IFREG | 0600;
  vn->ref_count = 1;
  fd->node = vn;
  fd->flags = O_RDWR;
  fd->ref_count = 1;
  mutex_init(&fd->lock);
  wait_queue_init(&fd->read_wq);
  wait_queue_init(&fd->write_wq);
  proc->fds[fdn] = fd;
  if (a.flags & DRM_CLOEXEC)
    fd_cloexec_set(proc, fdn);
  a.fd = fdn;
  rc = drm_put_user(arg, &a, sizeof(a));
  if (rc) { // el usuario no vera el fd: se cierra con el proceso
    return rc;
  }
  return 0;
}

int drm_ioctl_prime_fd_to_handle(drm_file_t *f, uint64_t arg) {
  struct drm_prime_handle a;
  int rc = drm_get_user(arg, &a, sizeof(a));
  if (rc)
    return rc;
  process_t *proc = process_current();
  if (!proc)
    return -EFAULT;
  dmabuf_t *d = dmabuf_of_fd(proc, a.fd);
  if (!d)
    return -EINVAL;
  drm_gem_t *g = d->gem;
  // Mismo GEM ya presente en este fichero: se devuelve su handle.
  for (int i = 0; i < DRM_MAX_HANDLES; i++)
    if (f->handles[i] == g) {
      a.handle = (uint32_t)i + 1;
      return drm_put_user(arg, &a, sizeof(a));
    }
  uint32_t h;
  rc = drm_handle_alloc(f, g, &h);
  if (rc)
    return rc;
  drm_gem_get(g);
  f->imported[h - 1] = 1;
  a.handle = h;
  rc = drm_put_user(arg, &a, sizeof(a));
  if (rc)
    drm_handle_close(f, h);
  return rc;
}

// ---------------------------------------------------------------------------
// [LinuxKPI L4] dma_buf de la capa LinuxKPI <-> PRIME de Aurora.
//
// Un dma_buf exportado por un driver de Linux es un dmabuf de Aurora sobre un
// GEM con paginas ajenas (drm_gem_new_ext): PRIME_FD_TO_HANDLE, mmap y
// SCM_RIGHTS funcionan igual que con un dumb buffer. En sentido contrario, la
// capa lee las paginas de cualquier dmabuf de Aurora (dmabuf_node_pages).
// ---------------------------------------------------------------------------
vfs_node_t *dmabuf_node_new_ext(uint32_t npages, const uint64_t *pages,
                                void (*release)(void *), void *priv) {
  dmabuf_t *d = (dmabuf_t *)kzalloc(sizeof(*d));
  if (!d)
    return NULL;
  d->resv = drm_resv_new();
  d->gem = d->resv ? drm_gem_new_ext(npages, pages, release, priv) : NULL;
  if (!d->gem) {
    drm_resv_put(d->resv);
    kfree(d);
    return NULL;
  }
  vfs_node_t *vn = vfs_node_alloc("dmabuf", VFS_FILE, S_IFREG | 0600, 0,
                                  &dmabuf_ops, d, d->gem->size);
  if (!vn) {
    d->gem->ext_release = NULL; // el llamante sigue siendo dueño de todo
    d->gem->npages = 0;
    drm_gem_put(d->gem);
    drm_resv_put(d->resv);
    kfree(d);
    return NULL;
  }
  return vn;
}

// Paginas de un dmabuf (estables mientras el nodo viva). -EINVAL si `n` no es
// un dmabuf.
int dmabuf_node_pages(vfs_node_t *n, uint32_t *npages, const uint64_t **pages) {
  if (!n || n->ops != &dmabuf_ops || !n->priv)
    return -EINVAL;
  dmabuf_t *d = (dmabuf_t *)n->priv;
  *npages = d->gem->npages;
  *pages = d->gem->pages;
  return 0;
}

// priv del exportador si `n` es un dmabuf creado por dmabuf_node_new_ext con
// esa funcion de liberacion; si no, NULL.
void *dmabuf_node_ext_priv(vfs_node_t *n, void (*release)(void *)) {
  if (!n || n->ops != &dmabuf_ops || !n->priv)
    return NULL;
  dmabuf_t *d = (dmabuf_t *)n->priv;
  return d->gem->ext_release == release ? d->gem->ext_priv : NULL;
}
