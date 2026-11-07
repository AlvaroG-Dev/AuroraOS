/* SPDX-License-Identifier: GPL-2.0 */
/* skbuff.h de la capa LinuxKPI-NET (Plan_Red.md): sk_buff de Linux con la
 * disposicion real (head/data/tail/end + skb_shared_info al final). Lineal y
 * sin GRO/TSO propio; los frags existen solo para que compilen los drivers. */
#ifndef _LKPI_LINUX_SKBUFF_H
#define _LKPI_LINUX_SKBUFF_H

#include <linux/types.h>
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/gfp.h>
#include <linux/mm.h>
#include <linux/string.h>
#include <linux/dma-mapping.h>
#include <linux/ktime.h>
#include <linux/atomic.h>
#include <linux/if_ether.h>

#define CHECKSUM_NONE		0
#define CHECKSUM_UNNECESSARY	1
#define CHECKSUM_COMPLETE	2
#define CHECKSUM_PARTIAL	3

#define PACKET_HOST 0
#define PACKET_BROADCAST 1
#define PACKET_MULTICAST 2
#define PACKET_OTHERHOST 3
#define SKBTX_HW_TSTAMP (1 << 0)
#define SKBTX_SW_TSTAMP (1 << 1)
#define SKBTX_IN_PROGRESS (1 << 2)
#define SKBTX_HW_TSTAMP_NETDEV (1 << 3)
#define SKBTX_HW_TSTAMP_USE_CYCLES (1 << 4)
#define MAX_SKB_FRAGS 17
#define SKB_DATA_ALIGN(x) ALIGN(x, SMP_CACHE_BYTES)
#define SKB_WITH_OVERHEAD(x) ((x) - SKB_DATA_ALIGN(sizeof(struct skb_shared_info)))
#define SKB_MAX_ORDER(X, ORDER) 0
#define NET_SKB_PAD 64
#define NET_IP_ALIGN 2

enum {
	SKB_GSO_TCPV4 = 1 << 0,
	SKB_GSO_DODGY = 1 << 1,
	SKB_GSO_TCP_ECN = 1 << 2,
	SKB_GSO_TCPV6 = 1 << 4,
	SKB_GSO_UDP_L4 = 1 << 16,
};

typedef struct skb_frag {
	struct page *page;
	unsigned int len;
	unsigned int offset;
} skb_frag_t;

struct skb_shared_hwtstamps { ktime_t hwtstamp; void *netdev_data; };
struct skb_shared_info {
	__u8 flags;
	struct skb_shared_hwtstamps hwtstamps;
	__u8 meta_len;
	__u8 nr_frags;
	__u8 tx_flags;
	unsigned short gso_size;
	unsigned short gso_segs;
	unsigned int gso_type;
	struct sk_buff *frag_list;
	skb_frag_t frags[MAX_SKB_FRAGS];
};

typedef unsigned int sk_buff_data_t;

struct net_device;

struct sock { u32 sk_tsflags; };
struct sk_buff {
	union { struct { struct sk_buff *next, *prev; }; struct list_head list; };
	struct net_device *dev;
	struct sock *sk;
	char cb[48] __aligned(8);
	unsigned int len, data_len;
	__u16 mac_len, hdr_len;
	__u16 queue_mapping;
	__u8 pkt_type;
	__u8 ip_summed;
	__u8 cloned;
	__u8 no_fcs:1;
	__u8 encapsulation:1;
	__u8 head_frag:1;
	__u8 wifi_acked_valid:1;
	__u8 wifi_acked:1;
	void (*destructor)(struct sk_buff *skb);
	__u8 vlan_present;
	__u16 vlan_tci;
	__be16 vlan_proto;
	__be16 protocol;
	__u16 transport_header, network_header, mac_header;
	__u32 priority;
	__wsum csum;
	__u16 csum_start, csum_offset;
	ktime_t tstamp;
	unsigned int truesize;
	refcount_t users;
	unsigned char *head, *data;
	sk_buff_data_t tail, end;
};

struct sk_buff_head {
	struct sk_buff *next, *prev;
	__u32 qlen;
	spinlock_t lock;
};

