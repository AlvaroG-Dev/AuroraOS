// kernel/unix_socket.h
//
// Sockets AF_UNIX (SOCK_STREAM / SOCK_DGRAM / SOCK_SEQPACKET) con SCM_RIGHTS
// y SCM_CREDENTIALS. Los usa libwayland (y D-Bus, X11, ...) tal cual.
//
// syscall.c llama a unix_fd_is_socket() al principio de cada syscall de
// sockets y, si el fd es AF_UNIX, delega en las funciones unix_sys_*.

#ifndef KERNEL_UNIX_SOCKET_H
#define KERNEL_UNIX_SOCKET_H

#include <stdbool.h>
#include <stdint.h>

#define AF_UNIX_FAMILY 1

struct process;

bool unix_fd_is_socket(struct process *proc, int fd);

int64_t unix_sys_socket(struct process *proc, uint64_t type, uint64_t proto);
int64_t unix_sys_socketpair(struct process *proc, uint64_t type,
                            uint64_t proto, uint64_t sv_uptr);
int64_t unix_sys_bind(struct process *proc, int fd, uint64_t addr,
                      uint64_t addrlen);
int64_t unix_sys_connect(struct process *proc, int fd, uint64_t addr,
                         uint64_t addrlen);
int64_t unix_sys_listen(struct process *proc, int fd, int backlog);
int64_t unix_sys_accept(struct process *proc, int fd, uint64_t addr,
                        uint64_t addrlen_ptr, uint64_t flags);
int64_t unix_sys_sendmsg(struct process *proc, int fd, uint64_t msg_uptr,
                         uint64_t flags);
int64_t unix_sys_recvmsg(struct process *proc, int fd, uint64_t msg_uptr,
                         uint64_t flags);
int64_t unix_sys_sendto(struct process *proc, int fd, uint64_t buf,
                        uint64_t len, uint64_t flags, uint64_t addr,
                        uint64_t addrlen);
int64_t unix_sys_recvfrom(struct process *proc, int fd, uint64_t buf,
                          uint64_t len, uint64_t flags, uint64_t addr,
                          uint64_t addrlen_ptr);
int64_t unix_sys_getsockname(struct process *proc, int fd, uint64_t addr,
                             uint64_t addrlen_ptr, bool peer);
int64_t unix_sys_shutdown(struct process *proc, int fd, int how);
int64_t unix_sys_setsockopt(struct process *proc, int fd, uint64_t level,
                            uint64_t optname, uint64_t optval,
                            uint64_t optlen);
int64_t unix_sys_getsockopt(struct process *proc, int fd, uint64_t level,
                            uint64_t optname, uint64_t optval,
                            uint64_t optlen_ptr);
// fcntl(F_SETFL): propaga O_NONBLOCK al socket.
void unix_set_nonblock(struct process *proc, int fd, bool nb);

#endif
