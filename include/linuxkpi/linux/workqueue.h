/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Workqueues con la semantica de Linux, escritas en la capa:
 *  - queue_work() devuelve false solo si el trabajo ya esta pendiente; si
 *    se esta ejecutando, vuelve a ejecutarse despues.
 *  - Un mismo work_struct nunca corre a la vez en dos hilos.
 *  - max_active por cola (1 en las ordenadas, que ejecutan en orden FIFO).
 *  - flush_work() espera a la ultima instancia encolada al llamar; no
 *    espera a otros trabajos.
 * Los trabajos de todas las colas comparten un grupo de hilos lkpi_wq/N que
 * crece cuando todos estan ocupados (un trabajo que duerme no bloquea a
 * los demas) y encoge tras un rato inactivo.
 */
#ifndef _LKPI_LINUX_WORKQUEUE_H
#define _LKPI_LINUX_WORKQUEUE_H

#include <linux/workqueue_types.h>
#include <linux/timer.h>
#include <linux/bitops.h>
#include <linux/rcupdate.h>

struct delayed_work {
	struct work_struct work;
	struct timer_list timer;
	struct workqueue_struct *wq;
	int cpu;
};

struct rcu_work {
	struct work_struct work;
	struct rcu_head rcu;
	struct workqueue_struct *wq;
};

enum {
	WQ_BH = 1 << 0,
	WQ_UNBOUND = 1 << 1,
	WQ_FREEZABLE = 1 << 2,
	WQ_MEM_RECLAIM = 1 << 3,
	WQ_HIGHPRI = 1 << 4,
	WQ_CPU_INTENSIVE = 1 << 5,
	WQ_SYSFS = 1 << 6,
	WQ_POWER_EFFICIENT = 1 << 7,
	WQ_PERCPU = 1 << 8,
	__WQ_DESTROYING = 1 << 15,
	__WQ_DRAINING = 1 << 16,
	__WQ_ORDERED = 1 << 17,
	__WQ_LEGACY = 1 << 18,
	WQ_MAX_ACTIVE = 2048,
	WQ_UNBOUND_MAX_ACTIVE = WQ_MAX_ACTIVE,
	WQ_DFL_ACTIVE = WQ_MAX_ACTIVE / 2,
	WQ_DFL_MIN_ACTIVE = 8,
};

#define WORK_CPU_UNBOUND 64

extern struct workqueue_struct *lkpi_system_wq;
extern struct workqueue_struct *lkpi_system_highpri_wq;
extern struct workqueue_struct *lkpi_system_long_wq;
extern struct workqueue_struct *lkpi_system_unbound_wq;
extern struct workqueue_struct *lkpi_system_freezable_wq;
extern struct workqueue_struct *lkpi_system_power_efficient_wq;
extern struct workqueue_struct *lkpi_system_freezable_power_efficient_wq;
#define system_wq lkpi_system_wq
#define system_percpu_wq lkpi_system_wq
#define system_highpri_wq lkpi_system_highpri_wq
#define system_long_wq lkpi_system_long_wq
#define system_unbound_wq lkpi_system_unbound_wq
#define system_dfl_wq lkpi_system_unbound_wq
#define system_freezable_wq lkpi_system_freezable_wq
#define system_power_efficient_wq lkpi_system_power_efficient_wq
#define system_freezable_power_efficient_wq lkpi_system_freezable_power_efficient_wq
#define system_bh_wq lkpi_system_highpri_wq
#define system_bh_highpri_wq lkpi_system_highpri_wq

void lkpi_timer_delayed_work_fn(struct timer_list *t);

static inline void __lkpi_init_work(struct work_struct *w, work_func_t f)
{
	atomic_long_set(&w->data, 0);
	INIT_LIST_HEAD(&w->entry);
	w->func = f;
	w->__lkpi_wq = NULL;
	w->__lkpi_seq = 0;
	w->__lkpi_flags = 0;
}
#define INIT_WORK(w, f) __lkpi_init_work((w), (f))
#define INIT_WORK_ONSTACK(w, f) __lkpi_init_work((w), (f))
#define PREPARE_WORK(w, f) do { (w)->func = (f); } while (0)
#define destroy_work_on_stack(w) do { (void)(w); } while (0)
#define destroy_delayed_work_on_stack(w) do { (void)(w); } while (0)

#define __INIT_DELAYED_WORK(dw, f, tflags) \
	do { \
		__lkpi_init_work(&(dw)->work, (f)); \
		timer_setup(&(dw)->timer, lkpi_timer_delayed_work_fn, (tflags)); \
		(dw)->wq = NULL; \
		(dw)->cpu = WORK_CPU_UNBOUND; \
	} while (0)
#define INIT_DELAYED_WORK(dw, f) __INIT_DELAYED_WORK(dw, f, 0)
#define INIT_DELAYED_WORK_ONSTACK(dw, f) __INIT_DELAYED_WORK(dw, f, 0)
#define INIT_DEFERRABLE_WORK(dw, f) __INIT_DELAYED_WORK(dw, f, TIMER_DEFERRABLE)
#define INIT_RCU_WORK(rw, f) INIT_WORK(&(rw)->work, (f))

#define __DELAYED_WORK_INITIALIZER(n, f, tflags) \
	{ .work = __WORK_INITIALIZER((n).work, (f)), \
	  .timer = __TIMER_INITIALIZER(lkpi_timer_delayed_work_fn, (tflags)), \
	  .cpu = WORK_CPU_UNBOUND }
