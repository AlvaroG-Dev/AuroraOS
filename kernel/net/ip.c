// kernel/net/ip.c
//
// IPv4. Sin fragmentación, sin forwarding, sin opciones.
//
// Estructura de la entrada:
//   netif_rx() -> ip_rx_ethernet (quita Ethernet) -> ip_input()
//   ip_input() -> dispatch al handler del protocolo
//
// Estructura de la salida:
//   ip_output() -> ip_route_lookup() -> netif_tx() -> lo_transmit/driver

#include "ip.h"
#include "../klog.h"
#include "../string.h"
#include "../uaccess.h"
#include "arp.h"
#include "byteorder.h"
#include "checksum.h"
#include "ip.h"
#include "loopback.h"
#include "netif.h"

#define IP_NUM_PROTOS 256
#define IP_DEFAULT_TTL 64

static ip_rx_handler_t g_proto_handlers[IP_NUM_PROTOS];
static uint16_t g_ip_id_counter = 0;

// ---------------------------------------------------------------------------
// Helpers de lectura/escritura big-endian (no usamos structs wire para
// evitar depender del layout exacto y de __attribute__((packed))).
// ---------------------------------------------------------------------------
static inline uint16_t rd16be(const uint8_t *p) {
  return (uint16_t)((p[0] << 8) | p[1]);
}
static inline uint32_t rd32be(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
static inline void wr16be(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)(v >> 8);
  p[1] = (uint8_t)(v & 0xFF);
}
static inline void wr32be(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v >> 24);
  p[1] = (uint8_t)(v >> 16);
  p[2] = (uint8_t)(v >> 8);
  p[3] = (uint8_t)(v & 0xFF);
}

