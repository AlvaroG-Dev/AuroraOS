/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * struct pid (L5): un objeto canonico por pid de Aurora, de modo que dos
 * task_tgid() del mismo proceso dan el mismo puntero (DRM compara
 * file->pid con el del proceso actual). Los objetos no se liberan nunca (el
 * numero de pids distintos que piden los drivers es pequeno); get/put solo
 * cuentan. No hay pid_task(): un pid no lleva al task_struct.
 */
#ifndef _LKPI_LINUX_PID_H
#define _LKPI_LINUX_PID_H

#include <linux/types.h>
#include <linux/refcount.h>

enum pid_type {
	PIDTYPE_PID,
	PIDTYPE_TGID,
	PIDTYPE_PGID,
	PIDTYPE_SID,
	PIDTYPE_MAX,
};

struct pid {
	refcount_t count;
	pid_t nr;
};

struct task_struct;
struct pid_namespace;

struct pid *lkpi_pid_of(pid_t nr);

static inline struct pid *get_pid(struct pid *pid)
{
	if (pid)
		refcount_inc(&pid->count);
	return pid;
}

static inline void put_pid(struct pid *pid)
{
	if (pid)
		refcount_dec(&pid->count);
}

static inline pid_t pid_nr(struct pid *pid)
{
	return pid ? pid->nr : 0;
}

static inline pid_t pid_vnr(struct pid *pid)
{
	return pid_nr(pid);
}

static inline pid_t pid_nr_ns(struct pid *pid, struct pid_namespace *ns)
{
	return pid_nr(pid);
}

static inline struct pid *find_get_pid(pid_t nr)
{
	return get_pid(lkpi_pid_of(nr));
}

static inline struct task_struct *pid_task(struct pid *pid, enum pid_type type)
{
	return NULL;
}

#endif
