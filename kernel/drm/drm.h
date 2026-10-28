// kernel/drm/drm.h
//
// Subsistema DRM/KMS al estilo Linux (Bloque 4 de Plan_Wayland_Unificado.md).
// Expone /dev/dri/card0 (KMS completo) y /dev/dri/renderD128 (solo GEM) con
// las ioctls y estructuras binarias de Linux, de modo que libdrm, Weston,
// kmscube, modetest, wlroots... funcionan sin tocar.
//
// Modelo de pantalla ("modo A" del plan): hay un unico conector virtual, un
// encoder, un CRTC y un plano primario. El "scanout" no toca el hardware: el
// contenido del framebuffer activo se copia al content_buffer de una ventana
// sin decoracion del compositor de Aurora (ver drm_core.c).

#ifndef KERNEL_DRM_H
#define KERNEL_DRM_H

#include "../spinlock.h"
#include "../vfs.h"
#include "../wait.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DRM_MAJOR 226
#define DRM_CARD_MINOR 0
#define DRM_RENDER_MINOR 128

#define DRM_FILE_MAGIC 0xD2F11E01u
#define DRM_MAX_HANDLES 128
#define DRM_MAX_SYNCOBJS 64
#define DRM_EVENT_RING 32

// Ids de objetos KMS (fijos: solo hay uno de cada).
#define DRM_CRTC_ID 1
#define DRM_ENCODER_ID 2
#define DRM_CONNECTOR_ID 3
#define DRM_PLANE_ID 4
#define DRM_FB_ID_BASE 100
#define DRM_BLOB_ID_BASE 200

#define DRM_FORMAT_XRGB8888 0x34325258u // 'XR24'
#define DRM_FORMAT_ARGB8888 0x34325241u // 'AR24'

struct drm_syncobj;

typedef struct drm_gem {
  uint32_t id;       // unico en el sistema; mmap offset = id << 32
  int refcnt;        // 1 del creador (hasta cerrar el fichero) + 1 por fb
  size_t size;       // bytes, multiplo de PAGE_SIZE
  uint32_t npages;
  uint64_t *pages;   // direcciones fisicas de cada pagina
  bool mapped;      // MAP_DUMB pedido: las paginas no se liberan hasta cerrar
  bool creator_ref; // aun cuenta la referencia del creador
  uint32_t flink_name; // 0 = sin nombre global (DRM_IOCTL_GEM_FLINK)
  struct drm_file *owner;
  struct drm_gem *next; // lista del fichero propietario
  // [LinuxKPI L4] GEM sobre paginas ajenas (dma_buf exportado por un driver
  // de Linux): no se liberan al soltar el GEM; se llama a ext_release.
  void (*ext_release)(void *priv);
  void *ext_priv;
} drm_gem_t;

typedef struct drm_fb {
  uint32_t id;
  int refcnt; // 1 del fichero + 1 si el CRTC lo esta mostrando
  uint32_t width, height, pitch, format, offset;
  drm_gem_t *gem;
  struct drm_file *owner;
  struct drm_fb *next;
} drm_fb_t;

typedef struct drm_event_rec {
  uint32_t type;
  uint32_t length;
  uint64_t user_data;
  uint32_t tv_sec, tv_usec, sequence, crtc_id;
} drm_event_rec_t;

typedef struct drm_file {
  uint32_t magic;
  bool render; // abierto por renderD128: sin KMS
  bool master;
  bool universal_planes;
  bool atomic;
  bool nonblock;
  drm_gem_t *handles[DRM_MAX_HANDLES]; // handle = indice + 1
  uint8_t imported[DRM_MAX_HANDLES];   // 1 = el handle posee su propia ref (PRIME)
  drm_gem_t *gems;                     // todos los GEM creados por el fichero
  struct drm_syncobj *syncobjs[DRM_MAX_SYNCOBJS]; // handle = indice + 1
  drm_event_rec_t ring[DRM_EVENT_RING];
  uint32_t head, tail;
  wait_queue_t wq;
  struct drm_file *next;
} drm_file_t;

// drm_dumb.c ----------------------------------------------------------------
int drm_gem_create(drm_file_t *f, size_t size, drm_gem_t **out);
void drm_gem_get(drm_gem_t *g); // toma una referencia
void drm_gem_put(drm_gem_t *g); // suelta una referencia
// [LinuxKPI L4] GEM sin dueño sobre `npages` paginas ajenas (se copian las
// direcciones); al soltar la ultima referencia llama a release(priv).
struct vfs_node;
// [LinuxKPI L4] dmabuf de Aurora para la capa (drm_prime.c).
struct vfs_node *dmabuf_node_new_ext(uint32_t npages, const uint64_t *pages,
                                     void (*release)(void *), void *priv);
int dmabuf_node_pages(struct vfs_node *n, uint32_t *npages,
                      const uint64_t **pages);
void *dmabuf_node_ext_priv(struct vfs_node *n, void (*release)(void *));
drm_gem_t *drm_gem_new_ext(uint32_t npages, const uint64_t *pages,
                           void (*release)(void *), void *priv);
