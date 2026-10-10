// kernel/net/arp.c
//
// ARP. Los paquetes llegan aquí ya sin cabecera Ethernet (netif_rx +
// ip_rx_ethernet-style pull). En salida, netif_tx prepende Ethernet.

#include "arp.h"
#include "../klog.h"
#include "../spinlock.h"
#include "../string.h"
#include "../time.h" // tick_count
#include "../uaccess.h"
#include "byteorder.h"
#include "ip.h"
#include "netif.h"

#define ETH_P_ARP 0x0806u
#define ARP_HTYPE_ETHERNET 1u
#define ARP_OP_REQUEST 1u
#define ARP_OP_REPLY 2u
#define ARP_PKT_LEN 28

typedef struct arp_entry {
  uint32_t ip; // host order
  uint8_t mac[6];
  uint8_t state; // ARP_ENTRY_*
  uint8_t _pad;
  netif_t *netif;
  uint64_t last_seen; // tick_count del último update
  skb_t *pending_head;
  skb_t *pending_tail;
  int pending_count;
} arp_entry_t;

static arp_entry_t g_arp_table[ARP_TABLE_SIZE];
static spinlock_t g_arp_lock;

// --- helpers wire ---
static inline uint16_t rd16be(const uint8_t *p) {
  return (uint16_t)((p[0] << 8) | p[1]);
}
static inline uint32_t rd32be(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
static inline void wr16be(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)(v >> 8);
  p[1] = (uint8_t)(v & 0xFF);
}
static inline void wr32be(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v >> 24);
  p[1] = (uint8_t)(v >> 16);
  p[2] = (uint8_t)(v >> 8);
  p[3] = (uint8_t)(v & 0xFF);
}

// --- helpers tabla ---
static arp_entry_t *find_entry_locked(netif_t *n, uint32_t ip) {
  for (int i = 0; i < ARP_TABLE_SIZE; i++) {
    arp_entry_t *e = &g_arp_table[i];
    if (e->state == ARP_ENTRY_FREE)
      continue;
    if (e->ip == ip && e->netif == n)
      return e;
  }
  return NULL;
}

static void free_pending_locked(arp_entry_t *e) {
  skb_t *p = e->pending_head;
  while (p) {
    skb_t *next = p->next;
    skb_free(p);
    p = next;
  }
  e->pending_head = e->pending_tail = NULL;
  e->pending_count = 0;
}

static arp_entry_t *alloc_entry_locked(netif_t *n, uint32_t ip) {
  // Preferimos un FREE.
  for (int i = 0; i < ARP_TABLE_SIZE; i++) {
    if (g_arp_table[i].state == ARP_ENTRY_FREE) {
      arp_entry_t *e = &g_arp_table[i];
      memset(e, 0, sizeof(*e));
      e->netif = n;
      e->ip = ip;
      return e;
    }
  }
  // Si no, robamos el más viejo que no sea INCOMPLETE.
  arp_entry_t *oldest = NULL;
  uint64_t oldest_ts = UINT64_MAX;
  for (int i = 0; i < ARP_TABLE_SIZE; i++) {
    arp_entry_t *e = &g_arp_table[i];
    if (e->state == ARP_ENTRY_INCOMPLETE)
      continue;
    if (e->last_seen < oldest_ts) {
      oldest_ts = e->last_seen;
      oldest = e;
    }
  }
  if (!oldest)
    return NULL;
  free_pending_locked(oldest);
  memset(oldest, 0, sizeof(*oldest));
  oldest->netif = n;
  oldest->ip = ip;
  return oldest;
}

// --- transmit ---
static void send_request(netif_t *n, uint32_t target_ip) {
  skb_t *s = skb_alloc();
  if (!s)
    return;

  uint8_t *a = (uint8_t *)skb_put(s, ARP_PKT_LEN);
  if (!a) {
    skb_free(s);
    return;
  }
  memset(a, 0, ARP_PKT_LEN);
  wr16be(a + 0, ARP_HTYPE_ETHERNET);
  wr16be(a + 2, ETH_P_IP);
  a[4] = 6; // hlen
  a[5] = 4; // plen
  wr16be(a + 6, ARP_OP_REQUEST);
  memcpy(a + 8, n->mac, 6);
  wr32be(a + 14, n->ip);
  // target_mac = 0
  wr32be(a + 24, target_ip);

  memset(s->dst_mac, 0xFF, 6);
  s->protocol = ETH_P_ARP;
  s->netif = n;

  (void)netif_tx(n, s);
  LOG_DEBUG("[ARP] request who-has %08x tell %08x", target_ip, n->ip);
}

