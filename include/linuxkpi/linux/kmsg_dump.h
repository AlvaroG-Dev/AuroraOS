/* SPDX-License-Identifier: GPL-2.0-only */
/* kmsg_dump (volcado del log en un panic): la capa solo declara el tipo. */
#ifndef _LKPI_LINUX_KMSG_DUMP_H
#define _LKPI_LINUX_KMSG_DUMP_H

#include <linux/types.h>
#include <linux/list.h>

enum kmsg_dump_reason {
	KMSG_DUMP_UNDEF,
	KMSG_DUMP_PANIC,
	KMSG_DUMP_OOPS,
	KMSG_DUMP_EMERG,
	KMSG_DUMP_SHUTDOWN,
	KMSG_DUMP_MAX
};

struct kmsg_dump_detail {
	enum kmsg_dump_reason reason;
	const char *description;
};

struct kmsg_dumper {
	struct list_head list;
	void (*dump)(struct kmsg_dumper *dumper, struct kmsg_dump_detail *detail);
	enum kmsg_dump_reason max_reason;
	bool registered;
};

static inline int kmsg_dump_register(struct kmsg_dumper *dumper) { return -EINVAL; }
static inline int kmsg_dump_unregister(struct kmsg_dumper *dumper) { return -EINVAL; }

#endif