/* ---- ciclo de vida ---- */
struct sk_buff *__alloc_skb(unsigned int size, gfp_t gfp, int flags, int node);
static inline struct sk_buff *alloc_skb(unsigned int size, gfp_t gfp)
{ return __alloc_skb(size, gfp, 0, -1); }
void kfree_skb(struct sk_buff *skb);
static inline void consume_skb(struct sk_buff *skb) { kfree_skb(skb); }
static inline void dev_kfree_skb(struct sk_buff *skb) { kfree_skb(skb); }
static inline void dev_kfree_skb_any(struct sk_buff *skb) { kfree_skb(skb); }
static inline void dev_consume_skb_any(struct sk_buff *skb) { kfree_skb(skb); }
static inline void dev_kfree_skb_irq(struct sk_buff *skb) { kfree_skb(skb); }
static inline void napi_consume_skb(struct sk_buff *skb, int budget) { kfree_skb(skb); }
static inline void kfree_skb_reason(struct sk_buff *skb, int reason) { kfree_skb(skb); }
#define dev_kfree_skb_irq_reason(s, r) kfree_skb(s)
#define dev_kfree_skb_any_reason(s, r) kfree_skb(s)
#include <net/dropreason.h>

static inline struct skb_shared_info *skb_shinfo(const struct sk_buff *skb)
{ return (struct skb_shared_info *)(skb->head + skb->end); }

/* ---- punteros ---- */
static inline unsigned char *skb_tail_pointer(const struct sk_buff *skb) { return skb->head + skb->tail; }
static inline unsigned char *skb_end_pointer(const struct sk_buff *skb) { return skb->head + skb->end; }
static inline unsigned int skb_headlen(const struct sk_buff *skb) { return skb->len - skb->data_len; }
static inline bool skb_is_nonlinear(const struct sk_buff *skb) { return skb->data_len; }
static inline unsigned int skb_headroom(const struct sk_buff *skb) { return skb->data - skb->head; }
static inline int skb_tailroom(const struct sk_buff *skb) { return skb->end - skb->tail; }
static inline void skb_reserve(struct sk_buff *skb, int len) { skb->data += len; skb->tail += len; }
static inline void *skb_put(struct sk_buff *skb, unsigned int len)
{ void *tmp = skb_tail_pointer(skb); skb->tail += len; skb->len += len; return tmp; }
static inline void *__skb_put(struct sk_buff *skb, unsigned int len) { return skb_put(skb, len); }
static inline void *skb_put_zero(struct sk_buff *skb, unsigned int len)
{ void *p = skb_put(skb, len); memset(p, 0, len); return p; }
static inline void *skb_put_data(struct sk_buff *skb, const void *d, unsigned int len)
{ void *p = skb_put(skb, len); memcpy(p, d, len); return p; }
static inline void *skb_push(struct sk_buff *skb, unsigned int len)
{ skb->data -= len; skb->len += len; return skb->data; }
static inline void *skb_pull(struct sk_buff *skb, unsigned int len)
{ skb->len -= len; return skb->data += len; }
static inline void __skb_trim(struct sk_buff *skb, unsigned int len)
{ skb->len = len; skb->tail = skb->data - skb->head + len; }
static inline void skb_trim(struct sk_buff *skb, unsigned int len) { if (skb->len > len) __skb_trim(skb, len); }
static inline void skb_set_tail_pointer(struct sk_buff *skb, const int offset)
{ skb->tail = skb->data - skb->head + offset; }
static inline void skb_reset_tail_pointer(struct sk_buff *skb) { skb->tail = skb->data - skb->head; }

