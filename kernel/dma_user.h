// kernel/dma_user.h
#ifndef KERNEL_DMA_USER_H
#define KERNEL_DMA_USER_H

#include <stdint.h>

// Ioctls de /dev/dma
#define DMA_ALLOC 0x100
#define DMA_SYNC  0x101

// Estructura para DMA_ALLOC.
// El usuario llama ioctl(fd_dma, DMA_ALLOC, &req).
// El kernel devuelve el fd del buffer y rellena req.phys.
struct dma_alloc_req {
  uint64_t size;   // in: tamaño en bytes (múltiplo de 4096, max 4MB)
  uint64_t phys;   // out: dirección física del buffer
  uint32_t flags;  // in: 0 (reservado para coherencia futura)
  uint32_t _pad;
};

// Estructura para DMA_SYNC.
#define DMA_SYNC_TO_DEVICE   0 // CPU escribió, HW leerá (write-back)
#define DMA_SYNC_FROM_DEVICE 1 // HW escribió, CPU leerá (invalidate)

struct dma_sync_req {
  uint64_t offset;
  uint64_t size;
  uint32_t dir;
  uint32_t _pad;
};

// Expuesta para que vfs.c pueda registrar el nodo en /dev
extern struct vfs_ops dma_dev_ops;

#endif // KERNEL_DMA_USER_H