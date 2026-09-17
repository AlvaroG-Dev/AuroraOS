// kernel/slab.c
#include "slab.h"
#include "heap.h"
#include "klog.h"
#include "paging.h"
#include "pmm.h"
#include "serial.h"
#include "string.h"

#define SLAB_MAGIC 0x51AB51AB
#define HEADER_SIZE 72 // tamaño de slab_header_t (alineado a 64)

// ---------------------------------------------------------------------------
// Header de cada slab. Va al principio de cada página (o grupo de páginas).
// Los objetos empiezan en offset HEADER_SIZE.
// ---------------------------------------------------------------------------
struct slab_header {
  uint32_t magic;
  uint32_t obj_size;
  uint16_t objs_total;
  uint16_t objs_free;
  uint32_t _pad0;
  struct slab_cache *cache;
  void *freelist; // objetos libres en este slab
  struct slab_header *next;
  struct slab_header *prev;
  uint64_t _pad1[3]; // relleno hasta 64 bytes
};

_Static_assert(sizeof(struct slab_header) == HEADER_SIZE,
               "slab_header debe medir 64 bytes");

// ---------------------------------------------------------------------------
// Estado global
// ---------------------------------------------------------------------------
const size_t slab_sizes[SLAB_NUM_CACHES] = {16,  32,  64,   128,
                                            256, 512, 1024, 2048};

static slab_cache_t caches[SLAB_NUM_CACHES];
static uint64_t slab_top = SLAB_VMA;
static spinlock_t slab_top_lock;
static int slab_ready = 0;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static inline size_t round_up(size_t n, size_t a) {
  return (n + a - 1) & ~(a - 1);
}

static inline int is_power_of_two(size_t n) {
  return n != 0 && (n & (n - 1)) == 0;
}

static slab_cache_t *cache_for_size(size_t size) {
  for (int i = 0; i < SLAB_NUM_CACHES; i++) {
    if (size <= slab_sizes[i])
      return &caches[i];
  }
  return NULL;
}

// ---------------------------------------------------------------------------
// Inicialización de una cache
// ---------------------------------------------------------------------------
static void cache_init(slab_cache_t *c, size_t obj_size, const char *name) {
  spin_init(&c->lock);
  // El objeto debe ser >= 8 (alineación mínima).
  // El objeto real que guardamos en el slab es de obj_size, pero
  // siempre >= sizeof(void*) para poder guardar el puntero de freelist
  // cuando el objeto está libre.
  size_t real = obj_size;
  if (real < sizeof(void *))
    real = sizeof(void *);
  real = round_up(real, 8);
  c->obj_size = real;
  c->objs_per_slab = (PAGE_SIZE - HEADER_SIZE) / real;
  if (c->objs_per_slab == 0)
    c->objs_per_slab = 1; // objetos gigantes (no debería pasar con obj<=2048)
  c->slabs = NULL;
  c->slabs_count = 0;
  c->used_count = 0;
  c->total_count = 0;
  c->name = name;
}

void slab_init(void) {
  spin_init(&slab_top_lock);

  static const char *names[SLAB_NUM_CACHES] = {
      "slab-16",  "slab-32",  "slab-64",   "slab-128",
      "slab-256", "slab-512", "slab-1024", "slab-2048",
  };

  for (int i = 0; i < SLAB_NUM_CACHES; i++) {
    cache_init(&caches[i], slab_sizes[i], names[i]);
  }

  slab_ready = 1;

  LOG_INFO("[SLAB] Inicializado: %d caches en %p", SLAB_NUM_CACHES,
           (void *)SLAB_VMA);
  for (int i = 0; i < SLAB_NUM_CACHES; i++) {
    LOG_DEBUG("[SLAB]   %s: obj=%lu objs/slab=%lu", caches[i].name,
              (unsigned long)caches[i].obj_size,
              (unsigned long)caches[i].objs_per_slab);
  }
}