static inline void skb_reset_mac_header(struct sk_buff *skb) { skb->mac_header = skb->data - skb->head; }
static inline void skb_reset_network_header(struct sk_buff *skb) { skb->network_header = skb->data - skb->head; }
static inline void skb_reset_transport_header(struct sk_buff *skb) { skb->transport_header = skb->data - skb->head; }
static inline void skb_set_transport_header(struct sk_buff *skb, const int offset)
{ skb->transport_header = skb->data - skb->head + offset; }
static inline void skb_set_network_header(struct sk_buff *skb, const int offset)
{ skb->network_header = skb->data - skb->head + offset; }
static inline unsigned char *skb_mac_header(const struct sk_buff *skb) { return skb->head + skb->mac_header; }
static inline unsigned char *skb_network_header(const struct sk_buff *skb) { return skb->head + skb->network_header; }
static inline unsigned char *skb_transport_header(const struct sk_buff *skb) { return skb->head + skb->transport_header; }
static inline bool skb_transport_header_was_set(const struct sk_buff *skb) { return skb->transport_header != (typeof(skb->transport_header))~0U; }
static inline int skb_network_offset(const struct sk_buff *skb) { return skb_network_header(skb) - skb->data; }
static inline int skb_transport_offset(const struct sk_buff *skb) { return skb_transport_header(skb) - skb->data; }
static inline u32 skb_network_header_len(const struct sk_buff *skb) { return skb->transport_header - skb->network_header; }
static inline int skb_checksum_start_offset(const struct sk_buff *skb) { return skb->csum_start - skb_headroom(skb); }

static inline void skb_copy_to_linear_data(struct sk_buff *skb, const void *from, const unsigned int len)
{ memcpy(skb->data, from, len); }
static inline void skb_copy_to_linear_data_offset(struct sk_buff *skb, const int offset, const void *from, const unsigned int len)
{ memcpy(skb->data + offset, from, len); }
static inline void skb_copy_from_linear_data(const struct sk_buff *skb, void *to, const unsigned int len)
{ memcpy(to, skb->data, len); }

void *skb_header_pointer_slow(const struct sk_buff *skb, int offset, int len, void *buffer);
static inline void *skb_header_pointer(const struct sk_buff *skb, int offset, int len, void *buffer)
{
	if (offset + len <= (int)skb_headlen(skb)) return skb->data + offset;
	return skb_header_pointer_slow(skb, offset, len, buffer);
}
int skb_copy_bits(const struct sk_buff *skb, int offset, void *to, int len);
int skb_linearize(struct sk_buff *skb);
int pskb_expand_head(struct sk_buff *skb, int nhead, int ntail, gfp_t gfp);
static inline int skb_cow_head(struct sk_buff *skb, unsigned int headroom)
{
	if (skb_headroom(skb) >= headroom && !skb->cloned) return 0;
	return pskb_expand_head(skb, headroom > skb_headroom(skb) ? headroom - skb_headroom(skb) : 0, 0, GFP_ATOMIC);
}
static inline int skb_padto(struct sk_buff *skb, unsigned int len)
{
	if (skb->len >= len) return 0;
	if (skb_tailroom(skb) < (int)(len - skb->len)) {
		int err = pskb_expand_head(skb, 0, len - skb->len, GFP_ATOMIC);
		if (err) { kfree_skb(skb); return err; }
	}
	memset(skb_put(skb, 0) , 0, 0);
	memset(skb->data + skb->len, 0, len - skb->len);
	skb->tail += len - skb->len; skb->len = len;
	return 0;
}
static inline int __skb_put_padto(struct sk_buff *skb, unsigned int len, bool free_on_error)
{ return skb_padto(skb, len); }
static inline int skb_put_padto(struct sk_buff *skb, unsigned int len) { return skb_padto(skb, len); }
static inline bool skb_is_gso(const struct sk_buff *skb) { return skb_shinfo(skb)->gso_size; }
static inline bool skb_is_gso_v6(const struct sk_buff *skb) { return skb_shinfo(skb)->gso_type & SKB_GSO_TCPV6; }
static inline void skb_checksum_none_assert(const struct sk_buff *skb) { }
static inline void skb_tx_timestamp(struct sk_buff *skb) { }
static inline void skb_mark_not_on_list(struct sk_buff *skb) { skb->next = NULL; }
static inline unsigned int skb_frag_size(const skb_frag_t *frag) { return frag->len; }
static inline void *skb_frag_address(const skb_frag_t *frag) { return page_address(frag->page) + frag->offset; }
static inline struct page *skb_frag_page(const skb_frag_t *frag) { return frag->page; }
static inline dma_addr_t skb_frag_dma_map(struct device *dev, const skb_frag_t *frag, size_t offset, size_t size, enum dma_data_direction dir)
{ return dma_map_page(dev, frag->page, frag->offset + offset, size, dir); }
static inline bool skb_vlan_tag_present(const struct sk_buff *skb) { return skb->vlan_present; }
static inline u16 skb_vlan_tag_get(const struct sk_buff *skb) { return skb->vlan_tci; }
static inline void __vlan_hwaccel_put_tag(struct sk_buff *skb, __be16 proto, u16 tci)
{ skb->vlan_proto = proto; skb->vlan_tci = tci; skb->vlan_present = 1; }
static inline struct sk_buff *skb_get(struct sk_buff *skb) { refcount_inc(&skb->users); return skb; }
static inline void skb_record_rx_queue(struct sk_buff *skb, u16 q) { skb->queue_mapping = q + 1; }
static inline void skb_set_queue_mapping(struct sk_buff *skb, u16 q) { skb->queue_mapping = q; }
static inline u16 skb_get_queue_mapping(const struct sk_buff *skb) { return skb->queue_mapping; }
static inline void skb_mark_for_recycle(struct sk_buff *skb) { }
static inline void skb_orphan(struct sk_buff *skb) { }
static inline bool skb_shared(const struct sk_buff *skb) { return refcount_read(&skb->users) != 1; }
static inline __wsum csum_unfold(__sum16 n) { return (__force __wsum)n; }
static inline void skb_tx_hash_dummy(void) { }
static inline u32 skb_get_hash(struct sk_buff *skb) { return 0; }
static inline unsigned int skb_frag_off(const skb_frag_t *frag) { return frag->offset; }

