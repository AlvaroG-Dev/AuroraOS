// kernel/net/loopback.c
//
// Loopback con framing Ethernet.
//
//   - netif_tx() prepende la cabecera Ethernet y llama a lo_transmit().
//   - lo_transmit() reinyecta el skb intacto en netif_rx().
//   - netif_rx() lee el ethertype en bytes 12-13 y despacha.
//
// No hay skb_pull(14) en ningún sitio: el paquete sale y vuelve con la
// misma forma. Los handlers de ethertype ven exactamente el mismo
// layout que verán cuando haya un driver real (e1000e).

#include "loopback.h"
#include "../klog.h"
#include "../string.h"
#include "../uaccess.h" // errno
#include "netif.h"
#include "skb.h"

netif_t g_lo_netif;

static int lo_transmit(netif_t *netif, skb_t *skb) {
  if (!netif || !skb)
    return -EINVAL;

  // El skb ya lleva la cabecera Ethernet puesta por netif_tx.
  // Reinyectamos tal cual. Antes fijamos skb->netif = netif para que
  // netif_rx() cuente las stats en 'lo' y no en otra interfaz.
  skb->netif = netif;
  netif_rx(skb);
  return 0;
}

static int lo_open(netif_t *netif) {
  (void)netif;
  return 0;
}

static int lo_close(netif_t *netif) {
  (void)netif;
  return 0;
}

static netif_ops_t g_lo_ops = {
    .transmit = lo_transmit,
    .open = lo_open,
    .close = lo_close,
};

void loopback_init(void) {
  memset(&g_lo_netif, 0, sizeof(g_lo_netif));

  // name[8]: "lo" + NUL + padding.
  g_lo_netif.name[0] = 'l';
  g_lo_netif.name[1] = 'o';
  g_lo_netif.name[2] = '\0';

  // MAC todo ceros (no hay dirección hardware real).
  // IP 127.0.0.1, netmask 255.0.0.0, host order.
  g_lo_netif.ip = 0x7F000001u;
  g_lo_netif.netmask = 0xFF000000u;
  g_lo_netif.gateway = 0;
  g_lo_netif.ops = &g_lo_ops;

  netif_register(&g_lo_netif);
  LOG_INFO("[LO] loopback registrado: lo (127.0.0.1/8)");
}

netif_t *loopback_netif(void) { return &g_lo_netif; }