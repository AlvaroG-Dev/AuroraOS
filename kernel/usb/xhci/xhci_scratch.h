// kernel/usb/xhci/xhci_scratch.h — cálculo puro (sin hardware) para scratchpads
#ifndef KERNEL_USB_XHCI_SCRATCH_H
#define KERNEL_USB_XHCI_SCRATCH_H
#include <stddef.h>
#include <stdint.h>

/* HCSPARAMS2: Max Scratchpad Bufs = Hi (bits 25:21) << 5 | Lo (bits 31:27). */
static inline uint32_t xhci_scratch_count(uint32_t hcsparams2) {
  return (((hcsparams2 >> 21) & 0x1Fu) << 5) | ((hcsparams2 >> 27) & 0x1Fu);
}

/* Registro PAGESIZE (op+0x08): bit n activo = página de 2^(n+12) bytes.
 * El xHC usa el menor tamaño soportado; si el registro es 0, 4 KiB. */
static inline size_t xhci_page_size(uint32_t pagesize_reg) {
  unsigned shift = 12;
  if (!pagesize_reg)
    return (size_t)1 << shift;
  while (!(pagesize_reg & 1u) && shift < 31) {
    pagesize_reg >>= 1;
    shift++;
  }
  return (size_t)1 << shift;
}

/* Desplazamiento necesario para alinear `phys` a `align` (potencia de 2). */
static inline size_t xhci_align_off(uint64_t phys, size_t align) {
  return (size_t)((align - (phys & (align - 1))) & (align - 1));
}
#endif