static inline struct skb_shared_hwtstamps *skb_hwtstamps(struct sk_buff *skb) { return &skb_shinfo(skb)->hwtstamps; }
static inline void skb_tstamp_tx(struct sk_buff *orig_skb, struct skb_shared_hwtstamps *hwtstamps) { }
enum pkt_hash_types { PKT_HASH_TYPE_NONE, PKT_HASH_TYPE_L2, PKT_HASH_TYPE_L3, PKT_HASH_TYPE_L4 };
static inline void skb_set_hash(struct sk_buff *skb, __u32 hash, enum pkt_hash_types type) { }
static inline void skb_fill_page_desc(struct sk_buff *skb, int i, struct page *page, int off, int size)
{
	skb_frag_t *f = &skb_shinfo(skb)->frags[i];
	f->page = page; f->offset = off; f->len = size;
	skb_shinfo(skb)->nr_frags = i + 1;
}
static inline void *__pskb_pull_tail(struct sk_buff *skb, int delta) { return NULL; }
static inline int pskb_trim(struct sk_buff *skb, unsigned int len) { skb_trim(skb, len); return 0; }
static inline bool pskb_may_pull(struct sk_buff *skb, unsigned int len) { return len <= skb_headlen(skb); }
static inline unsigned int skb_tcp_all_headers(const struct sk_buff *skb)
{ return skb_transport_offset(skb) + ((skb_transport_header(skb)[12] >> 4) * 4); }
static inline unsigned int skb_inner_transport_offset_dummy(void) { return 0; }
static inline void skb_frag_ref(struct sk_buff *skb, int f) { }
static inline void skb_frag_unref(struct sk_buff *skb, int f) { }
static inline unsigned int skb_frag_off_dummy(void) { return 0; }

static inline unsigned char *skb_checksum_start(const struct sk_buff *skb) { return skb->head + skb->csum_start; }
static inline bool skb_csum_is_sctp(struct sk_buff *skb) { return false; }
static inline void __page_frag_cache_drain(struct page *page, unsigned int count) { put_page(page); }
int skb_checksum_help(struct sk_buff *skb);
#define net_err_ratelimited(fmt, ...) do { } while (0)
#define net_info_ratelimited(fmt, ...) do { } while (0)

