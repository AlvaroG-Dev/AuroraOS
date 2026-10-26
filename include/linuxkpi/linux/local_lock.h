/* SPDX-License-Identifier: GPL-2.0-only */
/* local_lock: sin PREEMPT_RT protege los datos por CPU desactivando la expulsion. */
#ifndef _LKPI_LINUX_LOCAL_LOCK_H
#define _LKPI_LINUX_LOCAL_LOCK_H

#include <linux/preempt.h>
#include <linux/irqflags.h>

typedef struct { } local_lock_t;
typedef struct { } local_trylock_t;
#define INIT_LOCAL_LOCK(name) { }
#define local_lock_init(l) do { (void)(l); } while (0)
#define local_lock(l) do { (void)(l); preempt_disable(); } while (0)
#define local_unlock(l) do { (void)(l); preempt_enable(); } while (0)
#define local_lock_irq(l) do { (void)(l); local_irq_disable(); } while (0)
#define local_unlock_irq(l) do { (void)(l); local_irq_enable(); } while (0)
#define local_lock_irqsave(l, flags) do { (void)(l); local_irq_save(flags); } while (0)
#define local_unlock_irqrestore(l, flags) do { (void)(l); local_irq_restore(flags); } while (0)
#define local_lock_nested_bh(l) do { (void)(l); } while (0)
#define local_unlock_nested_bh(l) do { (void)(l); } while (0)

#endif
