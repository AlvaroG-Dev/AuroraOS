// kernel/pipe.h
#ifndef KERNEL_PIPE_H
#define KERNEL_PIPE_H

#include "vfs.h"

// Tamaño del buffer circular de cada pipe.
#define PIPE_BUF_SIZE 65536

// Crea una pareja de nodos VFS conectados por un pipe.
// El llamante debe hacer vfs_node_free() sobre cada extremo cuando
// termine (o pasarlos a fds de un proceso, que los liberarán).
//
// Errores: -EINVAL, -ENOMEM.
int vfs_pipe_create(vfs_node_t **read_end, vfs_node_t **write_end);

// 1 si el nodo es un extremo de pipe (fcntl F_GETPIPE_SZ).
int vfs_node_is_pipe(const vfs_node_t *n);

#endif