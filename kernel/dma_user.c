// kernel/dma_user.c
//
// Implementa /dev/dma para que los drivers .aur puedan usar DMA.
// Flujo:
// 1. open("/dev/dma") -> fd_ctl
// 2. ioctl(fd_ctl, DMA_ALLOC, &req) -> fd_buf, req.phys
// 3. mmap(NULL, size, PROT_READ|PROT_WRITE, MAP_SHARED, fd_buf, 0)
// 4. El usuario escribe datos, llama ioctl(fd_buf, DMA_SYNC, &sync),
//    y le pasa req.phys al hardware.

#include "dma_user.h"
#include "aur.h"
#include "heap.h"
#include "klog.h"
#include "paging.h"
#include "pmm.h"
#include "process.h"
#include "string.h"
#include "vfs.h"
#include "uaccess.h"

#define DMA_MAX_ALLOC  (4 * 1024 * 1024) // 4 MB máximo por buffer
#define DMA_MIN_ALLOC  PAGE_SIZE

typedef struct dma_buffer_ctx {
  uint64_t phys;
  uint64_t size;
  uint64_t npages;
  vfs_node_t *node;
} dma_buffer_ctx_t;

// ---------------------------------------------------------------------------
// Ops del nodo que se devuelve como fd_buf (el buffer en sí).
// ---------------------------------------------------------------------------
static int dma_buf_mmap(vfs_node_t *node, struct process *proc,
                        uint64_t vaddr, uint64_t length,
                        uint64_t prot, uint64_t flags, uint64_t offset) {
  (void)flags;
  dma_buffer_ctx_t *ctx = (dma_buffer_ctx_t *)node->priv;
  if (!ctx || !proc)
    return -EINVAL;

  if (!(proc->aur.caps & AUR_CAP_DMA))
    return -EPERM;

  if (offset >= ctx->size || length > ctx->size - offset)
    return -EINVAL;
  if ((offset & 0xFFFULL) != 0 || (length & 0xFFFULL) != 0)
    return -EINVAL;

  // Mapeo Write-Back (por defecto en x86) para máximo rendimiento.
  // La coherencia con el hardware se garantiza con DMA_SYNC (clflush).
  uint64_t pte_flags = PTE_USER | PTE_NX;
  if (prot & 0x02 /* PROT_WRITE */)
    pte_flags |= PTE_WRITABLE;

  uint64_t *pml4 = (uint64_t *)phys_to_virt(proc->pml4_phys);
  for (uint64_t off = 0; off < length; off += PAGE_SIZE) {
    int rc = paging_map_page_in(pml4, vaddr + off,
                                 ctx->phys + offset + off, pte_flags);
    if (rc != 0) {
      for (uint64_t back = 0; back < off; back += PAGE_SIZE)
        paging_unmap_page_in(pml4, vaddr + back);
      return -ENOMEM;
    }
  }

  LOG_INFO("[DMA] pid=%u mmap buf: phys=0x%lx len=0x%lx -> vaddr=0x%lx",
           proc->pid, (unsigned long)(ctx->phys + offset),
           (unsigned long)length, (unsigned long)vaddr);
  return 0;
}

static int64_t dma_buf_ioctl(vfs_node_t *node, unsigned long req,
                              uint64_t arg) {
  dma_buffer_ctx_t *ctx = (dma_buffer_ctx_t *)node->priv;
  if (!ctx)
    return -EINVAL;

  if (req == DMA_SYNC) {
    struct dma_sync_req sync;
    if (copy_from_user(&sync, (void *)arg, sizeof(sync)) < 0)
      return -EFAULT;

    if (sync.offset > ctx->size || sync.size > ctx->size - sync.offset)
      return -EINVAL;

    // Alinear el rango a líneas de caché (64 bytes)
    uint64_t start = (ctx->phys + sync.offset) & ~(uint64_t)63;
    uint64_t end = (ctx->phys + sync.offset + sync.size + 63) & ~(uint64_t)63;

    // En x86, clflush opera sobre direcciones VIRTUALES. La ventana
    // física en PHYS_MAP_BASE está mapeada con PTE_NOCACHE, pero para
    // hacer el flush necesitamos la dirección virtual del kernel.
    volatile uint8_t *virt = (volatile uint8_t *)phys_to_virt(start);
    uint64_t len = end - start;

    for (uint64_t i = 0; i < len; i += 64) {
      __asm__ volatile("clflush (%0)" : : "r"(virt + i) : "memory");
    }
    
    // Asegurar que las operaciones de clflush se completen antes de retornar
    __asm__ volatile("mfence" ::: "memory");
    return 0;
  }

  return -ENOTTY;
}

