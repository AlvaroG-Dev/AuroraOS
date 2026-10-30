// SPDX-License-Identifier: GPL-2.0-only
/* struct pid canonico por numero de pid (ver linux/pid.h). */
#include <linux/export.h>
#include <linux/pid.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

#define LKPI_PID_HASH 64

static DEFINE_SPINLOCK(lkpi_pid_lock);

struct lkpi_pid_node {
	struct pid pid;
	struct lkpi_pid_node *next;
};
static struct lkpi_pid_node *lkpi_pid_nodes[LKPI_PID_HASH];

struct pid *lkpi_pid_of(pid_t nr)
{
	unsigned int h = (unsigned int)nr % LKPI_PID_HASH;
	struct lkpi_pid_node *n, *nn = NULL;
	unsigned long flags;

	spin_lock_irqsave(&lkpi_pid_lock, flags);
	for (n = lkpi_pid_nodes[h]; n; n = n->next)
		if (n->pid.nr == nr)
			goto out;
	spin_unlock_irqrestore(&lkpi_pid_lock, flags);

	nn = kzalloc(sizeof(*nn), GFP_ATOMIC);
	if (!nn)
		return NULL;
	nn->pid.nr = nr;
	refcount_set(&nn->pid.count, 1);	/* la tabla */

	spin_lock_irqsave(&lkpi_pid_lock, flags);
	for (n = lkpi_pid_nodes[h]; n; n = n->next)
		if (n->pid.nr == nr)
			goto out;
	nn->next = lkpi_pid_nodes[h];
	lkpi_pid_nodes[h] = nn;
	n = nn;
	nn = NULL;
out:
	spin_unlock_irqrestore(&lkpi_pid_lock, flags);
	kfree(nn);
	return &n->pid;
}
EXPORT_SYMBOL(lkpi_pid_of);

/* Al descargar la capa: libera los objetos de la tabla. */
void lkpi_pid_exit(void)
{
	int i;

	for (i = 0; i < LKPI_PID_HASH; i++) {
		struct lkpi_pid_node *n = lkpi_pid_nodes[i], *next;

		lkpi_pid_nodes[i] = NULL;
		for (; n; n = next) {
			next = n->next;
			kfree(n);
		}
	}
}
