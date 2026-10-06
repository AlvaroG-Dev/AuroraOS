// kernel/swap.c
#include "swap.h"
#include "block.h"
#include "heap.h"
#include "klog.h"
#include "mutex.h"
#include "paging.h"
#include "pf.h"
#include "pmm.h"
#include "process.h"
#include "sched.h"
#include "string.h"
#include "uaccess.h"
#include "wait.h"
#include <stddef.h>

static swap_device_t *g_swap_devs[SWAP_MAX_DEVICES];
static mutex_t g_swap_lock;
static int g_swap_ready = 0;

// ===========================================================================
// [SYSCTL] Watermarks ajustables en runtime.
//
// Antes eran #define. Ahora son variables para que /proc/sys/vm/
// swap_{low,high}_pct puedan modificarlas sin recompilar.
// ===========================================================================
static uint32_t g_swap_low_pct = 15;
static uint32_t g_swap_high_pct = 25;

#define SWAP_LOW_MIN (8ULL * 1024 * 1024 / 4096)   // 8 MB
#define SWAP_HIGH_MIN (16ULL * 1024 * 1024 / 4096) // 16 MB

static uint64_t g_swap_low_wm = 0;
static uint64_t g_swap_high_wm = 0;

uint32_t swap_get_low_pct(void) { return g_swap_low_pct; }
uint32_t swap_get_high_pct(void) { return g_swap_high_pct; }

// Recalcula g_swap_{low,high}_wm a partir de los porcentajes actuales
// y del total de RAM usable. Llamado desde swap_init y desde los
// setters de sysctl.
static void swap_recalc_watermarks(void) {
  uint64_t total = pmm_total_usable_pages();
  uint64_t low = (total * g_swap_low_pct) / 100;
  uint64_t high = (total * g_swap_high_pct) / 100;
  if (low < SWAP_LOW_MIN)
    low = SWAP_LOW_MIN;
  if (high < SWAP_HIGH_MIN)
    high = SWAP_HIGH_MIN;
  if (high <= low)
    high = low + 1024;
  g_swap_low_wm = low;
  g_swap_high_wm = high;
}

void swap_set_low_pct(uint32_t pct) {
  if (pct > 90)
    pct = 90;
  g_swap_low_pct = pct;
  swap_recalc_watermarks();
  LOG_INFO("[SWAP] vm.swap_low_pct = %u%% (low=%lu páginas)", pct,
           (unsigned long)g_swap_low_wm);
}

void swap_set_high_pct(uint32_t pct) {
  if (pct > 95)
    pct = 95;
  g_swap_high_pct = pct;
  swap_recalc_watermarks();
  LOG_INFO("[SWAP] vm.swap_high_pct = %u%% (high=%lu páginas)", pct,
           (unsigned long)g_swap_high_wm);
}

// ===========================================================================
// swap_init
// ===========================================================================
void swap_init(void) {
  mutex_init(&g_swap_lock);
  for (int i = 0; i < SWAP_MAX_DEVICES; i++)
    g_swap_devs[i] = NULL;

  // Watermarks porcentuales en función de la RAM total. pmm_init()
  // ya se ha ejecutado antes de swap_init() en kmain, así que
  // pmm_total_usable_pages() devuelve un valor fiable.
  swap_recalc_watermarks();

  g_swap_ready = 1;
  LOG_INFO("[SWAP] Subsistema inicializado (LOW=%lu HIGH=%lu páginas, "
           "RAM total=%lu páginas, %u%%/%u%%)",
           (unsigned long)g_swap_low_wm, (unsigned long)g_swap_high_wm,
           (unsigned long)pmm_total_usable_pages(), g_swap_low_pct,
           g_swap_high_pct);
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

  mutex_lock(&g_swap_lock);
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

  // [FIX] Bajar de WARN a TRACE. El swap lleno es una condición normal
  // bajo presión de memoria; kswapd lo detecta aparte y rate-limita su
  // propio log. Este mensaje solo es útil en debugging fino.
  LOG_TRACE("[SWAP] type=%u sin slots (used=%u/%u)", type, d->used_slots,
            d->nr_slots);
  return 0xFFFFFFFFu;
}