static inline void skb_add_rx_frag(struct sk_buff *skb, int i, struct page *page, int off, int size, unsigned int truesize)
{
	skb_fill_page_desc(skb, i, page, off, size);
	skb->len += size; skb->data_len += size; skb->truesize += truesize;
}
static inline void net_prefetch(void *p) { prefetch(p); }
static inline struct sk_buff *napi_build_skb(void *data, unsigned int frag_size)
{
	/* igc: el buffer es una pagina; copia lineal para no compartir el frag */
	return NULL;
}
static inline void skb_metadata_set(struct sk_buff *skb, u8 meta_len) { }
static inline void *__skb_pull(struct sk_buff *skb, unsigned int len) { skb->len -= len; return skb->data += len; }
static inline void dummy_skb_end(void) { }

/* ---- colas ---- */
static inline void skb_queue_head_init(struct sk_buff_head *list)
{ spin_lock_init(&list->lock); list->prev = list->next = (struct sk_buff *)list; list->qlen = 0; }
static inline void __skb_queue_tail(struct sk_buff_head *list, struct sk_buff *newsk)
{
	struct sk_buff *prev = list->prev;
	newsk->next = (struct sk_buff *)list; newsk->prev = prev;
	prev->next = newsk; list->prev = newsk; list->qlen++;
}
static inline struct sk_buff *__skb_dequeue(struct sk_buff_head *list)
{
	struct sk_buff *skb = list->next;
	if (skb == (struct sk_buff *)list) return NULL;
	list->next = skb->next; skb->next->prev = (struct sk_buff *)list;
	skb->next = skb->prev = NULL; list->qlen--;
	return skb;
}
static inline __u32 skb_queue_len(const struct sk_buff_head *list) { return list->qlen; }
static inline int skb_queue_empty(const struct sk_buff_head *list) { return list->next == (const struct sk_buff *)list; }

#endif

