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

// La mini-VM mapea las paginas del memfd en su EPT (+1 al crear el slot, -1 al
// soltarlo). Si el nodo se cerrara con alguna aun puesta, no se liberan.
void memfd_vm_hold(struct vfs_node *node, int delta);

// [SysV shm] memfd interno (sin fd de usuario) de `size` bytes; una ref.
struct file_descriptor;
struct file_descriptor *memfd_new_kernel(const char *name, uint64_t size);

#endif