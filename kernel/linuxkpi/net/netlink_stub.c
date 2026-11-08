// SPDX-License-Identifier: GPL-2.0
/*
 * Netlink: AuroraOS no lo tiene. Estas funciones existen para que el codigo
 * de Linux que arma mensajes (cfg80211: pmsr, notificaciones) enlace; todo
 * falla de forma limpia (-EMSGSIZE/-EOPNOTSUPP). Plan_Red.md N7.
 */
#include <linux/kernel.h>
#include <linux/export.h>
#include <linux/slab.h>
#include <linux/skbuff.h>
#include <linux/device.h>
#include <linux/netlink.h>
#include <linux/device/faux.h>
#include <net/netlink.h>
#include "../lkpi_internal.h"

int nla_put(struct sk_buff *skb, int attrtype, int attrlen, const void *data) { return -EMSGSIZE; }
EXPORT_SYMBOL(nla_put);
int nla_put_64bit(struct sk_buff *skb, int attrtype, int attrlen, const void *data, int padattr) { return -EMSGSIZE; }
EXPORT_SYMBOL(nla_put_64bit);
int __nla_parse(struct nlattr **tb, int maxtype, const struct nlattr *head, int len,
		const struct nla_policy *policy, unsigned int validate, struct netlink_ext_ack *extack) { return -EOPNOTSUPP; }
EXPORT_SYMBOL(__nla_parse);
struct nlattr *nla_find(const struct nlattr *head, int len, int attrtype) { return NULL; }
EXPORT_SYMBOL(nla_find);
int netlink_unicast(struct sock *ssk, struct sk_buff *skb, __u32 portid, int nonblock)
{
	kfree_skb(skb);
	return -ECONNREFUSED;
}
EXPORT_SYMBOL(netlink_unicast);

/* faux bus: un dispositivo suelto con nombre (cfg80211 "regulatory") */
struct faux_device *faux_device_create(const char *name, struct device *parent, const struct faux_device_ops *faux_ops)
{
	struct faux_device *f = kzalloc(sizeof(*f), GFP_KERNEL);

	if (!f)
		return NULL;
	device_initialize(&f->dev);
	dev_set_name(&f->dev, "%s", name);
	f->dev.parent = parent;
	if (device_add(&f->dev)) {
		put_device(&f->dev);
		return NULL;
	}
	if (faux_ops && faux_ops->probe && faux_ops->probe(f)) {
		device_del(&f->dev);
		put_device(&f->dev);
		return NULL;
	}
	return f;
}
EXPORT_SYMBOL(faux_device_create);

void faux_device_destroy(struct faux_device *f)
{
	if (!f)
		return;
	device_del(&f->dev);
	put_device(&f->dev);
}
EXPORT_SYMBOL(faux_device_destroy);
