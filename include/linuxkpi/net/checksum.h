#ifndef _LKPI_NET_CHECKSUM_H
#define _LKPI_NET_CHECKSUM_H
#include <linux/types.h>
#include <linux/skbuff.h>
static inline __wsum csum_partial(const void *buff, int len, __wsum wsum)
{
	u32 sum = (__force u32)wsum; const u8 *p = buff; int i;
	for (i = 0; i + 1 < len; i += 2) sum += (p[i] << 8) | p[i + 1];
	if (len & 1) sum += p[len - 1] << 8;
	while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
	return (__force __wsum)sum;
}
static inline __sum16 csum_fold(__wsum csum)
{
	u32 sum = (__force u32)csum;
	sum = (sum & 0xffff) + (sum >> 16);
	sum = (sum & 0xffff) + (sum >> 16);
	return (__force __sum16)~sum;
}
static inline void csum_replace_by_diff(__sum16 *sum, __wsum diff) { *sum = csum_fold((__force __wsum)((__force u32)diff + ~(__force u32)csum_unfold(*sum))); }
/* pseudo-cabecera IPv4: direcciones en orden de red, len/proto en host */
static inline __wsum csum_tcpudp_nofold(__be32 saddr, __be32 daddr, __u32 len, __u8 proto, __wsum sum)
{
	u64 s = (__force u32)sum;
	s += (__force u32)saddr + (__force u32)daddr;
	s += ((u32)proto + len) << 8;	/* little endian: ntohs(proto+len) en orden de red */
	s = (s & 0xffffffff) + (s >> 32);
	s = (s & 0xffffffff) + (s >> 32);
	return (__force __wsum)s;
}
static inline __sum16 csum_tcpudp_magic(__be32 saddr, __be32 daddr, __u32 len, __u8 proto, __wsum sum)
{ return csum_fold(csum_tcpudp_nofold(saddr, daddr, len, proto, sum)); }
#endif
