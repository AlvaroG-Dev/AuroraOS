/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * timer_list. Las funciones de los timers corren en el hilo lkpi_softirqd
 * (contexto "softirq" de la capa), no en la IRQ del tick. Resolucion: la de
 * los plazos de espera de Aurora (el tick, 1 ms): un timer puede llegar uno
 * o dos ticks tarde (mas con la CPU cargada); nunca antes.
 */
#ifndef _LKPI_LINUX_TIMER_H
#define _LKPI_LINUX_TIMER_H

#include <linux/types.h>
#include <linux/list.h>
#include <linux/jiffies.h>
#include <linux/container_of.h>

struct timer_list {
	struct list_head entry;		/* entry.next == NULL: no pendiente */
	unsigned long expires;
	void (*function)(struct timer_list *);
	u32 flags;
};

#define TIMER_CPUMASK 0x0003FFFF
#define TIMER_MIGRATING 0x00040000
#define TIMER_DEFERRABLE 0x00080000
#define TIMER_PINNED 0x00100000
#define TIMER_IRQSAFE 0x00200000
#define TIMER_INIT_FLAGS (TIMER_DEFERRABLE | TIMER_PINNED | TIMER_IRQSAFE)

#define __TIMER_INITIALIZER(_function, _flags) \
	{ .entry = { NULL, NULL }, .function = (_function), .flags = (_flags) }
#define DEFINE_TIMER(_name, _function) \
	struct timer_list _name = __TIMER_INITIALIZER(_function, 0)

static inline void timer_setup(struct timer_list *timer,
			       void (*callback)(struct timer_list *), unsigned int flags)
{
	timer->entry.next = NULL;
	timer->entry.prev = NULL;
	timer->expires = 0;
	timer->function = callback;
	timer->flags = flags;
}
#define timer_setup_on_stack(t, cb, flags) timer_setup(t, cb, flags)
#define destroy_timer_on_stack(t) do { (void)(t); } while (0)
#define timer_container_of(var, callback_timer, timer_fieldname) \
	container_of(callback_timer, typeof(*var), timer_fieldname)
#define from_timer(var, callback_timer, timer_fieldname) \
	timer_container_of(var, callback_timer, timer_fieldname)

static inline int timer_pending(const struct timer_list *timer)
{
	return READ_ONCE(timer->entry.next) != NULL;
}

int lkpi_mod_timer(struct timer_list *timer, unsigned long expires, unsigned int mode);
int lkpi_timer_delete(struct timer_list *timer, bool shutdown);
int lkpi_timer_delete_sync(struct timer_list *timer, bool shutdown);
int lkpi_try_to_del_timer_sync(struct timer_list *timer);

#define __LKPI_MOD_TIMER 0
#define __LKPI_MOD_PENDING_ONLY 1
#define __LKPI_MOD_REDUCE 2
#define mod_timer(t, e) lkpi_mod_timer(t, e, __LKPI_MOD_TIMER)
#define mod_timer_pending(t, e) lkpi_mod_timer(t, e, __LKPI_MOD_PENDING_ONLY)
#define timer_reduce(t, e) lkpi_mod_timer(t, e, __LKPI_MOD_REDUCE)
#define add_timer(t) ((void)lkpi_mod_timer(t, (t)->expires, __LKPI_MOD_TIMER))
#define add_timer_global(t) add_timer(t)
#define add_timer_local(t) add_timer(t)
#define add_timer_on(t, cpu) add_timer(t)
#define timer_delete(t) lkpi_timer_delete(t, false)
#define timer_delete_sync(t) lkpi_timer_delete_sync(t, false)
#define timer_delete_sync_try(t) lkpi_try_to_del_timer_sync(t)
#define timer_shutdown(t) lkpi_timer_delete(t, true)
#define timer_shutdown_sync(t) lkpi_timer_delete_sync(t, true)
#define del_timer(t) timer_delete(t)
#define del_timer_sync(t) timer_delete_sync(t)
#define try_to_del_timer_sync(t) lkpi_try_to_del_timer_sync(t)
#define del_singleshot_timer_sync(t) timer_delete_sync(t)

#endif
