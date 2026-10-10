// kernel/net/e1000e.h
//
// Driver Intel 82574L (e1000e). Sin MSI: RX por polling desde un
// kthread. TX copia síncrona.

#ifndef KERNEL_NET_E1000E_H
#define KERNEL_NET_E1000E_H

#include "netif.h"

// Busca la NIC 00:02.0 (o la primera con vendor 0x8086 device 0x10d3),
// mapea MMIO, resetea el chip, configura rings y registra la netif.
// NO crea el kthread de RX: para eso está e1000e_start().
//
// Devuelve 0 si OK, <0 si no hay NIC o falló la inicialización.
int e1000e_init(void);

// Lanza el kthread de polling RX. Llamar SOLO desde una tarea del
// scheduler (kmain_task), nunca desde kmain.
int e1000e_start(void);

// Devuelve la netif registrada, o NULL si e1000e_init() no se ha
// llamado todavía o falló.
netif_t *e1000e_netif(void);

// Procesa el RX ring una vez. Público para tests; en producción lo
// llama el kthread.
void e1000e_poll(void);

#endif