// --- input ---
static void arp_rx_ethernet(netif_t *netif, skb_t *skb) {
  (void)netif;
  if (!skb)
    return;
  if (skb_len(skb) < 14) {
    skb_free(skb);
    return;
  }
  skb_pull(skb, 14);

  if (skb_len(skb) < ARP_PKT_LEN) {
    skb_free(skb);
    return;
  }
  uint8_t *a = skb_data(skb);

  uint16_t htype = rd16be(a + 0);
  uint16_t ptype = rd16be(a + 2);
  uint8_t hlen = a[4];
  uint8_t plen = a[5];
  uint16_t op = rd16be(a + 6);

  if (htype != ARP_HTYPE_ETHERNET || ptype != ETH_P_IP || hlen != 6 ||
      plen != 4) {
    skb_free(skb);
    return;
  }

  uint8_t smac[6];
  memcpy(smac, a + 8, 6);
  uint32_t sip = rd32be(a + 14);
  uint8_t tmac[6];
  memcpy(tmac, a + 18, 6);
  uint32_t tip = rd32be(a + 24);
  (void)tmac;

  netif_t *n = skb->netif;

  // 1) Aprender del emisor (si la IP no es 0.0.0.0).
  if (n && sip != 0) {
    unsigned long flags = spin_lock_irqsave(&g_arp_lock);
    arp_entry_t *e = find_entry_locked(n, sip);
    if (!e)
      e = alloc_entry_locked(n, sip);
    if (e) {
      memcpy(e->mac, smac, 6);
      e->state = ARP_ENTRY_REACHABLE;
      e->last_seen = tick_count;

      // Desencolar bajo lock (la cola entera). Luego enviamos fuera
      // del lock para no meter netif_tx dentro de la sección crítica.
      skb_t *p = e->pending_head;
      e->pending_head = e->pending_tail = NULL;
      e->pending_count = 0;
      spin_unlock_irqrestore(&g_arp_lock, flags);

      while (p) {
        skb_t *next = p->next;
        p->next = NULL;
        memcpy(p->dst_mac, smac, 6);
        p->protocol = ETH_P_IP;
        p->netif = n;
        (void)netif_tx(n, p);
        p = next;
      }
    } else {
      spin_unlock_irqrestore(&g_arp_lock, flags);
    }
  }

  // 2) Si es un request para nosotros, responder.
  if (op == ARP_OP_REQUEST && n && (tip == n->ip)) {
    skb_t *r = skb_alloc();
    if (r) {
      uint8_t *ra = (uint8_t *)skb_put(r, ARP_PKT_LEN);
      if (ra) {
        memset(ra, 0, ARP_PKT_LEN);
        wr16be(ra + 0, ARP_HTYPE_ETHERNET);
        wr16be(ra + 2, ETH_P_IP);
        ra[4] = 6;
        ra[5] = 4;
        wr16be(ra + 6, ARP_OP_REPLY);
        memcpy(ra + 8, n->mac, 6);
        wr32be(ra + 14, n->ip);
        memcpy(ra + 18, smac, 6);
        wr32be(ra + 24, sip);

        memcpy(r->dst_mac, smac, 6);
        r->protocol = ETH_P_ARP;
        r->netif = n;
        (void)netif_tx(n, r);
        LOG_DEBUG("[ARP] reply to %08x (%02x:%02x:%02x:%02x:%02x:%02x)", sip,
                  smac[0], smac[1], smac[2], smac[3], smac[4], smac[5]);
      } else {
        skb_free(r);
      }
    }
  }

  skb_free(skb);
}

// --- public ---
void arp_init(void) {
  memset(g_arp_table, 0, sizeof(g_arp_table));
  spin_init(&g_arp_lock);

  int rc = netif_register_handler(ETH_P_ARP, arp_rx_ethernet);
  if (rc != 0) {
    LOG_ERR("[ARP] netif_register_handler(ETH_P_ARP) rc=%d", rc);
    return;
  }
  LOG_INFO("[ARP] tabla inicializada (%d entries)", ARP_TABLE_SIZE);
}