// ¿Es dst_ip una dirección local (de algún netif o broadcast limitado)?
static int ip_is_local(uint32_t dst_ip) {
  if (dst_ip == IP_ADDR_BROADCAST)
    return 1;
  for (netif_t *n = netif_first(); n; n = n->next) {
    if (n->ip != 0 && n->ip == dst_ip)
      return 1;
  }
  return 0;
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------
static void ip_rx_ethernet(netif_t *netif, skb_t *skb);

void ip_init(void) {
  memset(g_proto_handlers, 0, sizeof(g_proto_handlers));
  g_ip_id_counter = 0;

  // Registramos el handler de ETH_P_IP en netif_rx. Máximo 8 handlers
  // por ethertype en netif; los que se registren después reemplazan.
  int rc = netif_register_handler(ETH_P_IP, ip_rx_ethernet);
  if (rc != 0) {
    LOG_ERR("[IP] netif_register_handler(ETH_P_IP) rc=%d", rc);
    return;
  }
  LOG_INFO("[IP] IPv4 listo (sin fragmentación ni forwarding)");
}

int ip_register_protocol(uint8_t proto, ip_rx_handler_t fn) {
  if (!fn)
    return -EINVAL;
  g_proto_handlers[proto] = fn;
  return 0;
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------
static void ip_rx_ethernet(netif_t *netif, skb_t *skb) {
  (void)netif;
  // El skb lleva la cabecera Ethernet (14 B). La quitamos y pasamos
  // a ip_input().
  if (skb_len(skb) < 14) {
    skb_free(skb);
    return;
  }
  skb_pull(skb, 14);
  ip_input(skb);
}

int ip_input(skb_t *skb) {
  if (!skb)
    return -EINVAL;

  size_t len = skb_len(skb);
  if (len < IP_HDR_MIN_LEN) {
    LOG_DEBUG("[IP] drop: len=%lu < 20", (unsigned long)len);
    skb_free(skb);
    return -EINVAL;
  }

  uint8_t *hdr = skb_data(skb);

  // Versión + IHL.
  uint8_t version = (hdr[0] >> 4) & 0x0F;
  uint8_t ihl = hdr[0] & 0x0F;
  if (version != 4) {
    LOG_DEBUG("[IP] drop: version=%u", version);
    skb_free(skb);
    return -EINVAL;
  }
  if (ihl < 5) {
    LOG_DEBUG("[IP] drop: ihl=%u", ihl);
    skb_free(skb);
    return -EINVAL;
  }
  size_t hdr_len = (size_t)ihl * 4;
  if (hdr_len > len) {
    skb_free(skb);
    return -EINVAL;
  }

  // Total length: cabe dentro del buffer y cubre al menos el header.
  uint16_t total_len = rd16be(hdr + 2);
  if (total_len < hdr_len || total_len > len) {
    LOG_DEBUG("[IP] drop: total_len=%u hdr_len=%lu len=%lu", total_len,
              (unsigned long)hdr_len, (unsigned long)len);
    skb_free(skb);
    return -EINVAL;
  }

  // Checksum del header.
  if (inet_csum_verify(hdr, hdr_len) != 0) {
    LOG_DEBUG("[IP] drop: checksum malo");
    skb_free(skb);
    return -EINVAL;
  }

  // Fragmentación no soportada.
  uint16_t frag = rd16be(hdr + 6);
  if ((frag & (IP_FLAG_MF | IP_FRAG_OFFSET_MASK)) != 0) {
    LOG_DEBUG("[IP] drop: fragmento (frag=0x%04x)", frag);
    skb_free(skb);
    return -ENOSYS;
  }

  if (hdr[8] == 0) { // TTL
    skb_free(skb);
    return -EINVAL;
  }

  uint8_t proto = hdr[9];
  uint32_t src_ip = rd32be(hdr + 12);
  uint32_t dst_ip = rd32be(hdr + 16);

  if (!ip_is_local(dst_ip)) {
    LOG_DEBUG("[IP] drop: dst %08x no es local", dst_ip);
    skb_free(skb);
    return -EINVAL;
  }

  // Si el driver entregó más bytes que total_len (padding Ethernet),
  // recortamos antes de quitar el header.
  if (len > total_len)
    skb_trim(skb, len - total_len);

  skb_pull(skb, hdr_len);

  ip_rx_handler_t fn = g_proto_handlers[proto];
  if (!fn) {
    LOG_DEBUG("[IP] drop: proto=%u sin handler", proto);
    skb_free(skb);
    return -ENOSYS;
  }

  fn(skb, src_ip, dst_ip, proto);
  return 0;
}

// ---------------------------------------------------------------------------
// Output
// ---------------------------------------------------------------------------
struct route_result {
  netif_t *netif;
  uint32_t next_hop; // host order (sin gateway real todavía)
  uint8_t dst_mac[6];
};

static int ip_route_lookup(uint32_t dst_ip, struct route_result *out) {
  netif_t *best = NULL;
  uint32_t next_hop = dst_ip;

  // 1) Misma red que algún netif.
  for (netif_t *n = netif_first(); n; n = n->next) {
    if (n->ip == 0)
      continue;
    if ((dst_ip & n->netmask) == (n->ip & n->netmask)) {
      best = n;
      break;
    }
  }

  // 2) Sin match directo: gateway si lo hay.
  if (!best) {
    for (netif_t *n = netif_first(); n; n = n->next) {
      if (n->gateway != 0) {
        best = n;
        next_hop = n->gateway;
        break;
      }
    }
  }
  if (!best)
    return -ENETUNREACH;

  out->netif = best;
  out->next_hop = next_hop;
  memset(out->dst_mac, 0, 6); // se rellena tras ARP o queda 0 en loopback
  return 0;
}

int ip_output(skb_t *skb, uint32_t src_ip, uint32_t dst_ip, uint8_t proto) {
  if (!skb)
    return -EINVAL;

  struct route_result route;
  int rc = ip_route_lookup(dst_ip, &route);
  if (rc != 0) {
    skb_free(skb);
    return rc;
  }

  if (src_ip == 0)
    src_ip = route.netif->ip;

  size_t payload_len = skb_len(skb);
  if (payload_len > 0xFFFFu - IP_HDR_MIN_LEN) {
    skb_free(skb);
    return -EMSGSIZE;
  }

  uint8_t *ip = (uint8_t *)skb_push(skb, IP_HDR_MIN_LEN);
  if (!ip) {
    skb_free(skb);
    return -ENOMEM;
  }
  memset(ip, 0, IP_HDR_MIN_LEN);

  ip[0] = 0x45; // v=4, ihl=5
  wr16be(ip + 2, (uint16_t)(IP_HDR_MIN_LEN + payload_len));
  g_ip_id_counter++;
  wr16be(ip + 4, g_ip_id_counter);
  // frag_off = 0 (DF=0, MF=0, offset=0)
  ip[8] = IP_DEFAULT_TTL;
  ip[9] = proto;
  // csum = 0 temporal
  wr32be(ip + 12, src_ip);
  wr32be(ip + 16, dst_ip);
  uint16_t csum = inet_csum(ip, IP_HDR_MIN_LEN);
  wr16be(ip + 10, csum);

  // Resolver MAC: loopback no usa ARP, el resto sí.
  if (route.netif == loopback_netif()) {
    memset(skb->dst_mac, 0, 6);
  } else {
    int arc = arp_resolve(route.netif, route.next_hop, skb->dst_mac);
    if (arc == -EAGAIN) {
      // No hay MAC todavía. Encolamos el skb en el entry INCOMPLETE.
      skb->protocol = ETH_P_IP;
      skb->netif = route.netif;
      int qrc = arp_queue_pending(route.netif, route.next_hop, skb);
      if (qrc != 0) {
        skb_free(skb);
        return qrc;
      }
      return 0; // ownership transferido al entry
    } else if (arc != 0) {
      skb_free(skb);
      return arc;
    }
  }

  skb->protocol = ETH_P_IP;
  return netif_tx(route.netif, skb);
}