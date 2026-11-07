#ifndef _LKPI_LINUX_TCP_H
#define _LKPI_LINUX_TCP_H
#include <linux/skbuff.h>
#include <linux/in.h>
struct tcphdr {
	__be16 source, dest;
	__be32 seq, ack_seq;
	__u16 res1:4, doff:4, fin:1, syn:1, rst:1, psh:1, ack:1, urg:1, ece:1, cwr:1;
	__be16 window;
	__sum16 check;
	__be16 urg_ptr;
};
static inline struct tcphdr *tcp_hdr(const struct sk_buff *skb) { return (struct tcphdr *)skb_transport_header(skb); }
static inline unsigned int __tcp_hdrlen(const struct tcphdr *th) { return th->doff * 4; }
static inline unsigned int tcp_hdrlen(const struct sk_buff *skb) { return __tcp_hdrlen(tcp_hdr(skb)); }
static inline unsigned int inner_tcp_hdrlen(const struct sk_buff *skb) { return tcp_hdrlen(skb); }
#include <linux/ipv6.h>
#include <linux/in6.h>
static inline __sum16 csum_ipv6_magic(const struct in6_addr *saddr, const struct in6_addr *daddr,
				      __u32 len, __u8 proto, __wsum csum)
{
	u64 sum = (__force u32)csum;
	const u16 *p;
	int i;

	p = (const u16 *)saddr; for (i = 0; i < 8; i++) sum += p[i];
	p = (const u16 *)daddr; for (i = 0; i < 8; i++) sum += p[i];
	sum += htons(len & 0xffff) + htons(len >> 16) + htons(proto);
	while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
	return (__force __sum16)~sum;
}
static inline void tcp_v6_gso_csum_prep(struct sk_buff *skb)
{
	struct ipv6hdr *ipv6h = ipv6_hdr(skb);
	struct tcphdr *th = tcp_hdr(skb);

	ipv6h->payload_len = 0;
	th->check = ~csum_ipv6_magic(&ipv6h->saddr, &ipv6h->daddr, 0, IPPROTO_TCP, 0);
}
#endif
