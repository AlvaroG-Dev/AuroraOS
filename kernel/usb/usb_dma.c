// kernel/usb/usb_dma.c
#include "usb_dma.h"
#include "paging.h"  // phys_to_virt, PAGE_SIZE
#include "pmm.h"     // pmm_alloc_pages, pmm_free_pages
#include "string.h"  // memset
#include "uaccess.h" // ENOMEM

int usb_dma_alloc(size_t size, usb_dma_buf_t *out) {
  if (!out || size == 0)
    return -EINVAL;

  out->virt = NULL;
  out->phys = 0;
  out->size = 0;

  uint64_t pages = ((uint64_t)size + PAGE_SIZE - 1) / PAGE_SIZE;
  if (pages == 0)
    return -EINVAL;

  uint64_t phys = pmm_alloc_pages(pages);
  if (!phys)
    return -ENOMEM;

  void *virt = phys_to_virt(phys);
  memset(virt, 0, (size_t)pages * PAGE_SIZE);

  out->virt = virt;
  out->phys = phys;
  out->size = size;
  return 0;
}

void usb_dma_free(usb_dma_buf_t *buf) {
  if (!buf || !buf->virt)
    return;
  uint64_t pages = ((uint64_t)buf->size + PAGE_SIZE - 1) / PAGE_SIZE;
  pmm_free_pages(buf->phys, pages);
  buf->virt = NULL;
  buf->phys = 0;
  buf->size = 0;
}

void usb_dma_sync(usb_dma_buf_t *buf, int direction) {
  (void)buf;
  (void)direction;
  // x86_64: DMA coherente. mfence para ordenar escrituras previas.
  __asm__ volatile("mfence" ::: "memory");
}