/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Wound/wait mutex. Dos algoritmos, segun la clase:
 *  - DEFINE_WD_CLASS (wait-die): un contexto mas joven que ya tiene cerrojos
 *    y choca con uno mas viejo "muere" con -EDEADLK.
 *  - DEFINE_WW_CLASS (wound-wait): un contexto mas viejo "hiere" al mas
 *    joven que tiene el cerrojo; este recibe -EDEADLK en su siguiente
 *    intento (o al despertar si estaba esperando otro cerrojo).
 * El llamante suelta todo, espera con ww_mutex_lock_slow() el cerrojo en
 * disputa y reintenta. -EALREADY si el contexto ya tiene ese cerrojo.
 */
#ifndef _LKPI_LINUX_WW_MUTEX_H
#define _LKPI_LINUX_WW_MUTEX_H

#include <linux/mutex.h>

struct task_struct;

struct ww_class {
	atomic_long_t stamp;
	struct lock_class_key acquire_key;
	struct lock_class_key mutex_key;
	const char *acquire_name;
	const char *mutex_name;
	unsigned int is_wait_die;
};

struct ww_acquire_ctx {
	struct task_struct *task;
	unsigned long stamp;		/* menor = mas viejo */
	unsigned int acquired;
	unsigned short wounded;
	unsigned short is_wait_die;
	struct ww_class *ww_class;
};

struct ww_mutex {
	struct mutex base;
	struct ww_acquire_ctx *ctx;
	struct ww_class *ww_class;
};

#define __WW_CLASS_INITIALIZER(ww_class, _is_wait_die) \
	{ .stamp = ATOMIC_LONG_INIT(0), .acquire_name = #ww_class "_acquire", \
	  .mutex_name = #ww_class "_mutex", .is_wait_die = _is_wait_die }
#define DEFINE_WD_CLASS(classname) \
	struct ww_class classname = __WW_CLASS_INITIALIZER(classname, 1)
#define DEFINE_WW_CLASS(classname) \
	struct ww_class classname = __WW_CLASS_INITIALIZER(classname, 0)
#define DEFINE_WW_MUTEX(mutexname, ww_class) \
	struct ww_mutex mutexname = { .base = __MUTEX_INITIALIZER(mutexname.base), \
				      .ctx = NULL, .ww_class = &(ww_class) }

static inline void ww_mutex_init(struct ww_mutex *lock, struct ww_class *ww_class)
{
	mutex_init(&lock->base);
	lock->ctx = NULL;
	lock->ww_class = ww_class;
}

void lkpi_ww_acquire_init(struct ww_acquire_ctx *ctx, struct ww_class *ww_class);
int lkpi_ww_mutex_lock(struct ww_mutex *lock, struct ww_acquire_ctx *ctx);
int lkpi_ww_mutex_lock_interruptible(struct ww_mutex *lock, struct ww_acquire_ctx *ctx);
void lkpi_ww_mutex_lock_slow(struct ww_mutex *lock, struct ww_acquire_ctx *ctx);
int lkpi_ww_mutex_lock_slow_interruptible(struct ww_mutex *lock, struct ww_acquire_ctx *ctx);
int lkpi_ww_mutex_trylock(struct ww_mutex *lock, struct ww_acquire_ctx *ctx);
void lkpi_ww_mutex_unlock(struct ww_mutex *lock);

#define ww_acquire_init(ctx, cls) lkpi_ww_acquire_init(ctx, cls)
#define ww_acquire_done(ctx) do { (void)(ctx); } while (0)
#define ww_acquire_fini(ctx) do { (void)(ctx); } while (0)
#define ww_mutex_lock(lock, ctx) lkpi_ww_mutex_lock(lock, ctx)
#define ww_mutex_lock_interruptible(lock, ctx) lkpi_ww_mutex_lock_interruptible(lock, ctx)
#define ww_mutex_lock_slow(lock, ctx) lkpi_ww_mutex_lock_slow(lock, ctx)
#define ww_mutex_lock_slow_interruptible(lock, ctx) \
	lkpi_ww_mutex_lock_slow_interruptible(lock, ctx)
#define ww_mutex_trylock(lock, ctx) lkpi_ww_mutex_trylock(lock, ctx)
#define ww_mutex_unlock(lock) lkpi_ww_mutex_unlock(lock)
#define ww_mutex_destroy(lock) mutex_destroy(&(lock)->base)
#define ww_mutex_is_locked(lock) mutex_is_locked(&(lock)->base)

#endif
