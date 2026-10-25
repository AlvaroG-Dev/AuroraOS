// kernel/memfd.c
//
// memfd_create(2) real: fichero anónimo backed por kmalloc.
//
// Soporta read/write posicionales, ftruncate, mmap file-backed (vía el
// camino estándar de page-in del VFS) y F_ADD_SEALS/F_GET_SEALS, que
// Python usa para marcar bytecode caches como inmutables.

#include "memfd.h"
#include "heap.h"
#include "klog.h"
#include "mutex.h"
#include "paging.h"
#include "pmm.h"
#include "process.h"
#include "spinlock.h"
#include "string.h"
#include "uaccess.h"
#include "vfs.h"
#include "wait.h"
#include <stddef.h>
#include <stdint.h>

#define MFD_CLOEXEC 0x0001U
#define MFD_ALLOW_SEALING 0x0002U

#define F_LINUX_SPECIFIC_BASE 1024
#define F_ADD_SEALS (F_LINUX_SPECIFIC_BASE + 9)  // 1033
#define F_GET_SEALS (F_LINUX_SPECIFIC_BASE + 10) // 1034

#define F_SEAL_SEAL 0x0001U
#define F_SEAL_SHRINK 0x0002U
#define F_SEAL_GROW 0x0004U
#define F_SEAL_WRITE 0x0008U
#define F_SEAL_FUTURE_WRITE 0x0010U

#define MFD_NAME_MAX 249
#define EFBIG_U 27

typedef struct memfd_ctx {
  spinlock_t lock;
  // Directorio de paginas fisicas (una entrada por pagina; 0 = hueco, se lee
  // como ceros y se asigna al escribir o al mapear). Las paginas son estables
  // mientras viva el ctx: asi un mmap MAP_SHARED puede apuntar a ellas.
  uint64_t *pages;
  size_t pages_cap;
  size_t size; // tamaño logico visible al usuario
  uint32_t seals;
  int allow_sealing;
  char name[MFD_NAME_MAX + 1];
  wait_queue_t wq;
} memfd_ctx_t;

#define MFD_MAX_PAGES (1u << 20) // 4 GiB de espacio de direcciones por memfd

// ---------------------------------------------------------------------
// Helpers (ctx->lock tomado)
// ---------------------------------------------------------------------
static int memfd_dir_grow_locked(memfd_ctx_t *ctx, size_t npages) {
  if (npages <= ctx->pages_cap)
    return 0;
  if (npages > MFD_MAX_PAGES)
    return -ENOMEM;
  size_t new_cap = ctx->pages_cap ? ctx->pages_cap : 16;
  while (new_cap < npages)
    new_cap *= 2;
  uint64_t *np = (uint64_t *)krealloc(ctx->pages, new_cap * sizeof(uint64_t));
  if (!np)
    return -ENOMEM;
  memset(np + ctx->pages_cap, 0,
         (new_cap - ctx->pages_cap) * sizeof(uint64_t));
  ctx->pages = np;
  ctx->pages_cap = new_cap;
  return 0;
}

// Devuelve la fisica de la pagina `idx` (0 si es hueco y !alloc, o si falla).
static uint64_t memfd_page_locked(memfd_ctx_t *ctx, size_t idx, int alloc) {
  if (idx >= ctx->pages_cap) {
    if (!alloc || memfd_dir_grow_locked(ctx, idx + 1) != 0)
      return 0;
  }
  if (!ctx->pages[idx] && alloc) {
    uint64_t p = pmm_alloc_page();
    if (!p)
      return 0;
    memset(phys_to_virt(p), 0, PAGE_SIZE);
    ctx->pages[idx] = p;
  }
  return ctx->pages[idx];
}

