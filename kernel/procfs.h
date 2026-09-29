// kernel/procfs.h
#ifndef KERNEL_PROCFS_H
#define KERNEL_PROCFS_H

// [3.3] procfs: sistema de ficheros virtual con contenido generado
// on-demand. Sin estado persistente, sin inodos, sin fds dedicados.
//
// El contenido se genera en cada lookup, así que cada open/read ve
// valores frescos (uptime, meminfo). El nodo lleva un buffer priv
// con el contenido y node->size = strlen(buffer). ops->read copia
// del buffer; ops->close libera el buffer.

struct vfs_fs_ops;

// Devuelve las vfs_fs_ops de procfs para pasarlas a vfs_mount().
struct vfs_fs_ops *procfs_get_vfs_ops(void);

#endif