#pragma once
#include <stdint.h>

typedef struct { volatile uint32_t locked; } spinlock_t;

void spin_init(spinlock_t *lock);
unsigned long spin_lock_irqsave(spinlock_t *lock);
void spin_unlock_irqrestore(spinlock_t *lock, unsigned long flags);