#define DECLARE_DELAYED_WORK(n, f) struct delayed_work n = __DELAYED_WORK_INITIALIZER(n, f, 0)
#define DECLARE_DEFERRABLE_WORK(n, f) \
	struct delayed_work n = __DELAYED_WORK_INITIALIZER(n, f, TIMER_DEFERRABLE)

#define work_data_bits(w) ((unsigned long *)(&(w)->data))
#define work_pending(w) test_bit(WORK_STRUCT_PENDING_BIT, work_data_bits(w))
#define delayed_work_pending(dw) work_pending(&(dw)->work)
static inline struct delayed_work *to_delayed_work(struct work_struct *work)
{
	return container_of(work, struct delayed_work, work);
}
static inline struct rcu_work *to_rcu_work(struct work_struct *work)
{
	return container_of(work, struct rcu_work, work);
}

struct workqueue_struct *lkpi_alloc_workqueue(const char *fmt, unsigned int flags,
					      int max_active, ...) __printf(1, 4);
void lkpi_destroy_workqueue(struct workqueue_struct *wq);
void lkpi_flush_workqueue(struct workqueue_struct *wq);
void lkpi_drain_workqueue(struct workqueue_struct *wq);
bool lkpi_queue_work(struct workqueue_struct *wq, struct work_struct *work);
bool lkpi_queue_delayed_work(struct workqueue_struct *wq, struct delayed_work *dw,
			     unsigned long delay);
bool lkpi_mod_delayed_work(struct workqueue_struct *wq, struct delayed_work *dw,
			   unsigned long delay);
bool lkpi_queue_rcu_work(struct workqueue_struct *wq, struct rcu_work *rw);
bool lkpi_flush_work(struct work_struct *work);
bool lkpi_flush_delayed_work(struct delayed_work *dw);
bool lkpi_flush_rcu_work(struct rcu_work *rw);
bool lkpi_cancel_work(struct work_struct *work);
bool lkpi_cancel_work_sync(struct work_struct *work);
bool lkpi_cancel_delayed_work(struct delayed_work *dw);
bool lkpi_cancel_delayed_work_sync(struct delayed_work *dw);
bool lkpi_workqueue_congested(int cpu, struct workqueue_struct *wq);
struct work_struct *lkpi_current_work(void);
void lkpi_workqueue_set_max_active(struct workqueue_struct *wq, int max_active);

#define alloc_workqueue(fmt, flags, max_active, args...) \
	lkpi_alloc_workqueue(fmt, flags, max_active, ##args)
#define alloc_ordered_workqueue(fmt, flags, args...) \
	lkpi_alloc_workqueue(fmt, WQ_UNBOUND | __WQ_ORDERED | (flags), 1, ##args)
#define create_workqueue(name) alloc_workqueue("%s", __WQ_LEGACY | WQ_MEM_RECLAIM, 1, (name))
#define create_freezable_workqueue(name) \
	alloc_workqueue("%s", __WQ_LEGACY | WQ_FREEZABLE | WQ_UNBOUND | WQ_MEM_RECLAIM, 1, (name))
#define create_singlethread_workqueue(name) alloc_ordered_workqueue("%s", WQ_MEM_RECLAIM, name)
#define destroy_workqueue(wq) lkpi_destroy_workqueue(wq)
#define flush_workqueue(wq) lkpi_flush_workqueue(wq)
#define drain_workqueue(wq) lkpi_drain_workqueue(wq)
#define workqueue_set_max_active(wq, n) lkpi_workqueue_set_max_active(wq, n)
#define workqueue_congested(cpu, wq) lkpi_workqueue_congested(cpu, wq)
#define current_work() lkpi_current_work()
#define current_is_workqueue_rescuer() false

#define queue_work(wq, w) lkpi_queue_work(wq, w)
#define queue_work_on(cpu, wq, w) lkpi_queue_work(wq, w)
#define queue_work_node(node, wq, w) lkpi_queue_work(wq, w)
#define queue_delayed_work(wq, dw, d) lkpi_queue_delayed_work(wq, dw, d)
#define queue_delayed_work_on(cpu, wq, dw, d) lkpi_queue_delayed_work(wq, dw, d)
#define mod_delayed_work(wq, dw, d) lkpi_mod_delayed_work(wq, dw, d)
#define mod_delayed_work_on(cpu, wq, dw, d) lkpi_mod_delayed_work(wq, dw, d)
#define queue_rcu_work(wq, rw) lkpi_queue_rcu_work(wq, rw)
#define schedule_work(w) lkpi_queue_work(system_wq, w)
#define schedule_work_on(cpu, w) lkpi_queue_work(system_wq, w)
#define schedule_delayed_work(dw, d) lkpi_queue_delayed_work(system_wq, dw, d)
#define schedule_delayed_work_on(cpu, dw, d) lkpi_queue_delayed_work(system_wq, dw, d)
#define flush_scheduled_work() lkpi_flush_workqueue(system_wq)
#define flush_work(w) lkpi_flush_work(w)
#define flush_delayed_work(dw) lkpi_flush_delayed_work(dw)
#define flush_rcu_work(rw) lkpi_flush_rcu_work(rw)
#define cancel_work(w) lkpi_cancel_work(w)
#define cancel_work_sync(w) lkpi_cancel_work_sync(w)
#define cancel_delayed_work(dw) lkpi_cancel_delayed_work(dw)
#define cancel_delayed_work_sync(dw) lkpi_cancel_delayed_work_sync(dw)
#define disable_work_sync(w) lkpi_cancel_work_sync(w)
#define disable_delayed_work_sync(dw) lkpi_cancel_delayed_work_sync(dw)

#endif
