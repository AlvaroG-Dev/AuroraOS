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
static inline void tcp_v6_gso_csum_prep(struct sk_buff *skb) { }
static inline unsigned int inner_tcp_hdrlen(const struct sk_buff *skb) { return tcp_hdrlen(skb); }
#endif
