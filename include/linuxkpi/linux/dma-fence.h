/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * dma_fence (K7, L5). La disposicion de struct dma_fence y de dma_fence_ops
 * es la de Linux 6.18 (los drivers acceden a sus campos); la implementacion
 * es drivers/dma-buf/dma-fence.c de Linux compilado contra estas cabeceras,
 * con los simbolos exportados renombrados con prefijo lkpi_ (macros objeto).
 * Los fences de Aurora (sync_file, syncobj, fence implicito de los dmabuf) se
 * enlazan en kernel/linuxkpi/dma/fence-aurora.c.
 */
#ifndef _LKPI_LINUX_DMA_FENCE_H
#define _LKPI_LINUX_DMA_FENCE_H

#include <linux/kernel.h>
#include <linux/export.h>
#include <linux/err.h>
#include <linux/wait.h>
#include <linux/list.h>
#include <linux/bitops.h>
#include <linux/kref.h>
#include <linux/sched.h>
#include <linux/printk.h>
#include <linux/rcupdate.h>
#include <linux/timekeeping.h>
#include <linux/ktime.h>
#include <linux/spinlock.h>

struct dma_fence;
struct dma_fence_ops;
struct dma_fence_cb;
struct seq_file;

struct dma_fence {
	spinlock_t *lock;
	const struct dma_fence_ops *ops;
	union {
		struct list_head cb_list;	/* hasta senalarse */
		ktime_t timestamp;		/* senalado */
		struct rcu_head rcu;		/* liberado */
	};
	u64 context;
	u64 seqno;
	unsigned long flags;
	struct kref refcount;
	int error;
};

enum dma_fence_flag_bits {
	DMA_FENCE_FLAG_SEQNO64_BIT,
	DMA_FENCE_FLAG_SIGNALED_BIT,
	DMA_FENCE_FLAG_TIMESTAMP_BIT,
	DMA_FENCE_FLAG_ENABLE_SIGNAL_BIT,
	DMA_FENCE_FLAG_USER_BITS,	/* siempre el ultimo */
};

typedef void (*dma_fence_func_t)(struct dma_fence *fence, struct dma_fence_cb *cb);

struct dma_fence_cb {
	struct list_head node;
	dma_fence_func_t func;
};

struct dma_fence_ops {
	const char *(*get_driver_name)(struct dma_fence *fence);
	const char *(*get_timeline_name)(struct dma_fence *fence);
	bool (*enable_signaling)(struct dma_fence *fence);
	bool (*signaled)(struct dma_fence *fence);
	signed long (*wait)(struct dma_fence *fence, bool intr, signed long timeout);
	void (*release)(struct dma_fence *fence);
	void (*set_deadline)(struct dma_fence *fence, ktime_t deadline);
};

/* Nombres de Linux -> simbolos de la capa */
#define dma_fence_init lkpi_dma_fence_init
#define dma_fence_init64 lkpi_dma_fence_init64
#define dma_fence_release lkpi_dma_fence_release
#define dma_fence_free lkpi_dma_fence_free
#define dma_fence_describe lkpi_dma_fence_describe
#define dma_fence_signal lkpi_dma_fence_signal
#define dma_fence_signal_locked lkpi_dma_fence_signal_locked
#define dma_fence_signal_timestamp lkpi_dma_fence_signal_timestamp
#define dma_fence_signal_timestamp_locked lkpi_dma_fence_signal_timestamp_locked
#define dma_fence_default_wait lkpi_dma_fence_default_wait
#define dma_fence_add_callback lkpi_dma_fence_add_callback
#define dma_fence_remove_callback lkpi_dma_fence_remove_callback
#define dma_fence_enable_sw_signaling lkpi_dma_fence_enable_sw_signaling
#define dma_fence_driver_name lkpi_dma_fence_driver_name
#define dma_fence_timeline_name lkpi_dma_fence_timeline_name
#define dma_fence_get_status lkpi_dma_fence_get_status
#define dma_fence_wait_timeout lkpi_dma_fence_wait_timeout
#define dma_fence_wait_any_timeout lkpi_dma_fence_wait_any_timeout
#define dma_fence_set_deadline lkpi_dma_fence_set_deadline
#define dma_fence_get_stub lkpi_dma_fence_get_stub
#define dma_fence_allocate_private_stub lkpi_dma_fence_allocate_private_stub
#define dma_fence_context_alloc lkpi_dma_fence_context_alloc
#define dma_fence_array_ops lkpi_dma_fence_array_ops
#define dma_fence_chain_ops lkpi_dma_fence_chain_ops
#define dma_fence_begin_signalling lkpi_dma_fence_begin_signalling
#define dma_fence_end_signalling lkpi_dma_fence_end_signalling
#define __dma_fence_might_wait lkpi___dma_fence_might_wait

