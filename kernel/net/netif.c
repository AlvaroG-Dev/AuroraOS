// kernel/net/netif.c

#include "netif.h"
#include "../klog.h"
#include "../spinlock.h"
#include "../string.h"
#include "../uaccess.h" // errno
#include "byteorder.h"

#define NETIF_MAX_HANDLERS 8

typedef struct {
  uint16_t proto;
  netif_rx_handler_t fn;
} rx_handler_t;

static netif_t *g_netif_list;
static rx_handler_t g_handlers[NETIF_MAX_HANDLERS];
static int g_n_handlers;
static spinlock_t g_netif_lock;

void net_init(void) {
  spin_init(&g_netif_lock);
  g_netif_list = NULL;
  g_n_handlers = 0;
  skb_init();
  LOG_INFO("[NET] networking core inicializado (Fase 0)");
}

void netif_register(netif_t *netif) {
  if (!netif)
    return;
  unsigned long flags = spin_lock_irqsave(&g_netif_lock);
  netif->next = g_netif_list;
  g_netif_list = netif;
  spin_unlock_irqrestore(&g_netif_lock, flags);

  LOG_DEBUG("[NET] netif registrado: %s (mac=%02x:%02x:%02x:%02x:%02x:%02x)",
            netif->name, netif->mac[0], netif->mac[1], netif->mac[2],
            netif->mac[3], netif->mac[4], netif->mac[5]);
}

void netif_unregister(netif_t *netif) {
  if (!netif)
    return;
  unsigned long flags = spin_lock_irqsave(&g_netif_lock);
  netif_t **pp = &g_netif_list;
  while (*pp) {
    if (*pp == netif) {
      *pp = netif->next;
      netif->next = NULL;
      break;
    }
    pp = &(*pp)->next;
  }
  spin_unlock_irqrestore(&g_netif_lock, flags);
}

netif_t *netif_lookup(const char *name) {
  if (!name)
    return NULL;
  unsigned long flags = spin_lock_irqsave(&g_netif_lock);
  netif_t *n = g_netif_list;
  while (n) {
    if (strcmp(n->name, name) == 0) {
      spin_unlock_irqrestore(&g_netif_lock, flags);
      return n;
    }
    n = n->next;
  }
  spin_unlock_irqrestore(&g_netif_lock, flags);
  return NULL;
}

netif_t *netif_lookup_by_ip(uint32_t ip) {
  unsigned long flags = spin_lock_irqsave(&g_netif_lock);
  netif_t *n = g_netif_list;
  while (n) {
    if (n->ip == ip) {
      spin_unlock_irqrestore(&g_netif_lock, flags);
      return n;
    }
    n = n->next;
  }
  spin_unlock_irqrestore(&g_netif_lock, flags);
  return NULL;
}

netif_t *netif_first(void) {
  unsigned long flags = spin_lock_irqsave(&g_netif_lock);
  netif_t *n = g_netif_list;
  spin_unlock_irqrestore(&g_netif_lock, flags);
  return n;
}

int netif_register_handler(uint16_t ethertype, netif_rx_handler_t fn) {
  if (!fn)
    return -EINVAL;

  unsigned long flags = spin_lock_irqsave(&g_netif_lock);

  for (int i = 0; i < g_n_handlers; i++) {
    if (g_handlers[i].proto == ethertype) {
      g_handlers[i].fn = fn;
      spin_unlock_irqrestore(&g_netif_lock, flags);
      return 0;
    }
  }
  if (g_n_handlers >= NETIF_MAX_HANDLERS) {
    spin_unlock_irqrestore(&g_netif_lock, flags);
    return -ENOMEM;
  }
  g_handlers[g_n_handlers].proto = ethertype;
  g_handlers[g_n_handlers].fn = fn;
  g_n_handlers++;

  spin_unlock_irqrestore(&g_netif_lock, flags);
  return 0;
}

int netif_tx(netif_t *netif, skb_t *skb) {
  if (!netif || !skb || !netif->ops || !netif->ops->transmit) {
    if (skb)
      skb_free(skb);
    return -EINVAL;
  }

  // Prepend Ethernet header.
  uint8_t *eth = (uint8_t *)skb_push(skb, 14);
  if (!eth) {
    netif->tx_errors++;
    skb_free(skb);
    return -ENOMEM;
  }
  memcpy(eth + 0, skb->dst_mac, 6);
  memcpy(eth + 6, netif->mac, 6);
  eth[12] = (uint8_t)((skb->protocol >> 8) & 0xFF);
  eth[13] = (uint8_t)(skb->protocol & 0xFF);

  netif->tx_packets++;
  netif->tx_bytes += skb_len(skb);

  int rc = netif->ops->transmit(netif, skb);
  if (rc != 0) {
    netif->tx_errors++;
    skb_free(skb);
  }
  return rc;
}

void netif_rx(skb_t *skb) {
  if (!skb)
    return;

  // El driver nos da el frame Ethernet completo. Leemos el EtherType
  // (bytes 12-13, big-endian) y despachamos. NO tocamos skb->netif
  // todavía: el handler lo rellena si necesita.
  size_t len = skb_len(skb);
  if (len < 14) {
    skb_free(skb);
    return;
  }

  const uint8_t *d = skb_data(skb);
  uint16_t ethertype = (uint16_t)((d[12] << 8) | d[13]);

  if (skb->netif) {
    skb->netif->rx_packets++;
    skb->netif->rx_bytes += len;
  }

  // Buscar handler.
  netif_rx_handler_t fn = NULL;
  unsigned long flags = spin_lock_irqsave(&g_netif_lock);
  for (int i = 0; i < g_n_handlers; i++) {
    if (g_handlers[i].proto == ethertype) {
      fn = g_handlers[i].fn;
      break;
    }
  }
  spin_unlock_irqrestore(&g_netif_lock, flags);

  if (!fn) {
    if (skb->netif)
      skb->netif->rx_dropped++;
    skb_free(skb);
    return;
  }

  fn(skb->netif, skb);
}