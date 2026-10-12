// kernel/slab.c
#include "slab.h"
#include "heap.h"
#include "klog.h"
#include "paging.h"
#include "pmm.h"
#include "serial.h"
#include "string.h"

#define SLAB_MAGIC 0x51AB51AB
#define HEADER_SIZE 72

struct slab_header {
  uint32_t magic;
  uint32_t obj_size;
  uint16_t objs_total;
  uint16_t objs_free;
  uint32_t _pad0;
  struct slab_cache *cache;
  void *freelist;
  struct slab_header *next;
  struct slab_header *prev;
  uint64_t _pad1[3];
};

_Static_assert(sizeof(struct slab_header) == HEADER_SIZE,
               "slab_header debe medir 72 bytes");

// ---------------------------------------------------------------------------
// Límites de retención de slabs vacíos.
//
// Cuando un slab se queda sin objetos en uso, en vez de liberarlo
// inmediatamente lo guardamos por si volvemos a necesitarlo pronto. Eso
// evita el churn (alloc page / free page) bajo carga intermitente.
//
//   - SLAB_EMPTY_PER_CACHE_MAX: cuántos slabs vacíos guarda cada cache.
//     Con 2 por cache, un burst corto se absorbe sin syscall de PMM.
//   - SLAB_EMPTY_GLOBAL_MAX: cuántos slabs vacíos en total en el sistema.
//     Si un cache tiene muchos vacíos pero otros necesitan crecer, el
//     global evita que el primero acapare la memoria.
//
// Todo lo que exceda esos límites se DEVUELVE al PMM.
// ---------------------------------------------------------------------------
#define SLAB_EMPTY_PER_CACHE_MAX 2
#define SLAB_EMPTY_GLOBAL_MAX 8

// ---------------------------------------------------------------------------
// Estado global
// ---------------------------------------------------------------------------
const size_t slab_sizes[SLAB_NUM_CACHES] = {16,  32,  64,   128,
                                            256, 512, 1024, 2048};

static slab_cache_t caches[SLAB_NUM_CACHES];
static uint64_t slab_top = SLAB_VMA;
static spinlock_t slab_top_lock;
static int slab_ready = 0;

// [FIX-OOM] Página compartida "tombstone". Cuando devolvemos una página
// del SLAB al PMM, remapeamos su VA a esta página compartida (llena de
// ceros) en vez de desmapearla.
//
// Ventaja: un kfree() tardío sobre un objeto ya liberado sigue sin
// disparar #PF. Lee hdr->magic = 0 != SLAB_MAGIC y sale limpiamente.
//
// Ventaja adicional: el VA queda "reservado" a la espera de reuso. No
// hay que liberar VAs ni gestionar un pool de direcciones libres.
//
// Coste: 1 página de 4 KB, para siempre.
static uint64_t g_tombstone_phys = 0;

// Contador global de slabs vacíos (suma sobre todos los caches).
// Se actualiza con atómicos desde fuera del c->lock para evitar
// contención entre caches.
static size_t g_empty_total = 0;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static inline size_t round_up(size_t n, size_t a) {
  return (n + a - 1) & ~(a - 1);
}

