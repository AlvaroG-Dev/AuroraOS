// kernel/net/tcp.c
//
// TCP completo con WSCALE, CWND, RTO dinámico, fast retransmit,
// fast recovery (NewReno) y autotune de ventana de recepción.

#include "tcp.h"
#include "../heap.h"
#include "../klog.h"
#include "../sched.h"
#include "../spinlock.h"
#include "../string.h"
#include "../time.h"
#include "../uaccess.h"
#include "checksum.h"
#include "ip.h"
#include "netif.h"

// ---------------------------------------------------------------------------
// Tabla hash
// ---------------------------------------------------------------------------
#define TCP_HASH_BUCKETS 64
#define TCP_CONN_MAX 64

static tcp_conn_t *g_tcp_buckets[TCP_HASH_BUCKETS];
static spinlock_t g_tcp_lock;
static int g_conn_count;

static inline unsigned tcp_hash(uint32_t lip, uint16_t lp, uint32_t rip,
                                uint16_t rp) {
  uint32_t h = lip * 0x9E3779B1u;
  h ^= rip * 0x85EBCA77u;
  h ^= ((uint32_t)lp << 16) | (uint32_t)rp;
  h ^= h >> 16;
  h *= 0x85EBCA6Bu;
  h ^= h >> 13;
  return h & (TCP_HASH_BUCKETS - 1);
}

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
static int tcp_send_segment(tcp_conn_t *c, uint16_t flags, uint32_t seq,
                            uint32_t ack, const void *payload,
                            size_t payload_len);
static int tcp_send_segment_ex(tcp_conn_t *c, uint16_t flags, uint32_t seq,
                               uint32_t ack, const void *payload,
                               size_t payload_len, int do_enqueue);
static size_t tcp_rx_window(tcp_conn_t *c);
static uint16_t tcp_rx_window_wire(tcp_conn_t *c, int is_syn);
static bool tcp_handshake_done(void *arg);
static uint32_t tcp_pick_isn(void);
static void tcp_rtt_update(tcp_conn_t *c, uint64_t rtt_ms);
static void tcp_autotune_sample(tcp_conn_t *c, size_t delivered);
static void tcp_fast_retransmit(tcp_conn_t *c);