void dma_fence_init(struct dma_fence *fence, const struct dma_fence_ops *ops,
		    spinlock_t *lock, u64 context, u64 seqno);
void dma_fence_init64(struct dma_fence *fence, const struct dma_fence_ops *ops,
		      spinlock_t *lock, u64 context, u64 seqno);
void dma_fence_release(struct kref *kref);
void dma_fence_free(struct dma_fence *fence);
void dma_fence_describe(struct dma_fence *fence, struct seq_file *seq);

static inline void dma_fence_put(struct dma_fence *fence)
{
	if (fence)
		kref_put(&fence->refcount, dma_fence_release);
}

static inline struct dma_fence *dma_fence_get(struct dma_fence *fence)
{
	if (fence)
		kref_get(&fence->refcount);
	return fence;
}

static inline struct dma_fence *dma_fence_get_rcu(struct dma_fence *fence)
{
	return kref_get_unless_zero(&fence->refcount) ? fence : NULL;
}

static inline struct dma_fence *dma_fence_get_rcu_safe(struct dma_fence __rcu **fencep)
{
	do {
		struct dma_fence *fence = rcu_dereference(*fencep);

		if (!fence)
			return NULL;
		if (!dma_fence_get_rcu(fence))
			continue;
		if (fence == rcu_access_pointer(*fencep))
			return rcu_pointer_handoff(fence);
		dma_fence_put(fence);
	} while (1);
}

/* Anotaciones de lockdep: sin efecto en la capa */
static inline bool dma_fence_begin_signalling(void) { return true; }
static inline void dma_fence_end_signalling(bool cookie) { }
static inline void __dma_fence_might_wait(void) { }

int dma_fence_signal(struct dma_fence *fence);
int dma_fence_signal_locked(struct dma_fence *fence);
int dma_fence_signal_timestamp(struct dma_fence *fence, ktime_t timestamp);
int dma_fence_signal_timestamp_locked(struct dma_fence *fence, ktime_t timestamp);
signed long dma_fence_default_wait(struct dma_fence *fence, bool intr, signed long timeout);
int dma_fence_add_callback(struct dma_fence *fence, struct dma_fence_cb *cb,
			   dma_fence_func_t func);
bool dma_fence_remove_callback(struct dma_fence *fence, struct dma_fence_cb *cb);
void dma_fence_enable_sw_signaling(struct dma_fence *fence);

const char __rcu *dma_fence_driver_name(struct dma_fence *fence);
const char __rcu *dma_fence_timeline_name(struct dma_fence *fence);

static inline bool dma_fence_is_signaled_locked(struct dma_fence *fence)
{
	if (test_bit(DMA_FENCE_FLAG_SIGNALED_BIT, &fence->flags))
		return true;
	if (fence->ops->signaled && fence->ops->signaled(fence)) {
		dma_fence_signal_locked(fence);
		return true;
	}
	return false;
}

static inline bool dma_fence_is_signaled(struct dma_fence *fence)
{
	if (test_bit(DMA_FENCE_FLAG_SIGNALED_BIT, &fence->flags))
		return true;
	if (fence->ops->signaled && fence->ops->signaled(fence)) {
		dma_fence_signal(fence);
		return true;
	}
	return false;
}

static inline bool __dma_fence_is_later(struct dma_fence *fence, u64 f1, u64 f2)
{
	/* Los drivers con seqno de 32 bits se comparan con vuelta */
	if (test_bit(DMA_FENCE_FLAG_SEQNO64_BIT, &fence->flags))
		return f1 > f2;
	return (int)(lower_32_bits(f1) - lower_32_bits(f2)) > 0;
}

