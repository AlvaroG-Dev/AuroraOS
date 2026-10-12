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

  // [CONFIG] Estado inicial: link up por defecto. Los drivers que
  // quieran arrancar abajo llaman a netif_set_up(n, 0) después.
  // Sin esto, netif_tx rechaza todo con -ENETDOWN.
  if (netif->is_up == 0)
    netif->is_up = 1;
  // Recordar si es loopback para SIOCGIFFLAGS / IFF_LOOPBACK.
  if (netif->is_loopback == 0 && netif->mac[0] == 0 && netif->mac[1] == 0 &&
      netif->mac[2] == 0 && netif->mac[3] == 0 && netif->mac[4] == 0 &&
      netif->mac[5] == 0) {
    netif->is_loopback = 1;
  }

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

// ---------------------------------------------------------------------------
// [CONFIG] Iteración por índice. El orden es el de la lista (más
// reciente primero). ifconfig -a los recorre todos vía SIOCGIFCONF.
// ---------------------------------------------------------------------------
int netif_count(void) {
  unsigned long flags = spin_lock_irqsave(&g_netif_lock);
  int n = 0;
  for (netif_t *p = g_netif_list; p; p = p->next)
    n++;
  spin_unlock_irqrestore(&g_netif_lock, flags);
  return n;
}

netif_t *netif_get_by_index(int idx) {
  if (idx < 0)
    return NULL;
  unsigned long flags = spin_lock_irqsave(&g_netif_lock);
  int i = 0;
  netif_t *found = NULL;
  for (netif_t *p = g_netif_list; p; p = p->next) {
    if (i == idx) {
      found = p;
      break;
    }
    i++;
  }
  spin_unlock_irqrestore(&g_netif_lock, flags);
  return found;
}

// ---------------------------------------------------------------------------
// [CONFIG] Configura IP/netmask/gateway en runtime. Llamado desde
// k_ioctl_netif() cuando ifconfig hace SIOCSIFADDR, SIOCSIFNETMASK o
// SIOCSIFBRDADDR.
//
// Si netmask == 0, se calcula por clase (A/B/C). Fallback /24.
// ---------------------------------------------------------------------------
void netif_set_addr(netif_t *n, uint32_t ip, uint32_t netmask,
                    uint32_t gateway) {
  if (!n)
    return;

  if (netmask == 0) {
    // Clase A: 0xxxxxxx, clase B: 10xxxxxx, clase C: 110xxxxx
    uint8_t top = (uint8_t)(ip >> 24);
    if (top < 0x80)
      netmask = 0xFF000000u;
    else if (top < 0xC0)
      netmask = 0xFFFF0000u;
    else if (top < 0xE0)
      netmask = 0xFFFFFF00u;
    else
      netmask = 0xFFFFFF00u; // fallback /24 para multicast / clase D+
  }

  unsigned long flags = spin_lock_irqsave(&g_netif_lock);
  n->ip = ip;
  n->netmask = netmask;
  n->gateway = gateway;
  spin_unlock_irqrestore(&g_netif_lock, flags);

  LOG_INFO("[NET] %s: ip=%u.%u.%u.%u mask=%u.%u.%u.%u gw=%u.%u.%u.%u", n->name,
           (ip >> 24) & 0xFF, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF,
           (netmask >> 24) & 0xFF, (netmask >> 16) & 0xFF,
           (netmask >> 8) & 0xFF, netmask & 0xFF, (gateway >> 24) & 0xFF,
           (gateway >> 16) & 0xFF, (gateway >> 8) & 0xFF, gateway & 0xFF);
}

// ---------------------------------------------------------------------------
// [CONFIG] Arriba/abajo. Llama a los ops correspondientes.
// ---------------------------------------------------------------------------
void netif_set_up(netif_t *n, int up) {
  if (!n || !n->ops)
    return;
  up = up ? 1 : 0;
  if (up == n->is_up)
    return;

  if (up) {
    if (n->ops->open)
      n->ops->open(n);
    n->is_up = 1;
    LOG_INFO("[NET] %s: link up", n->name);
  } else {
    if (n->ops->close)
      n->ops->close(n);
    n->is_up = 0;
    LOG_INFO("[NET] %s: link down", n->name);
  }
}

int netif_is_up(const netif_t *n) { return n ? n->is_up : 0; }

// ---------------------------------------------------------------------------
// Handlers de ethertype
// ---------------------------------------------------------------------------
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

// ---------------------------------------------------------------------------
// TX
// ---------------------------------------------------------------------------
int netif_tx(netif_t *netif, skb_t *skb) {
  if (!netif || !skb || !netif->ops || !netif->ops->transmit) {
    if (skb)
      skb_free(skb);
    return -EINVAL;
  }

  if (!netif->is_up) {
    // Interfaz abajo: no transmitir.
    skb_free(skb);
    return -ENETDOWN;
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

// ---------------------------------------------------------------------------
// RX dispatch
// ---------------------------------------------------------------------------
void netif_rx(skb_t *skb) {
  if (!skb)
    return;

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