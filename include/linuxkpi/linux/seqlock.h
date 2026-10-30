/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * seqcount y seqlock. Las variantes "asociadas a un cerrojo"
 * (seqcount_spinlock_t, seqcount_mutex_t...) solo sirven a lockdep en
 * Linux: aqui son el mismo seqcount_t.
 */
#ifndef _LKPI_LINUX_SEQLOCK_H
#define _LKPI_LINUX_SEQLOCK_H

#include <linux/compiler.h>
#include <linux/spinlock.h>
#include <linux/preempt.h>
#include <asm/barrier.h>

typedef struct seqcount {
	unsigned int sequence;
} seqcount_t;

typedef seqcount_t seqcount_raw_spinlock_t;
typedef seqcount_t seqcount_spinlock_t;
typedef seqcount_t seqcount_rwlock_t;
typedef seqcount_t seqcount_mutex_t;
typedef seqcount_t seqcount_ww_mutex_t;
typedef seqcount_t seqcount_latch_t;

#define SEQCNT_ZERO(name) { .sequence = 0 }
#define SEQCNT_SPINLOCK_ZERO(name, lock) SEQCNT_ZERO(name)
#define SEQCNT_RAW_SPINLOCK_ZERO(name, lock) SEQCNT_ZERO(name)
#define SEQCNT_MUTEX_ZERO(name, lock) SEQCNT_ZERO(name)
#define SEQCNT_LATCH_ZERO(name) SEQCNT_ZERO(name)

static inline void __lkpi_seqcount_init(seqcount_t *s) { s->sequence = 0; }
#define seqcount_init(s) __lkpi_seqcount_init(s)
#define seqcount_raw_spinlock_init(s, l) __lkpi_seqcount_init(s)
#define seqcount_spinlock_init(s, l) __lkpi_seqcount_init(s)
#define seqcount_rwlock_init(s, l) __lkpi_seqcount_init(s)
#define seqcount_mutex_init(s, l) __lkpi_seqcount_init(s)
#define seqcount_ww_mutex_init(s, l) __lkpi_seqcount_init(s)
#define seqcount_latch_init(s) __lkpi_seqcount_init(s)

/* ------------------------------------------------------------ lectura */

static inline unsigned int __read_seqcount_begin(const seqcount_t *s)
{
	unsigned int seq;
	while ((seq = READ_ONCE(s->sequence)) & 1)
		cpu_relax();
	return seq;
}
static inline unsigned int raw_read_seqcount_begin(const seqcount_t *s)
{
	unsigned int seq = __read_seqcount_begin(s);
	smp_rmb();
	return seq;
}
#define read_seqcount_begin(s) raw_read_seqcount_begin(s)
static inline unsigned int raw_read_seqcount(const seqcount_t *s)
{
	unsigned int seq = READ_ONCE(s->sequence);
	smp_rmb();
	return seq;
}
#define raw_seqcount_begin(s) (raw_read_seqcount(s) & ~1u)
#define raw_seqcount_try_begin(s, start) ({ (start) = raw_read_seqcount(s); !((start) & 1); })
static inline int __read_seqcount_retry(const seqcount_t *s, unsigned int start)
{
	return unlikely(READ_ONCE(s->sequence) != start);
}
static inline int read_seqcount_retry(const seqcount_t *s, unsigned int start)
{
	smp_rmb();
	return __read_seqcount_retry(s, start);
}
#define raw_read_seqcount_retry(s, start) read_seqcount_retry(s, start)
#define raw_read_seqcount_latch(s) raw_read_seqcount(s)
#define raw_read_seqcount_latch_retry(s, start) read_seqcount_retry(s, start)
#define read_seqcount_latch(s) raw_read_seqcount(s)
#define read_seqcount_latch_retry(s, start) read_seqcount_retry(s, start)

/* ---------------------------------------------------------- escritura */