// ---------------------------------------------------------------------------
// Parseo
// ---------------------------------------------------------------------------
static inline uint16_t rd16be(const uint8_t *p) {
  return (uint16_t)((p[0] << 8) | p[1]);
}
static inline uint32_t rd32be(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

int tcp_parse_header(const uint8_t *seg, size_t seg_len, tcp_hdr_t *out) {
  if (!seg || !out)
    return -EINVAL;
  if (seg_len < TCP_HDR_MIN_LEN)
    return -EINVAL;

  uint8_t data_offset = (uint8_t)((seg[12] >> 4) & 0x0F);
  if (data_offset < 5)
    return -EINVAL;

  size_t hdr_len = (size_t)data_offset * 4;
  if (hdr_len > seg_len || hdr_len > TCP_HDR_MAX_LEN)
    return -EINVAL;

  memset(out, 0, sizeof(*out));
  out->sport = rd16be(seg + 0);
  out->dport = rd16be(seg + 2);
  out->seq = rd32be(seg + 4);
  out->ack = rd32be(seg + 8);
  out->data_offset = data_offset;
  out->flags = (uint16_t)(((seg[12] & 0x01) << 8) | seg[13]);
  out->window = rd16be(seg + 14);
  out->csum = rd16be(seg + 16);
  out->urgent = rd16be(seg + 18);
  out->hdr_len = hdr_len;
  return 0;
}

// Parsea MSS y WSCALE del SYN/SYN-ACK.
static void tcp_parse_syn_opts(const uint8_t *seg, const tcp_hdr_t *h,
                               uint16_t *mss_out, uint8_t *wscale_out,
                               int *has_wscale) {
  *mss_out = 0;
  *wscale_out = 0;
  *has_wscale = 0;
  size_t i = TCP_HDR_MIN_LEN;
  while (i < h->hdr_len) {
    uint8_t kind = seg[i];
    if (kind == 0)
      break;
    if (kind == 1) {
      i++;
      continue;
    }
    if (i + 1 >= h->hdr_len)
      break;
    uint8_t olen = seg[i + 1];
    if (olen < 2 || i + olen > h->hdr_len)
      break;
    if (kind == 2 && olen == 4)
      *mss_out = rd16be(seg + i + 2);
    if (kind == 3 && olen == 3) {
      uint8_t s = seg[i + 2];
      if (s > 14)
        s = 14;
      *wscale_out = s;
      *has_wscale = 1;
    }
    i += olen;
  }
}

// ---------------------------------------------------------------------------
// Checksum
// ---------------------------------------------------------------------------
int tcp_checksum_verify(uint32_t src_ip, uint32_t dst_ip, const uint8_t *seg,
                        size_t seg_len) {
  return inet_csum_pseudo_verify(src_ip, dst_ip, IP_PROTO_TCP, seg, seg_len);
}
uint16_t tcp_checksum(uint32_t src_ip, uint32_t dst_ip, const uint8_t *seg,
                      size_t seg_len) {
  return inet_csum_pseudo(src_ip, dst_ip, IP_PROTO_TCP, seg, seg_len);
}

// ---------------------------------------------------------------------------
// RTT estimation (RFC 6298 Jacobson/Karels)
// ---------------------------------------------------------------------------
static void tcp_rtt_update(tcp_conn_t *c, uint64_t rtt_ms) {
  if (rtt_ms == 0)
    rtt_ms = 1;

  if (!c->rtt_measured) {
    c->srtt_ms = (uint32_t)rtt_ms;
    c->rttvar_ms = (uint32_t)(rtt_ms / 2);
    c->rtt_measured = 1;
  } else {
    int64_t delta = (int64_t)rtt_ms - (int64_t)c->srtt_ms;
    if (delta < 0)
      delta = -delta;
    // RTTVAR = 3/4 * RTTVAR + 1/4 * |SRTT - R|
    c->rttvar_ms = (c->rttvar_ms * 3 + (uint32_t)delta) / 4;
    // SRTT = 7/8 * SRTT + 1/8 * R
    c->srtt_ms = (c->srtt_ms * 7 + (uint32_t)rtt_ms) / 8;
  }

  uint64_t rto = c->srtt_ms + 4 * c->rttvar_ms;
  if (rto < TCP_RTO_MIN_MS)
    rto = TCP_RTO_MIN_MS;
  if (rto > TCP_RTO_MAX_MS)
    rto = TCP_RTO_MAX_MS;
  c->rto_ms = (uint32_t)rto;
}

// ---------------------------------------------------------------------------
// Autotune
//
// target = 2 * rate_kbps * RTT_ms  (Linux: tcp_rcv_space_adjust)
// Así el buffer de recepción cubre exactamente 2 RTT de datos al rate
// que la app está consumiendo.
// ---------------------------------------------------------------------------
static void tcp_autotune_sample(tcp_conn_t *c, size_t delivered) {
  if (!c || delivered == 0)
    return;
  c->rcv_sample_bytes += delivered;

  uint64_t now = sched_get_ticks();
  if (c->rcv_sample_tick == 0) {
    c->rcv_sample_tick = now;
    return;
  }
  uint64_t elapsed = now - c->rcv_sample_tick;
  if (elapsed < TCP_AUTOTUNE_SAMPLE_MS)
    return;

  if (elapsed == 0)
    elapsed = 1;
  uint64_t instant_rate = c->rcv_sample_bytes / elapsed;
  // Guard: primer sample con pocos bytes (slow-start calentando), ignorar.
  if (c->rate_ewma_kbps == 0 && c->rcv_sample_bytes < (uint64_t)(4u * 1460u)) {
    c->rcv_sample_bytes = 0;
    c->rcv_sample_tick = now;
    return;
  }

  // [EWMA] Suavizar el rate. 7/8 del anterior + 1/8 del nuevo. Ignora
  // los picos de "curl no está leyendo" que disparan el rate a 0.
  //
  // Además, si el rate instantáneo es >2x o <0.5x el suavizado, lo
  // descartamos (outlier). Evita que un solo sample malo tumbe la
  // ventana.
  if (c->rate_ewma_kbps == 0) {
    c->rate_ewma_kbps = instant_rate;
  } else {
    uint64_t r = c->rate_ewma_kbps;
    if (instant_rate > r * 2) {
      instant_rate = r * 2;
    } else if (instant_rate * 2 < r) {
      instant_rate = r / 2;
    }
    c->rate_ewma_kbps = (r * 7 + instant_rate) / 8;
  }

  uint32_t rtt = c->srtt_ms;
  if (rtt < TCP_AUTOTUNE_RTT_FLOOR_MS)
    rtt = TCP_AUTOTUNE_RTT_FLOOR_MS;
  if (rtt > 2000)
    rtt = 2000;

  uint64_t target = 2 * c->rate_ewma_kbps * (uint64_t)rtt;

  uint32_t min_w = c->rcv_window_min;
  uint32_t max_w = c->rcv_window_max;
  if (target < min_w)
    target = min_w;
  if (target > max_w)
    target = max_w;

  unsigned long flags = spin_lock_irqsave(&c->rx_lock);
  uint32_t old = c->rcv_window_target;
  // Limitar bajada brusca: max 25% menos por sample (sube libre, baja suave).
  if (old > 0 && (uint32_t)target < old) {
    uint32_t floor75 = old - old / 4;
    if ((uint32_t)target < floor75)
      target = floor75;
    if (target < min_w)
      target = min_w;
  }
  c->rcv_window_target = (uint32_t)target;
  spin_unlock_irqrestore(&c->rx_lock, flags);

  if (old == 0 || target * 4 > old * 5 || target * 4 < old * 3) {
    LOG_DEBUG("[TCP-AUTOTUNE] %u->%u win=%u->%u KB rate_ewma=%lu KB/s inst=%lu "
              "KB/s rtt=%u ms",
              c->local_port, c->remote_port, (unsigned)(old / 1024),
              (unsigned)(target / 1024), (unsigned long)c->rate_ewma_kbps,
              (unsigned long)instant_rate, rtt);
  }

  c->rcv_sample_bytes = 0;
  c->rcv_sample_tick = now;
}

// Cuando se retransmite, la red está descartando. Bajar target un 25%.
static void tcp_autotune_backoff(tcp_conn_t *c) {
  unsigned long flags = spin_lock_irqsave(&c->rx_lock);
  uint32_t t = c->rcv_window_target;
  uint32_t new_t = t * 3 / 4;
  if (new_t < c->rcv_window_min)
    new_t = c->rcv_window_min;
  c->rcv_window_target = new_t;
  spin_unlock_irqrestore(&c->rx_lock, flags);
}

// ---------------------------------------------------------------------------
// Conexiones
// ---------------------------------------------------------------------------
tcp_conn_t *tcp_conn_alloc(void) {
  unsigned long flags = spin_lock_irqsave(&g_tcp_lock);
  if (g_conn_count >= TCP_CONN_MAX) {
    spin_unlock_irqrestore(&g_tcp_lock, flags);
    return NULL;
  }
  g_conn_count++;
  spin_unlock_irqrestore(&g_tcp_lock, flags);

  tcp_conn_t *c = (tcp_conn_t *)kzalloc(sizeof(*c));
  if (!c) {
    flags = spin_lock_irqsave(&g_tcp_lock);
    g_conn_count--;
    spin_unlock_irqrestore(&g_tcp_lock, flags);
    return NULL;
  }
  c->state = TCP_STATE_CLOSED;
  wait_queue_init(&c->handshake_wq);
  wait_queue_init(&c->rx_wq);
  spin_init(&c->rx_lock);

  // Congestión: arrancar con 4 MSS.
  c->cwnd = 4 * 1460;
  c->ssthresh = 65535;

  // Autotune: empezar pequeño, crecerá con el uso.
  c->rcv_window_target = TCP_RX_WINDOW_INIT;
  c->rcv_window_max = TCP_RX_BUFFER_BYTES;
  c->rcv_window_min = TCP_RX_WINDOW_MIN;
  c->rcv_sample_tick = 0;
  c->rcv_sample_bytes = 0;

  // RTT: sin sample inicial.
  c->srtt_ms = 0;
  c->rttvar_ms = 0;
  c->rto_ms = TCP_RTO_INITIAL_MS;
  c->rtt_measured = 0;
  c->rtt_probe_outstanding = 0;

  // Fast retransmit.
  c->dup_ack_count = 0;
  c->in_fast_recovery = 0;
  c->recover_seq = 0;
  c->rate_ewma_kbps = 0;

  return c;
}

static void tcp_conn_destroy(tcp_conn_t *c) {
  skb_t *r = c->rtx_head;
  while (r) {
    skb_t *n = r->next;
    skb_free(r);
    r = n;
  }
  c->rtx_head = c->rtx_tail = NULL;

  skb_t *p = c->rx_head;
  while (p) {
    skb_t *n = p->next;
    skb_free(p);
    p = n;
  }
  p = c->ooo_head;
  while (p) {
    skb_t *n = p->next;
    skb_free(p);
    p = n;
  }
  kfree(c);
}

void tcp_conn_free(tcp_conn_t *c) {
  if (!c)
    return;
  tcp_conn_remove(c);
  int freed = 0;
  tcp_conn_t *a = c->accept_head;
  while (a) {
    tcp_conn_t *n = a->accept_next;
    tcp_conn_remove(a);
    tcp_conn_destroy(a);
    freed++;
    a = n;
  }
  tcp_conn_destroy(c);
  freed++;
  unsigned long flags = spin_lock_irqsave(&g_tcp_lock);
  g_conn_count -= freed;
  if (g_conn_count < 0)
    g_conn_count = 0;
  spin_unlock_irqrestore(&g_tcp_lock, flags);
}

int tcp_conn_insert(tcp_conn_t *c) {
  if (!c || c->local_ip == 0 || c->local_port == 0)
    return -EINVAL;
  if (!c->is_listener && (c->remote_ip == 0 || c->remote_port == 0))
    return -EINVAL;

  unsigned h =
      tcp_hash(c->local_ip, c->local_port, c->remote_ip, c->remote_port);
  unsigned long flags = spin_lock_irqsave(&g_tcp_lock);
  for (tcp_conn_t *p = g_tcp_buckets[h]; p; p = p->next) {
    if (p == c) {
      spin_unlock_irqrestore(&g_tcp_lock, flags);
      return 0;
    }
    if (p->local_ip == c->local_ip && p->local_port == c->local_port &&
        p->remote_ip == c->remote_ip && p->remote_port == c->remote_port) {
      spin_unlock_irqrestore(&g_tcp_lock, flags);
      return -EEXIST;
    }
  }
  c->next = g_tcp_buckets[h];
  g_tcp_buckets[h] = c;
  spin_unlock_irqrestore(&g_tcp_lock, flags);
  return 0;
}

void tcp_conn_remove(tcp_conn_t *c) {
  if (!c)
    return;
  unsigned h =
      tcp_hash(c->local_ip, c->local_port, c->remote_ip, c->remote_port);
  unsigned long flags = spin_lock_irqsave(&g_tcp_lock);
  tcp_conn_t **pp = &g_tcp_buckets[h];
  while (*pp) {
    if (*pp == c) {
      *pp = c->next;
      c->next = NULL;
      break;
    }
    pp = &(*pp)->next;
  }
  spin_unlock_irqrestore(&g_tcp_lock, flags);
}

tcp_conn_t *tcp_conn_lookup(uint32_t lip, uint16_t lp, uint32_t rip,
                            uint16_t rp) {
  unsigned h = tcp_hash(lip, lp, rip, rp);
  unsigned long flags = spin_lock_irqsave(&g_tcp_lock);
  tcp_conn_t *found = NULL;
  for (tcp_conn_t *p = g_tcp_buckets[h]; p; p = p->next) {
    if (p->local_ip == lip && p->local_port == lp && p->remote_ip == rip &&
        p->remote_port == rp) {
      found = p;
      break;
    }
  }
  spin_unlock_irqrestore(&g_tcp_lock, flags);
  return found;
}

static tcp_conn_t *tcp_listener_lookup(uint32_t lip, uint16_t lp) {
  unsigned h = tcp_hash(lip, lp, 0, 0);
  unsigned long flags = spin_lock_irqsave(&g_tcp_lock);
  tcp_conn_t *found = NULL;
  for (tcp_conn_t *p = g_tcp_buckets[h]; p; p = p->next) {
    if (p->is_listener && p->local_ip == lip && p->local_port == lp) {
      found = p;
      break;
    }
  }
  spin_unlock_irqrestore(&g_tcp_lock, flags);
  return found;
}

void tcp_flush(void) {
  unsigned long flags = spin_lock_irqsave(&g_tcp_lock);
  for (int i = 0; i < TCP_HASH_BUCKETS; i++) {
    tcp_conn_t *p = g_tcp_buckets[i];
    while (p) {
      tcp_conn_t *n = p->next;
      skb_t *s = p->rx_head;
      while (s) {
        skb_t *sn = s->next;
        skb_free(s);
        s = sn;
      }
      s = p->ooo_head;
      while (s) {
        skb_t *sn = s->next;
        skb_free(s);
        s = sn;
      }
      s = p->rtx_head;
      while (s) {
        skb_t *sn = s->next;
        skb_free(s);
        s = sn;
      }
      kfree(p);
      p = n;
    }
    g_tcp_buckets[i] = NULL;
  }
  g_conn_count = 0;
  spin_unlock_irqrestore(&g_tcp_lock, flags);
}

// ---------------------------------------------------------------------------
// Envío de segmento
// ---------------------------------------------------------------------------
static int tcp_send_segment_ex(tcp_conn_t *c, uint16_t flags, uint32_t seq,
                               uint32_t ack, const void *payload,
                               size_t payload_len, int do_enqueue) {
  size_t hdr_len = TCP_HDR_MIN_LEN + ((flags & TCP_SYN) ? 8 : 0);
  size_t seg_len = hdr_len + payload_len;
  skb_t *sk = skb_alloc();
  if (!sk)
    return -ENOMEM;

  uint8_t *seg = (uint8_t *)skb_put(sk, seg_len);
  if (!seg) {
    skb_free(sk);
    return -ENOMEM;
  }
  memset(seg, 0, hdr_len);

  seg[0] = (uint8_t)(c->local_port >> 8);
  seg[1] = (uint8_t)(c->local_port & 0xFF);
  seg[2] = (uint8_t)(c->remote_port >> 8);
  seg[3] = (uint8_t)(c->remote_port & 0xFF);
  seg[4] = (uint8_t)(seq >> 24);
  seg[5] = (uint8_t)(seq >> 16);
  seg[6] = (uint8_t)(seq >> 8);
  seg[7] = (uint8_t)(seq);
  seg[8] = (uint8_t)(ack >> 24);
  seg[9] = (uint8_t)(ack >> 16);
  seg[10] = (uint8_t)(ack >> 8);
  seg[11] = (uint8_t)(ack);
  seg[12] = (uint8_t)((hdr_len / 4) << 4);
  seg[13] = (uint8_t)(flags & 0xFF);
  if (flags & TCP_NS)
    seg[12] |= 0x01;

  uint16_t wire_window = tcp_rx_window_wire(c, (flags & TCP_SYN) != 0);
  seg[14] = (uint8_t)(wire_window >> 8);
  seg[15] = (uint8_t)(wire_window & 0xFF);

  if (flags & TCP_SYN) {
    seg[20] = 2; // MSS
    seg[21] = 4;
    seg[22] = (uint8_t)(TCP_RX_MSS >> 8);
    seg[23] = (uint8_t)(TCP_RX_MSS & 0xFF);
    seg[24] = 3; // WSCALE
    seg[25] = 3;
    seg[26] = TCP_WSCALE_SHIFT;
    seg[27] = 1; // NOP
  }

  if (payload_len)
    memcpy(seg + hdr_len, payload, payload_len);

  uint16_t csum = tcp_checksum(c->local_ip, c->remote_ip, seg, seg_len);
  seg[16] = (uint8_t)(csum >> 8);
  seg[17] = (uint8_t)(csum & 0xFF);

  // Copia para la cola de retransmisión.
  skb_t *copy = NULL;
  if (do_enqueue && payload_len > 0) {
    copy = skb_alloc();
    if (copy) {
      uint8_t *cp = (uint8_t *)skb_put(copy, seg_len);
      if (cp) {
        memcpy(cp, seg, seg_len);
        copy->tcp_seq = seq;
        copy->next = NULL;
      } else {
        skb_free(copy);
        copy = NULL;
      }
    }
  }

  // [RTT] Registrar el probe antes de enviar.
  if (payload_len > 0 && c->rtt_probe_outstanding == 0) {
    c->rtt_probe_seq = seq + (uint32_t)payload_len;
    c->rtt_probe_tick = sched_get_ticks();
    c->rtt_probe_outstanding = 1;
  }

  int rc = ip_output(sk, c->local_ip, c->remote_ip, IP_PROTO_TCP);
  if (rc != 0) {
    LOG_DEBUG("[TCP] ip_output falló en flag=0x%x", flags);
    if (copy)
      skb_free(copy);
    return rc;
  }

  if (copy) {
    unsigned long rf = spin_lock_irqsave(&c->rx_lock);
    if (c->rtx_tail)
      c->rtx_tail->next = copy;
    else
      c->rtx_head = copy;
    c->rtx_tail = copy;
    c->rtx_count++;
    spin_unlock_irqrestore(&c->rx_lock, rf);
  }

  unsigned long rx_flags = spin_lock_irqsave(&c->rx_lock);
  c->rcv_last_window = wire_window;
  c->pending_window_update = 0;
  spin_unlock_irqrestore(&c->rx_lock, rx_flags);
  return 0;
}

static int tcp_send_segment(tcp_conn_t *c, uint16_t flags, uint32_t seq,
                            uint32_t ack, const void *payload,
                            size_t payload_len) {
  return tcp_send_segment_ex(c, flags, seq, ack, payload, payload_len, 1);
}

static bool tcp_handshake_done(void *arg) {
  tcp_conn_t *c = (tcp_conn_t *)arg;
  return c->handshake_ok != 0;
}

static int tcp_seq_before(uint32_t a, uint32_t b) {
  return (int32_t)(a - b) < 0;
}

// ---------------------------------------------------------------------------
// Reensamblado RX
// ---------------------------------------------------------------------------
static void tcp_rx_append(tcp_conn_t *c, skb_t *skb) {
  skb->next = NULL;
  if (c->rx_tail)
    c->rx_tail->next = skb;
  else
    c->rx_head = skb;
  c->rx_tail = skb;
  c->rx_count++;
}

static void tcp_rx_drain_ooo_locked(tcp_conn_t *c) {
  while (c->ooo_head) {
    skb_t *next = c->ooo_head;
    size_t queued_len = skb_len(next);
    int32_t offset = (int32_t)(next->tcp_seq - c->rcv_nxt);
    if (offset > 0)
      break;
    c->ooo_head = next->next;
    c->ooo_count--;
    if (offset < 0) {
      size_t overlap = (size_t)(c->rcv_nxt - next->tcp_seq);
      if (overlap >= queued_len) {
        c->rx_buffered_bytes -= queued_len;
        c->rx_total_queued -= queued_len;
        skb_free(next);
        continue;
      }
      skb_pull(next, overlap);
      c->rx_buffered_bytes -= overlap;
      c->rx_total_queued -= overlap;
      next->tcp_seq = c->rcv_nxt;
    }
    size_t remaining = skb_len(next);
    c->rcv_nxt += (uint32_t)remaining;
    tcp_rx_append(c, next);
  }
}

// ---------------------------------------------------------------------------
// Ventana de recepción: real + wire
// ---------------------------------------------------------------------------
static size_t tcp_rx_window(tcp_conn_t *c) {
  unsigned long flags = spin_lock_irqsave(&c->rx_lock);
  size_t buffered = c->rx_buffered_bytes;
  size_t free_bytes =
      buffered < TCP_RX_BUFFER_BYTES ? TCP_RX_BUFFER_BYTES - buffered : 0;
  int used_skbs = c->rx_count + c->ooo_count;
  size_t free_skbs =
      used_skbs < TCP_RX_MAX_SKBS ? (size_t)(TCP_RX_MAX_SKBS - used_skbs) : 0;
  size_t free_slots_bytes = free_skbs * TCP_RX_MSS;
  uint32_t target = c->rcv_window_target;
  spin_unlock_irqrestore(&c->rx_lock, flags);

  size_t window = free_bytes < free_slots_bytes ? free_bytes : free_slots_bytes;
  if (window > target)
    window = target;
  return window;
}

static uint16_t tcp_rx_window_wire(tcp_conn_t *c, int is_syn) {
  size_t raw = tcp_rx_window(c);
  if (is_syn || !c->wscale_negotiated) {
    return raw > 65535u ? 65535u : (uint16_t)raw;
  }
  uint16_t w = (uint16_t)(raw >> c->rcv_wscale);
  if (w == 0 && raw > 0)
    w = 1;
  return w;
}

static void tcp_send_window_update(tcp_conn_t *c) {
  if (c->state != TCP_STATE_ESTABLISHED && c->state != TCP_STATE_CLOSE_WAIT)
    return;

  uint16_t wire_window = tcp_rx_window_wire(c, /*is_syn=*/0);

  unsigned long flags = spin_lock_irqsave(&c->rx_lock);
  uint16_t last_window = c->rcv_last_window;
  int pending = c->pending_window_update;
  spin_unlock_irqrestore(&c->rx_lock, flags);

  if (!pending && wire_window <= last_window)
    return;

  int rc = tcp_send_segment(c, TCP_ACK, c->snd_nxt, c->rcv_nxt, NULL, 0);
  if (rc != 0) {
    LOG_WARN("[TCP] window update falló (rc=%d)", rc);
    flags = spin_lock_irqsave(&c->rx_lock);
    c->pending_window_update = 1;
    spin_unlock_irqrestore(&c->rx_lock, flags);
  }
}

// ---------------------------------------------------------------------------
// FIN
// ---------------------------------------------------------------------------
static int tcp_consume_fin(tcp_conn_t *c, uint32_t fin_seq) {
  unsigned long flags = spin_lock_irqsave(&c->rx_lock);
  if (fin_seq != c->rcv_nxt) {
    spin_unlock_irqrestore(&c->rx_lock, flags);
    return 0;
  }
  c->rcv_nxt++;
  c->peer_closed = 1;
  if (c->state == TCP_STATE_ESTABLISHED)
    c->state = TCP_STATE_CLOSE_WAIT;
  spin_unlock_irqrestore(&c->rx_lock, flags);
  wake_up_all(&c->rx_wq);
  return 1;
}

// ---------------------------------------------------------------------------
// Encolar payload recibido (con reensamblado OOO)
// ---------------------------------------------------------------------------
static int tcp_rx_queue_payload(tcp_conn_t *c, skb_t *skb, uint32_t seq,
                                size_t payload_len, size_t tcp_hdr_len) {
  unsigned long flags = spin_lock_irqsave(&c->rx_lock);
  int32_t offset = (int32_t)(seq - c->rcv_nxt);
  if (offset < 0) {
    size_t overlap = (size_t)(c->rcv_nxt - seq);
    if (overlap >= payload_len) {
      spin_unlock_irqrestore(&c->rx_lock, flags);
      return 0;
    }
    skb_pull(skb, tcp_hdr_len + overlap);
    payload_len -= overlap;
    seq = c->rcv_nxt;
  } else {
    skb_pull(skb, tcp_hdr_len);
  }

  if (c->rx_buffered_bytes >= TCP_RX_BUFFER_BYTES ||
      payload_len > TCP_RX_BUFFER_BYTES - c->rx_buffered_bytes ||
      c->rx_count + c->ooo_count >= TCP_RX_MAX_SKBS) {
    spin_unlock_irqrestore(&c->rx_lock, flags);
    return 0;
  }

  if (seq == c->rcv_nxt) {
    c->rx_buffered_bytes += payload_len;
    c->rx_total_queued += payload_len;
    c->rcv_nxt += (uint32_t)payload_len;
    tcp_rx_append(c, skb);
    tcp_rx_drain_ooo_locked(c);
    spin_unlock_irqrestore(&c->rx_lock, flags);
    return 1;
  }

  uint32_t distance = seq - c->rcv_nxt;
  if (distance >= 65535u || c->ooo_count >= 64) {
    spin_unlock_irqrestore(&c->rx_lock, flags);
    return 0;
  }

  skb_t **link = &c->ooo_head;
  while (*link && tcp_seq_before((*link)->tcp_seq, seq)) {
    size_t queued_len = skb_len(*link);
    uint32_t queued_distance = (*link)->tcp_seq - c->rcv_nxt;
    if (distance < queued_distance + queued_len) {
      spin_unlock_irqrestore(&c->rx_lock, flags);
      return 0;
    }
    link = &(*link)->next;
  }
  if (*link) {
    size_t incoming_len = payload_len;
    uint32_t queued_distance = (*link)->tcp_seq - c->rcv_nxt;
    if (distance + incoming_len > queued_distance) {
      spin_unlock_irqrestore(&c->rx_lock, flags);
      return 0;
    }
  }

  skb->tcp_seq = seq;
  skb->next = *link;
  *link = skb;
  c->ooo_count++;
  c->rx_buffered_bytes += payload_len;
  c->rx_total_queued += payload_len;
  spin_unlock_irqrestore(&c->rx_lock, flags);
  return 1;
}

// ---------------------------------------------------------------------------
// Handshake
// ---------------------------------------------------------------------------
static void tcp_handle_syn_sent(tcp_conn_t *c, skb_t *skb, const tcp_hdr_t *h,
                                uint32_t src_ip) {
  (void)src_ip;

  if (h->flags & TCP_RST) {
    LOG_DEBUG("[TCP] connect: RST recibido");
    c->handshake_ok = -ECONNREFUSED;
    wake_up_all(&c->handshake_wq);
    skb_free(skb);
    return;
  }

  if ((h->flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK)) {
    if (h->ack != c->snd_nxt) {
      LOG_DEBUG("[TCP] connect: SYN-ACK con ack=%u, esperado %u", h->ack,
                c->snd_una);
      skb_free(skb);
      return;
    }
    // [RTT] Sample inicial del handshake.
    if (c->syn_sent_tick) {
      uint64_t rtt = sched_get_ticks() - c->syn_sent_tick;
      tcp_rtt_update(c, rtt);
    }

    c->rcv_nxt = h->seq + 1;
    c->snd_una = h->ack;
    c->snd_nxt = h->ack;

    uint16_t mss = 0;
    uint8_t wscale = 0;
    int has_ws = 0;
    tcp_parse_syn_opts(skb_data(skb), h, &mss, &wscale, &has_ws);
    c->peer_mss = mss;
    c->peer_window = h->window;
    c->peer_window_tick = sched_get_ticks();

    if (has_ws) {
      c->snd_wscale = wscale;
      c->rcv_wscale = TCP_WSCALE_SHIFT;
      c->wscale_negotiated = 1;
    } else {
      c->snd_wscale = 0;
      c->rcv_wscale = 0;
      c->wscale_negotiated = 0;
    }

    uint16_t mss0 = c->peer_mss ? c->peer_mss : 1460;
    c->cwnd = (uint32_t)mss0 * 4;
    c->ssthresh = 65535;
    c->last_ack_tick = sched_get_ticks();
    c->rtx_consec_retries = 0;

    c->state = TCP_STATE_ESTABLISHED;
    c->handshake_ok = 1;

    (void)tcp_send_segment(c, TCP_ACK, c->snd_nxt, c->rcv_nxt, NULL, 0);

    LOG_INFO("[TCP] conectado %u.%u.%u.%u:%u -> %u.%u.%u.%u:%u "
             "(peer_win=%u mss=%u cwnd=%u ws=%d/%d srtt=%u rto=%u)",
             (c->local_ip >> 24) & 0xFF, (c->local_ip >> 16) & 0xFF,
             (c->local_ip >> 8) & 0xFF, c->local_ip & 0xFF, c->local_port,
             (c->remote_ip >> 24) & 0xFF, (c->remote_ip >> 16) & 0xFF,
             (c->remote_ip >> 8) & 0xFF, c->remote_ip & 0xFF, c->remote_port,
             c->peer_window, c->peer_mss, c->cwnd,
             c->wscale_negotiated ? c->rcv_wscale : 0,
             c->wscale_negotiated ? c->snd_wscale : 0, c->srtt_ms, c->rto_ms);

    wake_up_all(&c->handshake_wq);
    skb_free(skb);
    return;
  }

  LOG_DEBUG("[TCP] connect: flags 0x%x inesperados en SYN_SENT", h->flags);
  skb_free(skb);
}

static void tcp_handle_listen(tcp_conn_t *listener, skb_t *skb,
                              const tcp_hdr_t *h, uint32_t src_ip) {
  tcp_conn_t *child = tcp_conn_alloc();
  if (!child) {
    skb_free(skb);
    return;
  }

  child->local_ip = listener->local_ip;
  child->local_port = listener->local_port;
  child->remote_ip = src_ip;
  child->remote_port = h->sport;
  child->snd_isn = tcp_pick_isn();
  child->snd_una = child->snd_isn;
  child->snd_nxt = child->snd_isn + 1;
  child->rcv_nxt = h->seq + 1;
  child->syn_sent_tick = sched_get_ticks();

  uint16_t mss = 0;
  uint8_t wscale = 0;
  int has_ws = 0;
  tcp_parse_syn_opts(skb_data(skb), h, &mss, &wscale, &has_ws);
  child->peer_mss = mss;
  child->peer_window = h->window;
  child->peer_window_tick = sched_get_ticks();
  if (has_ws) {
    child->snd_wscale = wscale;
    child->rcv_wscale = TCP_WSCALE_SHIFT;
    child->wscale_negotiated = 1;
  } else {
    child->snd_wscale = 0;
    child->rcv_wscale = 0;
    child->wscale_negotiated = 0;
  }

  uint16_t mss0 = child->peer_mss ? child->peer_mss : 1460;
  child->cwnd = (uint32_t)mss0 * 4;
  child->ssthresh = 65535;
  child->state = TCP_STATE_SYN_RECEIVED;
  child->parent_listener = listener;

  if (tcp_conn_insert(child) != 0) {
    tcp_conn_free(child);
    skb_free(skb);
    return;
  }

  int rc = tcp_send_segment(child, TCP_SYN | TCP_ACK, child->snd_isn,
                            child->rcv_nxt, NULL, 0);
  if (rc != 0) {
    tcp_conn_remove(child);
    tcp_conn_free(child);
    skb_free(skb);
    return;
  }

  LOG_DEBUG("[TCP] listen: SYN de %u.%u.%u.%u:%u, SYN-ACK enviado",
            (src_ip >> 24) & 0xFF, (src_ip >> 16) & 0xFF, (src_ip >> 8) & 0xFF,
            src_ip & 0xFF, h->sport);
  skb_free(skb);
}

static void tcp_handle_syn_received(tcp_conn_t *c, skb_t *skb,
                                    const tcp_hdr_t *h, uint32_t src_ip) {
  (void)src_ip;

  if (h->flags & TCP_RST) {
    LOG_DEBUG("[TCP] SYN_RECEIVED: RST, abortando");
    c->state = TCP_STATE_CLOSED;
    skb_free(skb);
    return;
  }
  if (!(h->flags & TCP_ACK)) {
    skb_free(skb);
    return;
  }
  if (h->ack != c->snd_nxt) {
    LOG_DEBUG("[TCP] SYN_RECEIVED: ack=%u, esperado %u", h->ack, c->snd_una);
    skb_free(skb);
    return;
  }

  // [RTT] Sample desde el SYN-ACK → ACK.
  if (c->syn_sent_tick) {
    uint64_t rtt = sched_get_ticks() - c->syn_sent_tick;
    tcp_rtt_update(c, rtt);
  }

  c->snd_una = h->ack;
  c->snd_nxt = h->ack;

  uint32_t w = h->window;
  if (c->wscale_negotiated)
    w <<= c->snd_wscale;
  if (w > TCP_PEER_WINDOW_MAX)
    w = TCP_PEER_WINDOW_MAX;
  c->peer_window = w;
  c->peer_window_tick = sched_get_ticks();
  c->last_ack_tick = sched_get_ticks();
  c->state = TCP_STATE_ESTABLISHED;

  tcp_conn_t *lis = c->parent_listener;
  if (lis) {
    unsigned long flags = spin_lock_irqsave(&g_tcp_lock);
    if (lis->accept_count >= TCP_ACCEPT_MAX) {
      spin_unlock_irqrestore(&g_tcp_lock, flags);
      LOG_WARN("[TCP] accept queue llena, drop conn");
      c->state = TCP_STATE_CLOSED;
      skb_free(skb);
      return;
    }
    c->accept_next = NULL;
    if (lis->accept_tail)
      lis->accept_tail->accept_next = c;
    else
      lis->accept_head = c;
    lis->accept_tail = c;
    lis->accept_count++;
    spin_unlock_irqrestore(&g_tcp_lock, flags);
    wake_up_all(&lis->accept_wq);
  }

  LOG_INFO("[TCP] aceptada conexión local=%u.%u.%u.%u:%u remote=%u.%u.%u.%u:%u",
           (c->local_ip >> 24) & 0xFF, (c->local_ip >> 16) & 0xFF,
           (c->local_ip >> 8) & 0xFF, c->local_ip & 0xFF, c->local_port,
           (c->remote_ip >> 24) & 0xFF, (c->remote_ip >> 16) & 0xFF,
           (c->remote_ip >> 8) & 0xFF, c->remote_ip & 0xFF, c->remote_port);
  skb_free(skb);
}

// ---------------------------------------------------------------------------
// Fast retransmit + fast recovery (NewReno)
// ---------------------------------------------------------------------------
static void tcp_fast_retransmit(tcp_conn_t *c) {
  unsigned long rf = spin_lock_irqsave(&c->rx_lock);
  skb_t *v = c->rtx_head;
  if (!v) {
    spin_unlock_irqrestore(&c->rx_lock, rf);
    return;
  }
  uint8_t *raw = skb_data(v);
  size_t raw_len = skb_len(v);
  uint8_t doff = (raw[12] >> 4) & 0x0F;
  size_t h2 = (size_t)doff * 4;
  size_t plen = raw_len - h2;
  uint8_t *payload = raw + h2;
  uint32_t seq = v->tcp_seq;

  // Actualizar ssthresh/cwnd (RFC 5681).
  uint16_t mss = c->peer_mss ? c->peer_mss : 1460;
  if (mss > TCP_RX_MSS)
    mss = TCP_RX_MSS;
  uint32_t flight = c->snd_nxt - c->snd_una;
  uint32_t new_ssthresh = flight / 2;
  if (new_ssthresh < 2u * mss)
    new_ssthresh = 2u * mss;
  c->ssthresh = new_ssthresh;
  c->cwnd = new_ssthresh + 3u * mss;
  c->in_fast_recovery = 1;
  c->recover_seq = c->snd_nxt;
  c->last_ack_tick = sched_get_ticks();
  c->rtx_consec_retries++;
  spin_unlock_irqrestore(&c->rx_lock, rf);

  LOG_DEBUG("[TCP] fast retransmit seq=%u len=%lu cwnd=%u ssthresh=%u", seq,
            (unsigned long)plen, c->cwnd, c->ssthresh);

  (void)tcp_send_segment_ex(c, TCP_ACK | TCP_PSH, seq, c->rcv_nxt, payload,
                            plen, /*do_enqueue=*/0);
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------
static void tcp_input(skb_t *skb, uint32_t src_ip, uint32_t dst_ip,
                      uint8_t proto) {
  (void)proto;
  if (!skb)
    return;

  size_t len = skb_len(skb);
  tcp_hdr_t hdr;
  if (tcp_parse_header(skb_data(skb), len, &hdr) != 0) {
    LOG_DEBUG("[TCP] drop: header mal formado (len=%lu)", (unsigned long)len);
    skb_free(skb);
    return;
  }
  if (tcp_checksum_verify(src_ip, dst_ip, skb_data(skb), len) != 0) {
    LOG_DEBUG("[TCP] drop: checksum malo");
    skb_free(skb);
    return;
  }

  tcp_conn_t *c = tcp_conn_lookup(dst_ip, hdr.dport, src_ip, hdr.sport);
  if (!c) {
    tcp_conn_t *lis = tcp_listener_lookup(dst_ip, hdr.dport);
    if (lis && (hdr.flags & TCP_SYN) && !(hdr.flags & TCP_ACK)) {
      tcp_handle_listen(lis, skb, &hdr, src_ip);
      return;
    }
    skb_free(skb);
    return;
  }

  if (c->state == TCP_STATE_SYN_SENT) {
    tcp_handle_syn_sent(c, skb, &hdr, src_ip);
    return;
  }
  if (c->state == TCP_STATE_SYN_RECEIVED) {
    tcp_handle_syn_received(c, skb, &hdr, src_ip);
    return;
  }

  if (c->state == TCP_STATE_ESTABLISHED || c->state == TCP_STATE_CLOSE_WAIT) {
    if (hdr.flags & TCP_RST) {
      unsigned long rx_flags = spin_lock_irqsave(&c->rx_lock);
      c->state = TCP_STATE_CLOSED;
      c->peer_closed = 1;
      spin_unlock_irqrestore(&c->rx_lock, rx_flags);
      wake_up_all(&c->rx_wq);
      wake_up_all(&c->handshake_wq);
      skb_free(skb);
      return;
    }

    size_t payload_len = len - hdr.hdr_len;

    // Procesar ACK.
    if (hdr.flags & TCP_ACK) {
      unsigned long rx_flags = spin_lock_irqsave(&c->rx_lock);
      int acked_new = 0;
      int is_dup_ack = 0;
      if (hdr.ack > c->snd_una && hdr.ack <= c->snd_nxt) {
        c->snd_una = hdr.ack;
        c->last_ack_tick = sched_get_ticks();
        c->rtx_consec_retries = 0;
        c->dup_ack_count = 0;
        acked_new = 1;

        // [RTT] Karn's algorithm: solo muestrear si no hubo retransmisión.
        if (c->rtt_probe_outstanding &&
            (int32_t)(hdr.ack - c->rtt_probe_seq) >= 0 &&
            c->rtx_total_retries == 0) {
          uint64_t rtt = sched_get_ticks() - c->rtt_probe_tick;
          c->rtt_probe_outstanding = 0;
          spin_unlock_irqrestore(&c->rx_lock, rx_flags);
          tcp_rtt_update(c, rtt);
          rx_flags = spin_lock_irqsave(&c->rx_lock);
        }

        // CWND: slow start vs congestion avoidance.
        uint16_t mss = c->peer_mss ? c->peer_mss : 1460;
        if (c->cwnd < c->ssthresh) {
          c->cwnd += mss;
        } else {
          uint32_t inc = (uint32_t)((uint64_t)mss * mss / c->cwnd);
          if (inc < 1)
            inc = 1;
          c->cwnd += inc;
        }
        if (c->cwnd > TCP_PEER_WINDOW_MAX)
          c->cwnd = TCP_PEER_WINDOW_MAX;
      } else if (hdr.ack == c->snd_una && c->snd_una != c->snd_nxt &&
                 payload_len == 0 &&
                 !(hdr.flags & (TCP_SYN | TCP_FIN | TCP_RST))) {
        // Dup ACK puro.
        is_dup_ack = 1;
        c->dup_ack_count++;
      }

      // Actualizar ventana del peer.
      uint32_t w = hdr.window;
      if (c->wscale_negotiated && !(hdr.flags & TCP_SYN))
        w <<= c->snd_wscale;
      if (w > TCP_PEER_WINDOW_MAX)
        w = TCP_PEER_WINDOW_MAX;
      c->peer_window = w;
      c->peer_window_tick = sched_get_ticks();
      spin_unlock_irqrestore(&c->rx_lock, rx_flags);

      // Fast retransmit.
      if (is_dup_ack && c->dup_ack_count == TCP_DUP_ACK_THRESHOLD &&
          !c->in_fast_recovery && c->rtx_head) {
        tcp_fast_retransmit(c);
      } else if (is_dup_ack && c->in_fast_recovery) {
        // Fast recovery: cada dup ACK extra infla cwnd en 1 MSS.
        uint16_t mss = c->peer_mss ? c->peer_mss : 1460;
        unsigned long f = spin_lock_irqsave(&c->rx_lock);
        c->cwnd += mss;
        if (c->cwnd > TCP_PEER_WINDOW_MAX)
          c->cwnd = TCP_PEER_WINDOW_MAX;
        spin_unlock_irqrestore(&c->rx_lock, f);
      }

      // Fast recovery exit.
      if (acked_new && c->in_fast_recovery) {
        unsigned long f = spin_lock_irqsave(&c->rx_lock);
        if ((int32_t)(c->snd_una - c->recover_seq) >= 0) {
          c->in_fast_recovery = 0;
          c->cwnd = c->ssthresh;
        }
        spin_unlock_irqrestore(&c->rx_lock, f);
      }

      // Liberar de la cola RTX lo ACKeado.
      if (acked_new) {
        unsigned long rxf = spin_lock_irqsave(&c->rx_lock);
        while (c->rtx_head) {
          skb_t *v = c->rtx_head;
          uint8_t *raw = skb_data(v);
          size_t raw_len = skb_len(v);
          uint8_t doff = (raw[12] >> 4) & 0x0F;
          size_t h2 = (size_t)doff * 4;
          size_t plen = raw_len - h2;
          uint32_t end_seq = v->tcp_seq + (uint32_t)plen;
          if ((int32_t)(hdr.ack - end_seq) >= 0) {
            c->rtx_head = v->next;
            if (!c->rtx_head)
              c->rtx_tail = NULL;
            c->rtx_count--;
            spin_unlock_irqrestore(&c->rx_lock, rxf);
            skb_free(v);
            rxf = spin_lock_irqsave(&c->rx_lock);
          } else {
            break;
          }
        }
        spin_unlock_irqrestore(&c->rx_lock, rxf);
        wake_up_all(&c->rx_wq);
      }
    }

    // Procesar payload.
    int queued = 0;
    if (payload_len > 0) {
      queued = tcp_rx_queue_payload(c, skb, hdr.seq, payload_len, hdr.hdr_len);
      if (!__atomic_exchange_n(&c->trace_rx_payload_logged, 1,
                               __ATOMIC_RELAXED)) {
        LOG_INFO("[TCP] primer payload RX local=%u remote=%u seq=%u "
                 "len=%lu rcv_nxt=%u queued=%d",
                 c->local_port, c->remote_port, hdr.seq,
                 (unsigned long)payload_len, c->rcv_nxt, queued);
      }
      int ack_rc =
          tcp_send_segment(c, TCP_ACK, c->snd_nxt, c->rcv_nxt, NULL, 0);
      if (ack_rc != 0 && !__atomic_exchange_n(&c->trace_ack_error_logged, 1,
                                              __ATOMIC_RELAXED)) {
        LOG_WARN("[TCP] ACK TX falló rc=%d", ack_rc);
      }
      if (queued)
        wake_up_all(&c->rx_wq);
    }

    uint32_t fin_seq = hdr.seq + (uint32_t)payload_len;

    if (c->pending_fin && c->pending_fin_seq == c->rcv_nxt) {
      uint32_t fs = c->pending_fin_seq;
      c->pending_fin = 0;
      (void)tcp_consume_fin(c, fs);
      (void)tcp_send_segment(c, TCP_ACK, c->snd_nxt, c->rcv_nxt, NULL, 0);
    }

    if (hdr.flags & TCP_FIN) {
      if (tcp_consume_fin(c, fin_seq)) {
        (void)tcp_send_segment(c, TCP_ACK, c->snd_nxt, c->rcv_nxt, NULL, 0);
      } else if (tcp_seq_before(c->rcv_nxt, fin_seq) &&
                 (!c->pending_fin ||
                  tcp_seq_before(fin_seq, c->pending_fin_seq))) {
        c->pending_fin_seq = fin_seq;
        c->pending_fin = 1;
        (void)tcp_send_segment(c, TCP_ACK, c->snd_nxt, c->rcv_nxt, NULL, 0);
      } else {
        (void)tcp_send_segment(c, TCP_ACK, c->snd_nxt, c->rcv_nxt, NULL, 0);
      }
      LOG_INFO("[TCP] peer cerró conexión (FIN) local=%u remote=%u",
               c->local_port, c->remote_port);
    }

    if (!queued)
      skb_free(skb);
    return;
  }

  if (c->handler) {
    c->handler(c, skb, &hdr, src_ip, dst_ip);
    return;
  }
  skb_free(skb);
}

// ---------------------------------------------------------------------------
// tcp_connect
// ---------------------------------------------------------------------------
static uint32_t tcp_pick_isn(void) {
  uint32_t lo, hi;
  __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
  uint32_t isn = lo ^ (hi * 0x9E3779B1u);
  if (isn == 0)
    isn = 1;
  return isn;
}

int tcp_connect(tcp_conn_t *c, uint32_t remote_ip, uint16_t remote_port,
                uint64_t timeout_ms) {
  if (!c || remote_ip == 0 || remote_port == 0)
    return -EINVAL;

  c->remote_ip = remote_ip;
  c->remote_port = remote_port;
  c->snd_isn = tcp_pick_isn();
  c->snd_una = c->snd_isn;
  c->snd_nxt = c->snd_isn + 1;
  c->rcv_nxt = 0;
  c->handshake_ok = 0;
  c->state = TCP_STATE_SYN_SENT;
  c->peer_window = 65535;
  c->cwnd = 4 * 1460;
  c->ssthresh = 65535;
  c->last_ack_tick = 0;
  c->rtx_consec_retries = 0;
  c->rtx_total_retries = 0;
  c->pending_window_update = 0;
  c->wscale_negotiated = 0;
  c->snd_wscale = 0;
  c->rcv_wscale = 0;
  c->syn_sent_tick = sched_get_ticks();
  // Inicializar sample tick para que el primer elapsed sea controlado.
  c->rcv_sample_tick = sched_get_ticks();
  c->rcv_sample_bytes = 0;
  c->rate_ewma_kbps = 0;

  int rc = tcp_conn_insert(c);
  if (rc != 0) {
    c->state = TCP_STATE_CLOSED;
    return rc;
  }

  if (tcp_send_segment(c, TCP_SYN, c->snd_isn, 0, NULL, 0) != 0) {
    tcp_conn_remove(c);
    c->state = TCP_STATE_CLOSED;
    return -ENOMEM;
  }

  uint64_t deadline = sched_get_ticks() + timeout_ms;
  uint64_t syn_rto = TCP_RTO_INITIAL_MS;
  uint64_t retry_at = sched_get_ticks() + syn_rto;
  while (c->handshake_ok == 0) {
    uint64_t now = sched_get_ticks();
    if (now >= deadline) {
      LOG_WARN("[TCP] connect timeout");
      tcp_conn_remove(c);
      c->state = TCP_STATE_CLOSED;
      return -ETIMEDOUT;
    }
    if (now >= retry_at) {
      (void)tcp_send_segment(c, TCP_SYN, c->snd_isn, 0, NULL, 0);
      syn_rto = syn_rto * 2;
      if (syn_rto > TCP_RTO_MAX_MS)
        syn_rto = TCP_RTO_MAX_MS;
      retry_at = now + syn_rto;
      continue;
    }
    uint64_t wait = deadline - now;
    if (wait > 100)
      wait = 100;
    if (retry_at - now < wait)
      wait = retry_at - now;
    wait_event_interruptible_timeout(&c->handshake_wq, tcp_handshake_done, c,
                                     wait);
  }

  if (c->handshake_ok < 0) {
    int err = c->handshake_ok;
    tcp_conn_remove(c);
    c->state = TCP_STATE_CLOSED;
    return err;
  }
  return 0;
}

// ---------------------------------------------------------------------------
// listen / accept
// ---------------------------------------------------------------------------
int tcp_listen(tcp_conn_t *c, uint32_t local_ip, uint16_t local_port) {
  if (!c || local_port == 0)
    return -EINVAL;
  if (c->state != TCP_STATE_CLOSED)
    return -EINVAL;

  if (local_ip == 0) {
    for (netif_t *n = netif_first(); n; n = n->next) {
      if (n->ip != 0) {
        local_ip = n->ip;
        break;
      }
    }
    if (local_ip == 0)
      return -EINVAL;
  }

  c->local_ip = local_ip;
  c->local_port = local_port;
  c->remote_ip = 0;
  c->remote_port = 0;
  c->is_listener = 1;
  c->state = TCP_STATE_LISTEN;
  wait_queue_init(&c->accept_wq);
  c->accept_head = c->accept_tail = NULL;
  c->accept_count = 0;

  int rc = tcp_conn_insert(c);
  if (rc != 0) {
    c->state = TCP_STATE_CLOSED;
    c->is_listener = 0;
    return rc;
  }
  LOG_INFO("[TCP] listen en %u.%u.%u.%u:%u", (local_ip >> 24) & 0xFF,
           (local_ip >> 16) & 0xFF, (local_ip >> 8) & 0xFF, local_ip & 0xFF,
           local_port);
  return 0;
}

static bool tcp_accept_has_pending(void *arg) {
  tcp_conn_t *l = (tcp_conn_t *)arg;
  unsigned long flags = spin_lock_irqsave(&g_tcp_lock);
  int has = l->accept_count > 0;
  spin_unlock_irqrestore(&g_tcp_lock, flags);
  return has;
}

tcp_conn_t *tcp_accept(tcp_conn_t *listener, uint64_t timeout_ms) {
  if (!listener || !listener->is_listener)
    return NULL;
  uint64_t deadline = (timeout_ms > 0) ? sched_get_ticks() + timeout_ms : 0;

  for (;;) {
    unsigned long flags = spin_lock_irqsave(&g_tcp_lock);
    tcp_conn_t *c = listener->accept_head;
    if (c) {
      listener->accept_head = c->accept_next;
      if (!listener->accept_head)
        listener->accept_tail = NULL;
      listener->accept_count--;
      c->accept_next = NULL;
      spin_unlock_irqrestore(&g_tcp_lock, flags);
      return c;
    }
    spin_unlock_irqrestore(&g_tcp_lock, flags);
    if (timeout_ms == 0)
      return NULL;
    uint64_t wait = 100;
    if (deadline > 0) {
      uint64_t now = sched_get_ticks();
      if (now >= deadline)
        return NULL;
      uint64_t left = deadline - now;
      if (left < wait)
        wait = left;
    }
    wait_event_interruptible_timeout(&listener->accept_wq,
                                     tcp_accept_has_pending, listener, wait);
  }
}

// ---------------------------------------------------------------------------
// tcp_send
// ---------------------------------------------------------------------------
static bool tcp_send_can_progress(void *arg) {
  tcp_conn_t *c = (tcp_conn_t *)arg;
  unsigned long flags = spin_lock_irqsave(&c->rx_lock);
  uint32_t in_flight = c->snd_nxt - c->snd_una;
  uint32_t peer_win = c->peer_window ? c->peer_window : 65535;
  uint32_t cwnd = c->cwnd < 1460 ? 1460 : c->cwnd;
  uint32_t window = (peer_win < cwnd) ? peer_win : cwnd;
  int can = (in_flight < window) || (c->state != TCP_STATE_ESTABLISHED &&
                                     c->state != TCP_STATE_CLOSE_WAIT);
  spin_unlock_irqrestore(&c->rx_lock, flags);
  return can;
}

int tcp_send(tcp_conn_t *c, const void *buf, size_t len) {
  if (!c || !buf)
    return -EINVAL;
  if (c->state != TCP_STATE_ESTABLISHED)
    return -ENOTCONN;
  if (len == 0)
    return 0;

  const uint8_t *p = (const uint8_t *)buf;
  size_t sent = 0;
  size_t mss = TCP_RX_MSS;
  if (c->peer_mss >= 64) {
    mss = c->peer_mss;
    if (mss > TCP_RX_MSS)
      mss = TCP_RX_MSS;
  }

  while (sent < len) {
    unsigned long flags = spin_lock_irqsave(&c->rx_lock);
    uint32_t in_flight = c->snd_nxt - c->snd_una;
    uint32_t peer_win = c->peer_window ? c->peer_window : 65535;
    uint32_t cwnd = c->cwnd < 1460 ? 1460 : c->cwnd;
    uint32_t window = (peer_win < cwnd) ? peer_win : cwnd;
    uint32_t credit = (in_flight < window) ? (window - in_flight) : 0;
    int state_ok = (c->state == TCP_STATE_ESTABLISHED);
    spin_unlock_irqrestore(&c->rx_lock, flags);

    if (!state_ok)
      return sent > 0 ? (int)sent : -ENOTCONN;

    if (credit == 0) {
      long r = wait_event_interruptible_timeout(&c->rx_wq,
                                                tcp_send_can_progress, c, 1000);
      if (r < 0)
        return sent > 0 ? (int)sent : -EINTR;
      if (r == 0) {
        LOG_WARN("[TCP] TX stall: in_flight=%u peer_win=%u cwnd=%u",
                 c->snd_nxt - c->snd_una, c->peer_window, c->cwnd);
        (void)tcp_send_segment(c, TCP_ACK, c->snd_nxt, c->rcv_nxt, NULL, 0);
        c->rtx_consec_retries++;
        if (c->rtx_consec_retries >= 6) {
          flags = spin_lock_irqsave(&c->rx_lock);
          c->state = TCP_STATE_CLOSED;
          spin_unlock_irqrestore(&c->rx_lock, flags);
          wake_up_all(&c->rx_wq);
          return sent > 0 ? (int)sent : -ETIMEDOUT;
        }
        continue;
      }
      continue;
    }

    size_t chunk = len - sent;
    if (chunk > mss)
      chunk = mss;
    if (chunk > credit)
      chunk = credit;

    int rc = tcp_send_segment(c, TCP_ACK | TCP_PSH, c->snd_nxt, c->rcv_nxt,
                              p + sent, chunk);
    if (!__atomic_exchange_n(&c->trace_tx_payload_logged, 1,
                             __ATOMIC_RELAXED)) {
      LOG_INFO("[TCP] primer payload TX local=%u remote=%u seq=%u "
               "len=%lu peer_mss=%u peer_win=%u cwnd=%u rc=%d",
               c->local_port, c->remote_port, c->snd_nxt, (unsigned long)chunk,
               c->peer_mss, c->peer_window, c->cwnd, rc);
    }
    if (rc != 0)
      return sent > 0 ? (int)sent : rc;

    c->snd_nxt += (uint32_t)chunk;
    sent += chunk;
  }
  return (int)sent;
}

// ---------------------------------------------------------------------------
// recv
// ---------------------------------------------------------------------------
static bool tcp_rx_has_data(void *arg) {
  tcp_conn_t *c = (tcp_conn_t *)arg;
  unsigned long flags = spin_lock_irqsave(&c->rx_lock);
  int has_data =
      c->rx_count > 0 || c->peer_closed || c->state == TCP_STATE_CLOSED;
  spin_unlock_irqrestore(&c->rx_lock, flags);
  return has_data;
}

static size_t tcp_rx_read_locked(tcp_conn_t *c, void *buf, size_t len) {
  size_t copied = 0;
  while (c->rx_head && copied < len) {
    skb_t *skb = c->rx_head;
    size_t available = skb_len(skb);
    size_t amount = len - copied;
    if (amount > available)
      amount = available;

    memcpy((uint8_t *)buf + copied, skb_data(skb), amount);
    copied += amount;
    c->rx_buffered_bytes -= amount;
    c->rx_total_consumed += amount;

    if (amount == available) {
      c->rx_head = skb->next;
      if (!c->rx_head)
        c->rx_tail = NULL;
      c->rx_count--;
      skb_free(skb);
    } else {
      skb_pull(skb, amount);
    }
  }
  return copied;
}

int tcp_recv(tcp_conn_t *c, void *buf, size_t len, uint64_t timeout_ms) {
  if (!c || !buf)
    return -EINVAL;
  if (len == 0)
    return 0;

  uint64_t deadline = (timeout_ms > 0) ? sched_get_ticks() + timeout_ms : 0;

  for (;;) {
    unsigned long flags = spin_lock_irqsave(&c->rx_lock);
    size_t n = tcp_rx_read_locked(c, buf, len);
    int closed = c->peer_closed || c->state == TCP_STATE_CLOSED;
    spin_unlock_irqrestore(&c->rx_lock, flags);

    if (n > 0) {
      tcp_autotune_sample(c, n);
      tcp_send_window_update(c);
      return (int)n;
    }
    if (closed)
      return 0;

    uint64_t wait = 100;
    if (deadline > 0) {
      uint64_t now = sched_get_ticks();
      if (now >= deadline)
        return -ETIMEDOUT;
      uint64_t left = deadline - now;
      if (left < wait)
        wait = left;
    }
    long r =
        wait_event_interruptible_timeout(&c->rx_wq, tcp_rx_has_data, c, wait);
    if (r < 0)
      return -EINTR;
  }
}

int tcp_recv_nonblock(tcp_conn_t *c, void *buf, size_t len) {
  if (!c || !buf)
    return -EINVAL;
  if (len == 0)
    return 0;
  unsigned long flags = spin_lock_irqsave(&c->rx_lock);
  size_t n = tcp_rx_read_locked(c, buf, len);
  int closed = c->peer_closed || c->state == TCP_STATE_CLOSED;
  spin_unlock_irqrestore(&c->rx_lock, flags);
  if (n > 0) {
    tcp_autotune_sample(c, n);
    tcp_send_window_update(c);
    return (int)n;
  }
  if (closed)
    return 0;
  return -EAGAIN;
}

int tcp_is_readable(tcp_conn_t *c) {
  if (!c)
    return 0;
  unsigned long flags = spin_lock_irqsave(&c->rx_lock);
  int readable =
      c->rx_count > 0 || c->peer_closed || c->state == TCP_STATE_CLOSED;
  spin_unlock_irqrestore(&c->rx_lock, flags);
  return readable;
}

int tcp_peer_closed(tcp_conn_t *c) {
  if (!c)
    return 0;
  unsigned long flags = spin_lock_irqsave(&c->rx_lock);
  int closed = c->peer_closed;
  spin_unlock_irqrestore(&c->rx_lock, flags);
  return closed;
}

// ---------------------------------------------------------------------------
// close
// ---------------------------------------------------------------------------
void tcp_close(tcp_conn_t *c) {
  if (!c)
    return;
  if (c->state == TCP_STATE_ESTABLISHED || c->state == TCP_STATE_CLOSE_WAIT) {
    (void)tcp_send_segment(c, TCP_FIN | TCP_ACK, c->snd_nxt, c->rcv_nxt, NULL,
                           0);
    c->snd_nxt++;
    if (c->state == TCP_STATE_ESTABLISHED)
      c->state = TCP_STATE_FIN_WAIT_1;
    else
      c->state = TCP_STATE_LAST_ACK;
  }
  tcp_conn_free(c);
}

// ---------------------------------------------------------------------------
// Timer tick: RTO, retransmisión, autotune backoff
// ---------------------------------------------------------------------------
void tcp_timer_tick(void) {
  uint64_t now = sched_get_ticks();

  for (int i = 0; i < TCP_HASH_BUCKETS; i++) {
    unsigned long flags = spin_lock_irqsave(&g_tcp_lock);
    tcp_conn_t *c = g_tcp_buckets[i];
    while (c) {
      tcp_conn_t *next = c->next;
      if ((c->state == TCP_STATE_ESTABLISHED ||
           c->state == TCP_STATE_CLOSE_WAIT) &&
          c->snd_una != c->snd_nxt && c->last_ack_tick > 0) {

        uint32_t rto = c->rto_ms ? c->rto_ms : TCP_RTO_INITIAL_MS;
        if (now - c->last_ack_tick >= rto) {
          unsigned long rf = spin_lock_irqsave(&c->rx_lock);
          skb_t *v = c->rtx_head;
          int cons = 0;
          if (v) {
            cons = ++c->rtx_consec_retries;
            c->rtx_total_retries++;
          }
          spin_unlock_irqrestore(&c->rx_lock, rf);

          // Backoff del autotune: la red está descartando.
          tcp_autotune_backoff(c);

          if (v) {
            uint8_t *raw = skb_data(v);
            size_t raw_len = skb_len(v);
            uint8_t doff = (raw[12] >> 4) & 0x0F;
            size_t h2 = (size_t)doff * 4;
            size_t plen = raw_len - h2;
            uint8_t *payload = raw + h2;
            LOG_WARN("[TCP] RTX %u->%u seq=%u len=%lu intento=%d",
                     c->local_port, c->remote_port, v->tcp_seq,
                     (unsigned long)plen, cons);
            (void)tcp_send_segment_ex(c, TCP_ACK | TCP_PSH, v->tcp_seq,
                                      c->rcv_nxt, payload, plen,
                                      /*do_enqueue=*/0);
            rf = spin_lock_irqsave(&c->rx_lock);
            c->last_ack_tick = now;
            spin_unlock_irqrestore(&c->rx_lock, rf);

            // RTO backoff exponencial.
            uint32_t new_rto = rto * 2;
            if (new_rto > TCP_RTO_MAX_MS)
              new_rto = TCP_RTO_MAX_MS;
            c->rto_ms = new_rto;
          } else if (c->snd_una != c->snd_nxt) {
            (void)tcp_send_segment(c, TCP_ACK, c->snd_nxt, c->rcv_nxt, NULL, 0);
            rf = spin_lock_irqsave(&c->rx_lock);
            c->last_ack_tick = now;
            c->rtx_consec_retries++;
            cons = c->rtx_consec_retries;
            spin_unlock_irqrestore(&c->rx_lock, rf);
          }

          if (cons >= 6) {
            LOG_ERR("[TCP] conn %u->%u abort tras %d reintentos", c->local_port,
                    c->remote_port, cons);
            rf = spin_lock_irqsave(&c->rx_lock);
            c->state = TCP_STATE_CLOSED;
            c->peer_closed = 1;
            spin_unlock_irqrestore(&c->rx_lock, rf);
            wake_up_all(&c->rx_wq);
            wake_up_all(&c->handshake_wq);
          }
        }
      }
      c = next;
    }
    spin_unlock_irqrestore(&g_tcp_lock, flags);
  }
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------
void tcp_init(void) {
  memset(g_tcp_buckets, 0, sizeof(g_tcp_buckets));
  spin_init(&g_tcp_lock);
  g_conn_count = 0;

  int rc = ip_register_protocol(IP_PROTO_TCP, tcp_input);
  if (rc != 0) {
    LOG_ERR("[TCP] ip_register_protocol(6) rc=%d", rc);
    return;
  }
  LOG_INFO("[TCP] listo (%d buckets, %d conns max, MSS=%u, WSCALE=%u, "
           "RXBUF=%u KB, autotune=%u-%u KB)",
           TCP_HASH_BUCKETS, TCP_CONN_MAX, TCP_RX_MSS, TCP_WSCALE_SHIFT,
           (unsigned)(TCP_RX_BUFFER_BYTES / 1024),
           (unsigned)(TCP_RX_WINDOW_MIN / 1024),
           (unsigned)(TCP_RX_BUFFER_BYTES / 1024));
}