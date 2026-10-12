// kernel/net/dhcp.c

#include "dhcp.h"
#include "../klog.h"
#include "../sched.h"
#include "../string.h"
#include "../time.h"
#include "../uaccess.h"
#include "../vfs.h"
#include "../wait.h"
#include "byteorder.h"
#include "ip.h"
#include "skb.h"
#include "udp.h"

// Puertos (RFC 2131).
#define DHCP_CLIENT_PORT 68
#define DHCP_SERVER_PORT 67
#define DHCP_BROADCAST 0xFFFFFFFFu
#define DHCP_MAGIC_COOKIE 0x63825363u

// Tipos de mensaje (opción 53).
#define DHCP_MSG_DISCOVER 1
#define DHCP_MSG_OFFER 2
#define DHCP_MSG_REQUEST 3
#define DHCP_MSG_DECLINE 4
#define DHCP_MSG_ACK 5
#define DHCP_MSG_NAK 6

// Flags field (RFC 2131 §2): bit 15 = Broadcast.
#define DHCP_FLAG_BROADCAST 0x8000u

// Offsets dentro del paquete BOOTP.
#define OFF_OP 0
#define OFF_HTYPE 1
#define OFF_HLEN 2
#define OFF_HOPS 3
#define OFF_XID 4
#define OFF_SECS 8
#define OFF_FLAGS 10
#define OFF_CIADDR 12
#define OFF_YIADDR 16
#define OFF_SIADDR 20
#define OFF_GIADDR 24
#define OFF_CHADDR 28
#define OFF_SNAME 44
#define OFF_FILE 108
#define OFF_COOKIE 236 // [FIX] magic cookie
#define OFF_OPTIONS 240

// RFC 1542 §2.1: el cliente MUST enviar al menos 300 bytes de BOOTP.
#define DHCP_MIN_PKT 300

// ---- estado global ----
typedef struct {
  wait_queue_t wq; // despierta al kthread cuando llega reply
  uint32_t xid;    // transaction ID (por sesión)
  uint8_t buf[1024];
  size_t len;
  volatile int got_reply;

  netif_t *netif;

  // Resultado del último reply parseado.
  uint8_t msg_type;
  uint32_t yiaddr;
  uint32_t server_id;
  uint32_t netmask;
  uint32_t gateway;
  uint32_t dns;
  uint32_t lease_time;
} dhcp_state_t;

static dhcp_state_t g_dhcp;

// ---------------------------------------------------------------------------
// Helpers big-endian
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

// Formatea "A.B.C.D" en `out` (necesita >= 16 bytes). Devuelve la
// longitud escrita SIN NUL.
static int fmt_ip(char *out, uint32_t ip) {
  int n = 0;
  for (int shift = 24; shift >= 0; shift -= 8) {
    uint8_t b = (uint8_t)((ip >> shift) & 0xFF);
    if (b >= 100)
      out[n++] = (char)('0' + b / 100);
    if (b >= 10)
      out[n++] = (char)('0' + (b / 10) % 10);
    out[n++] = (char)('0' + b % 10);
    if (shift)
      out[n++] = '.';
  }
  return n;
}

// ---------------------------------------------------------------------------
// RX: handler de UDP puerto 68.
// ---------------------------------------------------------------------------
static bool dhcp_reply_ready(void *arg) {
  (void)arg;
  return __atomic_load_n(&g_dhcp.got_reply, __ATOMIC_ACQUIRE) != 0;
}

static void dhcp_rx(skb_t *skb, uint32_t src_ip, uint16_t src_port,
                    uint32_t dst_ip, uint16_t dst_port) {
  (void)src_ip;
  (void)dst_ip;
  (void)dst_port;
  if (src_port != DHCP_SERVER_PORT) {
    skb_free(skb);
    return;
  }
  size_t len = skb_len(skb);
  if (len < OFF_OPTIONS + 4 || len > sizeof(g_dhcp.buf)) {
    skb_free(skb);
    return;
  }

  // Tomar el wq->lock (mismo patrón que ahci_irq_handler_msi).
  unsigned long flags = spin_lock_irqsave(&g_dhcp.wq.lock);
  memcpy(g_dhcp.buf, skb_data(skb), len);
  g_dhcp.len = len;
  __atomic_store_n(&g_dhcp.got_reply, 1, __ATOMIC_RELEASE);
  wake_up_all_locked(&g_dhcp.wq);
  spin_unlock_irqrestore(&g_dhcp.wq.lock, flags);

  skb_free(skb);
}

