// kernel/net/netif_lkpi.c — puente de NICs de Linux (LinuxKPI) con netif_t.
// Ver Plan_Red.md. El driver de Linux nunca ve skb_t: aquí se copia la trama.
#include "netif_lkpi.h"
#include "../heap.h"
#include "../klog.h"
#include "../string.h"
#include "dhcp.h"
#include "net_proxy.h"
#include "netif.h"
#include "skb.h"

typedef struct lkpi_nic {
  netif_t netif;
  netif_ops_t ops;
  void *cookie;
  lkpi_netif_ops_t kops;
  int dhcp_started;
} lkpi_nic_t;

static int lkpi_nic_transmit(netif_t *n, skb_t *skb) {
  lkpi_nic_t *nic = (lkpi_nic_t *)n->priv;
  int rc = nic->kops.xmit(nic->cookie, skb_data(skb), skb_len(skb));
  skb_free(skb);
  return rc;
}

static int lkpi_nic_open(netif_t *n) {
  lkpi_nic_t *nic = (lkpi_nic_t *)n->priv;
  return nic->kops.open ? nic->kops.open(nic->cookie) : 0;
}

static int lkpi_nic_close(netif_t *n) {
  lkpi_nic_t *nic = (lkpi_nic_t *)n->priv;
  return nic->kops.stop ? nic->kops.stop(nic->cookie) : 0;
}

void *lkpi_netif_register(const uint8_t mac[6], void *cookie,
                          const lkpi_netif_ops_t *ops, char name_out[8]) {
  lkpi_nic_t *nic = kmalloc(sizeof(*nic));
  if (!nic)
    return NULL;
  memset(nic, 0, sizeof(*nic));
  nic->cookie = cookie;
  nic->kops = *ops;
  memcpy(nic->netif.mac, mac, 6);
  nic->ops.transmit = lkpi_nic_transmit;
  nic->ops.open = lkpi_nic_open;
  nic->ops.close = lkpi_nic_close;
  nic->netif.ops = &nic->ops;
  nic->netif.priv = nic;

  // eth0 lo ocupa net_proxy (NIC de un driver .aur de usuario). Si ningún
  // driver de usuario lo ha reclamado (sin MAC propia), se cede.
  netif_t *p = net_proxy_netif();
  if (p && !net_proxy_claimed()) {
    netif_unregister(p);
    LOG_INFO("[NET] net_proxy cede eth0 al driver de Linux");
  }
  int idx = 0;
  for (; idx < 8; idx++) {
    char nm[8] = {'e', 't', 'h', (char)('0' + idx), 0};
    if (!netif_lookup(nm)) {
      memcpy(nic->netif.name, nm, 5);
      break;
    }
  }
  if (idx == 8) {
    kfree(nic);
    return NULL;
  }
  memcpy(name_out, nic->netif.name, 8);
  netif_register(&nic->netif);
  LOG_INFO("[NET] %s registrada (LinuxKPI) MAC %02x:%02x:%02x:%02x:%02x:%02x",
           nic->netif.name, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return nic;
}

void lkpi_netif_unregister(void *handle) {
  lkpi_nic_t *nic = handle;
  if (!nic)
    return;
  netif_unregister(&nic->netif);
  // netif_t puede seguir referenciado por TCP un instante: no se libera.
}

int lkpi_netif_rx(void *handle, const void *frame, size_t len) {
  lkpi_nic_t *nic = handle;
  if (!nic || len < 14 || len > SKB_BUF_SIZE - SKB_DEFAULT_HEADROOM) {
    if (nic)
      nic->netif.rx_dropped++;
    return -1;
  }
  skb_t *sk = skb_alloc();
  if (!sk) {
    nic->netif.rx_dropped++;
    return -1;
  }
  void *p = skb_put(sk, len);
  if (!p) {
    skb_free(sk);
    return -1;
  }
  memcpy(p, frame, len);
  sk->netif = &nic->netif;
  netif_rx(sk);
  return 0;
}

void lkpi_netif_carrier(void *handle, int up) {
  lkpi_nic_t *nic = handle;
  if (!nic)
    return;
  LOG_INFO("[NET] %s: enlace %s", nic->netif.name, up ? "ARRIBA" : "abajo");
  if (up && !nic->dhcp_started) {
    nic->dhcp_started = 1;
    dhcp_start(&nic->netif);
  }
}
