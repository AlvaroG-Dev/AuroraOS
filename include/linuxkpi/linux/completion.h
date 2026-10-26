/* SPDX-License-Identifier: GPL-2.0-only */
/* completion sobre una wait_queue_head de la capa. */
#ifndef _LKPI_LINUX_COMPLETION_H
#define _LKPI_LINUX_COMPLETION_H

#include <linux/wait.h>

struct completion {
	unsigned int done;	/* UINT_MAX tras complete_all() */
	struct wait_queue_head wait;
};

#define COMPLETION_INITIALIZER(work) { 0, __WAIT_QUEUE_HEAD_INITIALIZER((work).wait) }
#define COMPLETION_INITIALIZER_ONSTACK(work) (*({ init_completion(&work); &work; }))
#define DECLARE_COMPLETION(work) struct completion work = COMPLETION_INITIALIZER(work)
#define DECLARE_COMPLETION_ONSTACK(work) struct completion work = COMPLETION_INITIALIZER(work)
#define DECLARE_COMPLETION_ONSTACK_MAP(work, map) DECLARE_COMPLETION_ONSTACK(work)

static inline void init_completion(struct completion *x)
{
	x->done = 0;
	init_waitqueue_head(&x->wait);
}
static inline void reinit_completion(struct completion *x) { x->done = 0; }

void lkpi_complete(struct completion *x);
void lkpi_complete_all(struct completion *x);
long lkpi_wait_for_completion_state_timeout(struct completion *x, unsigned int state,
					    long timeout);
bool lkpi_try_wait_for_completion(struct completion *x);
bool lkpi_completion_done(struct completion *x);

#define complete(x) lkpi_complete(x)
#define complete_all(x) lkpi_complete_all(x)
#define complete_on_stack(x) lkpi_complete(x)
#define try_wait_for_completion(x) lkpi_try_wait_for_completion(x)
#define completion_done(x) lkpi_completion_done(x)

/*
 * Nucleo: devuelve el tiempo restante (>= 1) si se completo, 0 si vencio
 * el plazo y -ERESTARTSYS si una senal interrumpio (estados interrumpibles).
 */
static inline void wait_for_completion(struct completion *x)
{
	lkpi_wait_for_completion_state_timeout(x, TASK_UNINTERRUPTIBLE, MAX_SCHEDULE_TIMEOUT);
}
#define wait_for_completion_io(x) wait_for_completion(x)
static inline unsigned long wait_for_completion_timeout(struct completion *x,
							unsigned long timeout)
{
	return lkpi_wait_for_completion_state_timeout(x, TASK_UNINTERRUPTIBLE, timeout);
}
#define wait_for_completion_io_timeout(x, t) wait_for_completion_timeout(x, t)
static inline int wait_for_completion_interruptible(struct completion *x)
{
	long r = lkpi_wait_for_completion_state_timeout(x, TASK_INTERRUPTIBLE,
							MAX_SCHEDULE_TIMEOUT);
	return r < 0 ? (int)r : 0;
}
static inline long wait_for_completion_interruptible_timeout(struct completion *x,
							     unsigned long timeout)
{
	return lkpi_wait_for_completion_state_timeout(x, TASK_INTERRUPTIBLE, timeout);
}
static inline int wait_for_completion_killable(struct completion *x)
{
	long r = lkpi_wait_for_completion_state_timeout(x, TASK_KILLABLE, MAX_SCHEDULE_TIMEOUT);
	return r < 0 ? (int)r : 0;
}
static inline long wait_for_completion_killable_timeout(struct completion *x,
							unsigned long timeout)
{
	return lkpi_wait_for_completion_state_timeout(x, TASK_KILLABLE, timeout);
}
static inline int wait_for_completion_state(struct completion *x, unsigned int state)
{
	long r = lkpi_wait_for_completion_state_timeout(x, state, MAX_SCHEDULE_TIMEOUT);
	return r < 0 ? (int)r : 0;
}

#endif
