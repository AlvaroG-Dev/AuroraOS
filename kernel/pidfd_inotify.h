// kernel/pidfd_inotify.h
#ifndef KERNEL_PIDFD_INOTIFY_H
#define KERNEL_PIDFD_INOTIFY_H

#include <stdint.h>

// pidfd_open (434)
int64_t k_pidfd_open(uint64_t pid, uint64_t flags, uint64_t a3, uint64_t a4,
                     uint64_t a5);
// Lo llama process_exit (sin process_lock cogido) para despertar pidfds.
void pidfd_notify_exit(void);

// inotify (253, 254, 255, 294). Stub: nunca entrega eventos.
int64_t k_inotify_init(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                       uint64_t a5);
int64_t k_inotify_init1(uint64_t flags, uint64_t a2, uint64_t a3, uint64_t a4,
                        uint64_t a5);
int64_t k_inotify_add_watch(uint64_t fd, uint64_t path_uptr, uint64_t mask,
                            uint64_t a4, uint64_t a5);
int64_t k_inotify_rm_watch(uint64_t fd, uint64_t wd, uint64_t a3, uint64_t a4,
                           uint64_t a5);

#endif