// ---------------------------------------------------------------------
// Node ops
// ---------------------------------------------------------------------
static int64_t memfd_read_op(vfs_node_t *node, uint64_t off, size_t size,
                             void *buf) {
  memfd_ctx_t *ctx = (memfd_ctx_t *)node->priv;
  if (!ctx || !buf)
    return -EINVAL;

  unsigned long flags = spin_lock_irqsave(&ctx->lock);
  if (off >= ctx->size) {
    spin_unlock_irqrestore(&ctx->lock, flags);
    return 0;
  }
  size_t n = size;
  if (off + n > ctx->size)
    n = ctx->size - (size_t)off;
  size_t done = 0;
  while (done < n) {
    size_t pos = (size_t)off + done;
    size_t in_page = pos & (PAGE_SIZE - 1);
    size_t chunk = PAGE_SIZE - in_page;
    if (chunk > n - done)
      chunk = n - done;
    uint64_t p = memfd_page_locked(ctx, pos / PAGE_SIZE, 0);
    if (p)
      memcpy((uint8_t *)buf + done, (uint8_t *)phys_to_virt(p) + in_page,
             chunk);
    else
      memset((uint8_t *)buf + done, 0, chunk);
    done += chunk;
  }
  spin_unlock_irqrestore(&ctx->lock, flags);
  return (int64_t)n;
}

static int64_t memfd_write_op(vfs_node_t *node, uint64_t off, size_t size,
                              const void *buf) {
  memfd_ctx_t *ctx = (memfd_ctx_t *)node->priv;
  if (!ctx || !buf)
    return -EINVAL;
  if (size == 0)
    return 0;

  unsigned long flags = spin_lock_irqsave(&ctx->lock);

  if (ctx->seals & F_SEAL_WRITE) {
    spin_unlock_irqrestore(&ctx->lock, flags);
    return -EPERM;
  }

  if (off > SIZE_MAX - size) {
    spin_unlock_irqrestore(&ctx->lock, flags);
    return -EINVAL;
  }
  size_t end = (size_t)off + size;

  if (end > ctx->size && (ctx->seals & F_SEAL_GROW)) {
    spin_unlock_irqrestore(&ctx->lock, flags);
    return -EPERM;
  }

  size_t done = 0;
  int64_t rc = 0;
  while (done < size) {
    size_t pos = (size_t)off + done;
    size_t in_page = pos & (PAGE_SIZE - 1);
    size_t chunk = PAGE_SIZE - in_page;
    if (chunk > size - done)
      chunk = size - done;
    uint64_t p = memfd_page_locked(ctx, pos / PAGE_SIZE, 1);
    if (!p) {
      rc = -ENOMEM;
      break;
    }
    memcpy((uint8_t *)phys_to_virt(p) + in_page, (const uint8_t *)buf + done,
           chunk);
    done += chunk;
  }
  // Los huecos al escribir mas alla del final ya son ceros (paginas nuevas
  // o zeradas al encoger).
  if (done > 0 && (size_t)off + done > ctx->size)
    ctx->size = (size_t)off + done;
  node->size = ctx->size;

  spin_unlock_irqrestore(&ctx->lock, flags);
  if (done > 0)
    wake_up_all(&ctx->wq);
  return done > 0 ? (int64_t)done : rc;
}

static int memfd_truncate_op(vfs_node_t *node, uint64_t new_size) {
  memfd_ctx_t *ctx = (memfd_ctx_t *)node->priv;
  if (!ctx)
    return -EINVAL;

  unsigned long flags = spin_lock_irqsave(&ctx->lock);

  if (new_size < ctx->size && (ctx->seals & F_SEAL_SHRINK)) {
    spin_unlock_irqrestore(&ctx->lock, flags);
    return -EPERM;
  }
  if (new_size > ctx->size && (ctx->seals & F_SEAL_GROW)) {
    spin_unlock_irqrestore(&ctx->lock, flags);
    return -EPERM;
  }
  if (new_size > (uint64_t)MFD_MAX_PAGES * PAGE_SIZE) {
    spin_unlock_irqrestore(&ctx->lock, flags);
    return -EFBIG_U;
  }

  if (new_size < ctx->size) {
    // Encoger: se ponen a cero los bytes descartados, pero las paginas se
    // conservan (un mmap compartido podria seguir apuntando a ellas).
    size_t pos = (size_t)new_size;
    while (pos < ctx->size) {
      size_t in_page = pos & (PAGE_SIZE - 1);
      size_t chunk = PAGE_SIZE - in_page;
      if (chunk > ctx->size - pos)
        chunk = ctx->size - pos;
      uint64_t p = memfd_page_locked(ctx, pos / PAGE_SIZE, 0);
      if (p)
        memset((uint8_t *)phys_to_virt(p) + in_page, 0, chunk);
      pos += chunk;
    }
  }

  ctx->size = (size_t)new_size;
  node->size = ctx->size;
  spin_unlock_irqrestore(&ctx->lock, flags);
  wake_up_all(&ctx->wq);
  return 0;
}

