// kernel/net/arp.h
//
// ARP (RFC 826): resolución IP → MAC sobre Ethernet. Tabla con aging,
// cola de pendings por vecino. Sin probe (no mandamos ARP request
// para confirmar una entrada STALE: reenviamos el paquete con la MAC
// vieja y, en paralelo, encolamos un request implícito).

#ifndef KERNEL_NET_ARP_H
#define KERNEL_NET_ARP_H

#include "netif.h"
#include "skb.h"
#include <stdint.h>

#define ARP_TABLE_SIZE 32
#define ARP_PENDING_MAX 8

// Aging en ms (KERNEL_HZ = 1000 → 1 tick = 1 ms).
#define ARP_REACHABLE_TTL_MS 60000ULL
#define ARP_STALE_TTL_MS 120000ULL
#define ARP_INCOMPLETE_TTL_MS 3000ULL

enum {
  ARP_ENTRY_FREE = 0,
  ARP_ENTRY_INCOMPLETE,
  ARP_ENTRY_REACHABLE,
  ARP_ENTRY_STALE,
};

// Inicializa el módulo y registra el handler de ETH_P_ARP en netif_rx.
void arp_init(void);

// Resuelve la MAC de `ip` por `netif`.
//   0        → mac_out rellenada (REACHABLE o STALE)
//   -EAGAIN  → entry INCOMPLETE, request ya enviado (reintenta luego)
//   -ENOMEM  → tabla llena
//   -EINVAL  → parámetros malos
int arp_resolve(netif_t *netif, uint32_t ip, uint8_t mac_out[6]);

// Encola un skb (con header IP, SIN Ethernet) para enviarlo cuando
// llegue el reply. En éxito, ownership del skb pasa al entry.
//   0        → encolado
//   -ENOENT  → no hay entry INCOMPLETE para esa IP
//   -ENOMEM  → cola del entry llena
int arp_queue_pending(netif_t *netif, uint32_t ip, skb_t *skb);

// Aging. Llamar periódicamente (típicamente 1 Hz desde time_tick).
void arp_timer_tick(void);

// Debug: dumpea la tabla por klog.
void arp_dump(void);

// Vacía la tabla ARP. Pensado para tests y para cuando se reconfigura
// una interfaz (cambio de IP, down/up, etc.).
void arp_flush(void);

#endif