// kernel/net/skb.c

#include "skb.h"
#include "../klog.h"
#include "../spinlock.h"
#include "../string.h"

static skb_t g_skb_pool[SKB_POOL_SIZE];
static uint8_t g_skb_bufs[SKB_POOL_SIZE][SKB_BUF_SIZE];

static spinlock_t g_skb_lock;
static skb_t *g_skb_free_list;
static int g_skb_used;

void skb_init(void) {
  spin_init(&g_skb_lock);
  g_skb_used = 0;
  g_skb_free_list = NULL;

  for (int i = SKB_POOL_SIZE - 1; i >= 0; i--) {
    g_skb_pool[i].head_buf = g_skb_bufs[i];
    g_skb_pool[i].next = g_skb_free_list;
    g_skb_free_list = &g_skb_pool[i];
  }

  LOG_INFO("[SKB] pool: %d x %d B (%d KB)", SKB_POOL_SIZE, SKB_BUF_SIZE,
           (SKB_POOL_SIZE * SKB_BUF_SIZE) / 1024);
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
  s->flags = 0;
  s->netif = NULL;
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