#ifndef _LKPI_DEV_ALLOC_PAGES
#define _LKPI_DEV_ALLOC_PAGES
#include <linux/gfp.h>
#define dev_alloc_pages(order) alloc_pages(GFP_ATOMIC | __GFP_NOWARN | __GFP_COMP, order)
static inline bool dev_page_is_reusable(const struct page *page) { return true; }
#ifndef _LKPI_SKB_WIFI
#define _LKPI_SKB_WIFI
static inline bool skb_has_frag_list(const struct sk_buff *skb) { return false; }
static inline void *pskb_pull(struct sk_buff *skb, unsigned int len) { return len > skb->len ? NULL : skb_pull(skb, len); }
static inline void skb_postpull_rcsum(struct sk_buff *skb, const void *start, unsigned int len) { }
static inline void __skb_queue_purge(struct sk_buff_head *list) { struct sk_buff *skb; while ((skb = __skb_dequeue(list)) != NULL) kfree_skb(skb); }
#endif
/* ---- colas completas (net/core/skbuff.c + include/linux/skbuff.h) ---- */
#ifndef _LKPI_SKB_QUEUES
#define _LKPI_SKB_QUEUES
static inline struct sk_buff *skb_peek(const struct sk_buff_head *list_)
{ struct sk_buff *skb = list_->next; return skb == (struct sk_buff *)list_ ? NULL : skb; }
static inline struct sk_buff *skb_peek_tail(const struct sk_buff_head *list_)
{ struct sk_buff *skb = list_->prev; return skb == (struct sk_buff *)list_ ? NULL : skb; }
static inline void __skb_queue_head_init(struct sk_buff_head *list) { list->prev = list->next = (struct sk_buff *)list; list->qlen = 0; }
static inline bool skb_queue_is_last(const struct sk_buff_head *list, const struct sk_buff *skb) { return skb->next == (struct sk_buff *)list; }
static inline bool skb_queue_is_first(const struct sk_buff_head *list, const struct sk_buff *skb) { return skb->prev == (struct sk_buff *)list; }
static inline struct sk_buff *skb_queue_next(const struct sk_buff_head *list, const struct sk_buff *skb) { return skb->next; }
static inline struct sk_buff *skb_queue_prev(const struct sk_buff_head *list, const struct sk_buff *skb) { return skb->prev; }
static inline void __skb_insert(struct sk_buff *newsk, struct sk_buff *prev, struct sk_buff *next, struct sk_buff_head *list)
{ newsk->next = next; newsk->prev = prev; next->prev = newsk; prev->next = newsk; list->qlen++; }
static inline void __skb_queue_before(struct sk_buff_head *list, struct sk_buff *next, struct sk_buff *newsk) { __skb_insert(newsk, next->prev, next, list); }
static inline void __skb_queue_after(struct sk_buff_head *list, struct sk_buff *prev, struct sk_buff *newsk) { __skb_insert(newsk, prev, prev->next, list); }
static inline void __skb_queue_head(struct sk_buff_head *list, struct sk_buff *newsk) { __skb_queue_after(list, (struct sk_buff *)list, newsk); }
static inline void __skb_unlink(struct sk_buff *skb, struct sk_buff_head *list)
{ struct sk_buff *next = skb->next, *prev = skb->prev; skb->next = skb->prev = NULL; list->qlen--; next->prev = prev; prev->next = next; }
static inline void skb_queue_head(struct sk_buff_head *list, struct sk_buff *newsk)
{ unsigned long f; spin_lock_irqsave(&list->lock, f); __skb_queue_head(list, newsk); spin_unlock_irqrestore(&list->lock, f); }
static inline void skb_queue_tail(struct sk_buff_head *list, struct sk_buff *newsk)
{ unsigned long f; spin_lock_irqsave(&list->lock, f); __skb_queue_tail(list, newsk); spin_unlock_irqrestore(&list->lock, f); }
static inline struct sk_buff *skb_dequeue(struct sk_buff_head *list)
{ unsigned long f; struct sk_buff *r; spin_lock_irqsave(&list->lock, f); r = __skb_dequeue(list); spin_unlock_irqrestore(&list->lock, f); return r; }
static inline struct sk_buff *__skb_dequeue_tail(struct sk_buff_head *list)
{ struct sk_buff *skb = skb_peek_tail(list); if (skb) __skb_unlink(skb, list); return skb; }
static inline struct sk_buff *skb_dequeue_tail(struct sk_buff_head *list)
{ unsigned long f; struct sk_buff *r; spin_lock_irqsave(&list->lock, f); r = __skb_dequeue_tail(list); spin_unlock_irqrestore(&list->lock, f); return r; }
static inline void skb_unlink(struct sk_buff *skb, struct sk_buff_head *list)
{ unsigned long f; spin_lock_irqsave(&list->lock, f); __skb_unlink(skb, list); spin_unlock_irqrestore(&list->lock, f); }
static inline void skb_queue_purge(struct sk_buff_head *list)
{ struct sk_buff *skb; while ((skb = skb_dequeue(list)) != NULL) kfree_skb(skb); }
static inline void skb_insert(struct sk_buff *old, struct sk_buff *newsk, struct sk_buff_head *list)
{ unsigned long f; spin_lock_irqsave(&list->lock, f); __skb_insert(newsk, old->prev, old, list); spin_unlock_irqrestore(&list->lock, f); }
static inline void __skb_queue_splice(const struct sk_buff_head *list, struct sk_buff *prev, struct sk_buff *next)
{
	struct sk_buff *first = list->next, *last = list->prev;
	first->prev = prev; prev->next = first; last->next = next; next->prev = last;
}
static inline void skb_queue_splice(const struct sk_buff_head *list, struct sk_buff_head *head)
{ if (!skb_queue_empty(list)) { __skb_queue_splice(list, (struct sk_buff *)head, head->next); head->qlen += list->qlen; } }
static inline void skb_queue_splice_init(struct sk_buff_head *list, struct sk_buff_head *head)
{ if (!skb_queue_empty(list)) { __skb_queue_splice(list, (struct sk_buff *)head, head->next); head->qlen += list->qlen; __skb_queue_head_init(list); } }
static inline void skb_queue_splice_tail(const struct sk_buff_head *list, struct sk_buff_head *head)
{ if (!skb_queue_empty(list)) { __skb_queue_splice(list, head->prev, (struct sk_buff *)head); head->qlen += list->qlen; } }
static inline void skb_queue_splice_tail_init(struct sk_buff_head *list, struct sk_buff_head *head)
{ if (!skb_queue_empty(list)) { __skb_queue_splice(list, head->prev, (struct sk_buff *)head); head->qlen += list->qlen; __skb_queue_head_init(list); } }
#define skb_queue_walk(queue, skb) for (skb = (queue)->next; skb != (struct sk_buff *)(queue); skb = skb->next)
#define skb_queue_walk_safe(queue, skb, tmp) for (skb = (queue)->next, tmp = skb->next; skb != (struct sk_buff *)(queue); skb = tmp, tmp = skb->next)
#define skb_queue_walk_from(queue, skb) for (; skb != (struct sk_buff *)(queue); skb = skb->next)
#define skb_queue_reverse_walk(queue, skb) for (skb = (queue)->prev; skb != (struct sk_buff *)(queue); skb = skb->prev)
static inline void skb_list_del_init(struct sk_buff *skb) { list_del_init(&skb->list); }
static inline void *skb_put_u8(struct sk_buff *skb, u8 val) { u8 *p = skb_put(skb, 1); *p = val; return p; }
static inline bool skb_mac_header_was_set(const struct sk_buff *skb) { return skb->mac_header != (typeof(skb->mac_header))~0U; }
struct sk_buff *skb_clone(struct sk_buff *skb, gfp_t gfp);
struct sk_buff *skb_copy(const struct sk_buff *skb, gfp_t gfp);
struct sk_buff *pskb_copy(struct sk_buff *skb, gfp_t gfp);
struct sk_buff *skb_copy_expand(const struct sk_buff *skb, int newheadroom, int newtailroom, gfp_t gfp);
struct sk_buff *skb_realloc_headroom(struct sk_buff *skb, unsigned int headroom);
static inline bool skb_cloned(const struct sk_buff *skb) { return false; }
static inline bool skb_header_cloned(const struct sk_buff *skb) { return false; }
static inline struct sk_buff *skb_unshare(struct sk_buff *skb, gfp_t gfp) { return skb; }
static inline struct sk_buff *skb_share_check(struct sk_buff *skb, gfp_t gfp) { return skb; }
#endif
#define __skb_peek(list_) ((list_)->next)
#ifndef _LKPI_SKB_MORE
#define _LKPI_SKB_MORE
typedef u64 netdev_features_t;
#define skb_list_walk_safe(first, skb, next_skb) \
	for ((skb) = (first), (next_skb) = (skb) ? (skb)->next : NULL; (skb); \
	     (skb) = (next_skb), (next_skb) = (skb) ? (skb)->next : NULL)
