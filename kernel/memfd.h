// kernel/memfd.h
#ifndef KERNEL_MEMFD_H
#define KERNEL_MEMFD_H

#include <stdint.h>

struct vfs_node;

// Syscall 319. name_uptr es un puntero a userland con un string
// NUL-terminado (<= 249 bytes). flags: MFD_CLOEXEC | MFD_ALLOW_SEALING.
int64_t k_memfd_create(uint64_t name_uptr, uint64_t flags, uint64_t a3,
                       uint64_t a4, uint64_t a5);

// Helpers para k_fcntl(F_ADD_SEALS=1033 / F_GET_SEALS=1034).
int memfd_fcntl_add_seals(struct vfs_node *node, uint32_t seals);
int memfd_fcntl_get_seals(struct vfs_node *node, uint32_t *out);

// ¿El nodo es un memfd? (comparación de ops->).
int memfd_is_node(struct vfs_node *node);

#endif