int drm_handle_alloc(drm_file_t *f, drm_gem_t *g, uint32_t *handle);
drm_gem_t *drm_handle_lookup(drm_file_t *f, uint32_t handle);
int drm_handle_close(drm_file_t *f, uint32_t handle);
void drm_file_release_gems(drm_file_t *f);
// Copia `len` bytes del GEM desde `off` a dst (dst en memoria de kernel).
void drm_gem_read(const drm_gem_t *g, size_t off, void *dst, size_t len);
int drm_gem_get_page(drm_file_t *f, uint64_t byte_off, uint64_t *phys);

int drm_ioctl_gem_flink(drm_file_t *f, uint64_t arg);
int drm_ioctl_gem_open(drm_file_t *f, uint64_t arg);
int drm_ioctl_create_dumb(drm_file_t *f, uint64_t arg);
int drm_ioctl_map_dumb(drm_file_t *f, uint64_t arg);
int drm_ioctl_destroy_dumb(drm_file_t *f, uint64_t arg);

// drm_prime.c ---------------------------------------------------------------
int drm_ioctl_prime_handle_to_fd(drm_file_t *f, uint64_t arg);
int drm_ioctl_prime_fd_to_handle(drm_file_t *f, uint64_t arg);

// drm_syncobj.c -------------------------------------------------------------
// ioctls 0xBF..0xCD (SYNCOBJ_*); bloquean, asi que se llaman sin g_drm_lock.
int drm_syncobj_ioctl(drm_file_t *f, unsigned long req, uint64_t arg);
void drm_file_release_syncobjs(drm_file_t *f);
// Fence implicito de un dma-buf (DMA_BUF_IOCTL_EXPORT/IMPORT_SYNC_FILE).
struct drm_syncobj *drm_resv_new(void); // nace senalado
void drm_resv_put(struct drm_syncobj *o);
bool drm_resv_signaled(struct drm_syncobj *o);
int drm_resv_export_sync_file(struct drm_syncobj *o, int *fd_out); // fd nuevo
int drm_resv_import_sync_file(struct drm_syncobj **o, int fd);      // reemplaza el fence
wait_queue_t *drm_syncobj_wq(void);
// [LinuxKPI L5] aur_fence: fences para otros subsistemas (un syncobj binario).
// La capa LinuxKPI los envuelve en dma_fence; ver drm_syncobj.c.
typedef struct drm_syncobj aur_fence_t;
aur_fence_t *aur_fence_create(int signaled);
void aur_fence_get(aur_fence_t *f);
void aur_fence_put(aur_fence_t *f);
void aur_fence_signal(aur_fence_t *f);
bool aur_fence_signaled(aur_fence_t *f);
void aur_fence_wake(void);
int aur_fence_watch(aur_fence_t *f, void (*fn)(void *), void *data);
int aur_fence_unwatch(aur_fence_t *f, void (*fn)(void *), void *data);
int aur_fence_from_fd(int fd, aur_fence_t **out);
int aur_fence_to_fd(aur_fence_t *f, int cloexec, int *fd_out);
int aur_fence_alias(aur_fence_t *dst, aur_fence_t *src);
int aur_fence_alias_replace(aur_fence_t **dst, aur_fence_t *src);
// [LinuxKPI L5] Fence implicito de un dmabuf de un driver de Linux: lo sirve
// su dma_resv. export_fence devuelve una referencia nueva (senalado si no hay
// trabajo pendiente) para flags DMA_BUF_SYNC_*; import_fence recibe el fence
// (prestado) que pasa a ser el implicito; poll devuelve los eventos listos.
struct dmabuf_resv_ops {
  aur_fence_t *(*export_fence)(void *priv, uint32_t flags);
  int (*import_fence)(void *priv, uint32_t flags, aur_fence_t *f);
  int (*poll)(void *priv, short events);
};
struct vfs_node;
int dmabuf_node_set_resv_ops(struct vfs_node *n, void (*release)(void *),
                             const struct dmabuf_resv_ops *ops);
// Fence implicito de cualquier dmabuf de Aurora (nativo o de la capa).
int dmabuf_node_export_fence(struct vfs_node *n, uint32_t flags, aur_fence_t **out);
int dmabuf_node_import_fence(struct vfs_node *n, uint32_t flags, aur_fence_t *f);

// Para drivers de GPU: busca un syncobj por handle y senala un punto
// (point 0 = binario). Despierta a los que esperan.
struct drm_syncobj *drm_syncobj_find(drm_file_t *f, uint32_t handle);
void drm_syncobj_signal_point(struct drm_syncobj *o, uint64_t point);

// drm_sysfs.c ---------------------------------------------------------------
void drm_sysfs_publish(void);
void drm_sysfs_unpublish(void);
void drm_udev_publish(void);

// Utilidades de acceso a userland.
int drm_put_user(uint64_t arg, const void *src, size_t n);
int drm_get_user(uint64_t arg, void *dst, size_t n);

// Inicializacion (llamar tras devtmpfs/sysfs_dyn, en contexto de tarea).
void drm_init(void);
// El backend de Linux (lkpi_drm.kur) toma o devuelve /dev/dri y /sys/class/drm.
void drm_backend_claim(void);
void drm_backend_release(void);
// Scanout del backend de Linux: una ventana del compositor por CRTC.
void drm_lx_scanout_close(int idx);
int drm_lx_scanout_open(int idx, int w, int h);
int drm_lx_scanout_present(int idx, const void *src, uint32_t pitch,
                           uint32_t fourcc, int w, int h);
void drm_fb_put(drm_fb_t *fb);

#endif