// ---------------------------------------------------------------------------
// Parser de opciones.
// ---------------------------------------------------------------------------
static void dhcp_parse_options(const uint8_t *pkt, size_t len) {
  g_dhcp.msg_type = 0;
  g_dhcp.netmask = 0;
  g_dhcp.gateway = 0;
  g_dhcp.dns = 0;
  g_dhcp.lease_time = 0;
  g_dhcp.server_id = 0;

  if (len < OFF_OPTIONS)
    return;
  if (rd32be(pkt + OFF_COOKIE) != DHCP_MAGIC_COOKIE)
    return;

  size_t i = OFF_OPTIONS;
  while (i < len) {
    uint8_t opt = pkt[i];
    if (opt == 255) // End
      break;
    if (opt == 0) { // Pad
      i++;
      continue;
    }
    if (i + 1 >= len)
      break;
    uint8_t olen = pkt[i + 1];
    if (i + 2 + olen > len)
      break;
    const uint8_t *v = pkt + i + 2;
    switch (opt) {
    case 1: // Subnet Mask
      if (olen == 4)
        g_dhcp.netmask = rd32be(v);
      break;
    case 3: // Router (primer valor)
      if (olen >= 4)
        g_dhcp.gateway = rd32be(v);
      break;
    case 6: // DNS (primer valor)
      if (olen >= 4)
        g_dhcp.dns = rd32be(v);
      break;
    case 51: // Lease Time
      if (olen == 4)
        g_dhcp.lease_time = rd32be(v);
      break;
    case 53: // Message Type
      if (olen == 1)
        g_dhcp.msg_type = v[0];
      break;
    case 54: // Server Identifier
      if (olen == 4)
        g_dhcp.server_id = rd32be(v);
      break;
    default:
      break;
    }
    i += 2 + olen;
  }
}

// ---------------------------------------------------------------------------
// Envío de DISCOVER / REQUEST
// ---------------------------------------------------------------------------
static int dhcp_send(uint8_t msg_type, uint32_t requested_ip,
                     uint32_t server_id) {
  skb_t *sk = skb_alloc();
  if (!sk)
    return -ENOMEM;

  uint8_t *p = (uint8_t *)skb_put(sk, DHCP_MIN_PKT);
  if (!p) {
    skb_free(sk);
    return -ENOMEM;
  }
  memset(p, 0, DHCP_MIN_PKT);

  p[OFF_OP] = 1;    // BOOTREQUEST
  p[OFF_HTYPE] = 1; // Ethernet
  p[OFF_HLEN] = 6;
  p[OFF_HOPS] = 0;
  wr32be(p + OFF_XID, g_dhcp.xid);
  wr16be(p + OFF_SECS, 0);
  wr16be(p + OFF_FLAGS, DHCP_FLAG_BROADCAST);
  // ciaddr, yiaddr, siaddr, giaddr = 0
  memcpy(p + OFF_CHADDR, g_dhcp.netif->mac, 6);
  // sname, file: ceros

  // [FIX] El magic cookie va en el offset 236, justo después de
  // `file` (108+128=236). Las opciones empiezan en 240.
  wr32be(p + OFF_COOKIE, DHCP_MAGIC_COOKIE);
  uint8_t *o = p + OFF_OPTIONS;

  // 53: Message Type
  *o++ = 53;
  *o++ = 1;
  *o++ = msg_type;

  // 61: Client Identifier (htype + mac)
  *o++ = 61;
  *o++ = 7;
  *o++ = 1;
  memcpy(o, g_dhcp.netif->mac, 6);
  o += 6;

  // 12: Host Name
  *o++ = 12;
  *o++ = 6;
  memcpy(o, "aurora", 6);
  o += 6;

  // 50 + 54: Requested IP + Server ID (solo en REQUEST)
  if (msg_type == DHCP_MSG_REQUEST) {
    *o++ = 50;
    *o++ = 4;
    wr32be(o, requested_ip);
    o += 4;
    *o++ = 54;
    *o++ = 4;
    wr32be(o, server_id);
    o += 4;
  }

  // 55: Parameter Request List
  *o++ = 55;
  *o++ = 4;
  *o++ = 1;  // Subnet Mask
  *o++ = 3;  // Router
  *o++ = 6;  // DNS
  *o++ = 51; // Lease Time

  // 255: End
  *o++ = 255;

  // src_ip = 0 (no configurado aún). udp_output lo permite y pone el
  // checksum UDP a 0 (RFC 1542 §4.1.1). dst = broadcast.
  return udp_output(sk, 0, DHCP_BROADCAST, DHCP_CLIENT_PORT, DHCP_SERVER_PORT);
}

