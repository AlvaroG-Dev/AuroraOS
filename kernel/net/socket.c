// kernel/net/socket.c

#include "socket.h"
#include "../heap.h"
#include "../klog.h"
#include "../sched.h"
#include "../spinlock.h"
#include "../string.h"
#include "../uaccess.h"
#include "icmp.h"
#include "ip.h"
#include "netif.h"
#include "udp.h"

#ifndef EAGAIN
#define EAGAIN 11
#endif
#ifndef EPIPE
#define EPIPE 32
#endif
#ifndef ENOTCONN
#define ENOTCONN 107
#endif
#ifndef ECONNREFUSED
#define ECONNREFUSED 111
#endif

// ---------------------------------------------------------------------------
// Estructura interna del socket
// ---------------------------------------------------------------------------
struct socket {
  spinlock_t lock;
  int type;
  int protocol;
  int nonblock;
  int refcount;

  uint32_t local_ip;
  uint16_t local_port;
  uint32_t remote_ip;
  uint16_t remote_port;
  int connected;
  int bound;

  wait_queue_t rx_wq;
  skb_t *rx_head, *rx_tail;
  int rx_count;

  int is_ping;          // 1 si es SOCK_DGRAM + IPPROTO_ICMP
  uint16_t ping_ident;  // ident nuestro (índice en g_ping_socks)
  uint16_t user_ident;  // ident que el usuario puso en el header
  uint64_t rcvtimeo_ms; // 0 = sin timeout
};

// Tabla global de sockets UDP, indexada por puerto local.
#define UDP_SOCK_MAX 64
static socket_t *g_udp_socks[UDP_SOCK_MAX];
static spinlock_t g_socks_lock;

#define PING_SOCK_MAX 32
static socket_t *g_ping_socks[PING_SOCK_MAX];
static uint16_t g_ping_ident_next = 0x8000;

// Identificador del "puerto efímero" para asignaciones sin bind.
static uint16_t g_ephemeral_next = 0xC000;

// ---------------------------------------------------------------------------
// Handler global registrado con UDP: busca el socket por puerto y encola.
// ---------------------------------------------------------------------------
static void udp_socket_input(skb_t *skb, uint32_t src_ip, uint16_t src_port,
                             uint32_t dst_ip, uint16_t dst_port) {
  (void)dst_ip;

  unsigned long flags = spin_lock_irqsave(&g_socks_lock);
  socket_t *s = NULL;
  for (int i = 0; i < UDP_SOCK_MAX; i++) {
    if (g_udp_socks[i] && g_udp_socks[i]->local_port == dst_port) {
      s = g_udp_socks[i];
      break;
    }
  }
  if (s)
    socket_ref(s);
  spin_unlock_irqrestore(&g_socks_lock, flags);

  if (!s) {
    skb_free(skb);
    return;
  }

  // Aprendemos el peer si el socket no está conectado.
  flags = spin_lock_irqsave(&s->lock);
  if (!s->connected) {
    s->remote_ip = src_ip;
    s->remote_port = src_port;
  }
  spin_unlock_irqrestore(&s->lock, flags);

  socket_rx_enqueue(s, skb);
  socket_put(s);
}

// ---------------------------------------------------------------------------
// Slot en la tabla UDP
// ---------------------------------------------------------------------------
static int udp_table_insert(socket_t *s) {
  unsigned long flags = spin_lock_irqsave(&g_socks_lock);
  for (int i = 0; i < UDP_SOCK_MAX; i++) {
    if (!g_udp_socks[i]) {
      g_udp_socks[i] = s;
      spin_unlock_irqrestore(&g_socks_lock, flags);
      return 0;
    }
  }
  spin_unlock_irqrestore(&g_socks_lock, flags);
  return -ENOMEM;
}

static void udp_table_remove(socket_t *s) {
  unsigned long flags = spin_lock_irqsave(&g_socks_lock);
  for (int i = 0; i < UDP_SOCK_MAX; i++) {
    if (g_udp_socks[i] == s) {
      g_udp_socks[i] = NULL;
      break;
    }
  }
  spin_unlock_irqrestore(&g_socks_lock, flags);
}

