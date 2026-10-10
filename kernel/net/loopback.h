// kernel/net/loopback.h
//
// Interfaz loopback ('lo'). Registrada una vez por loopback_init().

#ifndef KERNEL_NET_LOOPBACK_H
#define KERNEL_NET_LOOPBACK_H

#include "netif.h"

// La netif de loopback (singleton). Válida tras loopback_init().
extern netif_t g_lo_netif;

// Registra 'lo' en el core de red. Llamar una vez, tras net_init().
void loopback_init(void);

// Devuelve la netif de loopback, o NULL si todavía no se ha llamado
// a loopback_init().
netif_t *loopback_netif(void);

#endif