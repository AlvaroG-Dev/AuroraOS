// kernel/net/netif.h
//
// Interfaz de red. Cada driver (loopback, e1000e, ...) registra una
// netif_t con un conjunto de ops. netif_tx() la usa para enviar;
// netif_rx() procesa paquetes recibidos.
//
// El driver NO configura la IP. El userland lo hace vía ioctls
// SIOCSIFADDR / SIOCSIFNETMASK que llegan a netif_set_addr().

#ifndef KERNEL_NET_NETIF_H
#define KERNEL_NET_NETIF_H

#include "skb.h"
#include <stddef.h>
#include <stdint.h>

typedef struct netif netif_t;

typedef struct netif_ops {
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

  // [CONFIG] Estado de configuración. Lo consulta /proc/net/dev e
  // ifconfig vía SIOCGIFFLAGS.
  uint8_t is_up;       // IFF_UP
  uint8_t is_loopback; // IFF_LOOPBACK
  uint16_t _pad_flags;

  netif_t *next;
};

typedef void (*netif_rx_handler_t)(netif_t *netif, skb_t *skb);

void net_init(void);

void netif_register(netif_t *netif);
void netif_unregister(netif_t *netif);

netif_t *netif_lookup(const char *name);
netif_t *netif_lookup_by_ip(uint32_t ip);
netif_t *netif_first(void);

// [CONFIG] Iteración por índice, para SIOCGIFCONF.
int netif_count(void);
netif_t *netif_get_by_index(int idx);

// [CONFIG] Configura IP/netmask/gateway. Host order. Si netmask es 0,
// se calcula a partir de la clase (fallback: /24).
void netif_set_addr(netif_t *n, uint32_t ip, uint32_t netmask,
                    uint32_t gateway);

// [CONFIG] Arriba/abajo. Llama a ops->open/close.
void netif_set_up(netif_t *n, int up);
int netif_is_up(const netif_t *n);

// Registra un handler para un ethertype. Máximo 8. Reemplaza si ya
// existe uno para ese ethertype.
int netif_register_handler(uint16_t ethertype, netif_rx_handler_t fn);

// Envía un skb por la interfaz. El skb debe tener dst_mac y protocol
// seteados. netif_tx prepend la cabecera Ethernet. En éxito el skb
// está consumido. En error, netif_tx lo libera.
int netif_tx(netif_t *netif, skb_t *skb);

// Punto de entrada de paquetes recibidos. Lo llama el driver (o
// loopback) desde IRQ o desde el path de envío del loopback.
void netif_rx(skb_t *skb);

#endif