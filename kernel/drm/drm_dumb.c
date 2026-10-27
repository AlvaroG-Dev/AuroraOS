// kernel/drm/drm_dumb.c
//
// GEM minimo + "dumb buffers" (DRM_IOCTL_MODE_CREATE/MAP/DESTROY_DUMB).
// Un objeto GEM es un array de paginas fisicas sueltas; el mmap del
// /dev/dri/cardN se resuelve por demand paging (vfs_ops.get_page) con un
// offset falso = id << 32.

#include "../heap.h"
#include "../paging.h"
#include "../pmm.h"
#include "../spinlock.h"
#include "../string.h"
#include "../uaccess.h"
#include "drm.h"

#define DUMB_MAX_DIM 8192
#define DUMB_MAX_BYTES (128u << 20)

static spinlock_t g_gem_lock = {.locked = 0};
static uint32_t g_gem_next_id = 1;

// Nombres globales (GEM_FLINK): tabla debil; se limpia al liberar el GEM.
#define FLINK_MAX 64
static drm_gem_t *g_flink[FLINK_MAX];
static uint32_t g_flink_next = 1;

int drm_put_user(uint64_t arg, const void *src, size_t n) {
  if (!arg || !access_ok((void *)arg, n) || copy_to_user((void *)arg, src, n) < 0)
    return -EFAULT;
  return 0;
}

int drm_get_user(uint64_t arg, void *dst, size_t n) {
  if (!arg || !access_ok((void *)arg, n) ||
      copy_from_user(dst, (void *)arg, n) < 0)
    return -EFAULT;
  return 0;
}

int drm_gem_create(drm_file_t *f, size_t size, drm_gem_t **out) {
  if (size == 0 || size > DUMB_MAX_BYTES)
    return -EINVAL;
  size = (size + PAGE_SIZE - 1) & ~(size_t)(PAGE_SIZE - 1);
  drm_gem_t *g = (drm_gem_t *)kzalloc(sizeof(*g));
  if (!g)
    return -ENOMEM;
  g->npages = (uint32_t)(size / PAGE_SIZE);
  g->pages = (uint64_t *)kzalloc((size_t)g->npages * sizeof(uint64_t));
  if (!g->pages) {
    kfree(g);
    return -ENOMEM;
  }
  for (uint32_t i = 0; i < g->npages; i++) {
    uint64_t p = pmm_alloc_page();
    if (!p) {
      while (i--)
        pmm_free_page(g->pages[i]);
      kfree(g->pages);
      kfree(g);
      return -ENOMEM;
    }
    memset(phys_to_virt(p), 0, PAGE_SIZE);
    g->pages[i] = p;
  }
  g->size = size;
  g->refcnt = 1;
  g->creator_ref = true;
  g->owner = f;
  unsigned long fl = spin_lock_irqsave(&g_gem_lock);
  g->id = g_gem_next_id++;
  g->next = f->gems;
  f->gems = g;
  spin_unlock_irqrestore(&g_gem_lock, fl);
  *out = g;
  return 0;
}

void drm_gem_get(drm_gem_t *g) {
  unsigned long fl = spin_lock_irqsave(&g_gem_lock);
  g->refcnt++;
  spin_unlock_irqrestore(&g_gem_lock, fl);
}

void drm_gem_put(drm_gem_t *g) {
  if (!g)
    return;
  unsigned long fl = spin_lock_irqsave(&g_gem_lock);
  if (--g->refcnt > 0) {
    spin_unlock_irqrestore(&g_gem_lock, fl);
    return;
  }
  if (g->flink_name) {
    for (int i = 0; i < FLINK_MAX; i++)
      if (g_flink[i] == g)
        g_flink[i] = NULL;
  }
  if (g->owner) {
    drm_gem_t **pp = &g->owner->gems;
    while (*pp && *pp != g)
      pp = &(*pp)->next;
    if (*pp)
      *pp = g->next;
  }
  spin_unlock_irqrestore(&g_gem_lock, fl);
  if (g->ext_release) {
    g->ext_release(g->ext_priv); // las paginas son del exportador
  } else {
    for (uint32_t i = 0; i < g->npages; i++)
      pmm_free_page(g->pages[i]);
  }
  kfree(g->pages);
  kfree(g);
}

drm_gem_t *drm_gem_new_ext(uint32_t npages, const uint64_t *pages,
                           void (*release)(void *), void *priv) {
  if (!npages || !pages || !release ||
      (uint64_t)npages * PAGE_SIZE > DUMB_MAX_BYTES)
    return NULL;
  drm_gem_t *g = (drm_gem_t *)kzalloc(sizeof(*g));
  if (!g)
    return NULL;
  g->pages = (uint64_t *)kmalloc((size_t)npages * sizeof(uint64_t));
  if (!g->pages) {
    kfree(g);
    return NULL;
  }
  memcpy(g->pages, pages, (size_t)npages * sizeof(uint64_t));
  g->npages = npages;
  g->size = (size_t)npages * PAGE_SIZE;
  g->refcnt = 1;
  g->mapped = true;
  g->ext_release = release;
  g->ext_priv = priv;
  unsigned long fl = spin_lock_irqsave(&g_gem_lock);
  g->id = g_gem_next_id++;
  spin_unlock_irqrestore(&g_gem_lock, fl);
  return g;
}

