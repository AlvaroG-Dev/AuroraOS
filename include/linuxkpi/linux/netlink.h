#ifndef _LKPI_LINUX_NETLINK_H
#define _LKPI_LINUX_NETLINK_H
#include <linux/types.h>
#include <linux/socket.h>
#include <uapi/linux/netlink.h>
struct sock; struct sk_buff; struct module; struct net;
struct nla_policy;
typedef int (*netlink_filter_fn)(struct sock *dsk, struct sk_buff *skb, void *data);
#define NETLINK_CTX_SIZE 48
struct netlink_callback {
	struct sk_buff *skb;
	const struct nlmsghdr *nlh;
	int (*dump)(struct sk_buff *skb, struct netlink_callback *cb);
	int (*done)(struct netlink_callback *cb);
	void *data;
	struct module *module;
	struct netlink_ext_ack *extack;
	u16 family;
	u16 answer_flags;
	u32 min_dump_alloc;
	unsigned int prev_seq, seq;
	int flags;
	bool strict_check;
	union { u8 ctx[NETLINK_CTX_SIZE]; long args[6]; };
};
struct netlink_dump_control {
	int (*start)(struct netlink_callback *);
	int (*dump)(struct sk_buff *skb, struct netlink_callback *);
	int (*done)(struct netlink_callback *);
	struct netlink_ext_ack *extack;
	void *data;
	struct module *module;
	u32 min_dump_alloc;
	int flags;
};
struct netlink_notify { struct net *net; u32 portid; int protocol; };
struct nlattr;
struct netlink_ext_ack {
	const char *_msg;
	const struct nlattr *bad_attr;
	const struct nla_policy *policy;
	const struct nlattr *miss_nest;
	u16 miss_type;
	u8 cookie[20];
	u8 cookie_len;
};
#define NL_SET_ERR_MSG(extack, msg) do { if (extack) (extack)->_msg = (msg); } while (0)
#define NL_SET_ERR_MSG_MOD(extack, msg) NL_SET_ERR_MSG(extack, msg)
#define NL_SET_ERR_MSG_FMT_MOD(extack, fmt, ...) do { } while (0)
#define NL_SET_ERR_MSG_FMT(extack, fmt, ...) do { } while (0)
#define NL_SET_ERR_MSG_ATTR(extack, attr, msg) NL_SET_ERR_MSG(extack, msg)
struct netlink_skb_parms { struct { u32 uid, gid; } creds; u32 portid; u32 dst_group; u32 flags; struct sock *sk; bool nsid_is_set; int nsid; };
#define NETLINK_CB(skb) (*(struct netlink_skb_parms *)&((skb)->cb))
#define NETLINK_CREDS(skb) (&NETLINK_CB((skb)).creds)
#ifndef NLMSG_GOODSIZE
#define NLMSG_GOODSIZE 3776
#endif
#define NLMSG_DEFAULT_SIZE (NLMSG_GOODSIZE - NLMSG_HDRLEN)
static inline void nl_set_extack_cookie_u64(struct netlink_ext_ack *extack, u64 cookie) { if (extack) { memcpy(extack->cookie, &cookie, sizeof(cookie)); extack->cookie_len = sizeof(cookie); } }
/* Funciones de netlink que usa net/netlink.h del arbol (inline nlmsg_put,
 * nlmsg_new_large, nlmsg_unicast...). Implementadas como stubs en
 * kernel/linuxkpi/net/netlink_stub.c. */
int netlink_unicast(struct sock *ssk, struct sk_buff *skb, __u32 portid, int nonblock);
int netlink_broadcast(struct sock *ssk, struct sk_buff *skb, __u32 portid, __u32 group, gfp_t allocation);
int netlink_broadcast_filtered(struct sock *ssk, struct sk_buff *skb, __u32 portid, __u32 group,
			       gfp_t allocation, netlink_filter_fn filter, void *filter_data);
struct nlmsghdr *__nlmsg_put(struct sk_buff *skb, u32 portid, u32 seq, int type, int len, int flags);
struct sk_buff *netlink_alloc_large_skb(unsigned int size, int broadcast);
#endif