static inline void skb_set_mac_header(struct sk_buff *skb, const int offset) { skb->mac_header = skb->data - skb->head + offset; }
static inline void skb_set_inner_transport_header(struct sk_buff *skb, const int offset) { }
static inline bool skb_needs_linearize(struct sk_buff *skb, netdev_features_t features) { return false; }
static inline int skb_ensure_writable(struct sk_buff *skb, unsigned int write_len) { return write_len <= skb->len ? 0 : -EFAULT; }
static inline int skb_csum_hwoffload_help(struct sk_buff *skb, const netdev_features_t features) { return 0; }
static inline void skb_copy_queue_mapping(struct sk_buff *to, const struct sk_buff *from) { to->queue_mapping = from->queue_mapping; }
static inline int skb_clone_writable(const struct sk_buff *skb, unsigned int len) { return 1; }
static inline struct sk_buff *skb_clone_sk(struct sk_buff *skb) { return NULL; }
static inline bool sk_requests_wifi_status(struct sock *sk) { return false; }
static inline void sk_pacing_shift_update(struct sock *sk, int val) { }
static inline int __skb_linearize(struct sk_buff *skb) { return 0; }
static inline int __pskb_trim(struct sk_buff *skb, unsigned int len) { if (len < skb->len) { skb->len = len; skb->tail = skb->data - skb->head + len; } return 0; }
#endif
#endif
