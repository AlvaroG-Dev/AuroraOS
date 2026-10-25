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

typedef struct drm_gem {
  uint32_t id;       // unico en el sistema; mmap offset = id << 32
  int refcnt;        // 1 del creador (hasta cerrar el fichero) + 1 por fb
  size_t size;       // bytes, multiplo de PAGE_SIZE
  uint32_t npages;
  uint64_t *pages;   // direcciones fisicas de cada pagina
  bool mapped;      // MAP_DUMB pedido: las paginas no se liberan hasta cerrar
  bool creator_ref; // aun cuenta la referencia del creador
  struct drm_file *owner;
  struct drm_gem *next; // lista del fichero propietario
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
  drm_gem_t *gems;                     // todos los GEM creados por el fichero
  drm_event_rec_t ring[DRM_EVENT_RING];
  uint32_t head, tail;
  wait_queue_t wq;
  struct drm_file *next;
} drm_file_t;

// drm_dumb.c ----------------------------------------------------------------
int drm_gem_create(drm_file_t *f, size_t size, drm_gem_t **out);
void drm_gem_put(drm_gem_t *g); // suelta una referencia
int drm_handle_alloc(drm_file_t *f, drm_gem_t *g, uint32_t *handle);
drm_gem_t *drm_handle_lookup(drm_file_t *f, uint32_t handle);
int drm_handle_close(drm_file_t *f, uint32_t handle);
void drm_file_release_gems(drm_file_t *f);
// Copia `len` bytes del GEM desde `off` a dst (dst en memoria de kernel).
void drm_gem_read(const drm_gem_t *g, size_t off, void *dst, size_t len);
int drm_gem_get_page(drm_file_t *f, uint64_t byte_off, uint64_t *phys);

int drm_ioctl_create_dumb(drm_file_t *f, uint64_t arg);
int drm_ioctl_map_dumb(drm_file_t *f, uint64_t arg);
int drm_ioctl_destroy_dumb(drm_file_t *f, uint64_t arg);

// drm_sysfs.c ---------------------------------------------------------------
void drm_sysfs_publish(void);
void drm_udev_publish(void);

// Utilidades de acceso a userland.
int drm_put_user(uint64_t arg, const void *src, size_t n);
int drm_get_user(uint64_t arg, void *dst, size_t n);

// Inicializacion (llamar tras devtmpfs/sysfs_dyn, en contexto de tarea).
void drm_init(void);
void drm_fb_put(drm_fb_t *fb);

#endif
