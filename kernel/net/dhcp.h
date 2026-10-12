// kernel/net/dhcp.h
//
// Cliente DHCP mínimo (RFC 2131/2132). Kthread que corre tras
// e1000e_init(), hace DISCOVER → OFFER → REQUEST → ACK, configura
// la netif y escribe /etc/resolv.conf.

#ifndef KERNEL_NET_DHCP_H
#define KERNEL_NET_DHCP_H

#include "netif.h"

// Lanza el kthread de DHCP sobre `n`. Llamar SOLO desde una tarea
// del scheduler (kmain_task). No bloquea.
void dhcp_start(netif_t *n);

#endif