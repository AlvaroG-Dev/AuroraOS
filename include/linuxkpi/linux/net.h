#ifndef _LKPI_LINUX_NET_H
#define _LKPI_LINUX_NET_H
#include <linux/types.h>
#include <linux/ratelimit.h>
static inline int net_ratelimit(void) { return 1; }
#endif
