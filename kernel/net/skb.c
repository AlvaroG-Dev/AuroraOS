// kernel/net/skb.c
//
// Pool de skb (Socket Buffers) del stack de red.
//
// [FIX-BSS] El pool vive en el HEAP, no en .bss. Con SKB_POOL_SIZE=4096
// y SKB_BUF_SIZE=2048 son ~8 MB que desbordaban HEAP_VMA en el linker
// script. Con el pool en heap, .bss queda pequeño y el linker respira.
//
// heap_init() ya ha corrido cuando skb_init() se llama desde net_init()
// (ver main.c: heap_init() → ... → net_init()), así que kmalloc es seguro.

#include "skb.h"
#include "../heap.h"
#include "../klog.h"
#include "../spinlock.h"
#include "../string.h"

// Punteros al pool y a los buffers, ambos allocados con kmalloc.
static skb_t *g_skb_pool = NULL;
static uint8_t *g_skb_bufs = NULL;

static spinlock_t g_skb_lock;
static skb_t *g_skb_free_list;
static int g_skb_used;

void skb_init(void) {
  spin_init(&g_skb_lock);
  g_skb_used = 0;
  g_skb_free_list = NULL;

  size_t pool_bytes = sizeof(skb_t) * (size_t)SKB_POOL_SIZE;
  size_t buf_bytes = (size_t)SKB_POOL_SIZE * SKB_BUF_SIZE;
  size_t total_bytes = pool_bytes + buf_bytes;

  g_skb_pool = (skb_t *)kmalloc(pool_bytes);
  g_skb_bufs = (uint8_t *)kmalloc(buf_bytes);

  if (!g_skb_pool || !g_skb_bufs) {
    LOG_ERR("[SKB] no hay memoria para el pool (%d x %d B = %lu KB)",
            SKB_POOL_SIZE, SKB_BUF_SIZE, (unsigned long)(total_bytes / 1024));
    if (g_skb_pool) {
      kfree(g_skb_pool);
      g_skb_pool = NULL;
    }
    if (g_skb_bufs) {
      kfree(g_skb_bufs);
      g_skb_bufs = NULL;
    }
    return;
  }

  // Enlazar todos los skbs en la free list. El head_buf de cada uno
  // apunta a su slot correspondiente en g_skb_bufs.
  for (int i = SKB_POOL_SIZE - 1; i >= 0; i--) {
    g_skb_pool[i].head_buf = &g_skb_bufs[(size_t)i * SKB_BUF_SIZE];
    g_skb_pool[i].next = g_skb_free_list;
    g_skb_free_list = &g_skb_pool[i];
  }

  LOG_INFO("[SKB] pool: %d x %d B (%lu KB en heap)", SKB_POOL_SIZE,
           SKB_BUF_SIZE, (unsigned long)(total_bytes / 1024));
}

skb_t *skb_alloc(void) {
  unsigned long flags = spin_lock_irqsave(&g_skb_lock);
  skb_t *s = g_skb_free_list;
  if (s) {
    g_skb_free_list = s->next;
    g_skb_used++;
  }
  spin_unlock_irqrestore(&g_skb_lock, flags);

  if (!s)
    return NULL;

  s->head = SKB_DEFAULT_HEADROOM;
  s->tail = s->head;
  s->end = SKB_BUF_SIZE;
  s->protocol = 0;
  s->ttl = 0;
  s->flags = 0;
  s->netif = NULL;
  s->tcp_seq = 0;
  s->next = NULL;
  memset(s->dst_mac, 0, 6);
  return s;
}

void skb_free(skb_t *s) {
  if (!s)
    return;
  unsigned long flags = spin_lock_irqsave(&g_skb_lock);
  s->next = g_skb_free_list;
  g_skb_free_list = s;
  g_skb_used--;
  spin_unlock_irqrestore(&g_skb_lock, flags);
}

int skb_pool_used(void) {
  unsigned long flags = spin_lock_irqsave(&g_skb_lock);
  int u = g_skb_used;
  spin_unlock_irqrestore(&g_skb_lock, flags);
  return u;
}

int skb_pool_free_count(void) {
  unsigned long flags = spin_lock_irqsave(&g_skb_lock);
  int f = SKB_POOL_SIZE - g_skb_used;
  spin_unlock_irqrestore(&g_skb_lock, flags);
  return f;
}