// Suelta la referencia del creador (una sola vez).
static void gem_drop_creator(drm_gem_t *g) {
  unsigned long fl = spin_lock_irqsave(&g_gem_lock);
  bool had = g->creator_ref;
  g->creator_ref = false;
  spin_unlock_irqrestore(&g_gem_lock, fl);
  if (had)
    drm_gem_put(g);
}

int drm_handle_alloc(drm_file_t *f, drm_gem_t *g, uint32_t *handle) {
  for (int i = 0; i < DRM_MAX_HANDLES; i++) {
    if (!f->handles[i]) {
      f->handles[i] = g;
      *handle = (uint32_t)i + 1;
      return 0;
    }
  }
  return -ENOSPC;
}

drm_gem_t *drm_handle_lookup(drm_file_t *f, uint32_t handle) {
  if (handle == 0 || handle > DRM_MAX_HANDLES)
    return NULL;
  return f->handles[handle - 1];
}

int drm_handle_close(drm_file_t *f, uint32_t handle) {
  drm_gem_t *g = drm_handle_lookup(f, handle);
  if (!g)
    return -EINVAL;
  f->handles[handle - 1] = NULL;
  if (f->imported[handle - 1]) { // importado por PRIME: tiene ref propia
    f->imported[handle - 1] = 0;
    drm_gem_put(g);
    return 0;
  }
  // Si alguien pudo mapearlo, las paginas siguen vivas hasta cerrar el fichero
  // (un munmap no avisa al driver; liberarlas dejaria mapeos colgando).
  if (!g->mapped)
    gem_drop_creator(g);
  return 0;
}

void drm_file_release_gems(drm_file_t *f) {
  for (int i = 0; i < DRM_MAX_HANDLES; i++) {
    drm_gem_t *g = f->handles[i];
    f->handles[i] = NULL;
    if (g && f->imported[i])
      drm_gem_put(g);
    f->imported[i] = 0;
  }
  for (;;) {
    unsigned long fl = spin_lock_irqsave(&g_gem_lock);
    drm_gem_t *g = f->gems;
    if (g) {
      f->gems = g->next;
      g->next = NULL;
      g->owner = NULL;
    }
    spin_unlock_irqrestore(&g_gem_lock, fl);
    if (!g)
      break;
    gem_drop_creator(g);
  }
}

void drm_gem_read(const drm_gem_t *g, size_t off, void *dst, size_t len) {
  uint8_t *d = (uint8_t *)dst;
  while (len && off < g->size) {
    size_t in = off & (PAGE_SIZE - 1);
    size_t n = PAGE_SIZE - in;
    if (n > len)
      n = len;
    if (off + n > g->size)
      n = g->size - off;
    memcpy(d, (const uint8_t *)phys_to_virt(g->pages[off / PAGE_SIZE]) + in, n);
    d += n;
    off += n;
    len -= n;
  }
}

// mmap: el offset falso es (id << 32) + desplazamiento dentro del objeto.
int drm_gem_get_page(drm_file_t *f, uint64_t byte_off, uint64_t *phys) {
  uint32_t id = (uint32_t)(byte_off >> 32);
  uint64_t local = byte_off & 0xFFFFFFFFull;
  int rc = -EFAULT;
  unsigned long fl = spin_lock_irqsave(&g_gem_lock);
  for (drm_gem_t *g = f->gems; g; g = g->next) {
    if (g->id == id && g->mapped) {
      if (local / PAGE_SIZE < g->npages) {
        *phys = g->pages[local / PAGE_SIZE];
        rc = 0;
      }
      break;
    }
  }
  if (rc) // GEM importado por PRIME: no esta en f->gems, solo en los handles
    for (int i = 0; i < DRM_MAX_HANDLES; i++) {
      drm_gem_t *g = f->handles[i];
      if (g && f->imported[i] && g->id == id && g->mapped &&
          local / PAGE_SIZE < g->npages) {
        *phys = g->pages[local / PAGE_SIZE];
        rc = 0;
        break;
      }
    }
  spin_unlock_irqrestore(&g_gem_lock, fl);
  return rc;
}

// ---------------------------------------------------------------------------
// ioctls dumb
// ---------------------------------------------------------------------------
struct drm_mode_create_dumb {
  uint32_t height, width, bpp, flags;
  uint32_t handle, pitch;
  uint64_t size;
};

struct drm_mode_map_dumb {
  uint32_t handle, pad;
  uint64_t offset;
};

struct drm_mode_destroy_dumb {
  uint32_t handle;
};

