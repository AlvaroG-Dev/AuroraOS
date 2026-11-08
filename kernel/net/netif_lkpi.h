// kernel/net/netif_lkpi.h — puente entre NICs de drivers de Linux (LinuxKPI)
// y la pila de Aurora (Plan_Red.md). Lo usa kernel/linuxkpi/net/netdev.c.
#ifndef KERNEL_NET_NETIF_LKPI_H
#define KERNEL_NET_NETIF_LKPI_H

#include <stddef.h>
#include <stdint.h>

typedef struct lkpi_netif_ops {
  int (*open)(void *cookie);
  int (*stop)(void *cookie);
  int (*xmit)(void *cookie, const void *frame, size_t len); // 0 = enviado
} lkpi_netif_ops_t;

// Registra la NIC. Toma el nombre eth0 si lo ocupa net_proxy y nadie lo ha
// reclamado (driver de usuario), si no el primer ethN libre. Devuelve el
// handle y deja el nombre en name_out (8 bytes).
void *lkpi_netif_register(const uint8_t mac[6], void *cookie,
                          const lkpi_netif_ops_t *ops, char name_out[8]);
void lkpi_netif_unregister(void *handle);
// Entrega una trama Ethernet completa a la pila. 0 si se aceptó.
int lkpi_netif_rx(void *handle, const void *frame, size_t len);
// Enlace físico: la primera vez que sube, lanza DHCP.
void lkpi_netif_carrier(void *handle, int up);

#endif
