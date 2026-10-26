/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * RCU no expulsable. rcu_read_lock() desactiva la expulsion; el kernel de
 * Aurora cuenta, por CPU, los ticks que llegan con la expulsion activa
 * (sched_rcu_qs_count): cada uno es un estado quiescente. synchronize_rcu()
 * espera a que el contador de todas las CPU en linea avance. call_rcu() y
 * kfree_rcu() agrupan las funciones en el hilo lkpi_rcu.
 */
#ifndef _LKPI_LINUX_RCUPDATE_H
#define _LKPI_LINUX_RCUPDATE_H

#include <linux/types.h>
#include <linux/compiler.h>
#include <linux/preempt.h>
#include <linux/build_bug.h>
#include <linux/stddef.h>
#include <linux/cleanup.h>
#include <asm/barrier.h>

typedef void (*rcu_callback_t)(struct rcu_head *head);

void lkpi_synchronize_rcu(void);
void lkpi_call_rcu(struct rcu_head *head, rcu_callback_t func);
void lkpi_rcu_barrier(void);
void lkpi_kvfree_call_rcu(struct rcu_head *head, void *ptr);
void lkpi_kvfree_rcu_mightsleep(void *ptr);
unsigned long lkpi_get_state_synchronize_rcu(void);
bool lkpi_poll_state_synchronize_rcu(unsigned long oldstate);

#define rcu_read_lock() preempt_disable()
#define rcu_read_unlock() preempt_enable()
#define rcu_read_lock_bh() preempt_disable()
#define rcu_read_unlock_bh() preempt_enable()
#define rcu_read_lock_sched() preempt_disable()
#define rcu_read_unlock_sched() preempt_enable()
#define rcu_read_lock_sched_notrace() preempt_disable()
#define rcu_read_unlock_sched_notrace() preempt_enable()
#define rcu_read_lock_held() 1
#define rcu_read_lock_bh_held() 1
#define rcu_read_lock_sched_held() 1
#define rcu_read_lock_any_held() 1
#define rcu_lockdep_current_cpu_online() 1
#define RCU_LOCKDEP_WARN(c, s) do { } while (0)
#define rcu_sleep_check() do { } while (0)
#define rcu_momentary_eqs() do { } while (0)

#define rcu_dereference_raw(p) READ_ONCE(p)
#define rcu_dereference_check(p, c) READ_ONCE(p)
#define rcu_dereference_bh_check(p, c) READ_ONCE(p)
#define rcu_dereference_sched_check(p, c) READ_ONCE(p)
#define rcu_dereference(p) READ_ONCE(p)
#define rcu_dereference_bh(p) READ_ONCE(p)
#define rcu_dereference_sched(p) READ_ONCE(p)
#define rcu_dereference_protected(p, c) (p)
#define rcu_dereference_raw_check(p) READ_ONCE(p)
#define rcu_access_pointer(p) READ_ONCE(p)
#define rcu_pointer_handoff(p) (p)
#define unrcu_pointer(p) (p)
#define RCU_INITIALIZER(v) (v)
#define RCU_INIT_POINTER(p, v) WRITE_ONCE(p, v)
#define RCU_POINTER_INITIALIZER(p, v) .p = (v)
#define rcu_assign_pointer(p, v) smp_store_release(&(p), (typeof(p))(v))
#define rcu_replace_pointer(rcu_ptr, ptr, c) \
	({ typeof(ptr) __tmp = rcu_dereference_protected((rcu_ptr), (c)); \
	   rcu_assign_pointer((rcu_ptr), (ptr)); __tmp; })

#define synchronize_rcu() lkpi_synchronize_rcu()
#define synchronize_rcu_expedited() lkpi_synchronize_rcu()
#define synchronize_net() lkpi_synchronize_rcu()
#define synchronize_rcu_bh() lkpi_synchronize_rcu()
#define synchronize_sched() lkpi_synchronize_rcu()
#define call_rcu(head, func) lkpi_call_rcu(head, func)
#define call_rcu_hurry(head, func) lkpi_call_rcu(head, func)
#define rcu_barrier() lkpi_rcu_barrier()
#define get_state_synchronize_rcu() lkpi_get_state_synchronize_rcu()
#define start_poll_synchronize_rcu() lkpi_get_state_synchronize_rcu()
#define poll_state_synchronize_rcu(s) lkpi_poll_state_synchronize_rcu(s)
#define cond_synchronize_rcu(s) \
	do { if (!lkpi_poll_state_synchronize_rcu(s)) lkpi_synchronize_rcu(); } while (0)
#define init_rcu_head(h) do { (void)(h); } while (0)
#define destroy_rcu_head(h) do { (void)(h); } while (0)
#define init_rcu_head_on_stack(h) do { (void)(h); } while (0)
#define destroy_rcu_head_on_stack(h) do { (void)(h); } while (0)

/*
 * kfree_rcu(ptr, campo) / kvfree_rcu(ptr, campo): la cabeza rcu esta dentro
 * del objeto; el desplazamiento (< 4096) se guarda en lugar de la funcion,
 * como en Linux. kfree_rcu(ptr) con un argumento espera un periodo de gracia
 * en el acto (puede dormir).
 */
#define __lkpi_kvfree_rcu2(ptr, rhf) \
	do { \
		typeof(ptr) ___p = (ptr); \
		BUILD_BUG_ON(offsetof(typeof(*(ptr)), rhf) >= 4096); \
		if (___p) \
			lkpi_kvfree_call_rcu(&((___p)->rhf), (void *)(___p)); \
	} while (0)
#define __lkpi_kvfree_rcu1(ptr) lkpi_kvfree_rcu_mightsleep((void *)(ptr))
#define __lkpi_kvfree_rcu_sel(_1, _2, NAME, ...) NAME
#define kfree_rcu(...) \
	__lkpi_kvfree_rcu_sel(__VA_ARGS__, __lkpi_kvfree_rcu2, __lkpi_kvfree_rcu1)(__VA_ARGS__)
#define kvfree_rcu(...) kfree_rcu(__VA_ARGS__)
#define kfree_rcu_mightsleep(ptr) __lkpi_kvfree_rcu1(ptr)
#define kvfree_rcu_mightsleep(ptr) __lkpi_kvfree_rcu1(ptr)

DEFINE_LOCK_GUARD_0(rcu, rcu_read_lock(), rcu_read_unlock())

#endif
