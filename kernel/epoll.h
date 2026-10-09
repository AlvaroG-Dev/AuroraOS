// kernel/epoll.h
#ifndef KERNEL_EPOLL_H
#define KERNEL_EPOLL_H

#include <stdint.h>

// Linux x86_64, 12 bytes (packed).
struct k_epoll_event {
  uint32_t events;
  uint64_t data;
} __attribute__((packed));

int64_t k_epoll_create(uint64_t size, uint64_t a2, uint64_t a3, uint64_t a4,
                       uint64_t a5);
int64_t k_epoll_create1(uint64_t flags, uint64_t a2, uint64_t a3, uint64_t a4,
                        uint64_t a5);
int64_t k_epoll_ctl(uint64_t epfd, uint64_t op, uint64_t fd,
                    uint64_t event_uptr, uint64_t a5);
int64_t k_epoll_wait(uint64_t epfd, uint64_t events_uptr, uint64_t maxevents,
                     uint64_t timeout, uint64_t a5);
int64_t k_epoll_pwait(uint64_t epfd, uint64_t events_uptr, uint64_t maxevents,
                      uint64_t timeout, uint64_t sigmask_uptr);
int64_t k_epoll_pwait2(uint64_t epfd, uint64_t events_uptr, uint64_t maxevents,
                       uint64_t ts_ptr, uint64_t sigmask_uptr);

int64_t k_eventfd(uint64_t initval, uint64_t a2, uint64_t a3, uint64_t a4,
                  uint64_t a5);
int64_t k_eventfd2(uint64_t initval, uint64_t flags, uint64_t a3, uint64_t a4,
                   uint64_t a5);

#endif