// kernel/nl_uevent.h
//
// AF_NETLINK / NETLINK_KOBJECT_UEVENT minimo: lo que libudev (y por tanto
// libinput, Weston, ...) necesita para crear su "monitor". Los sockets se
// pueden crear, enlazar, configurar y esperar; reciben los mensajes que el
// kernel emite con nl_uevent_broadcast() (hot-plug de input/DRM).

#ifndef KERNEL_NL_UEVENT_H
#define KERNEL_NL_UEVENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AF_NETLINK_FAMILY 16

struct process;

bool nl_fd_is_socket(struct process *proc, int fd);

int64_t nl_sys_socket(struct process *proc, uint64_t type, uint64_t proto);
int64_t nl_sys_bind(struct process *proc, int fd, uint64_t addr,
                    uint64_t addrlen);
int64_t nl_sys_getsockname(struct process *proc, int fd, uint64_t addr,
                           uint64_t addrlen_ptr);
int64_t nl_sys_setsockopt(struct process *proc, int fd, uint64_t level,
                          uint64_t optname, uint64_t optval, uint64_t optlen);
int64_t nl_sys_getsockopt(struct process *proc, int fd, uint64_t level,
                          uint64_t optname, uint64_t optval,
                          uint64_t optlen_ptr);
int64_t nl_sys_sendmsg(struct process *proc, int fd, uint64_t msg_uptr,
                       uint64_t flags);
int64_t nl_sys_recvmsg(struct process *proc, int fd, uint64_t msg_uptr,
                       uint64_t flags);
int64_t nl_sys_sendto(struct process *proc, int fd, uint64_t len);
int64_t nl_sys_recvfrom(struct process *proc, int fd, uint64_t buf,
                        uint64_t len, uint64_t flags);

// Emite un uevent a los sockets suscritos al grupo 1. `msg` es el formato del
// kernel: "add@/devices/...\0ACTION=add\0DEVPATH=...\0SUBSYSTEM=...\0".
void nl_uevent_broadcast(const char *msg, size_t len);

#endif
