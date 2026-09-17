#include "spinlock.h"

void spin_init(spinlock_t *lock) {
    lock->locked = 0;
}

unsigned long spin_lock_irqsave(spinlock_t *lock) {
    unsigned long flags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) : : "memory");
    // Acquire lock (xchg)
    while (__sync_lock_test_and_set(&lock->locked, 1)) {
        // spin
        while (lock->locked) {
            __asm__ volatile("pause");
        }
    }
    return flags;
}

void spin_unlock_irqrestore(spinlock_t *lock, unsigned long flags) {
    __sync_lock_release(&lock->locked);
    __asm__ volatile("push %0; popfq" : : "r"(flags) : "memory");
}
