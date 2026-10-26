/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Datos por CPU (basico). Las variables estaticas son un array de NR_CPUS
 * con nombre interno: se usan con per_cpu(var, cpu), this_cpu_read(var),
 * this_cpu_inc(var)... por su nombre (no con this_cpu_ptr(&var)).
 * alloc_percpu(tipo) reserva NR_CPUS copias seguidas: per_cpu_ptr(p, cpu) y
 * this_cpu_ptr(p) avanzan sizeof(*p) por CPU, asi que el puntero debe ser
 * de su tipo (no void *).
 */
#ifndef _LKPI_LINUX_PERCPU_H
#define _LKPI_LINUX_PERCPU_H

#include <linux/types.h>
#include <linux/smp.h>
#include <linux/preempt.h>
#include <linux/gfp.h>

#define __lkpi_pcpu(name) __lkpi_pcpu_##name
#define DEFINE_PER_CPU(type, name) __typeof__(type) __lkpi_pcpu(name)[NR_CPUS]
#define DECLARE_PER_CPU(type, name) extern __typeof__(type) __lkpi_pcpu(name)[NR_CPUS]
#define DEFINE_PER_CPU_ALIGNED(type, name) DEFINE_PER_CPU(type, name) __aligned(64)
#define DECLARE_PER_CPU_ALIGNED(type, name) DECLARE_PER_CPU(type, name)
#define DEFINE_PER_CPU_SHARED_ALIGNED(type, name) DEFINE_PER_CPU_ALIGNED(type, name)
#define DECLARE_PER_CPU_SHARED_ALIGNED(type, name) DECLARE_PER_CPU(type, name)
#define DEFINE_PER_CPU_READ_MOSTLY(type, name) DEFINE_PER_CPU(type, name)
#define DECLARE_PER_CPU_READ_MOSTLY(type, name) DECLARE_PER_CPU(type, name)

#define per_cpu(var, cpu) (__lkpi_pcpu(var)[(cpu)])
#define __lkpi_this(var) per_cpu(var, raw_smp_processor_id())
#define get_cpu_var(var) (*({ preempt_disable(); &__lkpi_this(var); }))
#define put_cpu_var(var) preempt_enable()

/* Operaciones this_cpu_*: atomicas respecto a la expulsion. */
#define __lkpi_this_op(var, op) \
	({ typeof(per_cpu(var, 0)) __r; preempt_disable(); \
	   __r = (__lkpi_this(var) op); preempt_enable(); __r; })
#define this_cpu_read(var) __lkpi_this_op(var, + 0)
#define this_cpu_write(var, v) ((void)__lkpi_this_op(var, = (v)))
#define this_cpu_add(var, v) ((void)__lkpi_this_op(var, += (v)))
#define this_cpu_sub(var, v) ((void)__lkpi_this_op(var, -= (v)))
#define this_cpu_inc(var) this_cpu_add(var, 1)
#define this_cpu_dec(var) this_cpu_sub(var, 1)
#define this_cpu_add_return(var, v) __lkpi_this_op(var, += (v))
#define this_cpu_inc_return(var) this_cpu_add_return(var, 1)
#define this_cpu_dec_return(var) this_cpu_add_return(var, -1)
#define this_cpu_or(var, v) ((void)__lkpi_this_op(var, |= (v)))
#define this_cpu_and(var, v) ((void)__lkpi_this_op(var, &= (v)))
#define __this_cpu_read(var) this_cpu_read(var)
#define __this_cpu_write(var, v) this_cpu_write(var, v)
#define __this_cpu_add(var, v) this_cpu_add(var, v)
#define __this_cpu_sub(var, v) this_cpu_sub(var, v)
#define __this_cpu_inc(var) this_cpu_inc(var)
#define __this_cpu_dec(var) this_cpu_dec(var)
#define __this_cpu_inc_return(var) this_cpu_inc_return(var)
#define __this_cpu_dec_return(var) this_cpu_dec_return(var)
#define raw_cpu_read(var) this_cpu_read(var)
#define raw_cpu_write(var, v) this_cpu_write(var, v)

/* ------------------------------------------------- memoria dinamica */

void __percpu *lkpi_alloc_percpu(size_t size, size_t align, gfp_t gfp);
void lkpi_free_percpu(void __percpu *p);
#define __alloc_percpu(size, align) lkpi_alloc_percpu(size, align, GFP_KERNEL)
#define __alloc_percpu_gfp(size, align, gfp) lkpi_alloc_percpu(size, align, gfp)
#define alloc_percpu(type) \
	((typeof(type) __percpu *)lkpi_alloc_percpu(sizeof(type), __alignof__(type), GFP_KERNEL))
#define alloc_percpu_gfp(type, gfp) \
	((typeof(type) __percpu *)lkpi_alloc_percpu(sizeof(type), __alignof__(type), gfp))
#define free_percpu(p) lkpi_free_percpu(p)

#define per_cpu_ptr(ptr, cpu) \
	((typeof(ptr))((char *)(ptr) + (unsigned long)(cpu) * sizeof(*(ptr))))
#define this_cpu_ptr(ptr) per_cpu_ptr(ptr, raw_smp_processor_id())
#define raw_cpu_ptr(ptr) this_cpu_ptr(ptr)
#define get_cpu_ptr(ptr) ({ preempt_disable(); this_cpu_ptr(ptr); })
#define put_cpu_ptr(ptr) do { (void)(ptr); preempt_enable(); } while (0)

#endif