static inline void raw_write_seqcount_begin(seqcount_t *s)
{
	s->sequence++;
	smp_wmb();
}
static inline void raw_write_seqcount_end(seqcount_t *s)
{
	smp_wmb();
	s->sequence++;
}
static inline void write_seqcount_begin(seqcount_t *s)
{
	preempt_disable();
	raw_write_seqcount_begin(s);
}
static inline void write_seqcount_end(seqcount_t *s)
{
	raw_write_seqcount_end(s);
	preempt_enable();
}
#define write_seqcount_begin_nested(s, sub) write_seqcount_begin(s)
static inline void write_seqcount_invalidate(seqcount_t *s)
{
	smp_wmb();
	s->sequence += 2;
}
static inline void raw_write_seqcount_barrier(seqcount_t *s)
{
	s->sequence++;
	smp_wmb();
	s->sequence++;
}
#define write_seqcount_latch_begin(s) raw_write_seqcount_begin(s)
#define write_seqcount_latch(s) raw_write_seqcount_barrier(s)
#define write_seqcount_latch_end(s) raw_write_seqcount_end(s)
#define raw_write_seqcount_latch(s) raw_write_seqcount_barrier(s)

/* ------------------------------------------------------------ seqlock */

typedef struct {
	seqcount_t seqcount;
	spinlock_t lock;
} seqlock_t;

#define __SEQLOCK_UNLOCKED(name) \
	{ .seqcount = SEQCNT_ZERO(name.seqcount), .lock = __SPIN_LOCK_INITIALIZER(name.lock) }
#define DEFINE_SEQLOCK(name) seqlock_t name = __SEQLOCK_UNLOCKED(name)
static inline void seqlock_init(seqlock_t *sl)
{
	seqcount_init(&sl->seqcount);
	spin_lock_init(&sl->lock);
}

static inline unsigned int read_seqbegin(const seqlock_t *sl)
{
	return read_seqcount_begin(&sl->seqcount);
}
static inline unsigned int read_seqretry(const seqlock_t *sl, unsigned int start)
{
	return read_seqcount_retry(&sl->seqcount, start);
}

#define __LKPI_SEQ_WRITE(sfx, lockop, unlockop, ...) \
static inline void write_seqlock##sfx(seqlock_t *sl) \
{ lockop(&sl->lock); raw_write_seqcount_begin(&sl->seqcount); } \
static inline void write_sequnlock##sfx(seqlock_t *sl) \
{ raw_write_seqcount_end(&sl->seqcount); unlockop(&sl->lock); }
__LKPI_SEQ_WRITE(, spin_lock, spin_unlock)
__LKPI_SEQ_WRITE(_bh, spin_lock_bh, spin_unlock_bh)
__LKPI_SEQ_WRITE(_irq, spin_lock_irq, spin_unlock_irq)
#undef __LKPI_SEQ_WRITE

static inline unsigned long __lkpi_write_seqlock_irqsave(seqlock_t *sl)
{
	unsigned long flags;
	spin_lock_irqsave(&sl->lock, flags);
	raw_write_seqcount_begin(&sl->seqcount);
	return flags;
}
#define write_seqlock_irqsave(sl, flags) \
	do { (flags) = __lkpi_write_seqlock_irqsave(sl); } while (0)
static inline void write_sequnlock_irqrestore(seqlock_t *sl, unsigned long flags)
{
	raw_write_seqcount_end(&sl->seqcount);
	spin_unlock_irqrestore(&sl->lock, flags);
}

/* Lector exclusivo (bloquea a los escritores). */
#define read_seqlock_excl(sl) spin_lock(&(sl)->lock)
#define read_sequnlock_excl(sl) spin_unlock(&(sl)->lock)
#define read_seqlock_excl_bh(sl) spin_lock_bh(&(sl)->lock)
#define read_sequnlock_excl_bh(sl) spin_unlock_bh(&(sl)->lock)
#define read_seqlock_excl_irq(sl) spin_lock_irq(&(sl)->lock)
#define read_sequnlock_excl_irq(sl) spin_unlock_irq(&(sl)->lock)
#define read_seqlock_excl_irqsave(sl, flags) spin_lock_irqsave(&(sl)->lock, flags)
#define read_sequnlock_excl_irqrestore(sl, flags) spin_unlock_irqrestore(&(sl)->lock, flags)

/* Primero sin cerrojo; si hay que reintentar (seq impar), con cerrojo. */
static inline void read_seqbegin_or_lock(seqlock_t *sl, int *seq)
{
	if (!(*seq & 1))
		*seq = read_seqbegin(sl);
	else
		read_seqlock_excl(sl);
}
static inline int need_seqretry(seqlock_t *sl, int seq)
{
	return !(seq & 1) && read_seqretry(sl, seq);
}
static inline void done_seqretry(seqlock_t *sl, int seq)
{
	if (seq & 1)
		read_sequnlock_excl(sl);
}

#define seqprop_sequence(s) READ_ONCE((s)->sequence)
#endif