static inline bool dma_fence_is_later(struct dma_fence *f1, struct dma_fence *f2)
{
	if (WARN_ON(f1->context != f2->context))
		return false;
	return __dma_fence_is_later(f1, f1->seqno, f2->seqno);
}

static inline bool dma_fence_is_later_or_same(struct dma_fence *f1, struct dma_fence *f2)
{
	return f1 == f2 || dma_fence_is_later(f1, f2);
}

static inline struct dma_fence *dma_fence_later(struct dma_fence *f1, struct dma_fence *f2)
{
	if (WARN_ON(f1->context != f2->context))
		return NULL;
	if (dma_fence_is_later(f1, f2))
		return dma_fence_is_signaled(f1) ? NULL : f1;
	else
		return dma_fence_is_signaled(f2) ? NULL : f2;
}

static inline int dma_fence_get_status_locked(struct dma_fence *fence)
{
	if (dma_fence_is_signaled_locked(fence))
		return fence->error ?: 1;
	return 0;
}

int dma_fence_get_status(struct dma_fence *fence);

static inline void dma_fence_set_error(struct dma_fence *fence, int error)
{
	WARN_ON(test_bit(DMA_FENCE_FLAG_SIGNALED_BIT, &fence->flags));
	WARN_ON(error >= 0 || error < -MAX_ERRNO);
	fence->error = error;
}

static inline ktime_t dma_fence_timestamp(struct dma_fence *fence)
{
	if (WARN_ON(!test_bit(DMA_FENCE_FLAG_SIGNALED_BIT, &fence->flags)))
		return ktime_get();
	while (!test_bit(DMA_FENCE_FLAG_TIMESTAMP_BIT, &fence->flags))
		cpu_relax();
	return fence->timestamp;
}

signed long dma_fence_wait_timeout(struct dma_fence *, bool intr, signed long timeout);
signed long dma_fence_wait_any_timeout(struct dma_fence **fences, uint32_t count, bool intr,
				       signed long timeout, uint32_t *idx);

static inline signed long dma_fence_wait(struct dma_fence *fence, bool intr)
{
	signed long ret = dma_fence_wait_timeout(fence, intr, MAX_SCHEDULE_TIMEOUT);

	/* Con MAX_SCHEDULE_TIMEOUT no hay vencimiento: 0 si fue bien */
	return ret < 0 ? ret : 0;
}

void dma_fence_set_deadline(struct dma_fence *fence, ktime_t deadline);

struct dma_fence *dma_fence_get_stub(void);
struct dma_fence *dma_fence_allocate_private_stub(ktime_t timestamp);
u64 dma_fence_context_alloc(unsigned num);

extern const struct dma_fence_ops dma_fence_array_ops;
extern const struct dma_fence_ops dma_fence_chain_ops;

static inline bool dma_fence_is_array(struct dma_fence *fence)
{
	return fence->ops == &dma_fence_array_ops;
}

static inline bool dma_fence_is_chain(struct dma_fence *fence)
{
	return fence->ops == &dma_fence_chain_ops;
}

static inline bool dma_fence_is_container(struct dma_fence *fence)
{
	return dma_fence_is_array(fence) || dma_fence_is_chain(fence);
}

/*
 * Enlace con los fences de Aurora (kernel/linuxkpi/dma/fence-aurora.c). Un
 * aurora_fence es un syncobj binario del kernel (sync_file, syncobj, fence
 * implicito de los dmabuf de Aurora).
 */
struct aurora_fence;
void lkpi_dma_fence_signalled(struct dma_fence *fence);
/* dma_fence nuevo (una referencia) que se senala cuando lo hace `af`; `af` conserva la suya */
struct dma_fence *lkpi_dma_fence_from_aurora(struct aurora_fence *af);
/* fd de sync_file/syncobj de Aurora del proceso actual -> dma_fence, o NULL */
struct dma_fence *lkpi_dma_fence_from_aurora_fd(int fd);
/* aurora_fence nuevo (una referencia) que se senala cuando lo hace `fence` */
struct aurora_fence *lkpi_dma_fence_to_aurora(struct dma_fence *fence);
/* fd de sync_file de Aurora (el que entienden los ioctl de syncobj); < 0 si falla */
int lkpi_dma_fence_to_aurora_fd(struct dma_fence *fence, bool cloexec);

#endif