// ---------------------------------------------------------------------------
// Espera un reply o timeout. Devuelve 1 si llegó, 0 si timeout.
// ---------------------------------------------------------------------------
static int dhcp_wait_reply(uint64_t timeout_ms) {
  uint64_t deadline = sched_get_ticks() + timeout_ms;
  for (;;) {
    if (__atomic_load_n(&g_dhcp.got_reply, __ATOMIC_ACQUIRE))
      return 1;
    uint64_t now = sched_get_ticks();
    if (now >= deadline)
      return 0;
    uint64_t left = deadline - now;
    if (left > 100)
      left = 100;
    wait_event_interruptible_timeout(&g_dhcp.wq, dhcp_reply_ready, NULL, left);
  }
}

// ---------------------------------------------------------------------------
// Escribe el DNS a /etc/resolv.conf (best-effort, sin proceso).
// ---------------------------------------------------------------------------
static void dhcp_write_resolv_conf(uint32_t dns) {
  if (dns == 0)
    return;

  vfs_node_t *n = vfs_lookup("/etc/resolv.conf");
  if (!n) {
    LOG_WARN("[DHCP] /etc/resolv.conf no existe; DNS no persistido");
    return;
  }
  if (!n->ops || !n->ops->write) {
    LOG_WARN("[DHCP] /etc/resolv.conf es read-only");
    vfs_node_free(n);
    return;
  }

  char buf[64];
  int o = 0;
  const char *ns = "nameserver ";
  while (*ns)
    buf[o++] = *ns++;
  o += fmt_ip(buf + o, dns);
  buf[o++] = '\n';

  int64_t wr = n->ops->write(n, 0, (size_t)o, buf);
  if (wr < 0) {
    LOG_WARN("[DHCP] no se pudo escribir /etc/resolv.conf (rc=%lld)",
             (long long)wr);
  } else {
    LOG_INFO("[DHCP] DNS %u.%u.%u.%u guardado en /etc/resolv.conf",
             (dns >> 24) & 0xFF, (dns >> 16) & 0xFF, (dns >> 8) & 0xFF,
             dns & 0xFF);
  }
  vfs_node_free(n);
}

// ---------------------------------------------------------------------------
// Kthread principal.
// ---------------------------------------------------------------------------
static void dhcp_never(void *arg) { (void)arg; }

static void dhcp_sleep_ms(uint64_t ms) {
  wait_queue_t wq;
  wait_queue_init(&wq);
  wait_event_interruptible_timeout(&wq, (bool (*)(void *))NULL, (void *)&wq,
                                   ms);
}