int drm_ioctl_create_dumb(drm_file_t *f, uint64_t arg) {
  struct drm_mode_create_dumb a;
  int rc = drm_get_user(arg, &a, sizeof(a));
  if (rc)
    return rc;
  if (a.width == 0 || a.height == 0 || a.width > DUMB_MAX_DIM ||
      a.height > DUMB_MAX_DIM)
    return -EINVAL;
  if (a.bpp != 8 && a.bpp != 16 && a.bpp != 24 && a.bpp != 32)
    return -EINVAL;
  uint64_t pitch = (((uint64_t)a.width * a.bpp + 7) / 8 + 63) & ~63ull;
  uint64_t size = pitch * a.height;
  if (size > DUMB_MAX_BYTES)
    return -EINVAL;
  drm_gem_t *g;
  rc = drm_gem_create(f, (size_t)size, &g);
  if (rc)
    return rc;
  uint32_t h;
  rc = drm_handle_alloc(f, g, &h);
  if (rc) {
    gem_drop_creator(g);
    return rc;
  }
  a.handle = h;
  a.pitch = (uint32_t)pitch;
  a.size = g->size;
  rc = drm_put_user(arg, &a, sizeof(a));
  if (rc)
    drm_handle_close(f, h);
  return rc;
}

int drm_ioctl_map_dumb(drm_file_t *f, uint64_t arg) {
  struct drm_mode_map_dumb a;
  int rc = drm_get_user(arg, &a, sizeof(a));
  if (rc)
    return rc;
  drm_gem_t *g = drm_handle_lookup(f, a.handle);
  if (!g)
    return -ENOENT;
  g->mapped = true;
  a.offset = (uint64_t)g->id << 32;
  return drm_put_user(arg, &a, sizeof(a));
}

int drm_ioctl_destroy_dumb(drm_file_t *f, uint64_t arg) {
  struct drm_mode_destroy_dumb a;
  int rc = drm_get_user(arg, &a, sizeof(a));
  if (rc)
    return rc;
  return drm_handle_close(f, a.handle);
}

// DRM_IOCTL_GEM_FLINK: nombre global de un GEM (entero que otro fichero DRM
// puede abrir con GEM_OPEN). Sin referencia propia: el nombre desaparece
// cuando el objeto se libera.
struct drm_gem_flink_arg { uint32_t handle, name; };
struct drm_gem_open_arg { uint32_t name, handle; uint64_t size; };

int drm_ioctl_gem_flink(drm_file_t *f, uint64_t arg) {
  struct drm_gem_flink_arg a;
  int rc = drm_get_user(arg, &a, sizeof(a));
  if (rc)
    return rc;
  drm_gem_t *g = drm_handle_lookup(f, a.handle);
  if (!g)
    return -ENOENT;
  unsigned long fl = spin_lock_irqsave(&g_gem_lock);
  if (!g->flink_name) {
    int slot = -1;
    for (int i = 0; i < FLINK_MAX; i++)
      if (!g_flink[i]) {
        slot = i;
        break;
      }
    if (slot < 0) {
      spin_unlock_irqrestore(&g_gem_lock, fl);
      return -ENOSPC;
    }
    g->flink_name = g_flink_next++;
    g_flink[slot] = g;
  }
  a.name = g->flink_name;
  spin_unlock_irqrestore(&g_gem_lock, fl);
  return drm_put_user(arg, &a, sizeof(a));
}

int drm_ioctl_gem_open(drm_file_t *f, uint64_t arg) {
  struct drm_gem_open_arg a;
  int rc = drm_get_user(arg, &a, sizeof(a));
  if (rc)
    return rc;
  drm_gem_t *g = NULL;
  unsigned long fl = spin_lock_irqsave(&g_gem_lock);
  for (int i = 0; i < FLINK_MAX; i++)
    if (g_flink[i] && g_flink[i]->flink_name == a.name && g_flink[i]->refcnt > 0) {
      g = g_flink[i];
      g->refcnt++; // la referencia del handle que se abre
      break;
    }
  spin_unlock_irqrestore(&g_gem_lock, fl);
  if (!g)
    return -ENOENT;
  // Mismo GEM ya presente en este fichero: se devuelve su handle.
  for (int i = 0; i < DRM_MAX_HANDLES; i++)
    if (f->handles[i] == g) {
      drm_gem_put(g);
      a.handle = (uint32_t)i + 1;
      a.size = g->size;
      return drm_put_user(arg, &a, sizeof(a));
    }
  uint32_t h;
  rc = drm_handle_alloc(f, g, &h);
  if (rc) {
    drm_gem_put(g);
    return rc;
  }
  f->imported[h - 1] = 1; // el handle posee la referencia tomada arriba
  g->mapped = true;
  a.handle = h;
  a.size = g->size;
  rc = drm_put_user(arg, &a, sizeof(a));
  if (rc)
    drm_handle_close(f, h);
  return rc;
}
