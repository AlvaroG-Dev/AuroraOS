// kernel/net/tcp.c
//
// Fase 7a: parseo, checksum y tabla de conexiones.
// Fase 7b: handshake activo (connect).
// Fase 7c: send/recv de datos.

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
// Tabla hash de conexiones
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

// Devuelve 0 si OK, <0 si error (skb_alloc falló o ip_output falló).
static int tcp_send_segment(tcp_conn_t *c, uint16_t flags, uint32_t seq,
                            uint32_t ack, const void *payload,
                            size_t payload_len);
static bool tcp_handshake_done(void *arg);

static uint32_t tcp_pick_isn(void);
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
  return c;
}

static void tcp_conn_destroy(tcp_conn_t *c) {
  skb_t *p = c->rx_head;
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

  // [FIX 7d] Quitar del hash primero (idempotente). Sin esto, un free
  // sin remove previo deja un dangling pointer en el hash y el
  // siguiente tcp_flush corrompe el heap.
  tcp_conn_remove(c);

  int freed = 0;

  // Si es listener, quitar y destruir la cola de aceptados.
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
  // Los listeners no tienen remote; el resto sí.
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

// [Fase 7d] Busca un listener por (local_ip, local_port).
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
      // [FIX] Liberar los skbs pendientes antes de kfree. Si una
      // conexión quedaba con datos sin consumir, filtrábamos memoria.
      skb_t *s = p->rx_head;
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

// Construye y envía un segmento TCP. Devuelve 0 si OK.
static int tcp_send_segment(tcp_conn_t *c, uint16_t flags, uint32_t seq,
                            uint32_t ack, const void *payload,
                            size_t payload_len) {
  size_t seg_len = TCP_HDR_MIN_LEN + payload_len;
  skb_t *sk = skb_alloc();
  if (!sk)
    return -ENOMEM;

  uint8_t *seg = (uint8_t *)skb_put(sk, seg_len);
  if (!seg) {
    skb_free(sk);
    return -ENOMEM;
  }
  memset(seg, 0, TCP_HDR_MIN_LEN);

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
  seg[12] = 5 << 4;
  seg[13] = (uint8_t)(flags & 0xFF);
  if (flags & TCP_NS)
    seg[12] |= 0x01;
  seg[14] = 0x10;
  seg[15] = 0x00;

  if (payload_len)
    memcpy(seg + TCP_HDR_MIN_LEN, payload, payload_len);

  uint16_t csum = tcp_checksum(c->local_ip, c->remote_ip, seg, seg_len);
  seg[16] = (uint8_t)(csum >> 8);
  seg[17] = (uint8_t)(csum & 0xFF);

  int rc = ip_output(sk, c->local_ip, c->remote_ip, IP_PROTO_TCP);
  if (rc != 0) {
    LOG_DEBUG("[TCP] ip_output falló en flag=0x%x", flags);
    return rc;
  }
  return 0;
}

static bool tcp_handshake_done(void *arg) {
  tcp_conn_t *c = (tcp_conn_t *)arg;
  return c->handshake_ok != 0;
}

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
    c->rcv_nxt = h->seq + 1;
    c->snd_una = h->ack;
    c->snd_nxt = h->ack;
    c->state = TCP_STATE_ESTABLISHED;
    c->handshake_ok = 1;

    (void)tcp_send_segment(c, TCP_ACK, c->snd_nxt, c->rcv_nxt, NULL, 0);

    LOG_INFO("[TCP] conectado %u.%u.%u.%u:%u -> %u.%u.%u.%u:%u",
             (c->local_ip >> 24) & 0xFF, (c->local_ip >> 16) & 0xFF,
             (c->local_ip >> 8) & 0xFF, c->local_ip & 0xFF, c->local_port,
             (c->remote_ip >> 24) & 0xFF, (c->remote_ip >> 16) & 0xFF,
             (c->remote_ip >> 8) & 0xFF, c->remote_ip & 0xFF, c->remote_port);

    wake_up_all(&c->handshake_wq);
    skb_free(skb);
    return;
  }

  LOG_DEBUG("[TCP] connect: flags 0x%x inesperados en SYN_SENT", h->flags);
  skb_free(skb);
}

// [Fase 7d] SYN recibido en un listener: crear conn hija, mandar SYN-ACK.
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

