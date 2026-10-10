// kernel/net/netif.h
//
// Interfaz de red. Cada driver (loopback, e1000, ...) registra una
// netif_t con un conjunto de ops. netif_tx() la usa para enviar;
// netif_rx() procesa paquetes recibidos.

#ifndef KERNEL_NET_NETIF_H
#define KERNEL_NET_NETIF_H

#include "skb.h"
#include <stddef.h>
#include <stdint.h>

typedef struct netif netif_t;

typedef struct netif_ops {
  // Transmitir un skb (con frame Ethernet completo). El driver
  // consume el skb en éxito (llama a skb_free internamente o lo
  // reutiliza). En error, el skb lo libera netif_tx.
  int (*transmit)(netif_t *netif, skb_t *skb);
  int (*open)(netif_t *netif);
  int (*close)(netif_t *netif);
} netif_ops_t;

struct netif {
  char name[8];
  uint8_t mac[6];
  uint16_t _pad_mac;
  uint32_t ip;      // host order
  uint32_t netmask; // host order
  uint32_t gateway; // host order

  netif_ops_t *ops;
  void *priv;

  uint64_t rx_packets, tx_packets;
  uint64_t rx_bytes, tx_bytes;
  uint64_t rx_errors, tx_errors, rx_dropped;

  netif_t *next;
};

// Handler de recepción por protocolo (ethertype).
typedef void (*netif_rx_handler_t)(netif_t *netif, skb_t *skb);

void net_init(void);

void netif_register(netif_t *netif);
void netif_unregister(netif_t *netif);

netif_t *netif_lookup(const char *name);
netif_t *netif_lookup_by_ip(uint32_t ip);
netif_t *netif_first(void);

// Registra un handler para un ethertype. Máximo 8. Reemplaza si ya
// existe uno para ese ethertype.
int netif_register_handler(uint16_t ethertype, netif_rx_handler_t fn);

// Envía un skb por la interfaz. El skb debe tener dst_mac y protocol
// seteados. netif_tx prepend la cabecera Ethernet. En éxito el skb
// está consumido. En error, netif_tx lo libera.
int netif_tx(netif_t *netif, skb_t *skb);

// Punto de entrada de paquetes recibidos. Lo llama el driver (o
// loopback) desde IRQ o desde el path de envío del loopback.
// Despacha por ethertype al handler registrado. Si no hay handler,
// libera el skb.
void netif_rx(skb_t *skb);

#endif