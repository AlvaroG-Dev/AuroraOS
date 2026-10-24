// kernel/net/net_proxy.c
#include "net_proxy.h"
#include "../klog.h"
#include "../sched.h"
#include "../spinlock.h"
#include "../string.h"
#include "../uaccess.h"
#include "../wait.h"
#include "skb.h"
#include <stddef.h>

#define PROXY_QUEUE_MAX 64

extern void dhcp_start(netif_t *n);
#define DHCP_RESTART 0x200

typedef struct {
  skb_t *head;
  skb_t *tail;
  int count;
  wait_queue_t wq;
  spinlock_t lock;
} proxy_queue_t;

static netif_t g_proxy_netif;
static proxy_queue_t g_tx_queue; // Kernel -> Userland

// ---------------------------------------------------------------------------
// Callback del stack IP (TX): Kernel quiere enviar un paquete
// ---------------------------------------------------------------------------
static int net_proxy_transmit(netif_t *netif, skb_t *skb) {
  (void)netif;
  if (!skb)
    return -EINVAL;

  unsigned long flags = spin_lock_irqsave(&g_tx_queue.lock);
  if (g_tx_queue.count >= PROXY_QUEUE_MAX) {
    spin_unlock_irqrestore(&g_tx_queue.lock, flags);
    skb_free(skb);
    return -ENOBUFS;
  }

  skb->next = NULL;
  if (g_tx_queue.tail)
    g_tx_queue.tail->next = skb;
  else
    g_tx_queue.head = skb;
  g_tx_queue.tail = skb;
  g_tx_queue.count++;

  spin_unlock_irqrestore(&g_tx_queue.lock, flags);
  wake_up_all(&g_tx_queue.wq);
  return 0;
}

// ---------------------------------------------------------------------------
// VFS ops: /dev/net0
// ---------------------------------------------------------------------------
static bool net_proxy_has_tx(void *arg) {
  (void)arg;
  return g_tx_queue.count > 0;
}

static int64_t net_proxy_read(vfs_node_t *node, uint64_t offset, size_t size,
                              void *buf) {
  (void)node;
  (void)offset;
  if (size < 14)
    return -EINVAL; // Ethernet frame mínimo

  int rc = wait_event_interruptible(&g_tx_queue.wq, net_proxy_has_tx, NULL);
  if (rc < 0)
    return rc;

  unsigned long flags = spin_lock_irqsave(&g_tx_queue.lock);
  skb_t *skb = g_tx_queue.head;
  if (!skb) {
    spin_unlock_irqrestore(&g_tx_queue.lock, flags);
    return -EAGAIN;
  }

  g_tx_queue.head = skb->next;
  if (!g_tx_queue.head)
    g_tx_queue.tail = NULL;
  g_tx_queue.count--;
  spin_unlock_irqrestore(&g_tx_queue.lock, flags);

  size_t len = skb_len(skb);
  if (len > size)
    len = size;
  memcpy(buf, skb_data(skb), len);
  skb_free(skb);
  return (int64_t)len;
}

static int64_t net_proxy_write(vfs_node_t *node, uint64_t offset, size_t size,
                               const void *buf) {
  (void)node;
  (void)offset;
  if (size < 14)
    return -EINVAL;

  skb_t *skb = skb_alloc();
  if (!skb)
    return -ENOMEM;

  uint8_t *p = (uint8_t *)skb_put(skb, size);
  if (!p) {
    skb_free(skb);
    return -ENOMEM;
  }
  memcpy(p, buf, size);
  skb->netif = &g_proxy_netif;

  // Inyectar en el stack del kernel
  netif_rx(skb);
  return (int64_t)size;
}

static int net_proxy_poll(vfs_node_t *node, short events) {
  (void)node;
  int rev = 0;
  if ((events & 1) && g_tx_queue.count > 0)
    rev |= 1; // POLLIN
  rev |= 4;   // POLLOUT (siempre listo para write)
  return rev;
}

static wait_queue_t *net_proxy_poll_wq(vfs_node_t *node) {
  (void)node;
  return &g_tx_queue.wq;
}

static int64_t net_proxy_ioctl(vfs_node_t *node, unsigned long req,
                               uint64_t arg) {
  (void)node;
  (void)arg;
  if (req == DHCP_RESTART) {
    LOG_INFO("[NET-PROXY] Driver userland listo. Relanzando DHCP...");
    dhcp_start(&g_proxy_netif);
    return 0;
  }
  return -ENOTTY;
}

vfs_ops_t net_proxy_ops = {
    .read = net_proxy_read,
    .write = net_proxy_write,
    .poll = net_proxy_poll,
    .poll_wq = net_proxy_poll_wq,
    .ioctl = net_proxy_ioctl, // <-- AÑADIR ESTO
};

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------
static netif_ops_t g_proxy_ops = {
    .transmit = net_proxy_transmit,
    .open = NULL,
    .close = NULL,
};

void net_proxy_init(void) {
  spin_init(&g_tx_queue.lock);
  wait_queue_init(&g_tx_queue.wq);

  memset(&g_proxy_netif, 0, sizeof(g_proxy_netif));
  strcpy(g_proxy_netif.name, "eth0");

  // MAC por defecto (el .aur no puede cambiarla sin un ioctl, así que
  // le damos la de QEMU por defecto).
  const uint8_t mac[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
  memcpy(g_proxy_netif.mac, mac, 6);
  g_proxy_netif.ops = &g_proxy_ops;
  g_proxy_netif.is_up = 1;

  netif_register(&g_proxy_netif);
  LOG_INFO(
      "[NET-PROXY] eth0 virtual listo (esperando e1000e.aur en /dev/net0)");
}

netif_t *e1000e_netif(void) { return netif_lookup("eth0"); }