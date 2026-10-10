// kernel/net/udp.c

#include "udp.h"
#include "../klog.h"
#include "../spinlock.h"
#include "../string.h"
#include "../uaccess.h"
#include "byteorder.h"
#include "checksum.h"
#include "ip.h"

#ifndef EMSGSIZE
#define EMSGSIZE 90
#endif

typedef struct {
  uint16_t port;
  uint16_t _pad;
  udp_handler_t handler;
} udp_binding_t;

static udp_binding_t g_bindings[UDP_PORT_MAX];
static spinlock_t g_udp_lock;

static inline uint16_t rd16be(const uint8_t *p) {
  return (uint16_t)((p[0] << 8) | p[1]);
}
static inline void wr16be(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)(v >> 8);
  p[1] = (uint8_t)(v & 0xFF);
}

static udp_binding_t *find_binding_locked(uint16_t port) {
  for (int i = 0; i < UDP_PORT_MAX; i++) {
    if (g_bindings[i].handler && g_bindings[i].port == port)
      return &g_bindings[i];
  }
  return NULL;
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------
static void udp_input(skb_t *skb, uint32_t src_ip, uint32_t dst_ip,
                      uint8_t proto);

void udp_init(void) {
  memset(g_bindings, 0, sizeof(g_bindings));
  spin_init(&g_udp_lock);

  int rc = ip_register_protocol(IP_PROTO_UDP, udp_input);
  if (rc != 0) {
    LOG_ERR("[UDP] ip_register_protocol(17) rc=%d", rc);
    return;
  }
  LOG_INFO("[UDP] listo (%d puertos)", UDP_PORT_MAX);
}

int udp_register_port(uint16_t port, udp_handler_t handler) {
  if (port == 0 || !handler)
    return -EINVAL;

  unsigned long flags = spin_lock_irqsave(&g_udp_lock);
  udp_binding_t *existing = find_binding_locked(port);
  if (existing) {
    existing->handler = handler;
    spin_unlock_irqrestore(&g_udp_lock, flags);
    return 0;
  }
  for (int i = 0; i < UDP_PORT_MAX; i++) {
    if (!g_bindings[i].handler) {
      g_bindings[i].port = port;
      g_bindings[i].handler = handler;
      spin_unlock_irqrestore(&g_udp_lock, flags);
      return 0;
    }
  }
  spin_unlock_irqrestore(&g_udp_lock, flags);
  return -ENOMEM;
}

int udp_unregister_port(uint16_t port) {
  unsigned long flags = spin_lock_irqsave(&g_udp_lock);
  udp_binding_t *b = find_binding_locked(port);
  int rc = 0;
  if (b) {
    b->handler = NULL;
    b->port = 0;
  } else {
    rc = -ENOENT;
  }
  spin_unlock_irqrestore(&g_udp_lock, flags);
  return rc;
}

// ---------------------------------------------------------------------------
// Input (llamado desde ip_input, skb_data apunta al header UDP).
// ---------------------------------------------------------------------------
static void udp_input(skb_t *skb, uint32_t src_ip, uint32_t dst_ip,
                      uint8_t proto) {
  (void)proto;

  size_t len = skb_len(skb);
  if (len < UDP_HDR_LEN) {
    LOG_DEBUG("[UDP] drop: len=%lu < 8", (unsigned long)len);
    skb_free(skb);
    return;
  }

  uint8_t *h = skb_data(skb);
  uint16_t src_port = rd16be(h + 0);
  uint16_t dst_port = rd16be(h + 2);
  uint16_t udp_len = rd16be(h + 4);
  uint16_t udp_csum = rd16be(h + 6);

  // El header UDP declara la longitud total del datagrama UDP. ip_input
  // ya recortó al total_len del IP, así que len == udp_len.
  if (udp_len < UDP_HDR_LEN || udp_len != len) {
    LOG_DEBUG("[UDP] drop: udp_len=%u skb_len=%lu", udp_len,
              (unsigned long)len);
    skb_free(skb);
    return;
  }

  // Checksum 0 = "no calculado" (RFC 768, IPv4 only).
  if (udp_csum != 0) {
    if (inet_csum_pseudo_verify(src_ip, dst_ip, IP_PROTO_UDP, h, udp_len) !=
        0) {
      LOG_DEBUG("[UDP] drop: checksum malo (sport=%u dport=%u)", src_port,
                dst_port);
      skb_free(skb);
      return;
    }
  }

  unsigned long flags = spin_lock_irqsave(&g_udp_lock);
  udp_binding_t *b = find_binding_locked(dst_port);
  udp_handler_t fn = b ? b->handler : NULL;
  spin_unlock_irqrestore(&g_udp_lock, flags);

  if (!fn) {
    LOG_DEBUG("[UDP] drop: dport=%u sin handler", dst_port);
    skb_free(skb);
    return;
  }

  // Quitar el header UDP; el handler recibe el payload limpio.
  skb_pull(skb, UDP_HDR_LEN);
  fn(skb, src_ip, src_port, dst_ip, dst_port);
}

// ---------------------------------------------------------------------------
// Output
// ---------------------------------------------------------------------------
int udp_output(skb_t *skb, uint32_t src_ip, uint32_t dst_ip, uint16_t src_port,
               uint16_t dst_port) {
  if (!skb)
    return -EINVAL;
  if (src_ip == 0 || src_port == 0) {
    skb_free(skb);
    return -EINVAL;
  }

  size_t payload_len = skb_len(skb);
  if (payload_len > 0xFFFFu - UDP_HDR_LEN) {
    skb_free(skb);
    return -EMSGSIZE;
  }

  uint8_t *h = (uint8_t *)skb_push(skb, UDP_HDR_LEN);
  if (!h) {
    skb_free(skb);
    return -ENOMEM;
  }

  uint16_t udp_len = (uint16_t)(UDP_HDR_LEN + payload_len);
  wr16be(h + 0, src_port);
  wr16be(h + 2, dst_port);
  wr16be(h + 4, udp_len);
  h[6] = 0;
  h[7] = 0; // csum temporal

  uint16_t csum = inet_csum_pseudo(src_ip, dst_ip, IP_PROTO_UDP, h, udp_len);
  // RFC 768: si el checksum calculado es 0, transmitir 0xFFFF.
  // El 0 en el wire significa "no computado" y no lo queremos.
  if (csum == 0)
    csum = 0xFFFF;
  wr16be(h + 6, csum);

  return ip_output(skb, src_ip, dst_ip, IP_PROTO_UDP);
}