// ---------------------------------------------------------------------------
// Crecimiento: pedir una nueva página para la cache
// ---------------------------------------------------------------------------
// Se llama SIN el lock del cache cogido.
static slab_header_t *slab_grow(slab_cache_t *c) {
  // Reservar la dirección virtual y mapear una página.
  // slab_top es global, hay que protegerlo.
  unsigned long tf = spin_lock_irqsave(&slab_top_lock);

  // ¿Hay sitio entre SLAB_VMA y MMIO_MAP_BASE?
  // MMIO_MAP_BASE = 0xFFFFC00000000000. Tenemos mucho hueco.
  uint64_t vaddr = slab_top;
  slab_top += PAGE_SIZE;

  spin_unlock_irqrestore(&slab_top_lock, tf);

  if (vmm_alloc_pages(vaddr, 1, PTE_WRITABLE | PTE_NX) != 0) {
    LOG_ERR("[SLAB] vmm_alloc_pages fallo en %p", (void *)vaddr);
    return NULL;
  }

  slab_header_t *hdr = (slab_header_t *)vaddr;
  memset(hdr, 0, HEADER_SIZE);

  hdr->magic = SLAB_MAGIC;
  hdr->obj_size = (uint32_t)c->obj_size;
  hdr->objs_total = (uint16_t)c->objs_per_slab;
  hdr->objs_free = (uint16_t)c->objs_per_slab;
  hdr->cache = c;
  hdr->freelist = NULL;

  // Construir la free list enlazando los objetos entre sí.
  // Cada objeto libre guarda, en sus primeros 8 bytes, un puntero
  // al siguiente objeto libre.
  uint8_t *base = (uint8_t *)hdr + HEADER_SIZE;
  for (size_t i = 0; i < c->objs_per_slab; i++) {
    void *obj = base + i * c->obj_size;
    *(void **)obj = hdr->freelist;
    hdr->freelist = obj;
  }

  // Enlazar en la lista de slabs del cache.
  unsigned long cf = spin_lock_irqsave(&c->lock);
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
    return NULL; // size > SLAB_MAX_SIZE

  unsigned long flags = spin_lock_irqsave(&c->lock);

  // Buscar un slab con hueco.
  slab_header_t *hdr = c->slabs;
  while (hdr && hdr->objs_free == 0)
    hdr = hdr->next;

  if (!hdr) {
    // No hay hueco. Crecimiento.
    spin_unlock_irqrestore(&c->lock, flags);
    hdr = slab_grow(c);
    if (!hdr)
      return NULL;
    flags = spin_lock_irqsave(&c->lock);
  }

  // Sacar el primer objeto de la free list.
  void *obj = hdr->freelist;
  if (!obj) {
    // No debería pasar: hdr tenía objs_free > 0 y por construcción
    // del slab todos los objetos están en la freelist hasta que se
    // asignan. Si esto pasa, la contabilidad está mal.
    LOG_ERR("[SLAB] %s: freelist vacía pero objs_free=%u", c->name,
            hdr->objs_free);
    spin_unlock_irqrestore(&c->lock, flags);
    return NULL;
  }
  hdr->freelist = *(void **)obj;
  hdr->objs_free--;
  c->used_count++;

  spin_unlock_irqrestore(&c->lock, flags);

  return obj;
}

// ---------------------------------------------------------------------------
// Free
// ---------------------------------------------------------------------------
// Recupera el header del slab al que pertenece 'ptr'. El objeto está
// en la misma página que el header (asumimos objetos <= 2048 bytes,
// así que siempre caben en una página junto al header).
static inline slab_header_t *header_of(void *ptr) {
  uint64_t p = (uint64_t)ptr;
  return (slab_header_t *)(p & ~(PAGE_SIZE - 1));
}

void slab_free(void *ptr) {
  if (!ptr || !slab_ready)
    return;

  slab_header_t *hdr = header_of(ptr);

  if (hdr->magic != SLAB_MAGIC) {
    LOG_ERR("[SLAB] kfree con puntero no SLAB: %p", ptr);
    return;
  }

  slab_cache_t *c = hdr->cache;
  if (!c) {
    LOG_ERR("[SLAB] slab sin cache: %p", ptr);
    return;
  }

  unsigned long flags = spin_lock_irqsave(&c->lock);

  // Devolver el objeto a la freelist del slab.
  *(void **)ptr = hdr->freelist;
  hdr->freelist = ptr;
  hdr->objs_free++;
  if (c->used_count > 0)
    c->used_count--;

  // ¿Está el slab completamente libre? Devolverlo al PMM.
  // Pero solo si hay otros slabs en la lista (nunca liberar el último,
  // porque podría dejar la cache sin memoria disponible).
  if (hdr->objs_free == hdr->objs_total && c->slabs_count > 1) {
    // Desenlazar de la lista
    if (hdr->prev)
      hdr->prev->next = hdr->next;
    else
      c->slabs = hdr->next;
    if (hdr->next)
      hdr->next->prev = hdr->prev;
    c->slabs_count--;
    c->total_count -= hdr->objs_total;

    spin_unlock_irqrestore(&c->lock, flags);

    // Devolver la página al PMM. No hace falta tocar slab_top
    // (es un bump pointer, no se reutiliza en esta versión simple).
    uint64_t vaddr = (uint64_t)hdr;
    vmm_free_pages(vaddr, 1);
    return;
  }

  spin_unlock_irqrestore(&c->lock, flags);
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
  if (hdr->magic != SLAB_MAGIC)
    return 0;
  return hdr->obj_size;
}

void slab_dump_stats(void) {
  LOG_INFO("[SLAB] Estado de las caches:");
  for (int i = 0; i < SLAB_NUM_CACHES; i++) {
    slab_cache_t *c = &caches[i];
    unsigned long flags = spin_lock_irqsave(&c->lock);
    LOG_INFO("  %s: slabs=%lu used=%lu total=%lu obj=%lu", c->name,
             (unsigned long)c->slabs_count, (unsigned long)c->used_count,
             (unsigned long)c->total_count, (unsigned long)c->obj_size);
    spin_unlock_irqrestore(&c->lock, flags);
  }
}