static slab_cache_t *cache_for_size(size_t size) {
  for (int i = 0; i < SLAB_NUM_CACHES; i++) {
    if (size <= slab_sizes[i])
      return &caches[i];
  }
  return NULL;
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------
static void cache_init(slab_cache_t *c, size_t obj_size, const char *name) {
  spin_init(&c->lock);
  size_t real = obj_size;
  if (real < sizeof(void *))
    real = sizeof(void *);
  real = round_up(real, 8);
  c->obj_size = real;
  c->objs_per_slab = (PAGE_SIZE - HEADER_SIZE) / real;
  if (c->objs_per_slab == 0)
    c->objs_per_slab = 1;
  c->slabs = NULL;
  c->empty_slabs = NULL;
  c->slabs_count = 0;
  c->empty_count = 0;
  c->used_count = 0;
  c->total_count = 0;
  c->name = name;
}

void slab_init(void) {
  spin_init(&slab_top_lock);

  // [FIX-OOM] Reservar la página tombstone antes que nada.
  g_tombstone_phys = pmm_alloc_page();
  if (g_tombstone_phys) {
    memset((void *)phys_to_virt(g_tombstone_phys), 0, PAGE_SIZE);
  } else {
    LOG_WARN("[SLAB] sin memoria para tombstone; los slabs liberados no se "
             "podrán devolver al PMM");
  }

  static const char *names[SLAB_NUM_CACHES] = {
      "slab-16",  "slab-32",  "slab-64",   "slab-128",
      "slab-256", "slab-512", "slab-1024", "slab-2048",
  };

  for (int i = 0; i < SLAB_NUM_CACHES; i++) {
    cache_init(&caches[i], slab_sizes[i], names[i]);
  }

  slab_ready = 1;

  LOG_INFO("[SLAB] Inicializado: %d caches en %p, tombstone=%p",
           SLAB_NUM_CACHES, (void *)SLAB_VMA,
           g_tombstone_phys ? (void *)phys_to_virt(g_tombstone_phys) : NULL);
  for (int i = 0; i < SLAB_NUM_CACHES; i++) {
    LOG_DEBUG("[SLAB]   %s: obj=%lu objs/slab=%lu", caches[i].name,
              (unsigned long)caches[i].obj_size,
              (unsigned long)caches[i].objs_per_slab);
  }
}

// ---------------------------------------------------------------------------
// Devuelve al PMM la página física de un slab.
//
// La VA NO se desmapea: se remapea a la página tombstone compartida. Así
// un kfree tardío sigue sin provocar #PF, y el VA queda reservado (nunca
// se reutiliza).
// ---------------------------------------------------------------------------
static void slab_release_page(slab_header_t *hdr) {
  if (!g_tombstone_phys) {
    // Sin tombstone, no podemos devolver la página con seguridad.
    // Preferimos filtrarla (queda mapeada, se reutilizará en el próximo
    // grow si algún día se necesita).
    return;
  }

  uint64_t va = (uint64_t)hdr;
  uint64_t old_phys = paging_get_phys(va);
  if (!old_phys)
    return;
  if (old_phys == g_tombstone_phys)
    return; // ya está liberada

  // Remapear el VA a la tombstone. paging_map_page() toma paging_lock y
  // hace invlpg. No falla en un VA ya mapeado (no necesita allocar
  // tablas).
  if (paging_map_page(va, g_tombstone_phys, PTE_WRITABLE | PTE_NX) != 0) {
    LOG_WARN("[SLAB] no se pudo remapear VA %p a tombstone", (void *)va);
    return;
  }
  pmm_free_page(old_phys);
}

// ---------------------------------------------------------------------------
// Crecimiento: consigue un slab para la cache.
//
// Preferencia:
//   1. Reusar un slab vacío de la propia cache (fast path, sin PMM).
//   2. Allocar una VA nueva + página física del PMM.
// ---------------------------------------------------------------------------
static slab_header_t *slab_grow(slab_cache_t *c) {
  slab_header_t *hdr = NULL;

  // --- Fast path: reusar un slab vacío de esta cache. ---
  unsigned long cf = spin_lock_irqsave(&c->lock);
  if (c->empty_slabs) {
    hdr = c->empty_slabs;
    c->empty_slabs = hdr->next;
    c->empty_count--;
    __atomic_fetch_sub(&g_empty_total, 1, __ATOMIC_RELAXED);

    // Reconstruir el freelist para este obj_size (el slab pudo haber
    // pertenecido antes a esta misma cache, o haber sido devuelto y
    // reasignado internamente — no debería pasar, pero es seguro).
    memset(hdr, 0, HEADER_SIZE);
    hdr->magic = SLAB_MAGIC;
    hdr->obj_size = (uint32_t)c->obj_size;
    hdr->objs_total = (uint16_t)c->objs_per_slab;
    hdr->objs_free = (uint16_t)c->objs_per_slab;
    hdr->cache = c;
    hdr->freelist = NULL;
    uint8_t *base = (uint8_t *)hdr + HEADER_SIZE;
    for (size_t i = 0; i < c->objs_per_slab; i++) {
      void *obj = base + i * c->obj_size;
      *(void **)obj = hdr->freelist;
      hdr->freelist = obj;
    }
    hdr->next = c->slabs;
    hdr->prev = NULL;
    if (c->slabs)
      c->slabs->prev = hdr;
    c->slabs = hdr;
    c->slabs_count++;
    c->total_count += c->objs_per_slab;
    spin_unlock_irqrestore(&c->lock, cf);
    return hdr;
  }
  spin_unlock_irqrestore(&c->lock, cf);

  // --- Slow path: VA nueva + página del PMM. ---
  unsigned long tf = spin_lock_irqsave(&slab_top_lock);
  uint64_t vaddr = slab_top;
  if (vaddr + PAGE_SIZE > SLAB_END) {
    spin_unlock_irqrestore(&slab_top_lock, tf);
    LOG_ERR("[SLAB] región SLAB agotada (SLAB_END=%p)", (void *)SLAB_END);
    return NULL;
  }
  slab_top += PAGE_SIZE;
  spin_unlock_irqrestore(&slab_top_lock, tf);

  if (vmm_alloc_pages(vaddr, 1, PTE_WRITABLE | PTE_NX) != 0) {
    LOG_ERR("[SLAB] vmm_alloc_pages fallo en %p", (void *)vaddr);
    return NULL;
  }
  hdr = (slab_header_t *)vaddr;
  memset(hdr, 0, HEADER_SIZE);
  hdr->magic = SLAB_MAGIC;
  hdr->obj_size = (uint32_t)c->obj_size;
  hdr->objs_total = (uint16_t)c->objs_per_slab;
  hdr->objs_free = (uint16_t)c->objs_per_slab;
  hdr->cache = c;
  hdr->freelist = NULL;
  uint8_t *base = (uint8_t *)hdr + HEADER_SIZE;
  for (size_t i = 0; i < c->objs_per_slab; i++) {
    void *obj = base + i * c->obj_size;
    *(void **)obj = hdr->freelist;
    hdr->freelist = obj;
  }

  cf = spin_lock_irqsave(&c->lock);
  hdr->next = c->slabs;
  hdr->prev = NULL;
  if (c->slabs)
    c->slabs->prev = hdr;
  c->slabs = hdr;
  c->slabs_count++;
  c->total_count += c->objs_per_slab;
  spin_unlock_irqrestore(&c->lock, cf);

  return hdr;
}

// ---------------------------------------------------------------------------
// Alloc
// ---------------------------------------------------------------------------
void *slab_alloc(size_t size) {
  if (!slab_ready || size == 0)
    return NULL;

  slab_cache_t *c = cache_for_size(size);
  if (!c)
    return NULL;

  for (;;) {
    unsigned long flags = spin_lock_irqsave(&c->lock);

    // Buscar un slab con hueco.
    slab_header_t *hdr = c->slabs;
    while (hdr && hdr->objs_free == 0)
      hdr = hdr->next;

    if (hdr) {
      void *obj = hdr->freelist;
      if (!obj) {
        // Invariante roto: objs_free > 0 pero freelist vacía. Indica
        // corrupción. Reportar y abortar sin tocar nada.
        LOG_ERR("[SLAB] %s: freelist vacía con objs_free=%u (corrupción)",
                c->name, hdr->objs_free);
        spin_unlock_irqrestore(&c->lock, flags);
        return NULL;
      }
      hdr->freelist = *(void **)obj;
      hdr->objs_free--;
      c->used_count++;
      spin_unlock_irqrestore(&c->lock, flags);
      return obj;
    }

    spin_unlock_irqrestore(&c->lock, flags);

    // [FIX-OOM] Crecimiento. Al soltar el lock antes de slab_grow, otro
    // thread podría consumir el slab recién creado antes de que lo
    // usemos. En vez de asumir que hdr->freelist != NULL al volver,
    // reintentamos la búsqueda desde cero.
    if (!slab_grow(c))
      return NULL;
    // Loop y reintentar.
  }
}

// ---------------------------------------------------------------------------
// Free
// ---------------------------------------------------------------------------
static inline slab_header_t *header_of(void *ptr) {
  uint64_t p = (uint64_t)ptr;
  return (slab_header_t *)(p & ~(PAGE_SIZE - 1));
}

void slab_free(void *ptr) {
  if (!ptr || !slab_ready)
    return;

  slab_header_t *hdr = header_of(ptr);

  if (hdr->magic != SLAB_MAGIC) {
    // Dos casos normales:
    //   - VA remapeada a tombstone (magic == 0) → kfree tardío detectado.
    //   - Puntero basura → error real.
    if (hdr->magic != 0) {
      LOG_ERR("[SLAB] kfree con puntero no SLAB: %p (magic=%08x)", ptr,
              hdr->magic);
    }
    return;
  }

  slab_cache_t *c = hdr->cache;
  if (!c) {
    LOG_ERR("[SLAB] slab sin cache: %p", ptr);
    return;
  }

  unsigned long flags = spin_lock_irqsave(&c->lock);

  if (hdr->objs_free >= hdr->objs_total) {
    LOG_ERR("[SLAB] kfree con objeto ya libre (slab vacío): %p", ptr);
    spin_unlock_irqrestore(&c->lock, flags);
    return;
  }

  // Devolver el objeto al freelist.
  *(void **)ptr = hdr->freelist;
  hdr->freelist = ptr;
  hdr->objs_free++;
  if (c->used_count > 0)
    c->used_count--;

  // ¿Slab completamente vacío?
  if (hdr->objs_free == hdr->objs_total) {
    int keep_active = (c->slabs_count == 1); // no dejar la cache a 0
    int cache_it = !keep_active &&
                   (c->empty_count < SLAB_EMPTY_PER_CACHE_MAX) &&
                   (__atomic_load_n(&g_empty_total, __ATOMIC_RELAXED) <
                    SLAB_EMPTY_GLOBAL_MAX);
    int can_release = (g_tombstone_phys != 0);

    if (keep_active || cache_it || !can_release) {
      // Dejarlo en c->slabs (keep_active) o mover a c->empty_slabs.
      if (!keep_active) {
        if (hdr->prev)
          hdr->prev->next = hdr->next;
        else
          c->slabs = hdr->next;
        if (hdr->next)
          hdr->next->prev = hdr->prev;
        c->slabs_count--;

        hdr->next = c->empty_slabs;
        hdr->prev = NULL;
        c->empty_slabs = hdr;
        c->empty_count++;
        __atomic_fetch_add(&g_empty_total, 1, __ATOMIC_RELAXED);
      }
      spin_unlock_irqrestore(&c->lock, flags);
      return;
    }

    // Devuelve la página al PMM.
    if (hdr->prev)
      hdr->prev->next = hdr->next;
    else
      c->slabs = hdr->next;
    if (hdr->next)
      hdr->next->prev = hdr->prev;
    c->slabs_count--;
    if (c->total_count >= hdr->objs_total)
      c->total_count -= hdr->objs_total;

    spin_unlock_irqrestore(&c->lock, flags);

    slab_release_page(hdr);
    return;
  }

  spin_unlock_irqrestore(&c->lock, flags);
}

// ---------------------------------------------------------------------------
// Reclaim explícito (opcional, para kswapd o low-memory handlers).
//
// Recorre todas las caches y libera los slabs que estén en c->empty_slabs
// (o los activos que ya estén vacíos) hasta alcanzar ~target_bytes.
// ---------------------------------------------------------------------------
size_t slab_reclaim_pages(size_t target_bytes) {
  if (!slab_ready || !g_tombstone_phys)
    return 0;

  size_t freed = 0;
  for (int i = 0; i < SLAB_NUM_CACHES && freed < target_bytes; i++) {
    slab_cache_t *c = &caches[i];
    for (;;) {
      unsigned long flags = spin_lock_irqsave(&c->lock);
      slab_header_t *victim = c->empty_slabs;
      if (!victim) {
        // También vale un slab activo vacío si la cache tiene más de 1.
        if (c->slabs_count > 1 && c->slabs &&
            c->slabs->objs_free == c->slabs->objs_total) {
          victim = c->slabs;
          if (victim->prev)
            victim->prev->next = victim->next;
          else
            c->slabs = victim->next;
          if (victim->next)
            victim->next->prev = victim->prev;
          c->slabs_count--;
          if (c->total_count >= victim->objs_total)
            c->total_count -= victim->objs_total;
          spin_unlock_irqrestore(&c->lock, flags);
          slab_release_page(victim);
          freed += PAGE_SIZE;
          continue;
        }
        spin_unlock_irqrestore(&c->lock, flags);
        break;
      }
      c->empty_slabs = victim->next;
      c->empty_count--;
      __atomic_fetch_sub(&g_empty_total, 1, __ATOMIC_RELAXED);
      spin_unlock_irqrestore(&c->lock, flags);
      slab_release_page(victim);
      freed += PAGE_SIZE;
      if (freed >= target_bytes)
        break;
    }
  }
  return freed;
}

// ---------------------------------------------------------------------------
// Introspección
// ---------------------------------------------------------------------------
int slab_is_slab_ptr(const void *ptr) {
  if (!ptr)
    return 0;
  uint64_t p = (uint64_t)ptr;
  return p >= SLAB_VMA && p < slab_top;
}

size_t slab_usable_size(const void *ptr) {
  if (!slab_is_slab_ptr(ptr))
    return 0;
  slab_header_t *hdr = header_of((void *)ptr);
  if (hdr->magic != SLAB_MAGIC || hdr->cache == NULL)
    return 0;
  return hdr->obj_size;
}

void slab_dump_stats(void) {
  extern int panic_in_progress(void);
  int in_panic = panic_in_progress();

  LOG_INFO("[SLAB] Estado de las caches:");
  for (int i = 0; i < SLAB_NUM_CACHES; i++) {
    slab_cache_t *c = &caches[i];
    int locked = !in_panic;
    unsigned long flags = 0;
    if (locked)
      flags = spin_lock_irqsave(&c->lock);

    LOG_INFO("  %s: slabs=%lu empty=%lu used=%lu total=%lu obj=%lu", c->name,
             (unsigned long)c->slabs_count, (unsigned long)c->empty_count,
             (unsigned long)c->used_count, (unsigned long)c->total_count,
             (unsigned long)c->obj_size);

    if (locked)
      spin_unlock_irqrestore(&c->lock, flags);
  }
  LOG_INFO("[SLAB] slabs vacíos en el sistema: %lu (cap=%d)",
           (unsigned long)g_empty_total, SLAB_EMPTY_GLOBAL_MAX);
}

uint64_t slab_vma_start(void) { return SLAB_VMA; }
uint64_t slab_vma_end(void) { return slab_top; }