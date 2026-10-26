/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * task_struct minimo para K2 (L2): estado de sueno, schedule() y
 * wake_up_process(). Cada tarea de Aurora que pasa por la capa tiene un
 * task_struct propio (lo crea la capa la primera vez que se pide `current`
 * en esa tarea, o kthread_create). El resto (mm, credenciales, senales,
 * pid real) es K8 (L4).
 *
 * Duerme sobre una cola de espera de Aurora propia de cada task_struct
 * ("aparcamiento"): schedule() se aparca solo si __state no es
 * TASK_RUNNING, y wake_up_process() pone TASK_RUNNING y desaparca, ambos
 * bajo el cerrojo de esa cola. Asi no se pierde un despertar entre
 * set_current_state() y schedule().
 */
#ifndef _LKPI_LINUX_SCHED_H
#define _LKPI_LINUX_SCHED_H

#include <linux/types.h>
#include <linux/compiler.h>
#include <linux/atomic.h>
#include <linux/jiffies.h>
#include <linux/preempt.h>
#include <linux/irqflags.h>
#include <linux/string.h>
#include <asm/barrier.h>

#define TASK_RUNNING 0x00000000
#define TASK_INTERRUPTIBLE 0x00000001
#define TASK_UNINTERRUPTIBLE 0x00000002
#define __TASK_STOPPED 0x00000004
#define __TASK_TRACED 0x00000008
#define EXIT_DEAD 0x00000010
#define EXIT_ZOMBIE 0x00000020
#define TASK_PARKED 0x00000040
#define TASK_DEAD 0x00000080
#define TASK_WAKEKILL 0x00000100
#define TASK_WAKING 0x00000200
#define TASK_NOLOAD 0x00000400
#define TASK_NEW 0x00000800
#define TASK_KILLABLE (TASK_WAKEKILL | TASK_UNINTERRUPTIBLE)
#define TASK_STOPPED (TASK_WAKEKILL | __TASK_STOPPED)
#define TASK_IDLE (TASK_UNINTERRUPTIBLE | TASK_NOLOAD)
#define TASK_NORMAL (TASK_INTERRUPTIBLE | TASK_UNINTERRUPTIBLE)
#define TASK_FREEZABLE 0
#define TASK_COMM_LEN 16

#define PF_IDLE 0x00000002
#define PF_EXITING 0x00000004
#define PF_WQ_WORKER 0x00000020
#define PF_MEMALLOC 0x00000800
#define PF_NOFREEZE 0x00008000
#define PF_KTHREAD 0x00200000

struct mm_struct;

struct task_struct {
	unsigned int __state;
	unsigned int flags;
	pid_t pid;
	pid_t tgid;
	char comm[TASK_COMM_LEN];
	struct mm_struct *mm;
	struct mm_struct *active_mm;
	atomic_t __lkpi_usage;		/* referencias (get/put_task_struct) */
	void *__lkpi_atask;		/* task_t de Aurora */
	struct task_struct *__lkpi_hnext; /* cadena de la tabla task_t -> task_struct */
	void *__lkpi_kthread;		/* datos de kthread de la capa (o NULL) */
	unsigned int __lkpi_softirq;	/* > 0: ejecuta bottom halves de la capa */
	u64 __lkpi_park[4] __aligned(8); /* wait_queue_t de Aurora (ver aurora.h) */
};

struct task_struct *lkpi_get_current(void);
void lkpi_schedule(void);
long lkpi_schedule_timeout(long timeout);
int lkpi_wake_up_process(struct task_struct *p);
int lkpi_wake_up_state(struct task_struct *p, unsigned int state);
void lkpi_yield(void);
void lkpi_put_task_struct(struct task_struct *p);

#define current lkpi_get_current()
#define get_current() lkpi_get_current()

#define __set_current_state(s) WRITE_ONCE(current->__state, (s))
#define set_current_state(s) smp_store_mb(current->__state, (s))
#define set_special_state(s) set_current_state(s)
#define get_current_state() READ_ONCE(current->__state)
#define task_is_running(p) (READ_ONCE((p)->__state) == TASK_RUNNING)
#define task_state_index(p) 0

#define schedule() lkpi_schedule()
#define schedule_timeout(t) lkpi_schedule_timeout(t)
#define io_schedule() lkpi_schedule()
#define io_schedule_timeout(t) lkpi_schedule_timeout(t)
#define wake_up_process(p) lkpi_wake_up_process(p)
#define wake_up_state(p, s) lkpi_wake_up_state(p, s)
#define yield() lkpi_yield()
/* El kernel de Aurora es expulsable por el tick: cond_resched() no hace nada. */
#define cond_resched() ({ 0; })
#define cond_resched_lock(l) ({ (void)(l); 0; })
#define need_resched() 0
#define schedule_preempt_disabled() \
	do { preempt_enable(); schedule(); preempt_disable(); } while (0)

static inline long schedule_timeout_interruptible(long t)
{
	__set_current_state(TASK_INTERRUPTIBLE);
	return schedule_timeout(t);
}
static inline long schedule_timeout_killable(long t)
{
	__set_current_state(TASK_KILLABLE);
	return schedule_timeout(t);
}
static inline long schedule_timeout_uninterruptible(long t)
{
	__set_current_state(TASK_UNINTERRUPTIBLE);
	return schedule_timeout(t);
}
static inline long schedule_timeout_idle(long t)
{
	__set_current_state(TASK_IDLE);
	return schedule_timeout(t);
}

/* Senales: K8 (L4). Hasta entonces ninguna espera se interrumpe. */
static inline int signal_pending(struct task_struct *p) { (void)p; return 0; }
static inline int fatal_signal_pending(struct task_struct *p) { (void)p; return 0; }
static inline int signal_pending_state(unsigned int state, struct task_struct *p)
{
	(void)state; (void)p;
	return 0;
}

static inline pid_t task_pid_nr(struct task_struct *p) { return p->pid; }
static inline pid_t task_tgid_nr(struct task_struct *p) { return p->tgid; }
static inline pid_t task_pid_vnr(struct task_struct *p) { return p->pid; }
#define task_pid(p) ((void *)(p))
#define get_task_comm(buf, tsk) ({ strscpy(buf, (tsk)->comm, sizeof(buf)); (buf); })
#define __get_task_comm(buf, len, tsk) ({ strscpy(buf, (tsk)->comm, len); (buf); })
#define set_task_comm(tsk, name) strscpy((tsk)->comm, name, TASK_COMM_LEN)
#define task_lock(p) do { (void)(p); } while (0)
#define task_unlock(p) do { (void)(p); } while (0)
#define same_thread_group(a, b) ((a) == (b))
#define task_cpu(p) 0
#define is_idle_task(p) 0
#define sched_set_fifo(p) do { (void)(p); } while (0)
#define sched_set_fifo_low(p) do { (void)(p); } while (0)
#define sched_set_normal(p, nice) do { (void)(p); } while (0)
#define set_user_nice(p, nice) do { (void)(p); } while (0)
#define task_nice(p) 0

#define MAX_RT_PRIO 100
#define MAX_PRIO 140
#define DEFAULT_PRIO 120

static inline struct task_struct *get_task_struct(struct task_struct *p)
{
	atomic_inc(&p->__lkpi_usage);
	return p;
}
#define put_task_struct(p) lkpi_put_task_struct(p)

#endif