// mmap(MAP_SHARED): pagina `pgoff` del fichero (la asigna si es un hueco).
// Pasada el final del fichero -> -EFAULT (SIGBUS en Linux).
static int memfd_get_page_op(vfs_node_t *node, uint64_t pgoff,
                             uint64_t *phys_out) {
  memfd_ctx_t *ctx = (memfd_ctx_t *)node->priv;
  if (!ctx || !phys_out)
    return -EINVAL;
  unsigned long flags = spin_lock_irqsave(&ctx->lock);
  int rc = 0;
  if (pgoff >= (ctx->size + PAGE_SIZE - 1) / PAGE_SIZE) {
    rc = -EFAULT;
  } else {
    uint64_t p = memfd_page_locked(ctx, (size_t)pgoff, 1);
    if (p)
      *phys_out = p;
    else
      rc = -ENOMEM;
  }
  spin_unlock_irqrestore(&ctx->lock, flags);
  return rc;
}

static int memfd_open_op(vfs_node_t *node, int flags) {
  (void)node;
  (void)flags;
  return 0;
}

static int memfd_close_op(vfs_node_t *node) {
  memfd_ctx_t *ctx = (memfd_ctx_t *)node->priv;
  if (!ctx)
    return 0;
  if (ctx->pages) {
    for (size_t i = 0; i < ctx->pages_cap; i++)
      if (ctx->pages[i])
        pmm_free_page(ctx->pages[i]);
    kfree(ctx->pages);
  }
  kfree(ctx);
  node->priv = NULL;
  return 0;
}

static int memfd_poll_op(vfs_node_t *node, short events) {
  (void)node;
  int rev = 0;
  if (events & 1)
    rev |= 1; // POLLIN: siempre listo (o EOF)
  if (events & 4)
    rev |= 4; // POLLOUT: siempre listo
  return rev;
}

static wait_queue_t *memfd_poll_wq_op(vfs_node_t *node) {
  memfd_ctx_t *ctx = (memfd_ctx_t *)node->priv;
  return ctx ? &ctx->wq : NULL;
}

static vfs_ops_t memfd_node_ops = {
    .read = memfd_read_op,
    .write = memfd_write_op,
    .open = memfd_open_op,
    .close = memfd_close_op,
    .truncate = memfd_truncate_op,
    .get_page = memfd_get_page_op,
    .poll = memfd_poll_op,
    .poll_wq = memfd_poll_wq_op,
};

int memfd_is_node(struct vfs_node *node) {
  return node && node->ops == &memfd_node_ops;
}

int memfd_fcntl_add_seals(struct vfs_node *node, uint32_t seals) {
  if (!memfd_is_node(node))
    return -EINVAL;
  memfd_ctx_t *ctx = (memfd_ctx_t *)node->priv;
  if (!ctx)
    return -EINVAL;

  unsigned long flags = spin_lock_irqsave(&ctx->lock);
  if (!ctx->allow_sealing) {
    spin_unlock_irqrestore(&ctx->lock, flags);
    return -EPERM;
  }
  if (ctx->seals & F_SEAL_SEAL) {
    spin_unlock_irqrestore(&ctx->lock, flags);
    return -EPERM;
  }
  ctx->seals |= seals;
  spin_unlock_irqrestore(&ctx->lock, flags);
  return 0;
}

