/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * poll() de los drivers (K8, L4). El puente de ficheros de la capa llama a
 * ->poll con un poll_table que, la primera vez que ve cada wait_queue_head,
 * cuelga de ella una entrada que despierta la cola de Aurora del fichero (la
 * que usan poll/select/epoll de Aurora). Las entradas viven hasta el release.
 */
#ifndef _LKPI_LINUX_POLL_H
#define _LKPI_LINUX_POLL_H

#include <linux/types.h>
#include <linux/wait.h>
#include <linux/fs.h>
#include <uapi/linux/eventpoll.h>

#define POLLIN 0x0001
#define POLLPRI 0x0002
#define POLLOUT 0x0004
#define POLLERR 0x0008
#define POLLHUP 0x0010
#define POLLNVAL 0x0020
#define POLLRDNORM 0x0040
#define POLLRDBAND 0x0080
#define POLLWRNORM 0x0100
#define POLLWRBAND 0x0200
#define POLLMSG 0x0400
#define POLLRDHUP 0x2000

struct poll_table_struct;
typedef void (*poll_queue_proc)(struct file *, wait_queue_head_t *, struct poll_table_struct *);
typedef struct poll_table_struct {
	poll_queue_proc _qproc;
	__poll_t _key;
} poll_table;

static inline void poll_wait(struct file *filp, wait_queue_head_t *wait_address, poll_table *p)
{
	if (p && p->_qproc && wait_address)
		p->_qproc(filp, wait_address, p);
}
static inline bool poll_does_not_wait(const poll_table *p) { return !p || !p->_qproc; }
static inline __poll_t poll_requested_events(const poll_table *p) { return p ? p->_key : ~0U; }
static inline void init_poll_funcptr(poll_table *pt, poll_queue_proc qproc)
{
	pt->_qproc = qproc;
	pt->_key = ~(__poll_t)0;
}
static inline __poll_t vfs_poll(struct file *file, struct poll_table_struct *pt)
{
	if (!file->f_op->poll)
		return EPOLLIN | EPOLLOUT | EPOLLRDNORM | EPOLLWRNORM;
	return file->f_op->poll(file, pt);
}
#define DEFAULT_POLLMASK (EPOLLIN | EPOLLOUT | EPOLLRDNORM | EPOLLWRNORM)

#endif