// [Fase 7d] ACK final del cliente en una conn SYN_RECEIVED.
// Transiciona a ESTABLISHED y encola en el listener.
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

  c->snd_una = h->ack;
  c->snd_nxt = h->ack;
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
    LOG_DEBUG("[TCP] drop: checksum malo %u->%u sport=%u dport=%u", src_ip,
              dst_ip, hdr.sport, hdr.dport);
    skb_free(skb);
    return;
  }

  tcp_conn_t *c = tcp_conn_lookup(dst_ip, hdr.dport, src_ip, hdr.sport);
  if (!c) {
    // [Fase 7d] Sin conn exacta: probar listener.
    tcp_conn_t *lis = tcp_listener_lookup(dst_ip, hdr.dport);
    if (lis && (hdr.flags & TCP_SYN) && !(hdr.flags & TCP_ACK)) {
      tcp_handle_listen(lis, skb, &hdr, src_ip);
      return;
    }
    LOG_DEBUG("[TCP] drop: no hay conn %u->%u (%u->%u)", src_ip, dst_ip,
              hdr.sport, hdr.dport);
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

  // [Fase 7f] Aceptamos ESTABLISHED y CLOSE_WAIT: en CLOSE_WAIT el peer
  // ya mandó FIN pero seguimos procesando ACKs y datos tardíos.
  if (c->state == TCP_STATE_ESTABLISHED || c->state == TCP_STATE_CLOSE_WAIT) {

    if (hdr.flags & TCP_RST) {
      LOG_DEBUG("[TCP] RST en %s", c->state == TCP_STATE_ESTABLISHED
                                       ? "ESTABLISHED"
                                       : "CLOSE_WAIT");
      c->state = TCP_STATE_CLOSED;
      c->peer_closed = 1;
      wake_up_all(&c->rx_wq);
      wake_up_all(&c->handshake_wq);
      skb_free(skb);
      return;
    }

    // Actualizar snd_una con el ACK del peer.
    if (hdr.flags & TCP_ACK) {
      if (hdr.ack > c->snd_una && hdr.ack <= c->snd_nxt)
        c->snd_una = hdr.ack;
    }

    size_t payload_len = len - hdr.hdr_len;

    // 1) Datos, si los hay y vienen en orden.
    int queued = 0;
    if (payload_len > 0) {
      if (hdr.seq == c->rcv_nxt) {
        c->rcv_nxt += (uint32_t)payload_len;
        skb_pull(skb, hdr.hdr_len);
        skb->next = NULL;
        if (c->rx_tail)
          c->rx_tail->next = skb;
        else
          c->rx_head = skb;
        c->rx_tail = skb;
        c->rx_count++;
        queued = 1;

        // ACK inmediato.
        (void)tcp_send_segment(c, TCP_ACK, c->snd_nxt, c->rcv_nxt, NULL, 0);
        wake_up_all(&c->rx_wq);
      } else {
        LOG_DEBUG("[TCP] seq inesperado: %u, esperado %u", hdr.seq, c->rcv_nxt);
        // TODO(7g): reensamblado fuera de orden.
      }
    }

    // 2) FIN, si lo hay. Consume 1 seq y marca peer_closed.
    if (hdr.flags & TCP_FIN) {
      c->rcv_nxt++;
      c->peer_closed = 1;

      // ACK del FIN.
      (void)tcp_send_segment(c, TCP_ACK, c->snd_nxt, c->rcv_nxt, NULL, 0);

      if (c->state == TCP_STATE_ESTABLISHED)
        c->state = TCP_STATE_CLOSE_WAIT;
      // Si ya estábamos en CLOSE_WAIT, seguimos ahí.

      wake_up_all(&c->rx_wq);

      LOG_INFO("[TCP] peer cerró conexión (FIN) "
               "local=%u.%u.%u.%u:%u remote=%u.%u.%u.%u:%u",
               (c->local_ip >> 24) & 0xFF, (c->local_ip >> 16) & 0xFF,
               (c->local_ip >> 8) & 0xFF, c->local_ip & 0xFF, c->local_port,
               (c->remote_ip >> 24) & 0xFF, (c->remote_ip >> 16) & 0xFF,
               (c->remote_ip >> 8) & 0xFF, c->remote_ip & 0xFF, c->remote_port);
    }

    if (!queued)
      skb_free(skb);
    return;
  }

  // Otros estados (FIN_WAIT_1/2, CLOSING, LAST_ACK, TIME_WAIT): si hay
  // handler, delegamos; si no, drop.
  if (c->handler) {
    c->handler(c, skb, &hdr, src_ip, dst_ip);
    return;
  }
  LOG_DEBUG("[TCP] drop: estado %d sin handler", c->state);
  skb_free(skb);
}