int arp_resolve(netif_t *n, uint32_t ip, uint8_t mac_out[6]) {
  if (!n || !mac_out)
    return -EINVAL;

  // Broadcast: MAC de broadcast.
  if (ip == IP_ADDR_BROADCAST) {
    memset(mac_out, 0xFF, 6);
    return 0;
  }

  unsigned long flags = spin_lock_irqsave(&g_arp_lock);
  arp_entry_t *e = find_entry_locked(n, ip);

  if (e && (e->state == ARP_ENTRY_REACHABLE || e->state == ARP_ENTRY_STALE)) {
    memcpy(mac_out, e->mac, 6);
    spin_unlock_irqrestore(&g_arp_lock, flags);
    return 0;
  }
  if (e && e->state == ARP_ENTRY_INCOMPLETE) {
    spin_unlock_irqrestore(&g_arp_lock, flags);
    return -EAGAIN;
  }

  if (!e) {
    e = alloc_entry_locked(n, ip);
    if (!e) {
      spin_unlock_irqrestore(&g_arp_lock, flags);
      return -ENOMEM;
    }
  }
  e->state = ARP_ENTRY_INCOMPLETE;
  e->last_seen = tick_count;
  spin_unlock_irqrestore(&g_arp_lock, flags);

  send_request(n, ip);
  return -EAGAIN;
}

int arp_queue_pending(netif_t *n, uint32_t ip, skb_t *skb) {
  if (!n || !skb)
    return -EINVAL;

  unsigned long flags = spin_lock_irqsave(&g_arp_lock);
  arp_entry_t *e = find_entry_locked(n, ip);
  if (!e) {
    spin_unlock_irqrestore(&g_arp_lock, flags);
    return -ENOENT;
  }
  if (e->pending_count >= ARP_PENDING_MAX) {
    spin_unlock_irqrestore(&g_arp_lock, flags);
    return -ENOMEM;
  }
  skb->next = NULL;
  if (e->pending_tail)
    e->pending_tail->next = skb;
  else
    e->pending_head = skb;
  e->pending_tail = skb;
  e->pending_count++;
  spin_unlock_irqrestore(&g_arp_lock, flags);
  return 0;
}

void arp_timer_tick(void) {
  uint64_t now = tick_count;

  unsigned long flags = spin_lock_irqsave(&g_arp_lock);
  for (int i = 0; i < ARP_TABLE_SIZE; i++) {
    arp_entry_t *e = &g_arp_table[i];
    if (e->state == ARP_ENTRY_FREE)
      continue;
    uint64_t age = now - e->last_seen;

    if (e->state == ARP_ENTRY_REACHABLE && age >= ARP_REACHABLE_TTL_MS) {
      e->state = ARP_ENTRY_STALE;
    } else if (e->state == ARP_ENTRY_STALE &&
               age >= ARP_REACHABLE_TTL_MS + ARP_STALE_TTL_MS) {
      free_pending_locked(e);
      memset(e, 0, sizeof(*e));
    } else if (e->state == ARP_ENTRY_INCOMPLETE &&
               age >= ARP_INCOMPLETE_TTL_MS) {
      free_pending_locked(e);
      memset(e, 0, sizeof(*e));
    }
  }
  spin_unlock_irqrestore(&g_arp_lock, flags);
}

void arp_dump(void) {
  static const char *names[] = {"FREE", "INCOMPLETE", "REACHABLE", "STALE"};
  LOG_INFO("[ARP] tabla:");
  unsigned long flags = spin_lock_irqsave(&g_arp_lock);
  for (int i = 0; i < ARP_TABLE_SIZE; i++) {
    arp_entry_t *e = &g_arp_table[i];
    if (e->state == ARP_ENTRY_FREE)
      continue;
    LOG_INFO("  [%d] %s ip=%08x mac=%02x:%02x:%02x:%02x:%02x:%02x pend=%d", i,
             names[e->state], e->ip, e->mac[0], e->mac[1], e->mac[2], e->mac[3],
             e->mac[4], e->mac[5], e->pending_count);
  }
  spin_unlock_irqrestore(&g_arp_lock, flags);
}

void arp_flush(void) {
  unsigned long flags = spin_lock_irqsave(&g_arp_lock);
  for (int i = 0; i < ARP_TABLE_SIZE; i++) {
    arp_entry_t *e = &g_arp_table[i];
    if (e->state != ARP_ENTRY_FREE)
      free_pending_locked(e);
    memset(e, 0, sizeof(*e));
  }
  spin_unlock_irqrestore(&g_arp_lock, flags);
}