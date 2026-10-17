// kernel/usb/usb_dma.h
#ifndef KERNEL_USB_DMA_H
#define KERNEL_USB_DMA_H

#include <stddef.h>
#include <stdint.h>

// Buffer DMA físicamente contiguo. Alineado a PAGE_SIZE (4 KB), lo que
// cubre de sobra todos los requisitos de alineación de xHCI (TRB 16 B,
// DCBAA 64 B, ERST 64 B, etc.).
typedef struct usb_dma_buf {
  void *virt;    // PHYS_MAP_BASE + phys
  uint64_t phys; // dirección física para el controlador
  size_t size;   // tamaño pedido por el llamante
} usb_dma_buf_t;

// Direcciones para usb_dma_sync. En x86_64 son no-op real; se dejan por
// si en el futuro se soporta una arquitectura con caché no coherente.
#define USB_DMA_TO_DEVICE 0
#define USB_DMA_FROM_DEVICE 1

// Reserva `size` bytes físicamente contiguos vía PMM y los zero-rellena.
// Devuelve 0 si OK, -ENOMEM si no hay páginas.
int usb_dma_alloc(size_t size, usb_dma_buf_t *out);

// Libera el buffer. Deja el struct a cero.
void usb_dma_free(usb_dma_buf_t *buf);

// No-op en x86_64 (caché coherente). Deja un mfence por ordering.
void usb_dma_sync(usb_dma_buf_t *buf, int direction);

#endif