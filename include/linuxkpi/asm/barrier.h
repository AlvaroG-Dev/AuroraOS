/* SPDX-License-Identifier: GPL-2.0-only */
/* Barreras de memoria de x86-64 (TSO). */
#ifndef _LKPI_ASM_BARRIER_H
#define _LKPI_ASM_BARRIER_H

#include <linux/compiler.h>

#define mb() __asm__ __volatile__("mfence" ::: "memory")
#define rmb() __asm__ __volatile__("lfence" ::: "memory")
#define wmb() __asm__ __volatile__("sfence" ::: "memory")
#define dma_rmb() barrier()
#define dma_wmb() barrier()
#define dma_mb() mb()
#define smp_mb() __asm__ __volatile__("lock; addl $0,-4(%%rsp)" ::: "memory", "cc")
#define smp_rmb() barrier()
#define smp_wmb() barrier()
#define __smp_mb() smp_mb()
#define smp_store_mb(var, value) do { WRITE_ONCE(var, value); smp_mb(); } while (0)
#define smp_mb__before_atomic() barrier()
#define smp_mb__after_atomic() barrier()
#define smp_mb__after_spinlock() barrier()
#define smp_store_release(p, v) do { barrier(); WRITE_ONCE(*(p), v); } while (0)
#define smp_load_acquire(p) ({ typeof(*(p)) ___p1 = READ_ONCE(*(p)); barrier(); ___p1; })
#define smp_acquire__after_ctrl_dep() smp_rmb()
#define smp_cond_load_relaxed(ptr, cond_expr) \
	({ typeof(ptr) __PTR = (ptr); __unqual_scalar_typeof(*ptr) VAL; \
	   for (;;) { VAL = READ_ONCE(*__PTR); if (cond_expr) break; \
		      __asm__ __volatile__("pause" ::: "memory"); } \
	   (typeof(*ptr))VAL; })
#define smp_cond_load_acquire(ptr, cond_expr) \
	({ __unqual_scalar_typeof(*ptr) _val = smp_cond_load_relaxed(ptr, cond_expr); \
	   barrier(); (typeof(*ptr))_val; })
#define virt_mb() smp_mb()
#define virt_rmb() smp_rmb()
#define virt_wmb() smp_wmb()
#define virt_store_mb(var, value) smp_store_mb(var, value)
#define cpu_relax() __asm__ __volatile__("pause" ::: "memory")

#endif
