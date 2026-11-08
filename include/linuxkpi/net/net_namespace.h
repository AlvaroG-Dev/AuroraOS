/* Espacios de nombres de red: AuroraOS tiene uno solo (init_net). */
#ifndef _LKPI_NET_NET_NAMESPACE_H
#define _LKPI_NET_NET_NAMESPACE_H
#include <linux/types.h>
#include <linux/list.h>
struct sock;
struct net { int ns_id; struct sock *genl_sock; };
extern struct net init_net;
typedef struct { struct net *net; } possible_net_t;
static inline void write_pnet(possible_net_t *pnet, struct net *net) { pnet->net = net; }
static inline struct net *read_pnet(const possible_net_t *pnet) { return pnet->net ? pnet->net : &init_net; }
static inline int net_eq(const struct net *a, const struct net *b) { return a == b; }
static inline struct net *get_net(struct net *net) { return net; }
static inline void put_net(struct net *net) { }
static inline bool check_net(const struct net *net) { return true; }
struct kobj_ns_type_operations { int type; };
extern const struct kobj_ns_type_operations net_ns_type_operations;
struct pernet_operations {
	struct list_head list;
	int (*init)(struct net *net);
	void (*pre_exit)(struct net *net);
	void (*exit)(struct net *net);
	void (*exit_batch)(struct list_head *net_exit_list);
	unsigned int *id;
	size_t size;
};
/* Un solo espacio: init() se llama al registrar y exit() al retirar. */
int register_pernet_device(struct pernet_operations *ops);
void unregister_pernet_device(struct pernet_operations *ops);
int register_pernet_subsys(struct pernet_operations *ops);
void unregister_pernet_subsys(struct pernet_operations *ops);
#endif
