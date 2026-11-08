// SPDX-License-Identifier: GPL-2.0
/* init_utsname() de los drivers de Linux (i915, cfg80211, ...). */
#include <linux/export.h>
#include <linux/utsname.h>

struct new_utsname lkpi_init_utsname = {
	.sysname = "Linux",
	.nodename = "aurora",
	.release = "6.18.59-aurora",
	.version = "#1",
	.machine = "x86_64",
};
EXPORT_SYMBOL(lkpi_init_utsname);
