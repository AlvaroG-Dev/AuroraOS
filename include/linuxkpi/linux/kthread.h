/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * kthreads sobre los kthread de Aurora. Como en Linux, kthread_create()
 * deja el hilo dormido hasta el primer wake_up_process(); kthread_run() lo
 * crea y lo despierta. kthread_stop() pide parar, despierta, espera a que la
 * funcion vuelva y devuelve su resultado (-EINTR si nunca llego a correr).
 * Un hilo que termina solo, sin kthread_stop(), deja su descriptor sin
 * liberar hasta descargar la capa.
 */
#ifndef _LKPI_LINUX_KTHREAD_H
#define _LKPI_LINUX_KTHREAD_H

#include <linux/err.h>
#include <linux/sched.h>
#include <linux/cpumask.h>

struct task_struct *lkpi_kthread_create(int (*threadfn)(void *data), void *data,
					const char *namefmt, ...) __printf(3, 4);
int lkpi_kthread_stop(struct task_struct *k);
bool lkpi_kthread_should_stop(void);
void *lkpi_kthread_data(struct task_struct *k);

#define kthread_create(threadfn, data, namefmt, arg...) \
	lkpi_kthread_create(threadfn, data, namefmt, ##arg)
#define kthread_create_on_node(threadfn, data, node, namefmt, arg...) \
	lkpi_kthread_create(threadfn, data, namefmt, ##arg)
#define kthread_create_on_cpu(threadfn, data, cpu, namefmt) \
	lkpi_kthread_create(threadfn, data, namefmt, cpu)
#define kthread_run(threadfn, data, namefmt, ...) \
	({ struct task_struct *__k = lkpi_kthread_create(threadfn, data, namefmt, ##__VA_ARGS__); \
	   if (!IS_ERR(__k)) { wake_up_process(__k); } \
	   __k; })
#define kthread_stop(k) lkpi_kthread_stop(k)
#define kthread_stop_put(k) ({ int __r = lkpi_kthread_stop(k); __r; })
#define kthread_should_stop() lkpi_kthread_should_stop()
#define kthread_data(k) lkpi_kthread_data(k)
#define kthread_bind(k, cpu) do { (void)(k); (void)(cpu); } while (0)
#define kthread_bind_mask(k, mask) do { (void)(k); (void)(mask); } while (0)
#define kthread_should_park() false
#define kthread_parkme() do { } while (0)
#define kthread_freezable_should_stop(was_frozen) lkpi_kthread_should_stop()
#define kthread_use_mm(mm) do { (void)(mm); } while (0)
#define kthread_unuse_mm(mm) do { (void)(mm); } while (0)
#define set_freezable() do { } while (0)
#define try_to_freeze() false
#define freezing(p) false

#endif
