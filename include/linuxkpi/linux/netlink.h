#ifndef _LKPI_LINUX_NETLINK_H
#define _LKPI_LINUX_NETLINK_H
#include <linux/types.h>
struct nlattr;
struct netlink_ext_ack {
	const char *_msg;
	const struct nlattr *bad_attr;
	u8 cookie[20];
	u8 cookie_len;
};
#define NL_SET_ERR_MSG(extack, msg) do { if (extack) (extack)->_msg = (msg); } while (0)
#define NL_SET_ERR_MSG_MOD(extack, msg) NL_SET_ERR_MSG(extack, msg)
#define NL_SET_ERR_MSG_FMT_MOD(extack, fmt, ...) do { } while (0)
#define NL_SET_ERR_MSG_FMT(extack, fmt, ...) do { } while (0)
#define NL_SET_ERR_MSG_ATTR(extack, attr, msg) NL_SET_ERR_MSG(extack, msg)
#endif
