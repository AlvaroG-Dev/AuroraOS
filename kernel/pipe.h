// kernel/pipe.h
#ifndef KERNEL_PIPE_H
#define KERNEL_PIPE_H

#include "vfs.h"

// Tamaño del buffer circular de cada pipe.
#define PIPE_BUF_SIZE 4096

// Crea una pareja de nodos VFS conectados por un pipe.
// El llamante debe hacer vfs_node_free() sobre cada extremo cuando
// termine (o pasarlos a fds de un proceso, que los liberarán).
//
// Errores: -EINVAL, -ENOMEM.
int vfs_pipe_create(vfs_node_t **read_end, vfs_node_t **write_end);

#endif