void swap_free_slot(uint32_t type, uint32_t slot) {
  if (type >= SWAP_MAX_DEVICES)
    return;
  swap_device_t *d = g_swap_devs[type];
  if (!d || !d->in_use)
    return;

  mutex_lock(&g_swap_lock);
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
  if (!d || !d->in_use) {
    mutex_unlock(&g_swap_lock);
    return -EINVAL;
  }
  if (slot >= d->nr_slots) {
    mutex_unlock(&g_swap_lock);
    return -EINVAL;
  }

  uint64_t byte_off = ((uint64_t)slot + 1) * PAGE_SIZE;
  uint32_t ssz = d->bdev->sector_size;
  if (ssz == 0 || byte_off % ssz != 0 || PAGE_SIZE % ssz != 0) {
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
  if (!d || !d->in_use) {
    mutex_unlock(&g_swap_lock);
    return -EINVAL;
  }
  if (slot >= d->nr_slots) {
    mutex_unlock(&g_swap_lock);
    return -EINVAL;
  }

  uint64_t byte_off = ((uint64_t)slot + 1) * PAGE_SIZE;
  uint32_t ssz = d->bdev->sector_size;
  if (ssz == 0 || byte_off % ssz != 0 || PAGE_SIZE % ssz != 0) {
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

// ---------------------------------------------------------------------------
// swap_on
// ---------------------------------------------------------------------------
static const char *basename_of(const char *p) {
  const char *b = p;
  for (const char *q = p; *q; q++)
    if (*q == '/')
      b = q + 1;
  return b;
}

int swap_on(const char *path) {
  if (!g_swap_ready || !path)
    return -EINVAL;

  const char *name = basename_of(path);
  block_device_t *bdev = blk_lookup(name);
  if (!bdev) {
    LOG_WARN("[SWAP] device '%s' no encontrado", name);
    return -ENOENT;
  }

  // ¿Ya activo?
  for (int i = 0; i < SWAP_MAX_DEVICES; i++) {
    if (g_swap_devs[i] && g_swap_devs[i]->bdev == bdev) {
      LOG_WARN("[SWAP] '%s' ya está activo", name);
      return -EBUSY;
    }
  }

  // Slot de type libre
  int slot_type = -1;
  for (int i = 0; i < SWAP_MAX_DEVICES; i++) {
    if (!g_swap_devs[i]) {
      slot_type = i;
      break;
    }
  }
  if (slot_type < 0)
    return -ENOMEM;

  // Leer y validar cabecera
  uint8_t *hdr = (uint8_t *)kmalloc(PAGE_SIZE);
  if (!hdr)
    return -ENOMEM;

  uint32_t nsec0 = PAGE_SIZE / bdev->sector_size;
  int rc = bdev_read(bdev, 0, nsec0, hdr);
  if (rc != 0) {
    kfree(hdr);
    LOG_WARN("[SWAP] '%s': no se pudo leer cabecera (rc=%d)", name, rc);
    return -EIO;
  }

  if (memcmp(hdr + SWAP_MAGIC_OFFSET, SWAP_MAGIC, SWAP_MAGIC_LEN) != 0) {
    LOG_WARN("[SWAP] '%s': no tiene SWAPSPACE2 válido", name);
    kfree(hdr);
    return -EINVAL;
  }

  uint32_t version = rd_u32(hdr, SWAP_VERSION_OFFSET);
  uint32_t last_page = rd_u32(hdr, SWAP_LASTPAGE_OFFSET);
  kfree(hdr);

  if (version != 1) {
    LOG_WARN("[SWAP] version=%u no soportada (solo 1)", version);
    return -EINVAL;
  }

  uint64_t dev_bytes = bdev->num_sectors * (uint64_t)bdev->sector_size;
  if (dev_bytes <= PAGE_SIZE) {
    LOG_WARN("[SWAP] device demasiado pequeño");
    return -EINVAL;
  }
  uint32_t max_slots = (uint32_t)((dev_bytes - PAGE_SIZE) / PAGE_SIZE);
  if (last_page == 0 || last_page > max_slots) {
    LOG_WARN("[SWAP] last_page=%u > max_slots=%u, clamp", last_page, max_slots);
    last_page = max_slots;
  }

  swap_device_t *d = (swap_device_t *)kzalloc(sizeof(*d));
  if (!d)
    return -ENOMEM;
  size_t bm_bytes = (last_page + 7) / 8;
  d->slot_bitmap = (uint8_t *)kzalloc(bm_bytes);
  if (!d->slot_bitmap) {
    kfree(d);
    return -ENOMEM;
  }

  d->bdev = bdev;
  d->type = (uint32_t)slot_type;
  d->nr_slots = last_page;
  d->next_slot = 0;
  d->used_slots = 0;
  d->total_bytes = (uint64_t)last_page * PAGE_SIZE;
  d->in_use = 1;
  d->frozen = 0;
  d->next = NULL;

  mutex_lock(&g_swap_lock);
  g_swap_devs[slot_type] = d;
  mutex_unlock(&g_swap_lock);

  LOG_INFO("[SWAP] Activado %s: type=%d slots=%u (%lu MB)", name, slot_type,
           last_page, (unsigned long)(d->total_bytes / (1024 * 1024)));
  return 0;
}

// ---------------------------------------------------------------------------
// swapoff: swap-in masivo.
//
// Congela el device, recorre todas las PTEs swapeadas que apunten a él
// (vía process_for_each), y trae cada página a RAM. Cuando ya no queda
// ninguna, desregistra el device.
//
// Fases por batch:
//   1. Recolectar hasta SWAPOFF_BATCH entradas (pml4_phys, vaddr, slot).
//   2. Fuera del process_lock, hacer swap-in de cada una.
//   3. Repetir hasta que una pasada no recolecte nada.
//
// El device queda "frozen" durante todo el proceso: kswapd no crea
// nuevas entradas, y swap_alloc_slot falla. Solo decrecen.
// ---------------------------------------------------------------------------
#define SWAPOFF_BATCH 256

struct swapoff_entry {
  uint64_t pml4_phys;
  uint64_t vaddr;
  uint32_t type;
  uint32_t slot;
};

struct swapoff_ctx {
  uint32_t target_type;
  struct swapoff_entry *batch;
  size_t capacity;
  size_t count;
};

static int swapoff_collect_cb(process_t *p, void *arg) {
  struct swapoff_ctx *ctx = (struct swapoff_ctx *)arg;
  if (ctx->count >= ctx->capacity)
    return 1;
  if (!p || p->is_zombie || !p->pml4_phys)
    return 0;

  uint64_t *pml4 = (uint64_t *)phys_to_virt(p->pml4_phys);

  for (vma_t *v = p->vma_list; v; v = v->next) {
    for (uint64_t pg = v->start; pg < v->end; pg += PAGE_SIZE) {
      uint64_t pte;
      if (!paging_get_pte_in(pml4, pg, &pte))
        continue;
      if (!pte_is_swap(pte))
        continue;
      if (pte_swap_type(pte) != ctx->target_type)
        continue;

      ctx->batch[ctx->count].pml4_phys = p->pml4_phys;
      ctx->batch[ctx->count].vaddr = pg;
      ctx->batch[ctx->count].type = ctx->target_type;
      ctx->batch[ctx->count].slot = pte_swap_offset(pte);
      ctx->count++;
      if (ctx->count >= ctx->capacity)
        return 1;
    }
  }
  return 0;
}

static int swapoff_do_one(struct swapoff_entry *e) {
  uint64_t *pml4 = (uint64_t *)phys_to_virt(e->pml4_phys);

  // Re-verificar el PTE. Si cambió (por ejemplo, #PF concurrente ya la
  // trajo), saltamos. Al device estar frozen, no debería pasar, pero
  // es una salvaguarda barata.
  uint64_t pte;
  if (!paging_get_pte_in(pml4, e->vaddr, &pte))
    return 0;
  if (!pte_is_swap(pte))
    return 0;
  if (pte_swap_type(pte) != e->type)
    return 0;
  if (pte_swap_offset(pte) != e->slot)
    return 0;

  uint64_t new_phys = pmm_alloc_page();
  if (!new_phys)
    return -ENOMEM;

  if (swap_read_page(e->type, e->slot, new_phys) != 0) {
    pmm_free_page(new_phys);
    return -EIO;
  }

  // Recuperar la VMA para aplicar los flags correctos. Si el proceso
  // murió entre el collect y el do, saltamos: el slot ya se liberó al
  // morir el proceso.
  process_t *proc = process_find_by_pml4(e->pml4_phys);
  if (!proc) {
    pmm_free_page(new_phys);
    return 0;
  }

  vma_t *vma = vma_find(proc, e->vaddr);
  if (!vma) {
    pmm_free_page(new_phys);
    return 0;
  }

  uint64_t map_flags = vma->flags | PTE_PRESENT;
  if (paging_map_page_in(pml4, e->vaddr, new_phys, map_flags) != 0) {
    pmm_free_page(new_phys);
    return -EIO;
  }

  swap_free_slot(e->type, e->slot);
  return 1;
}

int swap_off(const char *path) {
  if (!g_swap_ready || !path)
    return -EINVAL;

  const char *name = basename_of(path);
  block_device_t *bdev = blk_lookup(name);
  if (!bdev)
    return -ENOENT;

  for (int i = 0; i < SWAP_MAX_DEVICES; i++) {
    swap_device_t *d = g_swap_devs[i];
    if (!d || d->bdev != bdev)
      continue;

    mutex_lock(&g_swap_lock);
    d->frozen = 1;
    uint32_t used = d->used_slots;
    mutex_unlock(&g_swap_lock);

    int total_moved = 0;

    if (used > 0) {
      LOG_INFO("[SWAP] swapoff '%s': swap-in masivo de %u slots", name, used);

      struct swapoff_entry *batch =
          (struct swapoff_entry *)kmalloc(SWAPOFF_BATCH * sizeof(*batch));
      if (!batch) {
        mutex_lock(&g_swap_lock);
        d->frozen = 0;
        mutex_unlock(&g_swap_lock);
        return -ENOMEM;
      }

      int guard = 0;
      while (used > 0 && guard++ < 100000) {
        struct swapoff_ctx ctx = {
            .target_type = d->type,
            .batch = batch,
            .capacity = SWAPOFF_BATCH,
            .count = 0,
        };
        process_for_each(swapoff_collect_cb, &ctx);

        if (ctx.count == 0)
          break; // nada más que mover

        for (size_t k = 0; k < ctx.count; k++) {
          int r = swapoff_do_one(&batch[k]);
          if (r > 0)
            total_moved++;
        }

        mutex_lock(&g_swap_lock);
        used = d->used_slots;
        mutex_unlock(&g_swap_lock);
      }

      kfree(batch);

      if (used > 0) {
        LOG_WARN("[SWAP] swapoff '%s' incompleto: quedan %u slots", name, used);
        mutex_lock(&g_swap_lock);
        d->frozen = 0;
        mutex_unlock(&g_swap_lock);
        return -EBUSY;
      }
    }

    mutex_lock(&g_swap_lock);
    g_swap_devs[i] = NULL;
    mutex_unlock(&g_swap_lock);

    kfree(d->slot_bitmap);
    kfree(d);

    LOG_INFO("[SWAP] Desactivado %s (movidas %d páginas)", name, total_moved);
    return 0;
  }
  return -ENOENT;
}

uint64_t swap_total_bytes(void) {
  uint64_t t = 0;
  for (int i = 0; i < SWAP_MAX_DEVICES; i++)
    if (g_swap_devs[i] && g_swap_devs[i]->in_use)
      t += g_swap_devs[i]->total_bytes;
  return t;
}

uint64_t swap_used_bytes(void) {
  uint64_t u = 0;
  for (int i = 0; i < SWAP_MAX_DEVICES; i++)
    if (g_swap_devs[i] && g_swap_devs[i]->in_use)
      u += (uint64_t)g_swap_devs[i]->used_slots * PAGE_SIZE;
  return u;
}

uint64_t swap_free_bytes(void) {
  uint64_t t = swap_total_bytes(), u = swap_used_bytes();
  return t > u ? t - u : 0;
}

void swap_for_each(swap_iter_cb_t cb, void *arg) {
  if (!cb)
    return;
  for (int i = 0; i < SWAP_MAX_DEVICES; i++) {
    swap_device_t *d = g_swap_devs[i];
    if (!d || !d->in_use)
      continue;
    uint64_t tk = d->total_bytes / 1024;
    uint64_t uk = (uint64_t)d->used_slots * PAGE_SIZE / 1024;
    cb(d->bdev->name, tk, uk, tk - uk, arg);
  }
}

// ---------------------------------------------------------------------------
// Reclaim
//
// Fase 1: bajo process_lock, buscar un candidato (pml4, vaddr).
// Fase 2: fuera del lock, hacer el I/O y actualizar el PTE.
// ---------------------------------------------------------------------------
struct reclaim_ctx {
  uint64_t pml4_phys;
  uint64_t vaddr;
};

static int find_reclaim_candidate_cb(process_t *p, void *arg) {
  struct reclaim_ctx *c = (struct reclaim_ctx *)arg;
  if (c->pml4_phys)
    return 1;
  if (!p || p->is_zombie || !p->pml4_phys)
    return 0;

  uint64_t *pml4 = (uint64_t *)phys_to_virt(p->pml4_phys);

  // [FIX] Dos pasadas: primero VMA_ANON (mmap anónimo, la memoria
  // "fría" real). Solo si no hay ninguna, miramos ELF/stack. Esto
  // evita thrashear las pocas páginas del ELF mientras hay GBs
  // de mmap anónimo disponibles.
  for (int pass = 0; pass < 2; pass++) {
    for (vma_t *v = p->vma_list; v; v = v->next) {
      if (v->type == VMA_FILE)
        continue;
      int is_anon = (v->type == VMA_ANON);
      if (pass == 0 && !is_anon)
        continue;
      if (pass == 1 && is_anon)
        continue;

      for (uint64_t pg = v->start; pg < v->end; pg += PAGE_SIZE) {
        uint64_t pte;
        if (!paging_get_pte_in(pml4, pg, &pte))
          continue;
        if (!(pte & PTE_PRESENT))
          continue;
        if (!(pte & PTE_WRITABLE))
          continue;
        if (pte_is_swap(pte))
          continue;
        c->pml4_phys = p->pml4_phys;
        c->vaddr = pg;
        return 1;
      }
    }
  }
  return 0;
}

int swap_reclaim_one(void) {
  if (swap_device_count() == 0)
    return 0;

  int type = -1;
  for (int i = 0; i < SWAP_MAX_DEVICES; i++)
    if (g_swap_devs[i] && g_swap_devs[i]->in_use && !g_swap_devs[i]->frozen) {
      type = i;
      break;
    }
  if (type < 0)
    return 0;

  struct reclaim_ctx c = {0, 0};
  process_for_each(find_reclaim_candidate_cb, &c);
  if (c.pml4_phys == 0)
    return 0;

  uint64_t *pml4 = (uint64_t *)phys_to_virt(c.pml4_phys);

  // Paso 1: leer PTE
  uint64_t pte;
  if (!paging_get_pte_in(pml4, c.vaddr, &pte))
    return 0;
  if (!(pte & PTE_PRESENT) || !(pte & PTE_WRITABLE) || pte_is_swap(pte))
    return 0;
  uint64_t phys = pte & PTE_FRAME;

  // Paso 2: [CRÍTICO] Quitar WRITABLE antes del I/O. Si el proceso
  // escribe durante el I/O, el CPU dispara #PF(present+write) y el
  // handler re-marca RW + abortamos el reclaim. Así no perdemos
  // escrituras concurrentes.
  uint64_t ro_pte = pte & ~(uint64_t)PTE_WRITABLE;
  if (paging_set_pte_in(pml4, c.vaddr, ro_pte) != 0)
    return 0;

  // Paso 3: re-verificar que el PTE es el mismo RO (nadie lo tocó)
  uint64_t check;
  if (!paging_get_pte_in(pml4, c.vaddr, &check) ||
      (check & PTE_FRAME) != phys) {
    return 0;
  }

  // Paso 4: alloc slot y escribir a swap
  uint32_t slot = swap_alloc_slot((uint32_t)type);
  if (slot == 0xFFFFFFFFu) {
    paging_set_pte_in(pml4, c.vaddr, pte); // restaurar RW
    return 0;
  }

  if (swap_write_page((uint32_t)type, slot, phys) != 0) {
    swap_free_slot((uint32_t)type, slot);
    paging_set_pte_in(pml4, c.vaddr, pte); // restaurar RW
    return 0;
  }

  // Paso 5: [CRÍTICO] Re-verificar. Si durante el I/O el proceso
  // escribió, el PF handler ya marcó el PTE como RW. Abortamos el
  // reclaim (el slot se libera, la página se queda donde estaba).
  if (!paging_get_pte_in(pml4, c.vaddr, &check) ||
      (check & PTE_FRAME) != phys || (check & PTE_WRITABLE)) {
    swap_free_slot((uint32_t)type, slot);
    return 0;
  }

  // Paso 6: promover a swap marker y liberar el frame
  uint64_t spte = pte_encode_swap((uint32_t)type, slot);
  if (paging_set_pte_in(pml4, c.vaddr, spte) != 0) {
    swap_free_slot((uint32_t)type, slot);
    paging_set_pte_in(pml4, c.vaddr, pte); // restaurar RW
    return 0;
  }

  pmm_free_page(phys);
  return 1;
}

// ---------------------------------------------------------------------------
// kswapd: kernel thread
// ---------------------------------------------------------------------------
static bool kswapd_never(void *arg) {
  (void)arg;
  return false;
}

void kswapd_main(void) {
  LOG_INFO("[KSWAPD] Thread iniciado (low=%lu high=%lu páginas)",
           (unsigned long)g_swap_low_wm, (unsigned long)g_swap_high_wm);

  wait_queue_t wq;
  wait_queue_init(&wq);

  // [FIX] Rate-limiting del log. Antes, si el swap estaba lleno y
  // free seguía < LOW, kswapd spameaba 3 líneas cada 500 ms para
  // siempre.
  uint64_t last_log_tick = 0;

  while (1) {
    // Dormir ~0.5 segundos (nunca despierta por condición, solo por timeout)
    wait_event_interruptible_timeout(&wq, kswapd_never, NULL, 500);

    if (swap_device_count() == 0)
      continue;

    uint64_t free_pages = pmm_free_pages_count();
    if (free_pages >= g_swap_low_wm)
      continue;

    // [FIX] Antes de intentar reclaimar, comprobar si queda algún
    // slot libre. Si el swap está lleno, no tiene sentido iterar
    // 8192 veces. Loguear solo cada 5 s para no spamear.
    uint64_t total_bytes = swap_total_bytes();
    uint64_t used_bytes = swap_used_bytes();
    if (total_bytes > 0 && used_bytes >= total_bytes) {
      uint64_t now = sched_get_ticks();
      if (now - last_log_tick >= 5000) {
        last_log_tick = now;
        LOG_WARN("[KSWAPD] Swap lleno (%lu/%lu KB), free=%lu < low=%lu. "
                 "No se puede reclaimar. Amplía swap o libera memoria.",
                 (unsigned long)(used_bytes / 1024),
                 (unsigned long)(total_bytes / 1024), (unsigned long)free_pages,
                 (unsigned long)g_swap_low_wm);
      }
      continue;
    }

    int n = 0;
    while (pmm_free_pages_count() < g_swap_high_wm && n < 8192) {
      if (!swap_reclaim_one())
        break;
      n++;
    }

    // [FIX] Log rate-limited. Solo si reclamamos algo, o cada 5 s si
    // el reclaim no consiguió avanzar (para no perderse en silencio).
    uint64_t now = sched_get_ticks();
    if (n > 0 || now - last_log_tick >= 5000) {
      last_log_tick = now;
      LOG_INFO("[KSWAPD] Reclamadas %d páginas. free=%lu (high=%lu)", n,
               (unsigned long)pmm_free_pages_count(),
               (unsigned long)g_swap_high_wm);
    }
  }
}