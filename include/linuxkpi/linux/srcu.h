/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * SRCU basico: lectores que pueden dormir. Dos contadores de lectores
 * (indice 0/1); synchronize_srcu() cambia el indice y espera a que se vacie
 * el antiguo, dos veces, para cubrir a un lector que leyo el indice justo
 * antes del cambio. Los contadores son globales por srcu_struct (no por
 * CPU): suficiente para el uso que hacen los drivers de GPU.
 */
#ifndef _LKPI_LINUX_SRCU_H
#define _LKPI_LINUX_SRCU_H

#include <linux/mutex.h>
#include <linux/rcupdate.h>
#include <linux/list.h>

struct srcu_struct {
	atomic_long_t c[2];
	unsigned int idx;
	struct mutex gp_mutex;
};

#define __SRCU_STRUCT_INIT(name) \
	{ .c = { ATOMIC_LONG_INIT(0), ATOMIC_LONG_INIT(0) }, .idx = 0, \
	  .gp_mutex = __MUTEX_INITIALIZER(name.gp_mutex) }
#define DEFINE_SRCU(name) struct srcu_struct name = __SRCU_STRUCT_INIT(name)
#define DEFINE_STATIC_SRCU(name) static struct srcu_struct name = __SRCU_STRUCT_INIT(name)

static inline int init_srcu_struct(struct srcu_struct *ssp)
{
	atomic_long_set(&ssp->c[0], 0);
	atomic_long_set(&ssp->c[1], 0);
	ssp->idx = 0;
	mutex_init(&ssp->gp_mutex);
	return 0;
}
void lkpi_cleanup_srcu_struct(struct srcu_struct *ssp);
#define cleanup_srcu_struct(ssp) lkpi_cleanup_srcu_struct(ssp)

static inline int srcu_read_lock(struct srcu_struct *ssp)
{
	int idx = READ_ONCE(ssp->idx) & 1;
	atomic_long_inc(&ssp->c[idx]);
	smp_mb();	/* el contador se ve antes que las lecturas protegidas */
	return idx;
}
static inline void srcu_read_unlock(struct srcu_struct *ssp, int idx)
{
	smp_mb();	/* las lecturas protegidas terminan antes de soltar */
	atomic_long_dec(&ssp->c[idx & 1]);
}
#define srcu_read_lock_nmisafe(ssp) srcu_read_lock(ssp)
#define srcu_read_unlock_nmisafe(ssp, idx) srcu_read_unlock(ssp, idx)
#define srcu_read_lock_held(ssp) 1
#define srcu_dereference(p, ssp) READ_ONCE(p)
#define srcu_dereference_check(p, ssp, c) READ_ONCE(p)
#define srcu_dereference_notrace(p, ssp) READ_ONCE(p)

void lkpi_synchronize_srcu(struct srcu_struct *ssp);
void lkpi_call_srcu(struct srcu_struct *ssp, struct rcu_head *head, rcu_callback_t func);
void lkpi_srcu_barrier(struct srcu_struct *ssp);
#define synchronize_srcu(ssp) lkpi_synchronize_srcu(ssp)
#define synchronize_srcu_expedited(ssp) lkpi_synchronize_srcu(ssp)
#define call_srcu(ssp, head, func) lkpi_call_srcu(ssp, head, func)
#define srcu_barrier(ssp) lkpi_srcu_barrier(ssp)

typedef struct { struct srcu_struct *lock; int idx; } class_srcu_t;
static inline void class_srcu_destructor(class_srcu_t *_T)
{
	if (_T->lock)
		srcu_read_unlock(_T->lock, _T->idx);
}
static inline class_srcu_t class_srcu_constructor(struct srcu_struct *l)
{
	class_srcu_t t = { .lock = l, .idx = srcu_read_lock(l) };
	return t;
}
static inline void *class_srcu_lock_ptr(class_srcu_t *_T) { return _T->lock; }

#endif
