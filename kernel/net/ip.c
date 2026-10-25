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
#include "../heap.h" // kmalloc, kfree
#include "../klog.h"
#include "../sched.h" // sched_get_ticks
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
static void ip_frag_init(void);

void ip_init(void) {
  memset(g_proto_handlers, 0, sizeof(g_proto_handlers));
  g_ip_id_counter = 0;
  ip_frag_init(); // <-- AÑADIR

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

// ===========================================================================
// [IP-FRAG] Reensamblado de fragmentos IPv4 (RFC 791)
//
// Soporta hasta 8 datagramas pendientes simultáneamente. Cada entrada
// guarda un buffer de 64 KB (tamaño máximo de un datagrama IP) y un
// bitmap para trackear qué bytes han llegado. Cuando todos los bytes
// de [0, total_len) están presentes, el paquete se reensambla y se
// entrega al stack como un único datagrama sin fragmentar.
// ===========================================================================

#define IP_FRAG_MAX_ENTRIES 8
#define IP_FRAG_MAX_SIZE 65535
#define IP_FRAG_TIMEOUT_TICKS 30000 // 30 s

// Bitmap de 1 bit por unidad de 8 bytes. 65536/8 = 8192 bits = 1024 bytes.
#define IP_FRAG_BITMAP_BYTES 1024

typedef struct {
  int in_use;
  uint32_t src_ip;
  uint32_t dst_ip;
  uint16_t id;
  uint8_t proto;
  uint8_t ttl;
  uint16_t total_len; // 0 = desconocido (esperando último frag)
  uint8_t *data;      // kmalloc(IP_FRAG_MAX_SIZE)
  uint8_t bitmap[IP_FRAG_BITMAP_BYTES];
  uint64_t last_tick;
} ip_frag_entry_t;

static ip_frag_entry_t g_frag_table[IP_FRAG_MAX_ENTRIES];
static spinlock_t g_frag_lock;
static int g_frag_initialized = 0;

static void ip_frag_init(void) {
  if (g_frag_initialized)
    return;
  memset(g_frag_table, 0, sizeof(g_frag_table));
  spin_init(&g_frag_lock);
  g_frag_initialized = 1;
}

static void ip_frag_free_locked(ip_frag_entry_t *e) {
  if (e->data) {
    kfree(e->data);
    e->data = NULL;
  }
  memset(e, 0, sizeof(*e));
}

static ip_frag_entry_t *ip_frag_find_locked(uint32_t src, uint32_t dst,
                                            uint16_t id, uint8_t proto) {
  for (int i = 0; i < IP_FRAG_MAX_ENTRIES; i++) {
    ip_frag_entry_t *e = &g_frag_table[i];
    if (e->in_use && e->src_ip == src && e->dst_ip == dst && e->id == id &&
        e->proto == proto)
      return e;
  }
  return NULL;
}

static ip_frag_entry_t *ip_frag_alloc_locked(void) {
  for (int i = 0; i < IP_FRAG_MAX_ENTRIES; i++)
    if (!g_frag_table[i].in_use)
      return &g_frag_table[i];
  // Robar el más antiguo.
  ip_frag_entry_t *oldest = &g_frag_table[0];
  for (int i = 1; i < IP_FRAG_MAX_ENTRIES; i++)
    if (g_frag_table[i].last_tick < oldest->last_tick)
      oldest = &g_frag_table[i];
  ip_frag_free_locked(oldest);
  return oldest;
}

static void ip_frag_mark(ip_frag_entry_t *e, uint32_t start, uint32_t end) {
  uint32_t first = start / 8;
  uint32_t last = (end + 7) / 8;
  for (uint32_t u = first; u < last; u++)
    e->bitmap[u / 8] |= (uint8_t)(1u << (u % 8));
}

static int ip_frag_complete(ip_frag_entry_t *e) {
  if (e->total_len == 0)
    return 0;
  uint32_t units = ((uint32_t)e->total_len + 7) / 8;
  for (uint32_t u = 0; u < units; u++)
    if (!(e->bitmap[u / 8] & (1u << (u % 8))))
      return 0;
  return 1;
}

// Consume `skb` siempre. Devuelve 1 si entregó un datagrama reensamblado
// (llamando recursivamente a ip_input), 0 si solo acumuló.
static int ip_frag_handle(skb_t *skb, const uint8_t *hdr, size_t hdr_len,
                          size_t total_len, uint16_t frag) {
  size_t payload_len = total_len - hdr_len;
  uint32_t frag_offset = (uint32_t)(frag & IP_FRAG_OFFSET_MASK) * 8;
  int has_more = (frag & IP_FLAG_MF) != 0;
  uint32_t src_ip = rd32be(hdr + 12);
  uint32_t dst_ip = rd32be(hdr + 16);
  uint16_t id = rd16be(hdr + 4);
  uint8_t proto = hdr[9];
  uint8_t ttl = hdr[8];

  // Log para diagnóstico: confirmar que los fragmentos llegan.
  LOG_DEBUG("[IP-FRAG] frag id=%u proto=%u off=%u len=%zu more=%d", id, proto,
            frag_offset, payload_len, has_more);

  if (frag_offset + payload_len > IP_FRAG_MAX_SIZE) {
    LOG_DEBUG("[IP-FRAG] frag demasiado grande");
    skb_free(skb);
    return 0;
  }
  if (has_more && (payload_len % 8) != 0) {
    LOG_DEBUG("[IP-FRAG] frag no-final no múltiplo de 8 (len=%zu)",
              payload_len);
    skb_free(skb);
    return 0;
  }

  unsigned long flags = spin_lock_irqsave(&g_frag_lock);

  ip_frag_entry_t *e = ip_frag_find_locked(src_ip, dst_ip, id, proto);
  if (!e) {
    e = ip_frag_alloc_locked();
    if (!e) {
      spin_unlock_irqrestore(&g_frag_lock, flags);
      LOG_WARN("[IP-FRAG] tabla llena");
      skb_free(skb);
      return 0;
    }
    e->in_use = 1;
    e->src_ip = src_ip;
    e->dst_ip = dst_ip;
    e->id = id;
    e->proto = proto;
    e->ttl = ttl;
    e->total_len = 0;
    e->data = (uint8_t *)kmalloc(IP_FRAG_MAX_SIZE);
    if (!e->data) {
      memset(e, 0, sizeof(*e));
      spin_unlock_irqrestore(&g_frag_lock, flags);
      LOG_ERR("[IP-FRAG] kmalloc(%u) falló", IP_FRAG_MAX_SIZE);
      skb_free(skb);
      return 0;
    }
    memset(e->data, 0, IP_FRAG_MAX_SIZE);
    memset(e->bitmap, 0, sizeof(e->bitmap));
  }

  memcpy(e->data + frag_offset, hdr + hdr_len, payload_len);
  ip_frag_mark(e, frag_offset, frag_offset + payload_len);
  if (!has_more)
    e->total_len = (uint16_t)(frag_offset + payload_len);
  e->last_tick = sched_get_ticks();

  if (!ip_frag_complete(e)) {
    spin_unlock_irqrestore(&g_frag_lock, flags);
    skb_free(skb);
    return 0;
  }

  // Reensamblar: sacar datos y liberar la entrada.
  uint16_t full_len = e->total_len;
  uint8_t *full_data = e->data;
  e->data = NULL;
  uint8_t saved_ttl = e->ttl;
  ip_frag_free_locked(e);
  spin_unlock_irqrestore(&g_frag_lock, flags);

  skb_t *out = skb_alloc();
  if (!out) {
    kfree(full_data);
    skb_free(skb);
    return 0;
  }
  uint8_t *ip = (uint8_t *)skb_put(out, hdr_len);
  if (!ip) {
    kfree(full_data);
    skb_free(out);
    skb_free(skb);
    return 0;
  }
  memcpy(ip, hdr, hdr_len);
  wr16be(ip + 2, full_len);
  wr16be(ip + 6, 0); // limpiar flags de fragmentación
  ip[8] = saved_ttl;
  wr16be(ip + 10, 0);
  wr16be(ip + 10, inet_csum(ip, hdr_len));

  uint32_t pay_bytes = (uint32_t)full_len - (uint32_t)hdr_len;
  uint8_t *p = (uint8_t *)skb_put(out, pay_bytes);
  if (!p) {
    kfree(full_data);
    skb_free(out);
    skb_free(skb);
    return 0;
  }
  memcpy(p, full_data, pay_bytes);
  kfree(full_data);
  skb_free(skb);

  LOG_INFO("[IP-FRAG] reensamblado %u bytes (id=%u proto=%u)", full_len, id,
           proto);

  ip_input(out);
  return 1;
}

// Timeout de entradas huérfanas. Llamar cada ~1 s.
void ip_frag_timer_tick(void) {
  if (!g_frag_initialized)
    return;
  uint64_t now = sched_get_ticks();
  unsigned long flags = spin_lock_irqsave(&g_frag_lock);
  for (int i = 0; i < IP_FRAG_MAX_ENTRIES; i++) {
    ip_frag_entry_t *e = &g_frag_table[i];
    if (e->in_use && now - e->last_tick > IP_FRAG_TIMEOUT_TICKS) {
      LOG_DEBUG("[IP-FRAG] timeout id=%u", e->id);
      ip_frag_free_locked(e);
    }
  }
  spin_unlock_irqrestore(&g_frag_lock, flags);
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

  // Fragmentación: reensamblar (RFC 791).
  uint16_t frag = rd16be(hdr + 6);
  if ((frag & (IP_FLAG_MF | IP_FRAG_OFFSET_MASK)) != 0) {
    ip_frag_handle(skb, hdr, hdr_len, total_len, frag);
    return 0; // consumido por ip_frag_handle
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

  // [Fase 6e] Guardar TTL para los protocolos que lo necesiten (ICMP
  // para el cmsg IP_TTL). Se lee aquí porque después del pull el
  // header IP ya no está accesible.
  skb->ttl = hdr[8];

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
  // [DHCP] Si dst es broadcast limitado (255.255.255.255) y no hay ruta,
  // usar cualquier netif que esté up aunque no tenga IP ni gateway.
  // Necesario para DHCP DISCOVER antes de tener IP (src=0, dst=broadcast).
  if (!best && dst_ip == IP_ADDR_BROADCAST) {
    for (netif_t *n = netif_first(); n; n = n->next) {
      if (n->is_up) {
        best = n;
        next_hop = dst_ip;
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

uint32_t ip_pick_src(uint32_t dst_ip, uint32_t requested_src) {
  struct route_result route;
  if (ip_route_lookup(dst_ip, &route) != 0)
    return requested_src;
  if (requested_src == 0)
    return route.netif->ip;
  if ((requested_src & route.netif->netmask) ==
      (route.netif->ip & route.netif->netmask))
    return requested_src;
  return route.netif->ip;
}