static int udp_port_in_use(uint16_t port) {
  unsigned long flags = spin_lock_irqsave(&g_socks_lock);
  int found = 0;
  for (int i = 0; i < UDP_SOCK_MAX; i++) {
    if (g_udp_socks[i] && g_udp_socks[i]->local_port == port) {
      found = 1;
      break;
    }
  }
  spin_unlock_irqrestore(&g_socks_lock, flags);
  return found;
}

static uint16_t udp_alloc_ephemeral(void) {
  for (int i = 0; i < 0x3FFF; i++) {
    uint16_t p = g_ephemeral_next++;
    if (p < 0xC000)
      p = 0xC000;
    if (p == 0)
      continue;
    if (!udp_port_in_use(p))
      return p;
  }
  return 0;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------
void socket_subsystem_init(void) {
  memset(g_udp_socks, 0, sizeof(g_udp_socks));
  spin_init(&g_socks_lock);
  g_ephemeral_next = 0xC000;
  LOG_INFO("[SOCK] subsistema inicializado");
}

static int ping_table_insert(socket_t *s) {
  unsigned long flags = spin_lock_irqsave(&g_socks_lock);
  for (int i = 0; i < PING_SOCK_MAX; i++) {
    if (!g_ping_socks[i]) {
      g_ping_socks[i] = s;
      s->ping_ident = (uint16_t)(0x8000 + i);
      spin_unlock_irqrestore(&g_socks_lock, flags);
      return 0;
    }
  }
  spin_unlock_irqrestore(&g_socks_lock, flags);
  return -ENOMEM;
}

static void ping_table_remove(socket_t *s) {
  unsigned long flags = spin_lock_irqsave(&g_socks_lock);
  for (int i = 0; i < PING_SOCK_MAX; i++) {
    if (g_ping_socks[i] == s) {
      g_ping_socks[i] = NULL;
      break;
    }
  }
  spin_unlock_irqrestore(&g_socks_lock, flags);
}

static socket_t *ping_lookup(uint16_t ident) {
  if (ident < 0x8000 || ident >= 0x8000 + PING_SOCK_MAX)
    return NULL;
  unsigned long flags = spin_lock_irqsave(&g_socks_lock);
  socket_t *s = g_ping_socks[ident - 0x8000];
  if (s)
    socket_ref(s);
  spin_unlock_irqrestore(&g_socks_lock, flags);
  return s;
}

// Callback registrado con ICMP. Llega un echo reply ya validado; si el
// ident corresponde a un ping socket nuestro, mutamos el header para
// restaurar el ident del usuario y lo encolamos.
static void ping_reply_cb(uint32_t src_ip, uint16_t ident, uint16_t seq,
                          const uint8_t *data, size_t data_len) {
  (void)seq;
  socket_t *s = ping_lookup(ident);
  if (!s)
    return;

  // Reconstruir el ICMP completo (8 B header + payload) con el ident
  // del usuario.
  skb_t *sk = skb_alloc();
  if (!sk) {
    socket_put(s);
    return;
  }
  uint8_t *p = (uint8_t *)skb_put(sk, 8 + data_len);
  if (!p) {
    skb_free(sk);
    socket_put(s);
    return;
  }
  p[0] = 0; // echo reply
  p[1] = 0;
  p[2] = 0;
  p[3] = 0; // csum temporal
  p[4] = (uint8_t)(s->user_ident >> 8);
  p[5] = (uint8_t)(s->user_ident & 0xFF);
  p[6] = (uint8_t)(seq >> 8);
  p[7] = (uint8_t)(seq & 0xFF);
  if (data_len)
    memcpy(p + 8, data, data_len);

  // checksum
  uint32_t sum = 0;
  for (size_t i = 0; i < 8 + data_len; i += 2) {
    uint16_t w =
        (uint16_t)((p[i] << 8) | (i + 1 < 8 + data_len ? p[i + 1] : 0));
    sum += w;
  }
  while (sum >> 16)
    sum = (sum & 0xFFFF) + (sum >> 16);
  uint16_t csum = (uint16_t)(~sum & 0xFFFF);
  p[2] = (uint8_t)(csum >> 8);
  p[3] = (uint8_t)(csum & 0xFF);

  // El peer del socket queda fijado al primer reply (UDP style).
  unsigned long flags = spin_lock_irqsave(&s->lock);
  if (!s->connected) {
    s->remote_ip = src_ip;
  }
  spin_unlock_irqrestore(&s->lock, flags);

  socket_rx_enqueue(s, sk);
  socket_put(s);
}

socket_t *socket_create(int type, int protocol) {
  if (type != SOCK_DGRAM)
    return NULL;
  if (protocol != SOCK_PROTO_AUTO && protocol != SOCK_IPPROTO_UDP &&
      protocol != SOCK_IPPROTO_ICMP)
    return NULL;

  socket_t *s = (socket_t *)kzalloc(sizeof(*s));
  if (!s)
    return NULL;
  spin_init(&s->lock);
  wait_queue_init(&s->rx_wq);
  s->type = type;
  s->refcount = 1;
  s->local_ip = SOCK_ADDR_ANY;

  if (protocol == SOCK_IPPROTO_ICMP) {
    s->protocol = SOCK_IPPROTO_ICMP;
    s->is_ping = 1;
    if (ping_table_insert(s) != 0) {
      kfree(s);
      return NULL;
    }
    icmp_set_echo_reply_cb(ping_reply_cb);
  } else {
    s->protocol = SOCK_IPPROTO_UDP;
  }
  return s;
}

void socket_ref(socket_t *s) {
  if (s)
    __atomic_fetch_add(&s->refcount, 1, __ATOMIC_ACQ_REL);
}

void socket_put(socket_t *s) {
  if (!s)
    return;
  if (__atomic_fetch_sub(&s->refcount, 1, __ATOMIC_ACQ_REL) != 1)
    return;
  if (s->is_ping) {
    icmp_remove_echo_reply_cb(ping_reply_cb);
    ping_table_remove(s);
  } else if (s->bound) {
    udp_table_remove(s);
  }
  skb_t *p = s->rx_head;
  while (p) {
    skb_t *n = p->next;
    skb_free(p);
    p = n;
  }
  kfree(s);
}

// ---------------------------------------------------------------------------
// Bind / connect
// ---------------------------------------------------------------------------
int socket_bind(socket_t *s, uint32_t ip, uint16_t port) {
  if (!s)
    return -EINVAL;
  if (s->bound)
    return -EINVAL;

  if (ip == SOCK_ADDR_ANY) {
    // Preferimos eth0 (IP "real" en QEMU user-mode).
    netif_t *n = netif_lookup("eth0");
    LOG_DEBUG("[SOCK] bind: netif_lookup(eth0)=%p", (void *)n);
    if (n && n->ip != 0) {
      ip = n->ip;
    } else {
      for (n = netif_first(); n; n = n->next) {
        LOG_DEBUG("[SOCK] bind: candidato %s ip=%08x", n->name, n->ip);
        if (n->ip != 0) {
          ip = n->ip;
          break;
        }
      }
    }
    if (ip == 0) {
      // Último recurso: 10.0.2.15 (QEMU user-mode networking).
      ip = 0x0A00020Fu;
      LOG_WARN("[SOCK] bind: sin netif con IP, usando 10.0.2.15");
    }
  }

  if (port == 0) {
    port = udp_alloc_ephemeral();
    if (port == 0)
      return -EADDRINUSE;
  }

  if (udp_port_in_use(port))
    return -EADDRINUSE;

  s->local_ip = ip;
  s->local_port = port;

  int rc = udp_register_port(port, udp_socket_input);
  if (rc != 0)
    return rc;

  rc = udp_table_insert(s);
  if (rc != 0) {
    udp_unregister_port(port);
    return rc;
  }

  s->bound = 1;
  return 0;
}

int socket_connect(socket_t *s, uint32_t ip, uint16_t port) {
  if (!s)
    return -EINVAL;
  if (!s->bound) {
    int rc = socket_bind(s, SOCK_ADDR_ANY, 0);
    if (rc != 0)
      return rc;
  }
  s->remote_ip = ip;
  s->remote_port = port;
  s->connected = 1;
  return 0;
}

// ---------------------------------------------------------------------------
// Send
// ---------------------------------------------------------------------------
static int socket_sendto_udp(socket_t *s, const void *buf, size_t len,
                             uint32_t dst_ip, uint16_t dst_port) {
  if (!s->bound) {
    int rc = socket_bind(s, SOCK_ADDR_ANY, 0);
    if (rc != 0)
      return rc;
  }

  skb_t *sk = skb_alloc();
  if (!sk)
    return -ENOMEM;

  uint8_t *p = (uint8_t *)skb_put(sk, len);
  if (!p) {
    skb_free(sk);
    return -ENOMEM;
  }
  memcpy(p, buf, len);

  return udp_output(sk, s->local_ip, dst_ip, s->local_port, dst_port);
}

int socket_sendto(socket_t *s, const void *buf, size_t len, uint32_t dst_ip,
                  uint16_t dst_port) {
  (void)dst_port;
  if (!s || !buf)
    return -EINVAL;
  if (s->is_ping) {
    if (len < 8)
      return -EINVAL;
    if (!s->bound) {
      int rc = socket_bind(s, SOCK_ADDR_ANY, 0);
      if (rc != 0)
        return rc;
    }
    // Guardar el ident del usuario y reemplazarlo por el nuestro.
    const uint8_t *ub = (const uint8_t *)buf;
    s->user_ident = (uint16_t)((ub[4] << 8) | ub[5]);

    skb_t *sk = skb_alloc();
    if (!sk)
      return -ENOMEM;
    uint8_t *p = (uint8_t *)skb_put(sk, len);
    if (!p) {
      skb_free(sk);
      return -ENOMEM;
    }
    memcpy(p, buf, len);
    // ICMP echo request (asumimos que el user puso type=8).
    p[0] = 8;
    p[1] = 0;
    p[4] = (uint8_t)(s->ping_ident >> 8);
    p[5] = (uint8_t)(s->ping_ident & 0xFF);
    // Recalcular checksum sobre el buffer con id ya reemplazado.
    p[2] = 0;
    p[3] = 0;
    uint32_t sum = 0;
    for (size_t i = 0; i + 1 < len; i += 2)
      sum += (uint16_t)((p[i] << 8) | p[i + 1]);
    if (len & 1)
      sum += (uint16_t)(p[len - 1] << 8);
    while (sum >> 16)
      sum = (sum & 0xFFFF) + (sum >> 16);
    uint16_t csum = (uint16_t)(~sum & 0xFFFF);
    p[2] = (uint8_t)(csum >> 8);
    p[3] = (uint8_t)(csum & 0xFF);

    return ip_output(sk, s->local_ip, dst_ip, SOCK_IPPROTO_ICMP);
  }
  if (s->protocol != SOCK_IPPROTO_UDP && !s->is_ping)
    return -EOPNOTSUPP;
  return socket_sendto_udp(s, buf, len, dst_ip, dst_port);
}

int socket_send(socket_t *s, const void *buf, size_t len) {
  if (!s || !buf)
    return -EINVAL;
  if (!s->connected)
    return -ENOTCONN;
  return socket_sendto(s, buf, len, s->remote_ip, s->remote_port);
}

// ---------------------------------------------------------------------------
// Recv
// ---------------------------------------------------------------------------
static bool socket_has_data(void *arg) {
  socket_t *s = (socket_t *)arg;
  unsigned long flags = spin_lock_irqsave(&s->lock);
  int has = (s->rx_count > 0);
  spin_unlock_irqrestore(&s->lock, flags);
  return has;
}

static int socket_recvfrom_udp(socket_t *s, void *buf, size_t len,
                               uint32_t *src_ip, uint16_t *src_port,
                               int *would_block) {
  for (;;) {
    unsigned long flags = spin_lock_irqsave(&s->lock);
    skb_t *sk = s->rx_head;
    if (sk) {
      s->rx_head = sk->next;
      if (!s->rx_head)
        s->rx_tail = NULL;
      s->rx_count--;
      // El skb que recibimos ya viene con el payload limpio; los
      // metadatos los tenemos del socket (peer).
      uint32_t sip = s->remote_ip;
      uint16_t sp = s->remote_port;
      spin_unlock_irqrestore(&s->lock, flags);

      size_t n = skb_len(sk);
      if (n > len)
        n = len;
      memcpy(buf, skb_data(sk), n);
      skb_free(sk);

      if (src_ip)
        *src_ip = sip;
      if (src_port)
        *src_port = sp;
      return (int)n;
    }
    spin_unlock_irqrestore(&s->lock, flags);

    if (s->nonblock) {
      *would_block = 1;
      return -EAGAIN;
    }

    if (s->rcvtimeo_ms > 0) {
      // Dormir como máximo rcvtimeo_ms. Si expira, devolvemos -EAGAIN
      // SIN marcar would_block (no es non-block, es timeout).
      long r = wait_event_interruptible_timeout(&s->rx_wq, socket_has_data, s,
                                                s->rcvtimeo_ms);
      if (r == 0) {
        return -EAGAIN; // timeout
      }
      if (r < 0) {
        return -EINTR;
      }
      // r > 0: hay datos, seguir el bucle.
      continue;
    }

    wait_event_interruptible(&s->rx_wq, socket_has_data, s);
  }
}

int socket_recvfrom(socket_t *s, void *buf, size_t len, uint32_t *src_ip,
                    uint16_t *src_port, int *would_block) {
  if (!s || !buf)
    return -EINVAL;
  if (would_block)
    *would_block = 0;
  if (s->protocol != SOCK_IPPROTO_UDP && !s->is_ping)
    return -EOPNOTSUPP;
  return socket_recvfrom_udp(s, buf, len, src_ip, src_port, would_block);
}

int socket_recv(socket_t *s, void *buf, size_t len, int *would_block) {
  if (!s || !buf)
    return -EINVAL;
  if (!s->connected)
    return -ENOTCONN;
  return socket_recvfrom(s, buf, len, NULL, NULL, would_block);
}

// ---------------------------------------------------------------------------
// RX enqueue
// ---------------------------------------------------------------------------
void socket_rx_enqueue(socket_t *s, skb_t *skb) {
  if (!s || !skb)
    return;
  skb->next = NULL;
  unsigned long flags = spin_lock_irqsave(&s->lock);
  if (s->rx_tail)
    s->rx_tail->next = skb;
  else
    s->rx_head = skb;
  s->rx_tail = skb;
  s->rx_count++;
  spin_unlock_irqrestore(&s->lock, flags);
  wake_up_all(&s->rx_wq);
}

// ---------------------------------------------------------------------------
// Getters
// ---------------------------------------------------------------------------
uint16_t socket_local_port(const socket_t *s) { return s ? s->local_port : 0; }
uint32_t socket_local_ip(const socket_t *s) { return s ? s->local_ip : 0; }
uint32_t socket_remote_ip(const socket_t *s) { return s ? s->remote_ip : 0; }
uint16_t socket_remote_port(const socket_t *s) {
  return s ? s->remote_port : 0;
}
int socket_is_connected(const socket_t *s) { return s ? s->connected : 0; }

void socket_set_nonblock(socket_t *s, int nb) {
  if (s)
    s->nonblock = nb;
}

int socket_is_nonblock(const socket_t *s) { return s ? s->nonblock : 0; }

wait_queue_t *socket_rx_wq(socket_t *s) { return s ? &s->rx_wq : NULL; }

void socket_set_rcvtimeo(socket_t *s, uint64_t ms) {
  if (s)
    s->rcvtimeo_ms = ms;
}

uint64_t socket_get_rcvtimeo(const socket_t *s) {
  return s ? s->rcvtimeo_ms : 0;
}