// kernel/timerfd.h
#ifndef KERNEL_TIMERFD_H
#define KERNEL_TIMERFD_H

#include <stdint.h>

// syscalls 283, 286 y 287.
int64_t k_timerfd_create(uint64_t clockid, uint64_t flags, uint64_t a3,
                         uint64_t a4, uint64_t a5);
int64_t k_timerfd_settime(uint64_t fd, uint64_t flags, uint64_t new_uptr,
                          uint64_t old_uptr, uint64_t a5);
int64_t k_timerfd_gettime(uint64_t fd, uint64_t cur_uptr, uint64_t a3,
                          uint64_t a4, uint64_t a5);

// Reloj de pared en ms (definido en syscall.c), para TFD_TIMER_ABSTIME sobre
// CLOCK_REALTIME.
int64_t kernel_realtime_ms(void);

#endif
