// kernel/bcache.c — caché de bloques genérica (ver bcache.h).

#include "bcache.h"

#include "heap.h"
#include "klog.h"
#include "mutex.h"
#include "string.h"

#define BC_HASH_BUCKETS 1024
#define BC_DEFAULT_LIMIT (16u * 1024u * 1024u)
#define BC_MAX_BS 4096u

struct bc_buf {
  block_device_t *bdev;
  uint64_t blk;
  uint32_t bs;
  int refs;
  uint8_t dirty;
  uint8_t *data;
  struct bc_buf *hnext;
  struct bc_buf *prev, *next; // LRU: head = más reciente
};

static mutex_t g_bc_lock;
static int g_bc_inited;
static bc_buf_t *g_hash[BC_HASH_BUCKETS];
static bc_buf_t *g_lru_head, *g_lru_tail;
static size_t g_bytes, g_limit = BC_DEFAULT_LIMIT;
static struct bc_stats g_stats;

static inline uint32_t bc_hash(block_device_t *bdev, uint64_t blk) {
  uint64_t h = (uint64_t)(uintptr_t)bdev * 11400714819323198485ULL;
  h ^= blk * 2654435761ULL;
  return (uint32_t)(h >> 20) & (BC_HASH_BUCKETS - 1);
}

void bc_init(void) {
  if (g_bc_inited)
    return;
  mutex_init(&g_bc_lock);
  g_bc_inited = 1;
}

static void lru_unlink(bc_buf_t *b) {
  if (b->prev)
    b->prev->next = b->next;
  else
    g_lru_head = b->next;
  if (b->next)
    b->next->prev = b->prev;
  else
    g_lru_tail = b->prev;
  b->prev = b->next = NULL;
}

static void lru_push_front(bc_buf_t *b) {
  b->prev = NULL;
  b->next = g_lru_head;
  if (g_lru_head)
    g_lru_head->prev = b;
  g_lru_head = b;
  if (!g_lru_tail)
    g_lru_tail = b;
}

static void hash_remove(bc_buf_t *b) {
  bc_buf_t **pp = &g_hash[bc_hash(b->bdev, b->blk)];
  while (*pp) {
    if (*pp == b) {
      *pp = b->hnext;
      return;
    }
    pp = &(*pp)->hnext;
  }
}

static bc_buf_t *hash_find(block_device_t *bdev, uint64_t blk) {
  for (bc_buf_t *b = g_hash[bc_hash(bdev, blk)]; b; b = b->hnext)
    if (b->bdev == bdev && b->blk == blk)
      return b;
  return NULL;
}

// Escribe un búfer sucio (lock tomado). 0 o -EIO.
static int bc_write_locked(bc_buf_t *b) {
  uint32_t ss = b->bdev->sector_size;
  uint64_t lba = b->blk * (uint64_t)(b->bs / ss);
  if (bdev_write(b->bdev, lba, b->bs / ss, b->data) != 0) {
    LOG_ERR("[BCACHE] error escribiendo %s bloque %llu", b->bdev->name,
            (unsigned long long)b->blk);
    return -EIO;
  }
  b->dirty = 0;
  g_stats.writes++;
  if (g_stats.dirty)
    g_stats.dirty--;
  return 0;
}

static void bc_free_locked(bc_buf_t *b) {
  hash_remove(b);
  lru_unlink(b);
  g_bytes -= b->bs;
  if (g_stats.bufs)
    g_stats.bufs--;
  kfree(b->data);
  kfree(b);
}

// Expulsa búfers sin referencias, del más antiguo al más nuevo, hasta
// que `g_bytes + extra <= g_limit`. Un sucio que no se puede escribir se
// deja (la caché puede pasarse de la cota antes que perder datos).
static void bc_shrink_locked(size_t extra) {
  bc_buf_t *b = g_lru_tail;
  while (b && g_bytes + extra > g_limit) {
    bc_buf_t *prev = b->prev;
    if (b->refs == 0) {
      if (!b->dirty || bc_write_locked(b) == 0) {
        bc_free_locked(b);
        g_stats.evictions++;
      }
    }
    b = prev;
  }
}

static int bc_args_ok(block_device_t *bdev, uint32_t bs) {
  if (!g_bc_inited || !bdev || bdev->sector_size == 0)
    return 0;
  if (bs < 512 || bs > BC_MAX_BS || (bs & (bs - 1)))
    return 0;
  return bs >= bdev->sector_size && (bs % bdev->sector_size) == 0;
}

