#ifndef _LKPI_LINUX_RTNETLINK_H
#define _LKPI_LINUX_RTNETLINK_H
#include <linux/mutex.h>
void rtnl_lock(void);
void rtnl_unlock(void);
int rtnl_is_locked(void);
#define ASSERT_RTNL() do { } while (0)
#define rtnl_dereference(p) (p)
#endif