static int dma_buf_close(vfs_node_t *node) {
  dma_buffer_ctx_t *ctx = (dma_buffer_ctx_t *)node->priv;
  if (!ctx)
    return 0;

  if (ctx->phys) {
    pmm_free_pages(ctx->phys, ctx->npages);
  }
  kfree(ctx);
  node->priv = NULL;
  return 0;
}

static vfs_ops_t dma_buf_ops = {
    .mmap = dma_buf_mmap,
    .ioctl = dma_buf_ioctl,
    .close = dma_buf_close,
};

// ---------------------------------------------------------------------------
// Ops del nodo de control /dev/dma (el que abre el usuario primero).
// ---------------------------------------------------------------------------
static int dma_dev_open(vfs_node_t *node, int flags) {
  (void)node;
  (void)flags;
  return 0;
}

static int64_t dma_dev_ioctl(vfs_node_t *node, unsigned long req,
                              uint64_t arg) {
  (void)node;
  if (req != DMA_ALLOC)
    return -ENOTTY;

  process_t *proc = process_current();
  if (!proc)
    return -EFAULT;

  if (!(proc->aur.caps & AUR_CAP_DMA))
    return -EPERM;

  struct dma_alloc_req kreq;
  if (copy_from_user(&kreq, (void *)arg, sizeof(kreq)) < 0)
    return -EFAULT;

  if (kreq.size < DMA_MIN_ALLOC || kreq.size > DMA_MAX_ALLOC)
    return -EINVAL;
  if ((kreq.size & 0xFFFULL) != 0)
    return -EINVAL;

  uint64_t npages = kreq.size / PAGE_SIZE;
  uint64_t phys = pmm_alloc_pages(npages);
  if (!phys) {
    LOG_ERR("[DMA] pmm_alloc_pages(%lu) falló", (unsigned long)npages);
    return -ENOMEM;
  }

  // Limpiar el buffer (puede contener basura de asignaciones anteriores)
  memset(phys_to_virt(phys), 0, kreq.size);

  dma_buffer_ctx_t *ctx = (dma_buffer_ctx_t *)kzalloc(sizeof(*ctx));
  if (!ctx) {
    pmm_free_pages(phys, npages);
    return -ENOMEM;
  }
  ctx->phys = phys;
  ctx->size = kreq.size;
  ctx->npages = npages;

  vfs_node_t *vnode = (vfs_node_t *)kzalloc(sizeof(vfs_node_t));
  if (!vnode) {
    pmm_free_pages(phys, npages);
    kfree(ctx);
    return -ENOMEM;
  }

  strcpy(vnode->name, "dma_buf");
  vnode->flags = VFS_FILE;
  vnode->ops = &dma_buf_ops;
  vnode->priv = ctx;
  vnode->size = kreq.size;
  vnode->mode = S_IFREG | 0600;
  vnode->uid = 0;
  vnode->gid = 0;
  vnode->ref_count = 1;
  ctx->node = vnode;

  // Buscar un fd libre en el proceso para el buffer
  int free_fd = -1;
  for (int i = 0; i < MAX_PROCESS_FDS; i++) {
    if (!proc->fds[i]) {
      free_fd = i;
      break;
    }
  }
  if (free_fd < 0) {
    pmm_free_pages(phys, npages);
    kfree(ctx);
    kfree(vnode);
    return -EMFILE;
  }

  file_descriptor_t *fd = (file_descriptor_t *)kzalloc(sizeof(*fd));
  if (!fd) {
    pmm_free_pages(phys, npages);
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

  kreq.phys = phys;
  if (copy_to_user((void *)arg, &kreq, sizeof(kreq)) < 0) {
    vfs_close_for_proc(proc, free_fd);
    return -EFAULT;
  }

  LOG_INFO("[DMA] pid=%u alloc size=0x%lx phys=0x%lx fd=%d",
           proc->pid, (unsigned long)kreq.size,
           (unsigned long)phys, free_fd);
  return free_fd;
}

vfs_ops_t dma_dev_ops = {
    .open = dma_dev_open,
    .ioctl = dma_dev_ioctl,
};