static bc_buf_t *bc_get_impl(block_device_t *bdev, uint64_t blk, uint32_t bs,
                             int read) {
  if (!bc_args_ok(bdev, bs))
    return NULL;
  uint64_t nblocks = bdev->num_sectors * bdev->sector_size / bs;
  if (blk >= nblocks)
    return NULL;

  mutex_lock(&g_bc_lock);
  bc_buf_t *b = hash_find(bdev, blk);
  if (b) {
    if (b->bs != bs) { // mezcla de tamaños en un mismo bdev: bug del llamante
      mutex_unlock(&g_bc_lock);
      LOG_ERR("[BCACHE] %s: bs %u != %u", bdev->name, bs, b->bs);
      return NULL;
    }
    g_stats.hits++;
    b->refs++;
    lru_unlink(b);
    lru_push_front(b);
    mutex_unlock(&g_bc_lock);
    return b;
  }
  g_stats.misses++;

  bc_shrink_locked(bs);
  b = kzalloc(sizeof(*b));
  uint8_t *data = b ? kzalloc(bs) : NULL;
  if (!b || !data) {
    kfree(b);
    kfree(data);
    mutex_unlock(&g_bc_lock);
    return NULL;
  }
  b->bdev = bdev;
  b->blk = blk;
  b->bs = bs;
  b->data = data;
  if (read) {
    uint32_t ss = bdev->sector_size;
    if (bdev_read(bdev, blk * (uint64_t)(bs / ss), bs / ss, data) != 0) {
      LOG_ERR("[BCACHE] error leyendo %s bloque %llu", bdev->name,
              (unsigned long long)blk);
      kfree(data);
      kfree(b);
      mutex_unlock(&g_bc_lock);
      return NULL;
    }
  }
  b->refs = 1;
  uint32_t h = bc_hash(bdev, blk);
  b->hnext = g_hash[h];
  g_hash[h] = b;
  lru_push_front(b);
  g_bytes += bs;
  g_stats.bufs++;
  mutex_unlock(&g_bc_lock);
  return b;
}

bc_buf_t *bc_get(block_device_t *bdev, uint64_t blk, uint32_t bs) {
  return bc_get_impl(bdev, blk, bs, 1);
}

bc_buf_t *bc_get_new(block_device_t *bdev, uint64_t blk, uint32_t bs) {
  bc_buf_t *b = bc_get_impl(bdev, blk, bs, 0);
  if (b) { // si ya estaba en caché, el llamante pide contenido nuevo (a cero)
    mutex_lock(&g_bc_lock);
    memset(b->data, 0, b->bs);
    mutex_unlock(&g_bc_lock);
  }
  return b;
}

void *bc_data(bc_buf_t *b) { return b ? b->data : NULL; }
uint32_t bc_size(const bc_buf_t *b) { return b ? b->bs : 0; }

void bc_mark_dirty(bc_buf_t *b) {
  if (!b)
    return;
  mutex_lock(&g_bc_lock);
  if (!b->dirty) {
    b->dirty = 1;
    g_stats.dirty++;
  }
  mutex_unlock(&g_bc_lock);
}

void bc_put(bc_buf_t *b) {
  if (!b)
    return;
  mutex_lock(&g_bc_lock);
  if (b->refs > 0)
    b->refs--;
  mutex_unlock(&g_bc_lock);
}

static int bc_sync_locked(block_device_t *bdev) {
  int rc = 0;
  for (bc_buf_t *b = g_lru_tail; b; b = b->prev)
    if (b->dirty && (!bdev || b->bdev == bdev))
      if (bc_write_locked(b) != 0)
        rc = -EIO;
  return rc;
}

int bc_sync_dev(block_device_t *bdev) {
  if (!g_bc_inited || !bdev)
    return -EINVAL;
  mutex_lock(&g_bc_lock);
  int rc = bc_sync_locked(bdev);
  mutex_unlock(&g_bc_lock);
  if (bdev_flush(bdev) != 0)
    rc = -EIO;
  return rc;
}

int bc_sync_all(void) {
  if (!g_bc_inited)
    return 0;
  mutex_lock(&g_bc_lock);
  int rc = bc_sync_locked(NULL);
  mutex_unlock(&g_bc_lock);
  return rc;
}

int bc_invalidate_dev(block_device_t *bdev) {
  if (!g_bc_inited || !bdev)
    return -EINVAL;
  int rc = bc_sync_dev(bdev);
  mutex_lock(&g_bc_lock);
  for (bc_buf_t *b = g_lru_head; b; b = b->next)
    if (b->bdev == bdev && b->refs > 0) {
      mutex_unlock(&g_bc_lock);
      return -EBUSY;
    }
  if (rc == 0) {
    bc_buf_t *b = g_lru_head;
    while (b) {
      bc_buf_t *next = b->next;
      if (b->bdev == bdev)
        bc_free_locked(b);
      b = next;
    }
  }
  mutex_unlock(&g_bc_lock);
  return rc;
}

void bc_set_limit(size_t bytes) {
  if (!g_bc_inited)
    return;
  mutex_lock(&g_bc_lock);
  g_limit = bytes;
  bc_shrink_locked(0);
  mutex_unlock(&g_bc_lock);
}

void bc_get_stats(struct bc_stats *out) {
  if (!out)
    return;
  mutex_lock(&g_bc_lock);
  *out = g_stats;
  mutex_unlock(&g_bc_lock);
}