int memfd_fcntl_get_seals(struct vfs_node *node, uint32_t *out) {
  if (!memfd_is_node(node))
    return -EINVAL;
  memfd_ctx_t *ctx = (memfd_ctx_t *)node->priv;
  if (!ctx)
    return -EINVAL;
  unsigned long flags = spin_lock_irqsave(&ctx->lock);
  *out = ctx->seals;
  spin_unlock_irqrestore(&ctx->lock, flags);
  return 0;
}

int64_t k_memfd_create(uint64_t name_uptr, uint64_t flags, uint64_t a3,
                       uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;

  if (flags & ~(uint64_t)(MFD_CLOEXEC | MFD_ALLOW_SEALING))
    return -EINVAL;

  process_t *proc = process_current();
  if (!proc)
    return -EFAULT;

  char name[MFD_NAME_MAX + 1];
  long n = strncpy_from_user(name, (const char *)name_uptr, sizeof(name));
  if (n < 0)
    return -EFAULT;
  if (n == 0)
    name[0] = '\0';

  int free_fd = -1;
  for (int i = 0; i < MAX_PROCESS_FDS; i++) {
    if (!proc->fds[i]) {
      free_fd = i;
      break;
    }
  }
  if (free_fd < 0)
    return -EMFILE;

  memfd_ctx_t *ctx = (memfd_ctx_t *)kzalloc(sizeof(*ctx));
  if (!ctx)
    return -ENOMEM;
  spin_init(&ctx->lock);
  wait_queue_init(&ctx->wq);
  ctx->pages = NULL;
  ctx->pages_cap = 0;
  ctx->size = 0;
  ctx->seals = 0;
  ctx->allow_sealing = (flags & MFD_ALLOW_SEALING) ? 1 : 0;
  size_t nl = strlen(name);
  if (nl > MFD_NAME_MAX)
    nl = MFD_NAME_MAX;
  memcpy(ctx->name, name, nl);
  ctx->name[nl] = '\0';

  vfs_node_t *vnode = (vfs_node_t *)kzalloc(sizeof(vfs_node_t));
  if (!vnode) {
    kfree(ctx);
    return -ENOMEM;
  }
  // Nombre visible: "memfd:<name>" (formato Linux).
  const char *pfx = "memfd:";
  size_t pl = strlen(pfx);
  size_t nm_len = strlen(ctx->name);
  if (pl + nm_len >= sizeof(vnode->name))
    nm_len = sizeof(vnode->name) - pl - 1;
  memcpy(vnode->name, pfx, pl);
  memcpy(vnode->name + pl, ctx->name, nm_len);
  vnode->name[pl + nm_len] = '\0';

  vnode->flags = VFS_FILE;
  vnode->ops = &memfd_node_ops;
  vnode->priv = ctx;
  vnode->size = 0;
  vnode->mode = S_IFREG | 0600;
  vnode->uid = 0;
  vnode->gid = 0;
  vnode->ref_count = 1;

  file_descriptor_t *fd =
      (file_descriptor_t *)kzalloc(sizeof(file_descriptor_t));
  if (!fd) {
    kfree(ctx);
    kfree(vnode);
    return -ENOMEM;
  }
  fd->node = vnode;
  fd->offset = 0;
  fd->flags = O_RDWR;
  fd->ref_count = 1;
  mutex_init(&fd->lock);
  wait_queue_init(&fd->read_wq);
  wait_queue_init(&fd->write_wq);

  proc->fds[free_fd] = fd;
  if (flags & MFD_CLOEXEC)
    fd_cloexec_set(proc, free_fd);

  LOG_DEBUG("[MEMFD] fd=%d name='%s' flags=0x%lx", free_fd, ctx->name,
            (unsigned long)flags);
  return (int64_t)free_fd;
}