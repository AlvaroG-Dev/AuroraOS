// kernel/swap.c
#include "swap.h"
#include "block.h"
#include "heap.h"
#include "klog.h"
#include "paging.h"
#include "pf.h"
#include "pmm.h"
#include "process.h"
#include "sched.h"
#include "mutex.h"
#include "string.h"
#include "uaccess.h"
#include "wait.h"
#include <stddef.h>

static swap_device_t *g_swap_devs[SWAP_MAX_DEVICES];
static mutex_t g_swap_lock;
static int g_swap_ready = 0;

// Watermarks en páginas. Con 512 MB RAM (131072 páginas), el kernel
// usa ~25000 al arrancar. Libres ≈ 106000.
#define SWAP_LOW_WATERMARK (100000ULL)  // ≈ 390 MB libres
#define SWAP_HIGH_WATERMARK (110000ULL) // ≈ 430 MB libres

void swap_init(void) {
  mutex_init(&g_swap_lock);
  for (int i = 0; i < SWAP_MAX_DEVICES; i++)
    g_swap_devs[i] = NULL;
  g_swap_ready = 1;
  LOG_INFO("[SWAP] Subsistema inicializado");
}

int swap_device_count(void) {
  int n = 0;
  for (int i = 0; i < SWAP_MAX_DEVICES; i++)
    if (g_swap_devs[i] && g_swap_devs[i]->in_use)
      n++;
  return n;
}

static uint32_t rd_u32(const uint8_t *b, size_t o) {
  return (uint32_t)b[o] | ((uint32_t)b[o + 1] << 8) |
         ((uint32_t)b[o + 2] << 16) | ((uint32_t)b[o + 3] << 24);
}

static inline int slot_test(const swap_device_t *d, uint32_t s) {
  if (s >= d->nr_slots)
    return 1;
  return !!(d->slot_bitmap[s / 8] & (1u << (s % 8)));
}
static inline void slot_set(swap_device_t *d, uint32_t s) {
  if (s >= d->nr_slots)
    return;
  d->slot_bitmap[s / 8] |= (1u << (s % 8));
}
static inline void slot_clear(swap_device_t *d, uint32_t s) {
  if (s >= d->nr_slots)
    return;
  d->slot_bitmap[s / 8] &= ~(1u << (s % 8));
}

uint32_t swap_alloc_slot(uint32_t type) {
  if (type >= SWAP_MAX_DEVICES)
    return 0xFFFFFFFFu;
  swap_device_t *d = g_swap_devs[type];
  if (!d || !d->in_use || d->frozen)
    return 0xFFFFFFFFu;

  unsigned long flags = spin_lock_irqsave(&g_swap_lock);
  uint32_t start = d->next_slot;
  for (uint32_t i = 0; i < d->nr_slots; i++) {
    uint32_t s = (start + i) % d->nr_slots;
    if (!slot_test(d, s)) {
      slot_set(d, s);
      d->used_slots++;
      d->next_slot = (s + 1) % d->nr_slots;
      mutex_unlock(&g_swap_lock);
      return s;
    }
  }
  mutex_unlock(&g_swap_lock);
  LOG_WARN("[SWAP] type=%u sin slots (used=%u/%u)", type, d->used_slots,
           d->nr_slots);
  return 0xFFFFFFFFu;
}

void swap_free_slot(uint32_t type, uint32_t slot) {
  if (type >= SWAP_MAX_DEVICES)
    return;
  swap_device_t *d = g_swap_devs[type];
  if (!d || !d->in_use)
    return;

  unsigned long flags = spin_lock_irqsave(&g_swap_lock);
  if (slot < d->nr_slots && slot_test(d, slot)) {
    slot_clear(d, slot);
    if (d->used_slots > 0)
      d->used_slots--;
  }
  mutex_unlock(&g_swap_lock);
}

int swap_write_page(uint32_t type, uint32_t slot, uint64_t phys) {
  if (type >= SWAP_MAX_DEVICES)
    return -EINVAL;
  mutex_lock(&g_swap_lock);
  swap_device_t *d = g_swap_devs[type];
  if (!d || !d->in_use || d->frozen || slot >= d->nr_slots) {
    mutex_unlock(&g_swap_lock);
    return -EINVAL;
  }

  uint64_t byte_off = ((uint64_t)slot + 1) * PAGE_SIZE;
  uint32_t ssz = d->bdev->sector_size;
  if (byte_off % ssz != 0) {
    mutex_unlock(&g_swap_lock);
    return -EINVAL;
  }
  uint64_t lba = byte_off / ssz;
  uint32_t nsec = PAGE_SIZE / ssz;
  int rc = bdev_write(d->bdev, lba, nsec, phys_to_virt(phys));
  mutex_unlock(&g_swap_lock);
  if (rc != 0) {
    LOG_ERR("[SWAP] write type=%u slot=%u rc=%d", type, slot, rc);
    return -EIO;
  }
  return 0;
}

int swap_read_page(uint32_t type, uint32_t slot, uint64_t phys) {
  if (type >= SWAP_MAX_DEVICES)
    return -EINVAL;
  mutex_lock(&g_swap_lock);
  swap_device_t *d = g_swap_devs[type];
  if (!d || !d->in_use || slot >= d->nr_slots) {
    mutex_unlock(&g_swap_lock);
    return -EINVAL;
  }

  uint64_t byte_off = ((uint64_t)slot + 1) * PAGE_SIZE;
  uint32_t ssz = d->bdev->sector_size;
  if (byte_off % ssz != 0) {
    mutex_unlock(&g_swap_lock);
    return -EINVAL;
  }
  uint64_t lba = byte_off / ssz;
  uint32_t nsec = PAGE_SIZE / ssz;
  int rc = bdev_read(d->bdev, lba, nsec, phys_to_virt(phys));
  mutex_unlock(&g_swap_lock);
  if (rc != 0) {
    LOG_ERR("[SWAP] read type=%u slot=%u rc=%d", type, slot, rc);
    return -EIO;
  }
  return 0;
}

}