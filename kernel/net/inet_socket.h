// kernel/net/inet_socket.h
//
// Wrapper VFS para exponer un socket_t como fd de proceso.

#ifndef KERNEL_NET_INET_SOCKET_H
#define KERNEL_NET_INET_SOCKET_H

#include <stdint.h>

struct process;
struct vfs_node;

// Crea un socket y lo mete en proc->fds[] con un vnode propio.
// Devuelve el fd (>=0) o un errno negativo.
// Los flags SOCK_CLOEXEC / SOCK_NONBLOCK del argumento `type` se
// aplican al fd y al socket respectivamente.
int64_t inet_socket_create(struct process *proc, int type, int protocol);

// Busca el socket_t asociado a un fd, o NULL si no es un socket.
struct socket *inet_socket_from_fd(struct process *proc, int fd);

#endif