// ---------------------------------------------------------------------------
// [Fase 7b] tcp_connect
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
  while (c->handshake_ok == 0) {
    uint64_t now = sched_get_ticks();
    if (now >= deadline) {
      LOG_WARN("[TCP] connect: timeout a %u.%u.%u.%u:%u",
               (remote_ip >> 24) & 0xFF, (remote_ip >> 16) & 0xFF,
               (remote_ip >> 8) & 0xFF, remote_ip & 0xFF, remote_port);
      tcp_conn_remove(c);
      c->state = TCP_STATE_CLOSED;
      return -ETIMEDOUT;
    }
    uint64_t wait = deadline - now;
    if (wait > 100)
      wait = 100;
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
// [Fase 7d] listen / accept
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
      return NULL; // non-block y vacío

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
// [Fase 7c] send / recv
// ---------------------------------------------------------------------------
int tcp_send(tcp_conn_t *c, const void *buf, size_t len) {
  if (!c || !buf)
    return -EINVAL;
  if (c->state != TCP_STATE_ESTABLISHED)
    return -ENOTCONN;
  if (len == 0)
    return 0;

  const uint8_t *p = (const uint8_t *)buf;
  size_t sent = 0;
  while (sent < len) {
    size_t chunk = len - sent;
    if (chunk > 1400)
      chunk = 1400;
    int rc = tcp_send_segment(c, TCP_ACK | TCP_PSH, c->snd_nxt, c->rcv_nxt,
                              p + sent, chunk);
    if (rc != 0)
      return sent > 0 ? (int)sent : rc;
    c->snd_nxt += (uint32_t)chunk;
    sent += chunk;
  }
  return (int)sent;
}

static bool tcp_rx_has_data(void *arg) {
  tcp_conn_t *c = (tcp_conn_t *)arg;
  return c->rx_count > 0 || c->peer_closed || c->state == TCP_STATE_CLOSED;
}

int tcp_recv(tcp_conn_t *c, void *buf, size_t len, uint64_t timeout_ms) {
  if (!c || !buf)
    return -EINVAL;
  if (len == 0)
    return 0;

  uint64_t deadline = (timeout_ms > 0) ? sched_get_ticks() + timeout_ms : 0;

  for (;;) {
    if (c->rx_head) {
      skb_t *sk = c->rx_head;

      size_t n = skb_len(sk);
      if (n > len)
        n = len;
      memcpy(buf, skb_data(sk), n);

      // TCP es stream: si el caller pidió menos de lo que hay en el
      // skb, conservamos el resto en la cola. Consumimos n bytes del
      // frente con skb_pull. Si vaciamos el skb, lo liberamos.
      if (n < skb_len(sk)) {
        skb_pull(sk, n);
      } else {
        c->rx_head = sk->next;
        if (!c->rx_head)
          c->rx_tail = NULL;
        c->rx_count--;
        skb_free(sk);
      }
      return (int)n;
    }

    // [7f] EOF del peer: si no queda nada más en la cola, devolver 0.
    if (c->peer_closed || c->state == TCP_STATE_CLOSED)
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

// ---------------------------------------------------------------------------
// [Fase 7e] close / recv_nonblock
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
  // Esperaríamos el ACK del FIN y TIME_WAIT antes de liberar del todo,
  // pero eso requiere timers. En 7f liberamos directamente: el peer
  // recibirá el FIN y cerrará; nuestros ACKs posteriores se descartan
  // si el peer ya ha cerrado. Aceptable en esta fase.
  tcp_conn_free(c);
}

int tcp_recv_nonblock(tcp_conn_t *c, void *buf, size_t len) {
  if (!c || !buf)
    return -EINVAL;
  if (len == 0)
    return 0;

  if (c->rx_head) {
    skb_t *sk = c->rx_head;

    size_t n = skb_len(sk);
    if (n > len)
      n = len;
    memcpy(buf, skb_data(sk), n);

    if (n < skb_len(sk)) {
      skb_pull(sk, n);
    } else {
      c->rx_head = sk->next;
      if (!c->rx_head)
        c->rx_tail = NULL;
      c->rx_count--;
      skb_free(sk);
    }
    return (int)n;
  }

  if (c->peer_closed || c->state == TCP_STATE_CLOSED)
    return 0;
  return -EAGAIN;
}

int tcp_peer_closed(const tcp_conn_t *c) { return c ? c->peer_closed : 0; }

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
  LOG_INFO("[TCP] listo (%d buckets, %d conns max)", TCP_HASH_BUCKETS,
           TCP_CONN_MAX);
}
