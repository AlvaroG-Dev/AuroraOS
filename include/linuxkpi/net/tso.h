#ifndef _LKPI_NET_TSO_H
#define _LKPI_NET_TSO_H
#include <linux/skbuff.h>
/* Sin TSO por software: los drivers no deben anunciar GSO que lo necesite. */
struct tso_t {
	int next_frag_idx;
	int size;
	void *data;
	u16 ip_id;
	u8 tlen;
	bool ipv6;
	u32 tcp_seq;
};
static inline int tso_count_descs(const struct sk_buff *skb) { return 1; }
static inline void tso_build_hdr(const struct sk_buff *skb, char *hdr, struct tso_t *tso, int size, bool is_last) { }
static inline void tso_build_data(const struct sk_buff *skb, struct tso_t *tso, int size) { }
static inline int tso_start(struct sk_buff *skb, struct tso_t *tso) { return 0; }
#endif
