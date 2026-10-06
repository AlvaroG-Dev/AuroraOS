// kernel/futex.h
#pragma once
#include <stdint.h>

#define FUTEX_WAIT 0
#define FUTEX_WAKE 1
#define FUTEX_REQUEUE 3
#define FUTEX_CMP_REQUEUE 4
#define FUTEX_WAKE_OP 5
#define FUTEX_WAIT_BITSET 9
#define FUTEX_WAKE_BITSET 10
#define FUTEX_PRIVATE_FLAG 128
#define FUTEX_CLOCK_REALTIME 256

void futex_init(void);
void futex_cleanup_pml4(uint64_t pml4);

// [CLONE_CHILD_CLEARTID] Lo llama process.c al morir un hilo.
void futex_wake_user(uint64_t uaddr, int nr_wake, uint64_t pml4);

int64_t sys_futex(uint64_t uaddr, uint64_t op, uint64_t val,
                  uint64_t timeout_uptr, uint64_t uaddr2, uint64_t val3);