static void dhcp_thread(void) {
  netif_t *n = g_dhcp.netif;
  if (!n) {
    LOG_ERR("[DHCP] dhcp_thread sin netif");
    return;
  }

  LOG_INFO("[DHCP] kthread arrancado en %s", n->name);

  // [1] Esperar a que el link esté up.
  for (int i = 0; i < 50 && !n->is_up; i++)
    dhcp_sleep_ms(100);
  if (!n->is_up) {
    LOG_WARN("[DHCP] %s nunca llegó a up; abort", n->name);
    return;
  }

  // [2] Registrar el handler en el puerto 68.
  if (udp_register_port(DHCP_CLIENT_PORT, dhcp_rx) != 0) {
    LOG_ERR("[DHCP] no se pudo registrar puerto 68");
    return;
  }

  // [3] XID aleatorio.
  uint32_t lo, hi;
  __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
  g_dhcp.xid = lo ^ (hi * 0x9E3779B1u);
  if (g_dhcp.xid == 0)
    g_dhcp.xid = 0x12345678u;

  g_dhcp.yiaddr = 0;
  g_dhcp.server_id = 0;

  // [4] DISCOVER con retransmisiones.
  int got_offer = 0;
  for (int attempt = 0; attempt < 6 && !got_offer; attempt++) {
    __atomic_store_n(&g_dhcp.got_reply, 0, __ATOMIC_RELEASE);
    int rc = dhcp_send(DHCP_MSG_DISCOVER, 0, 0);
    if (rc != 0) {
      LOG_WARN("[DHCP] DISCOVER send failed: %d", rc);
      dhcp_sleep_ms(2000);
      continue;
    }
    LOG_INFO("[DHCP] DISCOVER enviado (xid=0x%08x, intento %d)", g_dhcp.xid,
             attempt + 1);

    if (!dhcp_wait_reply(3000))
      continue;

    // Parse.
    if (rd32be(g_dhcp.buf + OFF_XID) != g_dhcp.xid)
      continue;
    uint32_t yi = rd32be(g_dhcp.buf + OFF_YIADDR);
    if (yi == 0)
      continue;
    dhcp_parse_options(g_dhcp.buf, g_dhcp.len);
    if (g_dhcp.msg_type != DHCP_MSG_OFFER)
      continue;

    g_dhcp.yiaddr = yi;
    got_offer = 1;
    LOG_INFO("[DHCP] OFFER yiaddr=%u.%u.%u.%u server=%u.%u.%u.%u",
             (yi >> 24) & 0xFF, (yi >> 16) & 0xFF, (yi >> 8) & 0xFF, yi & 0xFF,
             (g_dhcp.server_id >> 24) & 0xFF, (g_dhcp.server_id >> 16) & 0xFF,
             (g_dhcp.server_id >> 8) & 0xFF, g_dhcp.server_id & 0xFF);
  }

  if (!got_offer) {
    LOG_ERR("[DHCP] sin OFFER tras 6 intentos; DHCP abortado");
    udp_unregister_port(DHCP_CLIENT_PORT);
    return;
  }

  // [5] REQUEST con retransmisiones.
  int got_ack = 0;
  for (int attempt = 0; attempt < 6 && !got_ack; attempt++) {
    __atomic_store_n(&g_dhcp.got_reply, 0, __ATOMIC_RELEASE);
    int rc = dhcp_send(DHCP_MSG_REQUEST, g_dhcp.yiaddr, g_dhcp.server_id);
    if (rc != 0) {
      dhcp_sleep_ms(2000);
      continue;
    }
    LOG_INFO("[DHCP] REQUEST enviado (intento %d)", attempt + 1);

    if (!dhcp_wait_reply(3000))
      continue;

    if (rd32be(g_dhcp.buf + OFF_XID) != g_dhcp.xid)
      continue;
    dhcp_parse_options(g_dhcp.buf, g_dhcp.len);
    if (g_dhcp.msg_type == DHCP_MSG_ACK) {
      got_ack = 1;
      break;
    }
    if (g_dhcp.msg_type == DHCP_MSG_NAK) {
      LOG_WARN("[DHCP] NAK recibido; reiniciando");
      break;
    }
  }

  udp_unregister_port(DHCP_CLIENT_PORT);

  if (!got_ack) {
    LOG_ERR("[DHCP] sin ACK; DHCP abortado");
    return;
  }

  // [6] Aplicar configuración.
  // Si el servidor no envió netmask, deducirla por clase (fallback).
  if (g_dhcp.netmask == 0) {
    uint8_t top = (uint8_t)(g_dhcp.yiaddr >> 24);
    if (top < 0x80)
      g_dhcp.netmask = 0xFF000000u;
    else if (top < 0xC0)
      g_dhcp.netmask = 0xFFFF0000u;
    else
      g_dhcp.netmask = 0xFFFFFF00u;
  }

  netif_set_addr(n, g_dhcp.yiaddr, g_dhcp.netmask, g_dhcp.gateway);

  LOG_INFO("[DHCP] lease adquirido: ip=%u.%u.%u.%u mask=%u.%u.%u.%u "
           "gw=%u.%u.%u.%u lease=%us",
           (g_dhcp.yiaddr >> 24) & 0xFF, (g_dhcp.yiaddr >> 16) & 0xFF,
           (g_dhcp.yiaddr >> 8) & 0xFF, g_dhcp.yiaddr & 0xFF,
           (g_dhcp.netmask >> 24) & 0xFF, (g_dhcp.netmask >> 16) & 0xFF,
           (g_dhcp.netmask >> 8) & 0xFF, g_dhcp.netmask & 0xFF,
           (g_dhcp.gateway >> 24) & 0xFF, (g_dhcp.gateway >> 16) & 0xFF,
           (g_dhcp.gateway >> 8) & 0xFF, g_dhcp.gateway & 0xFF,
           g_dhcp.lease_time);

  // [7] DNS.
  dhcp_write_resolv_conf(g_dhcp.dns);
}

// ---------------------------------------------------------------------------
// API pública.
// ---------------------------------------------------------------------------
void dhcp_start(netif_t *n) {
  if (!n) {
    LOG_ERR("[DHCP] dhcp_start(NULL)");
    return;
  }
  memset(&g_dhcp, 0, sizeof(g_dhcp));
  wait_queue_init(&g_dhcp.wq);
  g_dhcp.netif = n;

  task_t *t = sched_create_task(dhcp_thread);
  if (!t) {
    LOG_ERR("[DHCP] no se pudo crear el kthread");
    return;
  }
  t->cpu_affinity = 0;
  LOG_INFO("[DHCP] kthread lanzado en %s", n->name);
}