/* SPDX-License-Identifier: GPL-2.0-only */
/* Cadenas de notificacion (atomicas, bloqueantes y "raw"), ordenadas por prioridad. */
#ifndef _LKPI_LINUX_NOTIFIER_H
#define _LKPI_LINUX_NOTIFIER_H

#include <linux/types.h>
#include <linux/spinlock.h>
#include <linux/rwsem.h>
#include <linux/srcu.h>

struct notifier_block;
typedef int (*notifier_fn_t)(struct notifier_block *nb, unsigned long action, void *data);

struct notifier_block {
	notifier_fn_t notifier_call;
	struct notifier_block *next;
	int priority;
};

struct atomic_notifier_head {
	spinlock_t lock;
	struct notifier_block *head;
};
struct blocking_notifier_head {
	struct rw_semaphore rwsem;
	struct notifier_block *head;
};
struct raw_notifier_head {
	struct notifier_block *head;
};

#define ATOMIC_NOTIFIER_INIT(name) { .lock = __SPIN_LOCK_INITIALIZER(name.lock), .head = NULL }
#define BLOCKING_NOTIFIER_INIT(name) { .rwsem = __RWSEM_INITIALIZER((name).rwsem), .head = NULL }
#define RAW_NOTIFIER_INIT(name) { .head = NULL }
#define ATOMIC_NOTIFIER_HEAD(name) struct atomic_notifier_head name = ATOMIC_NOTIFIER_INIT(name)
#define BLOCKING_NOTIFIER_HEAD(name) \
	struct blocking_notifier_head name = BLOCKING_NOTIFIER_INIT(name)
#define RAW_NOTIFIER_HEAD(name) struct raw_notifier_head name = RAW_NOTIFIER_INIT(name)
#define ATOMIC_INIT_NOTIFIER_HEAD(nh) \
	do { spin_lock_init(&(nh)->lock); (nh)->head = NULL; } while (0)
#define BLOCKING_INIT_NOTIFIER_HEAD(nh) \
	do { init_rwsem(&(nh)->rwsem); (nh)->head = NULL; } while (0)
#define RAW_INIT_NOTIFIER_HEAD(nh) do { (nh)->head = NULL; } while (0)

#define NOTIFY_DONE 0x0000
#define NOTIFY_OK 0x0001
#define NOTIFY_STOP_MASK 0x8000
#define NOTIFY_BAD (NOTIFY_STOP_MASK | 0x0002)
#define NOTIFY_STOP (NOTIFY_OK | NOTIFY_STOP_MASK)

static inline int notifier_from_errno(int err)
{
	return err ? (NOTIFY_STOP_MASK | (NOTIFY_OK - err)) : NOTIFY_OK;
}
static inline int notifier_to_errno(int ret)
{
	ret &= ~NOTIFY_STOP_MASK;
	return ret > NOTIFY_OK ? NOTIFY_OK - ret : 0;
}

int lkpi_notifier_chain_register(struct notifier_block **nl, struct notifier_block *n);
int lkpi_notifier_chain_unregister(struct notifier_block **nl, struct notifier_block *n);
int lkpi_notifier_call_chain(struct notifier_block **nl, unsigned long val, void *v);

int lkpi_atomic_notifier_chain_register(struct atomic_notifier_head *nh, struct notifier_block *nb);
int lkpi_atomic_notifier_chain_unregister(struct atomic_notifier_head *nh,
					  struct notifier_block *nb);
int lkpi_atomic_notifier_call_chain(struct atomic_notifier_head *nh, unsigned long val, void *v);
int lkpi_blocking_notifier_chain_register(struct blocking_notifier_head *nh,
					  struct notifier_block *nb);
int lkpi_blocking_notifier_chain_unregister(struct blocking_notifier_head *nh,
					    struct notifier_block *nb);
int lkpi_blocking_notifier_call_chain(struct blocking_notifier_head *nh, unsigned long val, void *v);

#define atomic_notifier_chain_register(nh, nb) lkpi_atomic_notifier_chain_register(nh, nb)
#define atomic_notifier_chain_unregister(nh, nb) lkpi_atomic_notifier_chain_unregister(nh, nb)
#define atomic_notifier_call_chain(nh, val, v) lkpi_atomic_notifier_call_chain(nh, val, v)
#define blocking_notifier_chain_register(nh, nb) lkpi_blocking_notifier_chain_register(nh, nb)
#define blocking_notifier_chain_unregister(nh, nb) lkpi_blocking_notifier_chain_unregister(nh, nb)
#define blocking_notifier_call_chain(nh, val, v) lkpi_blocking_notifier_call_chain(nh, val, v)
#define raw_notifier_chain_register(nh, nb) lkpi_notifier_chain_register(&(nh)->head, nb)
#define raw_notifier_chain_unregister(nh, nb) lkpi_notifier_chain_unregister(&(nh)->head, nb)
#define raw_notifier_call_chain(nh, val, v) lkpi_notifier_call_chain(&(nh)->head, val, v)

#endif
