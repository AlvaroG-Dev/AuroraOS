// kernel/irq_user.h
#ifndef KERNEL_IRQ_USER_H
#define KERNEL_IRQ_USER_H

#include "vfs.h"
#include <stdint.h>

// Índice directo por número de IRQ. 0..15 legacy, 0x60..0x7F MSI.
#define IRQ_USER_MAX 0x80

// VFS ops del nodo /dev/irq/<N> (char device).
extern struct vfs_ops aur_irq_node_ops;

// VFS ops del directorio /dev/irq (readdir lista los IRQs disponibles).
extern struct vfs_ops aur_irq_dir_ops;

// Inicializa g_irq_ctxs[] (spinlocks + wqs). Idempotente. Se llama desde
// vfs_init() antes de que nadie pueda abrir /dev/irq/*.
void aur_irq_init(void);

struct aur_irq_ctx;
struct aur_irq_ctx *aur_irq_get_ctx_pub(uint32_t irq);

#endif