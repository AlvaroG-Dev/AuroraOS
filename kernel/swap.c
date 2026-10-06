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
  swap_device_t *d = g_swap_devs[type];
  if (!d || !d->in_use)
    return -EINVAL;
  if (slot >= d->nr_slots)
    return -EINVAL;

  uint64_t byte_off = ((uint64_t)slot + 1) * PAGE_SIZE;
  uint32_t ssz = d->bdev->sector_size;
  if (byte_off % ssz != 0)
    return -EINVAL;
  uint64_t lba = byte_off / ssz;
  uint32_t nsec = PAGE_SIZE / ssz;

  int rc = bdev_write(d->bdev, lba, nsec, phys_to_virt(phys));
  if (rc != 0) {
    LOG_ERR("[SWAP] write type=%u slot=%u rc=%d", type, slot, rc);
    return -EIO;
  }
  return 0;
}

int swap_read_page(uint32_t type, uint32_t slot, uint64_t phys) {
  if (type >= SWAP_MAX_DEVICES)
    return -EINVAL;
  swap_device_t *d = g_swap_devs[type];
  if (!d || !d->in_use)
    return -EINVAL;
  if (slot >= d->nr_slots)
    return -EINVAL;

  uint64_t byte_off = ((uint64_t)slot + 1) * PAGE_SIZE;
  uint32_t ssz = d->bdev->sector_size;
  if (byte_off % ssz != 0)
    return -EINVAL;
  uint64_t lba = byte_off / ssz;
  uint32_t nsec = PAGE_SIZE / ssz;

  int rc = bdev_read(d->bdev, lba, nsec, phys_to_virt(phys));
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

  unsigned long flags = spin_lock_irqsave(&g_swap_lock);
  g_swap_devs[slot_type] = d;
  mutex_unlock(&g_swap_lock);

  LOG_INFO("[SWAP] Activado %s: type=%d slots=%u (%lu MB)", name, slot_type,
           last_page, (unsigned long)(d->total_bytes / (1024 * 1024)));
  return 0;
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

    unsigned long flags = spin_lock_irqsave(&g_swap_lock);
    d->frozen = 1;
    uint32_t used = d->used_slots;
    mutex_unlock(&g_swap_lock);

    if (used > 0) {
      flags = spin_lock_irqsave(&g_swap_lock);
      d->frozen = 0;
      mutex_unlock(&g_swap_lock);
      LOG_WARN("[SWAP] '%s' tiene %u slots en uso, no se puede desactivar",
               name, used);
      return -EBUSY;
    }

    flags = spin_lock_irqsave(&g_swap_lock);
    g_swap_devs[i] = NULL;
    mutex_unlock(&g_swap_lock);

    kfree(d->slot_bitmap);
    kfree(d);
    LOG_INFO("[SWAP] Desactivado %s", name);
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
static bool kswapd_never(void *arg) {
  (void)arg;
  return false;
}

void kswapd_main(void) {
  LOG_INFO("[KSWAPD] Thread iniciado (low=%lu high=%lu páginas)",
           (unsigned long)SWAP_LOW_WATERMARK,
           (unsigned long)SWAP_HIGH_WATERMARK);

  wait_queue_t wq;
  wait_queue_init(&wq);

  while (1) {
    // Dormir ~0,5 segundos (nunca despierta por condición, solo por timeout)
    wait_event_interruptible_timeout(&wq, kswapd_never, NULL, 500);

    if (swap_device_count() == 0)
      continue;

    uint64_t free_pages = pmm_free_pages_count();
    if (free_pages >= SWAP_LOW_WATERMARK)
      continue;

    LOG_INFO("[KSWAPD] free=%lu < low=%lu, iniciando reclaim",
             (unsigned long)free_pages, (unsigned long)SWAP_LOW_WATERMARK);

    int n = 0;
    while (pmm_free_pages_count() < SWAP_HIGH_WATERMARK && n < 8192) {
      if (!swap_reclaim_one())
        break;
      n++;
    }
    LOG_INFO("[KSWAPD] Reclamadas %d páginas. free=%lu", n,
             (unsigned long)pmm_free_pages_count());
  }
}