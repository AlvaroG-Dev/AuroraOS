/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Mascaras de CPU sobre un bitmap de NR_CPUS (64, el MAX_CPUS de Aurora).
 * Sin hotplug: posibles = presentes = en linea, fijadas al cargar la capa.
 */
#ifndef _LKPI_LINUX_CPUMASK_H
#define _LKPI_LINUX_CPUMASK_H

#include <linux/types.h>
#include <linux/bitmap.h>
#include <linux/bitops.h>
#include <linux/find.h>
#include <linux/gfp_types.h>

#define NR_CPUS CONFIG_NR_CPUS

typedef struct cpumask {
	DECLARE_BITMAP(bits, NR_CPUS);
} cpumask_t;
typedef struct cpumask cpumask_var_t[1];

extern struct cpumask lkpi_cpu_online_mask;
extern unsigned int lkpi_nr_cpu_ids;
#define cpu_online_mask ((const struct cpumask *)&lkpi_cpu_online_mask)
#define cpu_possible_mask cpu_online_mask
#define cpu_present_mask cpu_online_mask
#define cpu_active_mask cpu_online_mask
#define nr_cpu_ids lkpi_nr_cpu_ids
#define nr_cpumask_bits NR_CPUS
#define small_cpumask_bits NR_CPUS
#define large_cpumask_bits NR_CPUS

#define cpumask_bits(m) ((m)->bits)
#define to_cpumask(b) ((struct cpumask *)(b))
#define CPU_MASK_NONE { { 0 } }
#define CPU_MASK_ALL { { ~0UL } }

static inline void cpumask_set_cpu(unsigned int cpu, struct cpumask *m) { set_bit(cpu, m->bits); }
static inline void __cpumask_set_cpu(unsigned int cpu, struct cpumask *m) { __set_bit(cpu, m->bits); }
static inline void cpumask_clear_cpu(unsigned int cpu, struct cpumask *m) { clear_bit(cpu, m->bits); }
static inline void __cpumask_clear_cpu(unsigned int cpu, struct cpumask *m) { __clear_bit(cpu, m->bits); }
static inline bool cpumask_test_cpu(int cpu, const struct cpumask *m) { return test_bit(cpu, m->bits); }
static inline bool cpumask_test_and_set_cpu(int cpu, struct cpumask *m)
{
	return test_and_set_bit(cpu, m->bits);
}
static inline bool cpumask_test_and_clear_cpu(int cpu, struct cpumask *m)
{
	return test_and_clear_bit(cpu, m->bits);
}
static inline void cpumask_clear(struct cpumask *m) { bitmap_zero(m->bits, NR_CPUS); }
static inline void cpumask_setall(struct cpumask *m) { bitmap_fill(m->bits, NR_CPUS); }
static inline void cpumask_copy(struct cpumask *d, const struct cpumask *s)
{
	bitmap_copy(d->bits, s->bits, NR_CPUS);
}
static inline bool cpumask_and(struct cpumask *d, const struct cpumask *a, const struct cpumask *b)
{
	return bitmap_and(d->bits, a->bits, b->bits, NR_CPUS);
}
static inline void cpumask_or(struct cpumask *d, const struct cpumask *a, const struct cpumask *b)
{
	unsigned int i;
	for (i = 0; i < BITS_TO_LONGS(NR_CPUS); i++)
		d->bits[i] = a->bits[i] | b->bits[i];
}
static inline bool cpumask_equal(const struct cpumask *a, const struct cpumask *b)
{
	return bitmap_equal(a->bits, b->bits, NR_CPUS);
}
static inline bool cpumask_empty(const struct cpumask *m) { return bitmap_empty(m->bits, NR_CPUS); }
static inline unsigned int cpumask_weight(const struct cpumask *m)
{
	return bitmap_weight(m->bits, NR_CPUS);
}
static inline unsigned int cpumask_first(const struct cpumask *m)
{
	return find_first_bit(m->bits, NR_CPUS);
}
static inline unsigned int cpumask_next(int n, const struct cpumask *m)
{
	return find_next_bit(m->bits, NR_CPUS, n + 1);
}
static inline unsigned int cpumask_any_but(const struct cpumask *m, unsigned int cpu)
{
	unsigned int i;
	for (i = cpumask_first(m); i < NR_CPUS; i = cpumask_next(i, m))
		if (i != cpu)
			break;
	return i;
}
#define cpumask_any(m) cpumask_first(m)
#define cpumask_of(cpu) (__lkpi_cpumask_of(cpu))
const struct cpumask *__lkpi_cpumask_of(unsigned int cpu);

#define for_each_cpu(cpu, mask) \
	for ((cpu) = cpumask_first(mask); (cpu) < NR_CPUS; (cpu) = cpumask_next((cpu), (mask)))
#define for_each_online_cpu(cpu) for_each_cpu(cpu, cpu_online_mask)
#define for_each_possible_cpu(cpu) for_each_cpu(cpu, cpu_possible_mask)
#define for_each_present_cpu(cpu) for_each_cpu(cpu, cpu_present_mask)

#define num_online_cpus() cpumask_weight(cpu_online_mask)
#define num_possible_cpus() cpumask_weight(cpu_possible_mask)
#define num_present_cpus() cpumask_weight(cpu_present_mask)
#define cpu_online(cpu) cpumask_test_cpu((cpu), cpu_online_mask)
#define cpu_possible(cpu) cpumask_test_cpu((cpu), cpu_possible_mask)
#define cpu_present(cpu) cpumask_test_cpu((cpu), cpu_present_mask)
#define cpu_is_offline(cpu) (!cpu_online(cpu))

static inline bool alloc_cpumask_var(cpumask_var_t *m, gfp_t flags) { (void)m; (void)flags; return true; }
static inline bool zalloc_cpumask_var(cpumask_var_t *m, gfp_t flags)
{
	(void)flags;
	cpumask_clear(*m);
	return true;
}
static inline void free_cpumask_var(cpumask_var_t m) { (void)m; }

#endif
