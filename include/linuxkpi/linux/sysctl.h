/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LKPI_LINUX_SYSCTL_H
#define _LKPI_LINUX_SYSCTL_H
/* Sin /proc/sys: register_sysctl() no registra nada. */
#include <linux/types.h>
struct ctl_table_header;
struct ctl_table {
	const char *procname;
	void *data;
	int maxlen;
	umode_t mode;
	int (*proc_handler)(const struct ctl_table *, int, void *, size_t *, loff_t *);
	void *extra1, *extra2;
};
extern const int lkpi_sysctl_zero, lkpi_sysctl_one;
#define SYSCTL_ZERO	((void *)&lkpi_sysctl_zero)
#define SYSCTL_ONE	((void *)&lkpi_sysctl_one)
static inline int proc_dointvec_minmax(const struct ctl_table *t, int write, void *buf, size_t *len, loff_t *pos)
{
	return 0;
}
static inline struct ctl_table_header *register_sysctl(const char *path, const struct ctl_table *table) { return NULL; }
static inline void unregister_sysctl_table(struct ctl_table_header *header) {}
#endif
