// kernel/net/net_proxy.h
#ifndef KERNEL_NET_PROXY_H
#define KERNEL_NET_PROXY_H

#include "../vfs.h"
#include "netif.h"


// Inicializa la interfaz virtual eth0 y la conecta al stack IP del kernel.
// Se llama desde net_init_task() en main.c.
void net_proxy_init(void);

// VFS ops para /dev/net0. Se registran en vfs.c.
extern vfs_ops_t net_proxy_ops;

// Shim para que dhcp.c y ping.c sigan funcionando sin cambios.
netif_t *e1000e_netif(void);

#endif