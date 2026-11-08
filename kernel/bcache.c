// kernel/bcache.c — caché de bloques genérica (ver bcache.h).

#include "bcache.h"

#include "heap.h"
#include "klog.h"
#include "mutex.h"
#include "pmm.h"
#include "string.h"

#define BC_HASH_BUCKETS 65536u
#define BC_DEFAULT_LIMIT (16u * 1024u * 1024u)
#define BC_MAX_BS 4096u
#define BC_RA_MAX 64u // bloques por lectura anticipada

struct bc_buf {
  block_device_t *bdev;
  uint64_t blk;
  uint32_t bs;
  int refs;
  uint8_t dirty;
  uint8_t datablk; // contenido de fichero: se escribe antes que los metadatos
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

#define BC_MAX_JNL 4
static struct {
  block_device_t *bdev;
  bc_commit_fn commit;
  bc_done_fn done;
  void *priv;
  uint32_t max_blocks;
} g_jnl[BC_MAX_JNL];

static int bc_jnl_slot(block_device_t *bdev) {
  for (int i = 0; i < BC_MAX_JNL; i++)
    if (g_jnl[i].bdev == bdev && g_jnl[i].commit)
      return i;
  return -1;
}

void bc_set_journal(block_device_t *bdev, bc_commit_fn commit, bc_done_fn done,
                    void *priv, uint32_t max_blocks) {
  mutex_lock(&g_bc_lock);
  int s = -1;
  for (int i = 0; i < BC_MAX_JNL; i++)
    if (g_jnl[i].bdev == bdev) {
      s = i;
      break;
    }
  if (s < 0)
    for (int i = 0; i < BC_MAX_JNL; i++)
      if (!g_jnl[i].bdev) {
        s = i;
        break;
      }
  if (s >= 0) {
    g_jnl[s].bdev = commit ? bdev : NULL;
    g_jnl[s].commit = commit;
    g_jnl[s].done = done;
    g_jnl[s].priv = priv;
    g_jnl[s].max_blocks = max_blocks;
  }
  mutex_unlock(&g_bc_lock);
}

static inline uint32_t bc_hash(block_device_t *bdev, uint64_t blk) {
  uint64_t h = (uint64_t)(uintptr_t)bdev * 11400714819323198485ULL;
  h ^= blk * 2654435761ULL;
  return (uint32_t)(h >> 16) & (BC_HASH_BUCKETS - 1);
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
  b->datablk = 0;
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
    if (b->refs == 0 && !(b->dirty && !b->datablk && bc_jnl_slot(b->bdev) >= 0)) {
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

// Como bc_get pero solo si ya está en caché (NULL si no): una sola toma del
// cerrojo en el camino caliente de lecturas.
bc_buf_t *bc_get_cached(block_device_t *bdev, uint64_t blk, uint32_t bs) {
  if (!g_bc_inited || !bdev)
    return NULL;
  mutex_lock(&g_bc_lock);
  bc_buf_t *b = hash_find(bdev, blk);
  if (b && b->bs == bs) {
    g_stats.hits++;
    b->refs++;
    lru_unlink(b);
    lru_push_front(b);
  } else {
    b = NULL;
  }
  mutex_unlock(&g_bc_lock);
  return b;
}

int bc_present(block_device_t *bdev, uint64_t blk) {
  if (!g_bc_inited || !bdev)
    return 0;
  mutex_lock(&g_bc_lock);
  int r = hash_find(bdev, blk) != NULL;
  mutex_unlock(&g_bc_lock);
  return r;
}

// Lectura anticipada: trae de una sola petición los bloques consecutivos
// blk..blk+count-1 que aún no estén en caché (se detiene en el primero que ya
// esté). Los deja sin referencias. Devuelve cuántos cargó (0 si no hizo nada;
// el llamante sigue con bc_get normal, que informa de los errores).
int bc_readahead(block_device_t *bdev, uint64_t blk, uint32_t count,
                 uint32_t bs) {
  if (!bc_args_ok(bdev, bs) || count == 0)
    return 0;
  if (count > BC_RA_MAX)
    count = BC_RA_MAX;
  uint64_t nblocks = bdev->num_sectors * bdev->sector_size / bs;
  mutex_lock(&g_bc_lock);
  uint32_t n = 0;
  while (n < count && blk + n < nblocks && !hash_find(bdev, blk + n))
    n++;
  if (n == 0) {
    mutex_unlock(&g_bc_lock);
    return 0;
  }
  bc_shrink_locked((size_t)n * bs);
  uint8_t *tmp = kmalloc((size_t)n * bs);
  if (!tmp) {
    mutex_unlock(&g_bc_lock);
    return 0;
  }
  uint32_t ss = bdev->sector_size;
  if (bdev_read(bdev, blk * (uint64_t)(bs / ss), n * (bs / ss), tmp) != 0) {
    kfree(tmp);
    mutex_unlock(&g_bc_lock);
    return 0;
  }
  uint32_t loaded = 0;
  for (uint32_t i = 0; i < n; i++) {
    bc_buf_t *b = kzalloc(sizeof(*b));
    uint8_t *data = b ? kmalloc(bs) : NULL;
    if (!b || !data) {
      kfree(b);
      kfree(data);
      break;
    }
    memcpy(data, tmp + (size_t)i * bs, bs);
    b->bdev = bdev;
    b->blk = blk + i;
    b->bs = bs;
    b->data = data;
    uint32_t h = bc_hash(bdev, b->blk);
    b->hnext = g_hash[h];
    g_hash[h] = b;
    lru_push_front(b);
    g_bytes += bs;
    g_stats.bufs++;
    g_stats.misses++;
    loaded++;
  }
  kfree(tmp);
  mutex_unlock(&g_bc_lock);
  return (int)loaded;
}

void *bc_data(bc_buf_t *b) { return b ? b->data : NULL; }
uint32_t bc_size(const bc_buf_t *b) { return b ? b->bs : 0; }
uint64_t bc_block_no(const bc_buf_t *b) { return b ? b->blk : 0; }

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

// Como bc_mark_dirty para datos de ficheros (data=ordered): en cada volcado
// salen antes que los metadatos (inodos, bitmaps, directorios), con una barrera
// entre medias, para que un corte no deje un inodo apuntando a bloques viejos.
void bc_mark_dirty_data(bc_buf_t *b) {
  if (!b)
    return;
  mutex_lock(&g_bc_lock);
  if (!b->dirty) {
    b->dirty = 1;
    g_stats.dirty++;
  }
  b->datablk = 1;
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

// Escritura agrupada: los sucios se ordenan por (dispositivo, bloque) y los
// bloques contiguos salen en una sola petición (hasta BC_RA_MAX). Con el disco
// real esto convierte miles de escrituras de 4 KiB en pocas de 256 KiB.
#define BC_SYNC_BATCH 2048u

static void bc_sort_dirty(bc_buf_t **a, uint32_t n) {
  // Shell sort: sin recursión ni memoria extra.
  for (uint32_t gap = n / 2; gap > 0; gap /= 2)
    for (uint32_t i = gap; i < n; i++) {
      bc_buf_t *t = a[i];
      uint32_t j = i;
      while (j >= gap &&
             ((uintptr_t)a[j - gap]->bdev > (uintptr_t)t->bdev ||
              ((a[j - gap]->bdev == t->bdev) && a[j - gap]->blk > t->blk))) {
        a[j] = a[j - gap];
        j -= gap;
      }
      a[j] = t;
    }
}

static int bc_sync_locked(block_device_t *bdev) {
  int rc = 0;
  if (!g_stats.dirty)
    return 0;
  bc_buf_t **arr = kmalloc(sizeof(*arr) * BC_SYNC_BATCH);
  uint8_t *tmp = kmalloc((size_t)BC_RA_MAX * BC_MAX_BS);
  if (!arr || !tmp) { // sin memoria para agrupar: de uno en uno
    kfree(arr);
    kfree(tmp);
    for (bc_buf_t *b = g_lru_tail; b; b = b->prev)
      if (b->dirty && (!bdev || b->bdev == bdev) &&
          !(!b->datablk && bc_jnl_slot(b->bdev) >= 0))
        if (bc_write_locked(b) != 0)
          rc = -EIO;
    return rc;
  }
  for (int phase = 0; phase < 2; phase++) {
   int wrote_data = 0;
   if (phase == 1)
     for (int ji = 0; ji < BC_MAX_JNL; ji++) {
       if (!g_jnl[ji].commit || (bdev && g_jnl[ji].bdev != bdev))
         continue;
       block_device_t *jd = g_jnl[ji].bdev;
       for (;;) {
         uint32_t n = 0, cap = g_jnl[ji].max_blocks;
         if (cap > BC_SYNC_BATCH)
           cap = BC_SYNC_BATCH;
         for (bc_buf_t *b = g_lru_tail; b && n < cap; b = b->prev)
           if (b->dirty && !b->datablk && b->bdev == jd)
             arr[n++] = b;
         if (n == 0)
           break;
         bc_sort_dirty(arr, n);
         if (g_jnl[ji].commit(g_jnl[ji].priv, arr, n) != 0) {
           rc = -EIO;
           break;
         }
         int werr = 0;
         for (uint32_t k = 0; k < n; k++)
           if (bc_write_locked(arr[k]) != 0)
             werr = 1;
         if (werr) {
           rc = -EIO;
           break;
         }
         if (bdev_flush(jd) != 0) {
           rc = -EIO;
           break;
         }
         if (g_jnl[ji].done)
           g_jnl[ji].done(g_jnl[ji].priv);
       }
     }
   block_device_t *flush_dev[4] = {0};
   for (;;) {
    uint32_t n = 0;
    for (bc_buf_t *b = g_lru_tail; b && n < BC_SYNC_BATCH; b = b->prev)
      if (b->dirty && (!bdev || b->bdev == bdev) && (phase == 0) == (b->datablk != 0) &&
          !(phase == 1 && bc_jnl_slot(b->bdev) >= 0))
        arr[n++] = b;
    if (n == 0)
      break;
    if (phase == 0) {
      wrote_data = 1;
      for (uint32_t k = 0; k < n; k++) {
        int seen = 0, slot = -1;
        for (int d = 0; d < 4; d++) {
          if (flush_dev[d] == arr[k]->bdev) seen = 1;
          if (!flush_dev[d] && slot < 0) slot = d;
        }
        if (!seen && slot >= 0) flush_dev[slot] = arr[k]->bdev;
      }
    }
    bc_sort_dirty(arr, n);
    uint32_t progress = 0;
    for (uint32_t i = 0; i < n;) {
      uint32_t run = 1;
      while (i + run < n && run < BC_RA_MAX && arr[i + run]->bdev == arr[i]->bdev &&
             arr[i + run]->bs == arr[i]->bs &&
             arr[i + run]->blk == arr[i]->blk + run)
        run++;
      if (run == 1) {
        if (bc_write_locked(arr[i]) == 0)
          progress++;
        else
          rc = -EIO;
      } else {
        uint32_t bs = arr[i]->bs, ss = arr[i]->bdev->sector_size;
        for (uint32_t k = 0; k < run; k++)
          memcpy(tmp + (size_t)k * bs, arr[i + k]->data, bs);
        if (bdev_write(arr[i]->bdev, arr[i]->blk * (uint64_t)(bs / ss),
                       run * (bs / ss), tmp) == 0) {
          for (uint32_t k = 0; k < run; k++) {
            arr[i + k]->dirty = 0;
            arr[i + k]->datablk = 0;
            if (g_stats.dirty)
              g_stats.dirty--;
          }
          g_stats.writes += run;
          progress += run;
        } else {
          LOG_ERR("[BCACHE] error escribiendo %s bloques %llu..+%u",
                  arr[i]->bdev->name, (unsigned long long)arr[i]->blk, run);
          rc = -EIO;
        }
      }
      i += run;
    }
    if (progress == 0 || n < BC_SYNC_BATCH)
      break; // sin avance (error persistente) o ya no quedan más
   }
   if (phase == 0 && wrote_data)
     for (int d = 0; d < 4; d++)
       if (flush_dev[d] && bdev_flush(flush_dev[d]) != 0)
         rc = -EIO; // barrera: los datos antes que los metadatos
  }
  kfree(arr);
  kfree(tmp);
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

// Cota de la caché según la RAM: un cuarto, entre 16 y 512 MiB. Así los
// binarios grandes (cc1, ld, ...) se quedan en memoria entre ejecuciones.
void bc_autosize(void) {
  uint64_t ram = pmm_total_usable_pages() * 4096ULL;
  uint64_t lim = ram / 4;
  if (lim < (16ULL << 20))
    lim = 16ULL << 20;
  if (lim > (512ULL << 20))
    lim = 512ULL << 20;
  bc_set_limit((size_t)lim);
  LOG_INFO("[BCACHE] cota de la caché de bloques: %lu MiB (RAM %lu MiB)",
           (unsigned long)(lim >> 20), (unsigned long)(ram >> 20));
}

// Devuelve memoria al sistema: expulsa hasta `bytes` de búfers sin uso (los
// más antiguos). Lo llama kswapd cuando queda poca memoria libre.
size_t bc_reclaim(size_t bytes) {
  if (!g_bc_inited)
    return 0;
  size_t freed = 0;
  mutex_lock(&g_bc_lock);
  bc_buf_t *b = g_lru_tail;
  while (b && freed < bytes) {
    bc_buf_t *prev = b->prev;
    if (b->refs == 0 && !b->dirty) {
      freed += b->bs;
      bc_free_locked(b);
      g_stats.evictions++;
    }
    b = prev;
  }
  mutex_unlock(&g_bc_lock);
  return freed;
}

void bc_get_stats(struct bc_stats *out) {
  if (!out)
    return;
  mutex_lock(&g_bc_lock);
  *out = g_stats;
  mutex_unlock(&g_bc_lock);
}
