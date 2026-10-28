// kernel/drm/drm_core.c
//
// Nodos /dev/dri/card0 y /dev/dri/renderD128, ioctls DRM/KMS de Linux
// (legacy + atomic), propiedades, blobs, eventos de flip/vblank (timer a
// 60 Hz) y scanout hacia una ventana del compositor (modo A del plan).

#include "drm.h"
#include "../display_edid.h"
#include "../devtmpfs.h"
#include "../gfx/compositor.h"
#include "../gfx/window.h"
#include "../gpu_proxy.h"
#include "../heap.h"
#include "../klog.h"
#include "../kthread.h"
#include "../mutex.h"
#include "../paging.h"
#include "../spinlock.h"
#include "../string.h"
#include "../time.h"
#include "../uaccess.h"
#include "../wait.h"

extern uint32_t fb_width, fb_height;

#define O_NONBLOCK_L 0x800

// ---------------------------------------------------------------------------
// Estructuras binarias de Linux (include/uapi/drm/drm.h, drm_mode.h)
// ---------------------------------------------------------------------------
struct drm_version_u {
  int32_t major, minor, patch;
  uint64_t name_len;
  uint64_t name;
  uint64_t date_len;
  uint64_t date;
  uint64_t desc_len;
  uint64_t desc;
};

struct drm_unique_u {
  uint64_t len;
  uint64_t ptr;
};

struct drm_get_cap_u {
  uint64_t capability, value;
};

struct drm_gem_close_u {
  uint32_t handle, pad;
};

struct drm_auth_u {
  uint32_t magic;
};

struct drm_set_version_u {
  int32_t di_major, di_minor, dd_major, dd_minor;
};

struct modeinfo {
  uint32_t clock;
  uint16_t hdisplay, hsync_start, hsync_end, htotal, hskew;
  uint16_t vdisplay, vsync_start, vsync_end, vtotal, vscan;
  uint32_t vrefresh, flags, type;
  char name[32];
};
_Static_assert(sizeof(struct modeinfo) == 68, "drm_mode_modeinfo");

struct mode_card_res {
  uint64_t fb_id_ptr, crtc_id_ptr, connector_id_ptr, encoder_id_ptr;
  uint32_t count_fbs, count_crtcs, count_connectors, count_encoders;
  uint32_t min_width, max_width, min_height, max_height;
};

struct mode_crtc {
  uint64_t set_connectors_ptr;
  uint32_t count_connectors, crtc_id, fb_id, x, y, gamma_size, mode_valid;
  struct modeinfo mode;
};
_Static_assert(sizeof(struct mode_crtc) == 104, "drm_mode_crtc");

struct mode_get_encoder {
  uint32_t encoder_id, encoder_type, crtc_id, possible_crtcs, possible_clones;
};

struct mode_get_connector {
  uint64_t encoders_ptr, modes_ptr, props_ptr, prop_values_ptr;
  uint32_t count_modes, count_props, count_encoders;
  uint32_t encoder_id, connector_id, connector_type, connector_type_id;
  uint32_t connection, mm_width, mm_height, subpixel, pad;
};
_Static_assert(sizeof(struct mode_get_connector) == 80, "drm_mode_get_connector");

struct mode_get_property {
  uint64_t values_ptr, enum_blob_ptr;
  uint32_t prop_id, flags;
  char name[32];
  uint32_t count_values, count_enum_blobs;
};

struct prop_enum_u {
  uint64_t value;
  char name[32];
};

struct mode_connector_set_property {
  uint64_t value;
  uint32_t prop_id, connector_id;
};

struct mode_get_blob {
  uint32_t blob_id, length;
  uint64_t data;
};

struct mode_fb_cmd {
  uint32_t fb_id, width, height, pitch, bpp, depth, handle;
};

struct mode_fb_cmd2 {
  uint32_t fb_id, width, height, pixel_format, flags;
  uint32_t handles[4], pitches[4], offsets[4];
  uint64_t modifier[4];
};
_Static_assert(sizeof(struct mode_fb_cmd2) == 104, "drm_mode_fb_cmd2");

struct mode_crtc_page_flip {
  uint32_t crtc_id, fb_id, flags, reserved;
  uint64_t user_data;
};

struct mode_fb_dirty_cmd {
  uint32_t fb_id, flags, color, num_clips;
  uint64_t clips_ptr;
};

struct mode_get_plane_res {
  uint64_t plane_id_ptr;
  uint32_t count_planes;
};

struct mode_get_plane {
  uint32_t plane_id, crtc_id, fb_id, possible_crtcs, gamma_size,
      count_format_types;
  uint64_t format_type_ptr;
};

struct mode_set_plane {
  uint32_t plane_id, crtc_id, fb_id, flags;
  int32_t crtc_x, crtc_y;
  uint32_t crtc_w, crtc_h, src_x, src_y, src_h, src_w;
};

struct mode_obj_get_properties {
  uint64_t props_ptr, prop_values_ptr;
  uint32_t count_props, obj_id, obj_type;
};

struct mode_obj_set_property {
  uint64_t value;
  uint32_t prop_id, obj_id, obj_type;
};

struct mode_atomic {
  uint32_t flags, count_objs;
  uint64_t objs_ptr, count_props_ptr, props_ptr, prop_values_ptr, reserved,
      user_data;
};

struct mode_create_blob {
  uint64_t data;
  uint32_t length, blob_id;
};

struct wait_vblank_u {
  uint32_t type, sequence;
  uint64_t signal_or_tval_sec; // request.signal / reply.tval_sec
  uint64_t tval_usec;          // solo en reply
};

#define OBJ_CRTC 0xcccccccc
#define OBJ_CONNECTOR 0xc0c0c0c0
#define OBJ_ENCODER 0xe0e0e0e0
#define OBJ_FB 0xfbfbfbfb
#define OBJ_BLOB 0xbbbbbbbb
#define OBJ_PLANE 0xeeeeeeee

#define PROP_RANGE 0x2
#define PROP_IMMUTABLE 0x4
#define PROP_ENUM 0x8
#define PROP_BLOB 0x10
#define PROP_OBJECT 0x40
#define PROP_SIGNED_RANGE 0x80
#define PROP_ATOMIC 0x80000000u

#define PAGE_FLIP_EVENT 0x01
#define ATOMIC_TEST_ONLY 0x100
#define ATOMIC_NONBLOCK 0x200
#define ATOMIC_ALLOW_MODESET 0x400
#define ATOMIC_FLAGS_VALID                                                     \
  (PAGE_FLIP_EVENT | 0x02 | ATOMIC_TEST_ONLY | ATOMIC_NONBLOCK |               \
   ATOMIC_ALLOW_MODESET)

#define DRM_EVENT_VBLANK 0x01
#define DRM_EVENT_FLIP_COMPLETE 0x02

#define MODE_TYPE_PREFERRED (1u << 3)
#define MODE_TYPE_DRIVER (1u << 6)
#define MODE_FLAG_PHSYNC 1u
#define MODE_FLAG_PVSYNC 4u
#define MODE_FLAG_NHSYNC 2u
#define MODE_FLAG_NVSYNC 8u
#define MODE_FLAG_INTERLACE 16u

// Propiedades.
enum {
  P_PLANE_TYPE = 10,
  P_PLANE_FB_ID,
  P_PLANE_CRTC_ID,
  P_PLANE_CRTC_X,
  P_PLANE_CRTC_Y,
  P_PLANE_CRTC_W,
  P_PLANE_CRTC_H,
  P_PLANE_SRC_X,
  P_PLANE_SRC_Y,
  P_PLANE_SRC_W,
  P_PLANE_SRC_H,
  P_CRTC_ACTIVE,
  P_CRTC_MODE_ID,
  P_CONN_DPMS,
  P_CONN_CRTC_ID,
  P_CONN_EDID,
  P_LAST
};

typedef struct {
  uint32_t id;
  const char *name;
  uint32_t flags;
  int64_t min, max;
  uint32_t obj_type; // para OBJECT: tipo apuntado
} prop_def_t;

static const prop_def_t g_props[] = {
    {P_PLANE_TYPE, "type", PROP_ENUM | PROP_IMMUTABLE, 0, 2, 0},
    {P_PLANE_FB_ID, "FB_ID", PROP_OBJECT | PROP_ATOMIC, 0, 0, OBJ_FB},
    {P_PLANE_CRTC_ID, "CRTC_ID", PROP_OBJECT | PROP_ATOMIC, 0, 0, OBJ_CRTC},
    {P_PLANE_CRTC_X, "CRTC_X", PROP_SIGNED_RANGE | PROP_ATOMIC, INT32_MIN,
     INT32_MAX, 0},
    {P_PLANE_CRTC_Y, "CRTC_Y", PROP_SIGNED_RANGE | PROP_ATOMIC, INT32_MIN,
     INT32_MAX, 0},
    {P_PLANE_CRTC_W, "CRTC_W", PROP_RANGE | PROP_ATOMIC, 0, INT32_MAX, 0},
    {P_PLANE_CRTC_H, "CRTC_H", PROP_RANGE | PROP_ATOMIC, 0, INT32_MAX, 0},
    {P_PLANE_SRC_X, "SRC_X", PROP_RANGE | PROP_ATOMIC, 0, UINT32_MAX, 0},
    {P_PLANE_SRC_Y, "SRC_Y", PROP_RANGE | PROP_ATOMIC, 0, UINT32_MAX, 0},
    {P_PLANE_SRC_W, "SRC_W", PROP_RANGE | PROP_ATOMIC, 0, UINT32_MAX, 0},
    {P_PLANE_SRC_H, "SRC_H", PROP_RANGE | PROP_ATOMIC, 0, UINT32_MAX, 0},
    {P_CRTC_ACTIVE, "ACTIVE", PROP_RANGE | PROP_ATOMIC, 0, 1, 0},
    {P_CRTC_MODE_ID, "MODE_ID", PROP_BLOB | PROP_ATOMIC, 0, 0, 0},
    {P_CONN_DPMS, "DPMS", PROP_ENUM, 0, 3, 0},
    {P_CONN_CRTC_ID, "CRTC_ID", PROP_OBJECT | PROP_ATOMIC, 0, 0, OBJ_CRTC},
    {P_CONN_EDID, "EDID", PROP_BLOB | PROP_IMMUTABLE, 0, 0, 0},
};
#define NPROPS ((int)(sizeof(g_props) / sizeof(g_props[0])))

static const uint32_t g_plane_props[] = {
    P_PLANE_TYPE,   P_PLANE_FB_ID,  P_PLANE_CRTC_ID, P_PLANE_CRTC_X,
    P_PLANE_CRTC_Y, P_PLANE_CRTC_W, P_PLANE_CRTC_H,  P_PLANE_SRC_X,
    P_PLANE_SRC_Y,  P_PLANE_SRC_W,  P_PLANE_SRC_H};
static const uint32_t g_crtc_props[] = {P_CRTC_ACTIVE, P_CRTC_MODE_ID};
static const uint32_t g_conn_props[] = {P_CONN_DPMS, P_CONN_CRTC_ID,
                                        P_CONN_EDID};

static const struct {
  uint64_t v;
  const char *n;
} g_plane_type_enum[] = {{0, "Overlay"}, {1, "Primary"}, {2, "Cursor"}};
static const struct {
  uint64_t v;
  const char *n;
} g_dpms_enum[] = {{0, "On"}, {1, "Standby"}, {2, "Suspend"}, {3, "Off"}};

// ---------------------------------------------------------------------------
// Estado global
// ---------------------------------------------------------------------------
typedef struct {
  uint32_t id, len;
  uint8_t *data;
  drm_file_t *owner;
} drm_blob_t;

#define MAX_BLOBS 32
#define MAX_PEND 16

typedef struct {
  drm_file_t *f;
  uint64_t user_data;
  uint32_t type; // DRM_EVENT_*
  bool used;
} pend_ev_t;

static mutex_t g_drm_lock;      // serializa ioctls y close
static spinlock_t g_ev_lock = {.locked = 0}; // eventos, files, flip_busy
static drm_file_t *g_files;
static drm_file_t *g_master;
static drm_fb_t *g_fbs;
static uint32_t g_next_fb = DRM_FB_ID_BASE;
static drm_blob_t g_blobs[MAX_BLOBS];
static uint32_t g_next_blob = DRM_BLOB_ID_BASE;
static uint32_t g_magic_ctr = 0x1000;

typedef struct {
  bool active;
  bool mode_valid;
  struct modeinfo mode;
  drm_fb_t *fb;
  uint32_t x, y;
  uint32_t mode_blob; // ultimo MODE_ID aplicado por atomic (informativo)
} crtc_state_t;
static crtc_state_t g_crtc;

static window_t *g_win;
static int g_win_w, g_win_h;

static pend_ev_t g_pend[MAX_PEND];
static volatile int g_flip_busy;   // flip aplicado y su evento aun sin entregar
static volatile int g_vbl_waiters; // WAIT_VBLANK bloqueados
static volatile uint32_t g_vbl_seq;
static wait_queue_t g_kick_wq, g_tick_wq, g_vbl_wq;

// ---------------------------------------------------------------------------
// Modos
// ---------------------------------------------------------------------------
static void fill_mode(struct modeinfo *m, int w, int h, uint32_t type) {
  memset(m, 0, sizeof(*m));
  m->hdisplay = (uint16_t)w;
  m->hsync_start = (uint16_t)(w + 48);
  m->hsync_end = (uint16_t)(w + 80);
  m->htotal = (uint16_t)(w + 160);
  m->vdisplay = (uint16_t)h;
  m->vsync_start = (uint16_t)(h + 3);
  m->vsync_end = (uint16_t)(h + 8);
  m->vtotal = (uint16_t)(h + 40);
  m->vrefresh = 60;
  m->clock = (uint32_t)((uint64_t)m->htotal * m->vtotal * 60 / 1000);
  m->flags = MODE_FLAG_PHSYNC | MODE_FLAG_PVSYNC;
  m->type = type;
  snprintf(m->name, sizeof(m->name), "%dx%d", w, h);
}

static void work_size(int *w, int *h) {
  rect_t wa = compositor_work_area();
  int ww = wa.w, hh = wa.h;
  if (ww > 4096)
    ww = 4096;
  if (hh > 4096)
    hh = 4096;
  if (ww < 64)
    ww = 64;
  if (hh < 64)
    hh = 64;
  *w = ww & ~1; // ancho par
  *h = hh;
}

#define MAX_MODES 24
static drm_blob_t *edid_blob(void);

static void mode_from_edid(struct modeinfo *m, const struct edid_mode *e, uint32_t type) {
  memset(m, 0, sizeof(*m));
  m->clock = e->clock_khz;
  m->hdisplay = e->hdisplay;
  m->hsync_start = e->hsync_start;
  m->hsync_end = e->hsync_end;
  m->htotal = e->htotal;
  m->vdisplay = e->vdisplay;
  m->vsync_start = e->vsync_start;
  m->vsync_end = e->vsync_end;
  m->vtotal = e->vtotal;
  m->vrefresh = e->refresh_hz;
  m->flags = ((e->flags & EDID_MODE_PHSYNC) ? MODE_FLAG_PHSYNC : MODE_FLAG_NHSYNC) |
             ((e->flags & EDID_MODE_PVSYNC) ? MODE_FLAG_PVSYNC : MODE_FLAG_NVSYNC) |
             ((e->flags & EDID_MODE_INTERLACED) ? MODE_FLAG_INTERLACE : 0);
  m->type = type | (e->detailed ? MODE_TYPE_DRIVER : 0);
  snprintf(m->name, sizeof(m->name), "%dx%d", e->hdisplay, e->vdisplay);
}

static bool mode_listed(const struct modeinfo *out, int n, int w, int h, int hz) {
  for (int i = 0; i < n; i++)
    if (out[i].hdisplay == w && out[i].vdisplay == h && (!hz || out[i].vrefresh == hz))
      return true;
  return false;
}

// Modos del conector: el del area de trabajo (preferido: es lo que se muestra),
// los del EDID del monitor (temporizaciones reales) que quepan en ella y los
// estandar de siempre. El scanout es una ventana, asi que un modo mas grande
// que el area de trabajo no se ofrece.
static int build_modes(struct modeinfo *out) {
  int w, h, n = 0;
  work_size(&w, &h);
  struct edid_mode em[16];
  int ne = 0;
  drm_blob_t *eb = edid_blob();
  if (eb)
    ne = edid_modes(eb->data, eb->len, em, 16);
  // Si el EDID trae la temporizacion exacta del modo del area de trabajo, se usa.
  int pref = -1;
  for (int i = 0; i < ne; i++)
    if (em[i].hdisplay == w && em[i].vdisplay == h && em[i].detailed) {
      pref = i;
      break;
    }
  if (pref >= 0)
    mode_from_edid(&out[n++], &em[pref], MODE_TYPE_PREFERRED);
  else
    fill_mode(&out[n++], w, h, MODE_TYPE_PREFERRED | MODE_TYPE_DRIVER);
  for (int i = 0; i < ne && n < MAX_MODES; i++) {
    if (i == pref || em[i].hdisplay > w || em[i].vdisplay > h ||
        (em[i].flags & EDID_MODE_INTERLACED))
      continue;
    if (mode_listed(out, n, em[i].hdisplay, em[i].vdisplay, em[i].refresh_hz))
      continue;
    mode_from_edid(&out[n++], &em[i], 0);
  }
  static const int std[][2] = {
      {1920, 1080}, {1280, 720}, {1024, 768}, {800, 600}, {640, 480}};
  for (unsigned i = 0; i < sizeof(std) / sizeof(std[0]) && n < MAX_MODES; i++) {
    if (std[i][0] <= w && std[i][1] <= h && !mode_listed(out, n, std[i][0], std[i][1], 0))
      fill_mode(&out[n++], std[i][0], std[i][1], MODE_TYPE_DRIVER);
  }
  return n;
}

// ---------------------------------------------------------------------------
// Framebuffers
// ---------------------------------------------------------------------------
void drm_fb_put(drm_fb_t *fb) {
  if (!fb)
    return;
  if (--fb->refcnt > 0)
    return;
  drm_gem_put(fb->gem);
  kfree(fb);
}

static drm_fb_t *fb_find(uint32_t id) {
  for (drm_fb_t *f = g_fbs; f; f = f->next)
    if (f->id == id)
      return f;
  return NULL;
}

static void fb_unlink(drm_fb_t *fb) {
  drm_fb_t **pp = &g_fbs;
  while (*pp && *pp != fb)
    pp = &(*pp)->next;
  if (*pp)
    *pp = fb->next;
}

static int fb_create(drm_file_t *f, uint32_t w, uint32_t h, uint32_t pitch,
                     uint32_t format, uint32_t offset, uint32_t handle,
                     uint32_t *out_id) {
  if (w == 0 || h == 0 || w > 8192 || h > 8192)
    return -EINVAL;
  if (format != DRM_FORMAT_XRGB8888 && format != DRM_FORMAT_ARGB8888)
    return -EINVAL;
  if (pitch < w * 4 || (pitch & 3) || (offset & 3))
    return -EINVAL;
  drm_gem_t *g = drm_handle_lookup(f, handle);
  if (!g)
    return -ENOENT;
  if ((uint64_t)offset + (uint64_t)pitch * h > g->size)
    return -EINVAL;
  drm_fb_t *fb = (drm_fb_t *)kzalloc(sizeof(*fb));
  if (!fb)
    return -ENOMEM;
  fb->id = g_next_fb++;
  fb->refcnt = 1;
  fb->width = w;
  fb->height = h;
  fb->pitch = pitch;
  fb->format = format;
  fb->offset = offset;
  fb->gem = g;
  g->refcnt++; // protegido por g_drm_lock: aun no hay concurrencia con put
  fb->owner = f;
  fb->next = g_fbs;
  g_fbs = fb;
  *out_id = fb->id;
  return 0;
}

// ---------------------------------------------------------------------------
// Scanout (ventana del compositor)
// ---------------------------------------------------------------------------
static void scanout_close(void) {
  if (g_win) {
    compositor_close_window(g_win);
    g_win = NULL;
  }
  g_win_w = g_win_h = 0;
}

static int scanout_open(int w, int h) {
  if (g_win && g_win_w == w && g_win_h == h)
    return 0;
  scanout_close();
  rect_t wa = compositor_work_area();
  int x = wa.x + (wa.w - w) / 2;
  int y = wa.y + (wa.h - h) / 2;
  if (x < 0)
    x = 0;
  if (y < wa.y)
    y = wa.y;
  window_t *win = compositor_create_window(
      x, y, w, h, "AuroraOS DRM", WIN_FLAGS_NODECORATION | WIN_FLAGS_FIXED_SIZE);
  if (!win)
    return -ENOMEM;
  g_win = win;
  g_win_w = w;
  g_win_h = h;
  compositor_focus_window(win);
  return 0;
}

// Copia el framebuffer activo a la ventana.
static void scanout_present(void) {
  drm_fb_t *fb = g_crtc.fb;
  window_t *win = g_win;
  if (!fb || !win || !g_crtc.active)
    return;
  unsigned long fl = spin_lock_irqsave(&win->content_lock);
  uint32_t *buf = win->content_buffer;
  if (buf) {
    int cw = win->content_w, ch = win->content_h;
    uint32_t sx = g_crtc.x, sy = g_crtc.y;
    int rows = ch;
    if (sy >= fb->height)
      rows = 0;
    else if ((uint32_t)rows > fb->height - sy)
      rows = (int)(fb->height - sy);
    int cols = cw;
    if (sx >= fb->width)
      cols = 0;
    else if ((uint32_t)cols > fb->width - sx)
      cols = (int)(fb->width - sx);
    bool opaque = fb->format == DRM_FORMAT_XRGB8888;
    for (int y = 0; y < rows; y++) {
      uint32_t *dst = buf + (size_t)y * cw;
      drm_gem_read(fb->gem,
                   fb->offset + (size_t)(y + sy) * fb->pitch + (size_t)sx * 4,
                   dst, (size_t)cols * 4);
      if (opaque)
        for (int x = 0; x < cols; x++)
          dst[x] |= 0xFF000000u;
    }
    window_mark_content_locked(win, (rect_t){0, 0, cw, ch});
  }
  int ix = win->x, iy = win->y + window_titlebar_h(win);
  int iw = win->content_w, ih = win->content_h;
  spin_unlock_irqrestore(&win->content_lock, fl);
  compositor_invalidate_rect((rect_t){ix, iy, iw, ih});
}

// Aplica un nuevo estado al CRTC. `fb`==NULL o !active apaga la salida.
// Toma su propia referencia sobre fb.
static int crtc_commit(drm_fb_t *fb, uint32_t x, uint32_t y,
                       const struct modeinfo *mode, bool active) {
  if (!active || !fb || !mode) {
    drm_fb_t *old = g_crtc.fb;
    g_crtc.fb = NULL;
    g_crtc.active = false;
    scanout_close();
    drm_fb_put(old);
    return 0;
  }
  int rc = scanout_open(mode->hdisplay, mode->vdisplay);
  if (rc)
    return rc;
  fb->refcnt++;
  drm_fb_t *old = g_crtc.fb;
  g_crtc.fb = fb;
  g_crtc.x = x;
  g_crtc.y = y;
  g_crtc.mode = *mode;
  g_crtc.mode_valid = true;
  g_crtc.active = true;
  drm_fb_put(old);
  scanout_present();
  return 0;
}

static int mode_check(const struct modeinfo *m) {
  if (m->hdisplay < 64 || m->vdisplay < 64 || m->hdisplay > fb_width ||
      m->vdisplay > fb_height)
    return -EINVAL;
  return 0;
}

// ---------------------------------------------------------------------------
// Eventos y tick de "vblank"
// ---------------------------------------------------------------------------
static bool vbl_have_work(void *a) {
  (void)a;
  return g_flip_busy > 0 || g_vbl_waiters > 0;
}

static bool never(void *a) {
  (void)a;
  return false;
}

static void ring_push_locked(drm_file_t *f, const drm_event_rec_t *e) {
  if (f->head - f->tail >= DRM_EVENT_RING)
    return;
  f->ring[f->head++ % DRM_EVENT_RING] = *e;
}

static int vbl_thread(void *arg) {
  (void)arg;
  for (;;) {
    if (!vbl_have_work(NULL)) {
      wait_event_timeout(&g_kick_wq, vbl_have_work, NULL, 0);
      continue;
    }
    wait_event_timeout(&g_tick_wq, never, NULL, 16);

    uint64_t ms = tick_count;
    unsigned long fl = spin_lock_irqsave(&g_ev_lock);
    g_vbl_seq++;
    g_flip_busy = 0;
    for (int i = 0; i < MAX_PEND; i++) {
      pend_ev_t *p = &g_pend[i];
      if (!p->used)
        continue;
      drm_event_rec_t e;
      e.type = p->type;
      e.length = sizeof(e);
      e.user_data = p->user_data;
      e.tv_sec = (uint32_t)(ms / 1000);
      e.tv_usec = (uint32_t)(ms % 1000) * 1000;
      e.sequence = g_vbl_seq;
      e.crtc_id = DRM_CRTC_ID;
      ring_push_locked(p->f, &e);
      p->used = false;
      wake_up_all(&p->f->wq);
    }
    spin_unlock_irqrestore(&g_ev_lock, fl);
    wake_up_all(&g_vbl_wq);
  }
  return 0;
}

static int queue_event(drm_file_t *f, uint32_t type, uint64_t user_data) {
  unsigned long fl = spin_lock_irqsave(&g_ev_lock);
  for (int i = 0; i < MAX_PEND; i++) {
    if (!g_pend[i].used) {
      g_pend[i].used = true;
      g_pend[i].f = f;
      g_pend[i].type = type;
      g_pend[i].user_data = user_data;
      spin_unlock_irqrestore(&g_ev_lock, fl);
      wake_up_all(&g_kick_wq);
      return 0;
    }
  }
  spin_unlock_irqrestore(&g_ev_lock, fl);
  return -ENOMEM;
}

static void mark_flip(void) {
  g_flip_busy = 1;
  wake_up_all(&g_kick_wq);
}

// ---------------------------------------------------------------------------
// Blobs
// ---------------------------------------------------------------------------
// EDID del adaptador (si el driver GPU .aur registro uno): blob fijo y de solo
// lectura, id reservado por debajo de DRM_BLOB_ID_BASE.
#define DRM_EDID_BLOB_ID (DRM_BLOB_ID_BASE - 1)
static drm_blob_t g_edid_blob;
static drm_blob_t *edid_blob(void) {
  const struct aur_gpu_reg *a = gpup_adapter();
  g_edid_blob.id = DRM_EDID_BLOB_ID;
  g_edid_blob.owner = NULL;
  if (a && a->edid_len == 128) {
    g_edid_blob.data = (uint8_t *)a->edid;
    g_edid_blob.len = a->edid_len;
    return &g_edid_blob;
  }
  // Sin driver .aur: el EDID del monitor que dio el firmware (UEFI).
  uint32_t n = 0;
  const uint8_t *e = display_edid_data(&n);
  if (!e)
    return NULL;
  g_edid_blob.data = (uint8_t *)e;
  g_edid_blob.len = n;
  return &g_edid_blob;
}

static drm_blob_t *blob_find(uint32_t id) {
  if (id == DRM_EDID_BLOB_ID)
    return edid_blob();
  for (int i = 0; i < MAX_BLOBS; i++)
    if (g_blobs[i].data && g_blobs[i].id == id)
      return &g_blobs[i];
  return NULL;
}

static void blob_free(drm_blob_t *b) {
  kfree(b->data);
  memset(b, 0, sizeof(*b));
}

static int ioctl_create_blob(drm_file_t *f, uint64_t arg) {
  struct mode_create_blob a;
  int rc = drm_get_user(arg, &a, sizeof(a));
  if (rc)
    return rc;
  if (a.length == 0 || a.length > 4096)
    return -EINVAL;
  drm_blob_t *slot = NULL;
  for (int i = 0; i < MAX_BLOBS; i++)
    if (!g_blobs[i].data) {
      slot = &g_blobs[i];
      break;
    }
  if (!slot)
    return -ENOSPC;
  uint8_t *d = (uint8_t *)kmalloc(a.length);
  if (!d)
    return -ENOMEM;
  if (!access_ok((void *)a.data, a.length) ||
      copy_from_user(d, (void *)a.data, a.length) < 0) {
    kfree(d);
    return -EFAULT;
  }
  slot->data = d;
  slot->len = a.length;
  slot->id = g_next_blob++;
  slot->owner = f;
  a.blob_id = slot->id;
  return drm_put_user(arg, &a, sizeof(a));
}

static int ioctl_destroy_blob(uint64_t arg) {
  uint32_t id;
  int rc = drm_get_user(arg, &id, sizeof(id));
  if (rc)
    return rc;
  if (id == DRM_EDID_BLOB_ID)
    return -EPERM;
  drm_blob_t *b = blob_find(id);
  if (!b)
    return -ENOENT;
  blob_free(b);
  return 0;
}

static int ioctl_get_blob(uint64_t arg) {
  struct mode_get_blob a;
  int rc = drm_get_user(arg, &a, sizeof(a));
  if (rc)
    return rc;
  drm_blob_t *b = blob_find(a.blob_id);
  if (!b)
    return -ENOENT;
  uint32_t want = a.length;
  a.length = b->len;
  if (a.data && want >= b->len) {
    if (!access_ok((void *)a.data, b->len) ||
        copy_to_user((void *)a.data, b->data, b->len) < 0)
      return -EFAULT;
  }
  return drm_put_user(arg, &a, sizeof(a));
}

// ---------------------------------------------------------------------------
// Version / caps
// ---------------------------------------------------------------------------
static int put_string(uint64_t ptr, uint64_t *len, const char *s) {
  uint64_t n = strlen(s);
  uint64_t want = *len;
  *len = n;
  if (ptr && want) {
    uint64_t c = n < want ? n : want;
    if (!access_ok((void *)ptr, c) || copy_to_user((void *)ptr, s, c) < 0)
      return -EFAULT;
  }
  return 0;
}

static int ioctl_version(uint64_t arg) {
  struct drm_version_u v;
  int rc = drm_get_user(arg, &v, sizeof(v));
  if (rc)
    return rc;
  v.major = 1;
  v.minor = 0;
  v.patch = 0;
  if ((rc = put_string(v.name, &v.name_len, "aurora")) ||
      (rc = put_string(v.date, &v.date_len, "20260101")) ||
      (rc = put_string(v.desc, &v.desc_len,
                       gpup_adapter() ? gpup_adapter()->name
                                      : "AuroraOS DRM/KMS")))
    return rc;
  return drm_put_user(arg, &v, sizeof(v));
}

static int ioctl_get_cap(uint64_t arg) {
  struct drm_get_cap_u c;
  int rc = drm_get_user(arg, &c, sizeof(c));
  if (rc)
    return rc;
  switch (c.capability) {
  case 0x1: c.value = 1; break;   // DUMB_BUFFER
  case 0x2: c.value = 1; break;   // VBLANK_HIGH_CRTC
  case 0x3: c.value = 24; break;  // DUMB_PREFERRED_DEPTH
  case 0x4: c.value = 0; break;   // DUMB_PREFER_SHADOW
  case 0x5: c.value = 3; break;   // PRIME: import | export
  case 0x6: c.value = 1; break;   // TIMESTAMP_MONOTONIC
  case 0x7: c.value = 0; break;   // ASYNC_PAGE_FLIP
  case 0x8: c.value = 64; break;  // CURSOR_WIDTH
  case 0x9: c.value = 64; break;  // CURSOR_HEIGHT
  case 0x10: c.value = 0; break;  // ADDFB2_MODIFIERS
  case 0x11: c.value = 0; break;  // PAGE_FLIP_TARGET
  case 0x12: c.value = 1; break;  // CRTC_IN_VBLANK_EVENT
  case 0x13: c.value = 1; break;  // SYNCOBJ
  case 0x14: c.value = 1; break;  // SYNCOBJ_TIMELINE
  default: return -EINVAL;
  }
  return drm_put_user(arg, &c, sizeof(c));
}

static int ioctl_set_client_cap(drm_file_t *f, uint64_t arg) {
  struct drm_get_cap_u c;
  int rc = drm_get_user(arg, &c, sizeof(c));
  if (rc)
    return rc;
  if (c.value > 1)
    return -EINVAL;
  switch (c.capability) {
  case 2:
    f->universal_planes = c.value != 0;
    return 0;
  case 3:
    if (c.value && f->render)
      return -EACCES;
    f->atomic = c.value != 0;
    if (f->atomic)
      f->universal_planes = true;
    return 0;
  default:
    return -EINVAL;
  }
}

// ---------------------------------------------------------------------------
// Recursos / CRTC / encoder / conector
// ---------------------------------------------------------------------------
static int put_u32_array(uint64_t ptr, uint32_t user_count, const uint32_t *v,
                         uint32_t n) {
  if (!ptr || user_count < n || n == 0)
    return 0;
  if (!access_ok((void *)ptr, n * 4) || copy_to_user((void *)ptr, v, n * 4) < 0)
    return -EFAULT;
  return 0;
}

static int ioctl_get_resources(drm_file_t *f, uint64_t arg) {
  struct mode_card_res r;
  int rc = drm_get_user(arg, &r, sizeof(r));
  if (rc)
    return rc;
  uint32_t fbs[32];
  uint32_t nfb = 0;
  for (drm_fb_t *fb = g_fbs; fb && nfb < 32; fb = fb->next)
    if (fb->owner == f)
      fbs[nfb++] = fb->id;
  uint32_t crtc = DRM_CRTC_ID, conn = DRM_CONNECTOR_ID, enc = DRM_ENCODER_ID;
  if ((rc = put_u32_array(r.fb_id_ptr, r.count_fbs, fbs, nfb)) ||
      (rc = put_u32_array(r.crtc_id_ptr, r.count_crtcs, &crtc, 1)) ||
      (rc = put_u32_array(r.connector_id_ptr, r.count_connectors, &conn, 1)) ||
      (rc = put_u32_array(r.encoder_id_ptr, r.count_encoders, &enc, 1)))
    return rc;
  r.count_fbs = nfb;
  r.count_crtcs = r.count_connectors = r.count_encoders = 1;
  r.min_width = 64;
  r.min_height = 64;
  r.max_width = 4096;
  r.max_height = 4096;
  return drm_put_user(arg, &r, sizeof(r));
}

static void crtc_to_user(struct mode_crtc *c) {
  c->crtc_id = DRM_CRTC_ID;
  c->fb_id = g_crtc.fb ? g_crtc.fb->id : 0;
  c->x = g_crtc.x;
  c->y = g_crtc.y;
  c->gamma_size = 0;
  c->mode_valid = g_crtc.mode_valid && g_crtc.active;
  if (c->mode_valid)
    c->mode = g_crtc.mode;
  else
    memset(&c->mode, 0, sizeof(c->mode));
}

static int ioctl_get_crtc(uint64_t arg) {
  struct mode_crtc c;
  int rc = drm_get_user(arg, &c, sizeof(c));
  if (rc)
    return rc;
  if (c.crtc_id != DRM_CRTC_ID)
    return -ENOENT;
  crtc_to_user(&c);
  return drm_put_user(arg, &c, sizeof(c));
}

static int ioctl_set_crtc(drm_file_t *f, uint64_t arg) {
  if (!f->master)
    return -EACCES;
  struct mode_crtc c;
  int rc = drm_get_user(arg, &c, sizeof(c));
  if (rc)
    return rc;
  if (c.crtc_id != DRM_CRTC_ID)
    return -ENOENT;
  if (c.fb_id == 0 || !c.mode_valid)
    return crtc_commit(NULL, 0, 0, NULL, false);
  drm_fb_t *fb = fb_find(c.fb_id);
  if (!fb)
    return -ENOENT;
  if (c.count_connectors != 1 || !c.set_connectors_ptr)
    return -EINVAL;
  uint32_t conn;
  if ((rc = drm_get_user(c.set_connectors_ptr, &conn, 4)))
    return rc;
  if (conn != DRM_CONNECTOR_ID)
    return -ENOENT;
  if ((rc = mode_check(&c.mode)))
    return rc;
  if ((uint64_t)c.x + c.mode.hdisplay > fb->width ||
      (uint64_t)c.y + c.mode.vdisplay > fb->height)
    return -ENOSPC;
  return crtc_commit(fb, c.x, c.y, &c.mode, true);
}

static int ioctl_get_encoder(uint64_t arg) {
  struct mode_get_encoder e;
  int rc = drm_get_user(arg, &e, sizeof(e));
  if (rc)
    return rc;
  if (e.encoder_id != DRM_ENCODER_ID)
    return -ENOENT;
  e.encoder_type = 5; // DRM_MODE_ENCODER_VIRTUAL
  e.crtc_id = g_crtc.active ? DRM_CRTC_ID : 0;
  e.possible_crtcs = 1;
  e.possible_clones = 0;
  return drm_put_user(arg, &e, sizeof(e));
}

static uint64_t prop_value(uint32_t pid) {
  drm_fb_t *fb = g_crtc.fb;
  bool act = g_crtc.active;
  switch (pid) {
  case P_PLANE_TYPE: return 1;
  case P_PLANE_FB_ID: return fb ? fb->id : 0;
  case P_PLANE_CRTC_ID: return act ? DRM_CRTC_ID : 0;
  case P_PLANE_CRTC_X: case P_PLANE_CRTC_Y: return 0;
  case P_PLANE_CRTC_W: return act ? g_crtc.mode.hdisplay : 0;
  case P_PLANE_CRTC_H: return act ? g_crtc.mode.vdisplay : 0;
  case P_PLANE_SRC_X: return (uint64_t)g_crtc.x << 16;
  case P_PLANE_SRC_Y: return (uint64_t)g_crtc.y << 16;
  case P_PLANE_SRC_W: return act ? (uint64_t)g_crtc.mode.hdisplay << 16 : 0;
  case P_PLANE_SRC_H: return act ? (uint64_t)g_crtc.mode.vdisplay << 16 : 0;
  case P_CRTC_ACTIVE: return act;
  case P_CRTC_MODE_ID: return act ? g_crtc.mode_blob : 0;
  case P_CONN_DPMS: return act ? 0 : 3;
  case P_CONN_CRTC_ID: return act ? DRM_CRTC_ID : 0;
  case P_CONN_EDID: return edid_blob() ? DRM_EDID_BLOB_ID : 0;
  }
  return 0;
}

static int ioctl_get_connector(uint64_t arg) {
  struct mode_get_connector c;
  int rc = drm_get_user(arg, &c, sizeof(c));
  if (rc)
    return rc;
  if (c.connector_id != DRM_CONNECTOR_ID)
    return -ENOENT;

  struct modeinfo modes[MAX_MODES];
  int nm = build_modes(modes);

  uint32_t user_modes = c.count_modes, user_props = c.count_props,
           user_enc = c.count_encoders;
  uint64_t mptr = c.modes_ptr, pptr = c.props_ptr, vptr = c.prop_values_ptr,
           eptr = c.encoders_ptr;

  if (mptr && user_modes >= (uint32_t)nm) {
    if (!access_ok((void *)mptr, nm * sizeof(modes[0])) ||
        copy_to_user((void *)mptr, modes, nm * sizeof(modes[0])) < 0)
      return -EFAULT;
  }
  uint32_t enc = DRM_ENCODER_ID;
  if ((rc = put_u32_array(eptr, user_enc, &enc, 1)))
    return rc;
  uint32_t np = sizeof(g_conn_props) / sizeof(g_conn_props[0]);
  if (pptr && vptr && user_props >= np) {
    uint64_t vals[8];
    for (uint32_t i = 0; i < np; i++)
      vals[i] = prop_value(g_conn_props[i]);
    if ((rc = put_u32_array(pptr, user_props, g_conn_props, np)))
      return rc;
    if (!access_ok((void *)vptr, np * 8) ||
        copy_to_user((void *)vptr, vals, np * 8) < 0)
      return -EFAULT;
  }

  c.count_modes = (uint32_t)nm;
  c.count_props = np;
  c.count_encoders = 1;
  c.encoder_id = g_crtc.active ? DRM_ENCODER_ID : 0;
  c.connector_type = 15; // DRM_MODE_CONNECTOR_VIRTUAL
  c.connector_type_id = 1;
  c.connection = 1; // connected
  int w, h;
  work_size(&w, &h);
  c.mm_width = (uint32_t)w * 254 / 960; // 96 dpi por defecto
  c.mm_height = (uint32_t)h * 254 / 960;
  const struct edid_info *ei = display_edid_info();
  if (ei && ei->width_mm && ei->height_mm) { // tamano fisico real del monitor
    c.mm_width = ei->width_mm;
    c.mm_height = ei->height_mm;
  }
  c.subpixel = 1; // unknown
  c.pad = 0;
  return drm_put_user(arg, &c, sizeof(c));
}

// ---------------------------------------------------------------------------
// Propiedades
// ---------------------------------------------------------------------------
static const prop_def_t *prop_find(uint32_t id) {
  for (int i = 0; i < NPROPS; i++)
    if (g_props[i].id == id)
      return &g_props[i];
  return NULL;
}

static int ioctl_get_property(uint64_t arg) {
  struct mode_get_property p;
  int rc = drm_get_user(arg, &p, sizeof(p));
  if (rc)
    return rc;
  const prop_def_t *d = prop_find(p.prop_id);
  if (!d)
    return -ENOENT;
  uint32_t user_values = p.count_values, user_enums = p.count_enum_blobs;
  uint64_t vptr = p.values_ptr, eptr = p.enum_blob_ptr;
  p.flags = d->flags;
  memset(p.name, 0, sizeof(p.name));
  strncpy(p.name, d->name, sizeof(p.name) - 1);

  uint32_t nvals = 0, nenum = 0;
  uint64_t vals[4];
  struct prop_enum_u en[4];
  if (d->flags & PROP_ENUM) {
    const void *tab = d->id == P_PLANE_TYPE ? (const void *)g_plane_type_enum
                                            : (const void *)g_dpms_enum;
    nenum = d->id == P_PLANE_TYPE ? 3 : 4;
    const struct {
      uint64_t v;
      const char *n;
    } *t = tab;
    for (uint32_t i = 0; i < nenum; i++) {
      memset(&en[i], 0, sizeof(en[i]));
      en[i].value = t[i].v;
      strncpy(en[i].name, t[i].n, sizeof(en[i].name) - 1);
    }
  } else if (d->flags & (PROP_RANGE | PROP_SIGNED_RANGE)) {
    vals[0] = (uint64_t)d->min;
    vals[1] = (uint64_t)d->max;
    nvals = 2;
  } else if (d->flags & PROP_OBJECT) {
    vals[0] = d->obj_type;
    nvals = 1;
  }
  if (nvals && vptr && user_values >= nvals) {
    if (!access_ok((void *)vptr, nvals * 8) ||
        copy_to_user((void *)vptr, vals, nvals * 8) < 0)
      return -EFAULT;
  }
  if (nenum && eptr && user_enums >= nenum) {
    if (!access_ok((void *)eptr, nenum * sizeof(en[0])) ||
        copy_to_user((void *)eptr, en, nenum * sizeof(en[0])) < 0)
      return -EFAULT;
  }
  p.count_values = nvals;
  p.count_enum_blobs = nenum;
  return drm_put_user(arg, &p, sizeof(p));
}

static const uint32_t *obj_props(uint32_t obj_type, uint32_t obj_id,
                                 uint32_t *n) {
  if (obj_type == OBJ_PLANE && obj_id == DRM_PLANE_ID) {
    *n = sizeof(g_plane_props) / 4;
    return g_plane_props;
  }
  if (obj_type == OBJ_CRTC && obj_id == DRM_CRTC_ID) {
    *n = sizeof(g_crtc_props) / 4;
    return g_crtc_props;
  }
  if (obj_type == OBJ_CONNECTOR && obj_id == DRM_CONNECTOR_ID) {
    *n = sizeof(g_conn_props) / 4;
    return g_conn_props;
  }
  *n = 0;
  return NULL;
}

static int ioctl_obj_get_properties(uint64_t arg) {
  struct mode_obj_get_properties o;
  int rc = drm_get_user(arg, &o, sizeof(o));
  if (rc)
    return rc;
  uint32_t n;
  const uint32_t *ids = obj_props(o.obj_type, o.obj_id, &n);
  if (!ids)
    return -ENOENT;
  if (o.props_ptr && o.prop_values_ptr && o.count_props >= n) {
    uint64_t vals[16];
    for (uint32_t i = 0; i < n; i++)
      vals[i] = prop_value(ids[i]);
    if ((rc = put_u32_array(o.props_ptr, o.count_props, ids, n)))
      return rc;
    if (!access_ok((void *)o.prop_values_ptr, n * 8) ||
        copy_to_user((void *)o.prop_values_ptr, vals, n * 8) < 0)
      return -EFAULT;
  }
  o.count_props = n;
  return drm_put_user(arg, &o, sizeof(o));
}

// ---------------------------------------------------------------------------
// Planos
// ---------------------------------------------------------------------------
static int ioctl_get_plane_res(drm_file_t *f, uint64_t arg) {
  struct mode_get_plane_res r;
  int rc = drm_get_user(arg, &r, sizeof(r));
  if (rc)
    return rc;
  uint32_t n = f->universal_planes ? 1 : 0;
  uint32_t id = DRM_PLANE_ID;
  if ((rc = put_u32_array(r.plane_id_ptr, r.count_planes, &id, n)))
    return rc;
  r.count_planes = n;
  return drm_put_user(arg, &r, sizeof(r));
}

static int ioctl_get_plane(uint64_t arg) {
  struct mode_get_plane p;
  int rc = drm_get_user(arg, &p, sizeof(p));
  if (rc)
    return rc;
  if (p.plane_id != DRM_PLANE_ID)
    return -ENOENT;
  static const uint32_t fmts[2] = {DRM_FORMAT_XRGB8888, DRM_FORMAT_ARGB8888};
  if ((rc = put_u32_array(p.format_type_ptr, p.count_format_types, fmts, 2)))
    return rc;
  p.count_format_types = 2;
  p.crtc_id = g_crtc.active ? DRM_CRTC_ID : 0;
  p.fb_id = g_crtc.fb ? g_crtc.fb->id : 0;
  p.possible_crtcs = 1;
  p.gamma_size = 0;
  return drm_put_user(arg, &p, sizeof(p));
}

static int ioctl_set_plane(drm_file_t *f, uint64_t arg) {
  if (!f->master)
    return -EACCES;
  struct mode_set_plane s;
  int rc = drm_get_user(arg, &s, sizeof(s));
  if (rc)
    return rc;
  if (s.plane_id != DRM_PLANE_ID)
    return -ENOENT;
  if (s.fb_id == 0 || s.crtc_id == 0)
    return crtc_commit(NULL, 0, 0, NULL, false);
  if (s.crtc_id != DRM_CRTC_ID)
    return -ENOENT;
  drm_fb_t *fb = fb_find(s.fb_id);
  if (!fb)
    return -ENOENT;
  if (!g_crtc.mode_valid)
    return -EINVAL;
  uint32_t sx = s.src_x >> 16, sy = s.src_y >> 16;
  if ((uint64_t)sx + g_crtc.mode.hdisplay > fb->width ||
      (uint64_t)sy + g_crtc.mode.vdisplay > fb->height)
    return -ENOSPC;
  rc = crtc_commit(fb, sx, sy, &g_crtc.mode, true);
  if (!rc)
    mark_flip();
  return rc;
}

// ---------------------------------------------------------------------------
// Framebuffers: ioctls
// ---------------------------------------------------------------------------
static int ioctl_add_fb(drm_file_t *f, uint64_t arg) {
  struct mode_fb_cmd a;
  int rc = drm_get_user(arg, &a, sizeof(a));
  if (rc)
    return rc;
  if (a.bpp != 32 || (a.depth != 24 && a.depth != 32))
    return -EINVAL;
  uint32_t id;
  rc = fb_create(f, a.width, a.height, a.pitch,
                 a.depth == 24 ? DRM_FORMAT_XRGB8888 : DRM_FORMAT_ARGB8888, 0,
                 a.handle, &id);
  if (rc)
    return rc;
  a.fb_id = id;
  return drm_put_user(arg, &a, sizeof(a));
}

static int ioctl_add_fb2(drm_file_t *f, uint64_t arg) {
  struct mode_fb_cmd2 a;
  int rc = drm_get_user(arg, &a, sizeof(a));
  if (rc)
    return rc;
  if (a.flags & ~0x3u) // FB_INTERLACED | FB_MODIFIERS
    return -EINVAL;
  if (a.handles[1] || a.handles[2] || a.handles[3])
    return -EINVAL;
  uint32_t id;
  rc = fb_create(f, a.width, a.height, a.pitches[0], a.pixel_format,
                 a.offsets[0], a.handles[0], &id);
  if (rc)
    return rc;
  a.fb_id = id;
  return drm_put_user(arg, &a, sizeof(a));
}

static int ioctl_rm_fb(drm_file_t *f, uint64_t arg) {
  uint32_t id;
  int rc = drm_get_user(arg, &id, sizeof(id));
  if (rc)
    return rc;
  drm_fb_t *fb = fb_find(id);
  if (!fb || fb->owner != f)
    return -ENOENT;
  // Linux apaga el CRTC si estaba mostrando este fb.
  if (g_crtc.fb == fb)
    crtc_commit(NULL, 0, 0, NULL, false);
  fb_unlink(fb);
  drm_fb_put(fb);
  return 0;
}

static int ioctl_dirty_fb(drm_file_t *f, uint64_t arg) {
  (void)f;
  struct mode_fb_dirty_cmd d;
  int rc = drm_get_user(arg, &d, sizeof(d));
  if (rc)
    return rc;
  drm_fb_t *fb = fb_find(d.fb_id);
  if (!fb)
    return -ENOENT;
  if (g_crtc.fb == fb)
    scanout_present();
  return 0;
}

static int ioctl_page_flip(drm_file_t *f, uint64_t arg) {
  if (!f->master)
    return -EACCES;
  struct mode_crtc_page_flip p;
  int rc = drm_get_user(arg, &p, sizeof(p));
  if (rc)
    return rc;
  if (p.crtc_id != DRM_CRTC_ID)
    return -ENOENT;
  if (p.flags & ~(uint32_t)(PAGE_FLIP_EVENT | 0x02))
    return -EINVAL;
  if (!g_crtc.active)
    return -EINVAL;
  drm_fb_t *fb = fb_find(p.fb_id);
  if (!fb)
    return -ENOENT;
  if ((uint64_t)g_crtc.x + g_crtc.mode.hdisplay > fb->width ||
      (uint64_t)g_crtc.y + g_crtc.mode.vdisplay > fb->height)
    return -EINVAL;
  if (g_flip_busy)
    return -EBUSY;
  rc = crtc_commit(fb, g_crtc.x, g_crtc.y, &g_crtc.mode, true);
  if (rc)
    return rc;
  if (p.flags & PAGE_FLIP_EVENT) {
    rc = queue_event(f, DRM_EVENT_FLIP_COMPLETE, p.user_data);
    if (rc)
      return rc;
  }
  mark_flip();
  return 0;
}

// ---------------------------------------------------------------------------
// Atomic
// ---------------------------------------------------------------------------
typedef struct {
  uint32_t fb_id, plane_crtc;
  uint32_t src_x, src_y, src_w, src_h; // 16.16
  int32_t crtc_x, crtc_y;
  uint32_t crtc_w, crtc_h;
  bool active;
  bool mode_valid;
  struct modeinfo mode;
  uint32_t mode_blob;
  uint32_t conn_crtc;
  bool touched_mode;
} atomic_state_t;

#define ATOMIC_MAX_OBJS 8
#define ATOMIC_MAX_PROPS 64

static int atomic_set(atomic_state_t *s, uint32_t obj, uint32_t prop,
                      uint64_t val) {
  if (obj == DRM_PLANE_ID) {
    switch (prop) {
    case P_PLANE_FB_ID: s->fb_id = (uint32_t)val; return 0;
    case P_PLANE_CRTC_ID: s->plane_crtc = (uint32_t)val; return 0;
    case P_PLANE_CRTC_X: s->crtc_x = (int32_t)val; return 0;
    case P_PLANE_CRTC_Y: s->crtc_y = (int32_t)val; return 0;
    case P_PLANE_CRTC_W: s->crtc_w = (uint32_t)val; return 0;
    case P_PLANE_CRTC_H: s->crtc_h = (uint32_t)val; return 0;
    case P_PLANE_SRC_X: s->src_x = (uint32_t)val; return 0;
    case P_PLANE_SRC_Y: s->src_y = (uint32_t)val; return 0;
    case P_PLANE_SRC_W: s->src_w = (uint32_t)val; return 0;
    case P_PLANE_SRC_H: s->src_h = (uint32_t)val; return 0;
    case P_PLANE_TYPE: return -EINVAL; // inmutable
    }
    return -EINVAL;
  }
  if (obj == DRM_CRTC_ID) {
    if (prop == P_CRTC_ACTIVE) {
      if (val > 1)
        return -EINVAL;
      s->active = val != 0;
      return 0;
    }
    if (prop == P_CRTC_MODE_ID) {
      if (val == 0) {
        s->mode_valid = false;
        s->mode_blob = 0;
      } else {
        drm_blob_t *b = blob_find((uint32_t)val);
        if (!b || b->len != sizeof(struct modeinfo))
          return -EINVAL;
        memcpy(&s->mode, b->data, sizeof(s->mode));
        s->mode_valid = true;
        s->mode_blob = (uint32_t)val;
      }
      s->touched_mode = true;
      return 0;
    }
    return -EINVAL;
  }
  if (obj == DRM_CONNECTOR_ID) {
    if (prop == P_CONN_CRTC_ID) {
      if (val != 0 && val != DRM_CRTC_ID)
        return -ENOENT;
      s->conn_crtc = (uint32_t)val;
      return 0;
    }
    if (prop == P_CONN_DPMS)
      return 0;
    return -EINVAL;
  }
  return -ENOENT;
}

static int ioctl_atomic(drm_file_t *f, uint64_t arg) {
  if (!f->atomic)
    return -EINVAL;
  if (!f->master)
    return -EACCES;
  struct mode_atomic a;
  int rc = drm_get_user(arg, &a, sizeof(a));
  if (rc)
    return rc;
  if ((a.flags & ~(uint32_t)ATOMIC_FLAGS_VALID) || a.reserved)
    return -EINVAL;
  if (a.count_objs > ATOMIC_MAX_OBJS)
    return -EINVAL;
  if ((a.flags & PAGE_FLIP_EVENT) && (a.flags & ATOMIC_TEST_ONLY))
    return -EINVAL;

  uint32_t objs[ATOMIC_MAX_OBJS], counts[ATOMIC_MAX_OBJS];
  uint32_t total = 0;
  if (a.count_objs) {
    if ((rc = drm_get_user(a.objs_ptr, objs, a.count_objs * 4)) ||
        (rc = drm_get_user(a.count_props_ptr, counts, a.count_objs * 4)))
      return rc;
    for (uint32_t i = 0; i < a.count_objs; i++) {
      total += counts[i];
      if (total > ATOMIC_MAX_PROPS)
        return -EINVAL;
    }
  }
  uint32_t pids[ATOMIC_MAX_PROPS];
  uint64_t pvals[ATOMIC_MAX_PROPS];
  if (total) {
    if ((rc = drm_get_user(a.props_ptr, pids, total * 4)) ||
        (rc = drm_get_user(a.prop_values_ptr, pvals, total * 8)))
      return rc;
  }

  atomic_state_t *s = (atomic_state_t *)kzalloc(sizeof(*s));
  if (!s)
    return -ENOMEM;
  s->fb_id = g_crtc.fb ? g_crtc.fb->id : 0;
  s->plane_crtc = g_crtc.active ? DRM_CRTC_ID : 0;
  s->active = g_crtc.active;
  s->mode_valid = g_crtc.mode_valid;
  s->mode = g_crtc.mode;
  s->mode_blob = g_crtc.mode_blob;
  s->conn_crtc = g_crtc.active ? DRM_CRTC_ID : 0;
  s->src_x = g_crtc.x << 16;
  s->src_y = g_crtc.y << 16;
  s->src_w = g_crtc.mode.hdisplay << 16;
  s->src_h = g_crtc.mode.vdisplay << 16;
  s->crtc_w = g_crtc.mode.hdisplay;
  s->crtc_h = g_crtc.mode.vdisplay;

  uint32_t k = 0;
  for (uint32_t i = 0; i < a.count_objs && !rc; i++)
    for (uint32_t j = 0; j < counts[i] && !rc; j++, k++)
      rc = atomic_set(s, objs[i], pids[k], pvals[k]);

  drm_fb_t *fb = NULL;
  if (!rc && s->active) {
    // Estado coherente para una salida encendida.
    if (!s->mode_valid || s->fb_id == 0 || s->plane_crtc != DRM_CRTC_ID ||
        s->conn_crtc != DRM_CRTC_ID)
      rc = -EINVAL;
    else if (mode_check(&s->mode) != 0)
      rc = -EINVAL;
    else if (!(fb = fb_find(s->fb_id)))
      rc = -ENOENT;
    else if (s->crtc_x != 0 || s->crtc_y != 0 ||
             (s->crtc_w && s->crtc_w != s->mode.hdisplay) ||
             (s->crtc_h && s->crtc_h != s->mode.vdisplay))
      rc = -EINVAL; // sin escalado ni desplazamiento
    else if ((uint64_t)(s->src_x >> 16) + s->mode.hdisplay > fb->width ||
             (uint64_t)(s->src_y >> 16) + s->mode.vdisplay > fb->height)
      rc = -ENOSPC;
  } else if (!rc && !s->active && (s->plane_crtc || s->conn_crtc)) {
    // Linux exige quitar tambien el plano/conector al apagar el CRTC.
    if (s->fb_id && s->plane_crtc)
      rc = -EINVAL;
  }
  if (!rc && (a.flags & ATOMIC_TEST_ONLY)) {
    kfree(s);
    return 0;
  }
  if (!rc) {
    bool flip_only = g_crtc.active && s->active && g_crtc.fb != fb &&
                     g_crtc.mode.hdisplay == s->mode.hdisplay &&
                     g_crtc.mode.vdisplay == s->mode.vdisplay;
    if (flip_only && g_flip_busy && (a.flags & ATOMIC_NONBLOCK)) {
      rc = -EBUSY;
    } else {
      rc = crtc_commit(fb, s->src_x >> 16, s->src_y >> 16, &s->mode,
                       s->active);
      if (!rc) {
        g_crtc.mode_blob = s->mode_blob;
        if (s->active) {
          mark_flip();
          if (a.flags & PAGE_FLIP_EVENT)
            rc = queue_event(f, DRM_EVENT_FLIP_COMPLETE, a.user_data);
        }
      }
    }
  }
  kfree(s);
  return rc;
}

// ---------------------------------------------------------------------------
// WAIT_VBLANK
// ---------------------------------------------------------------------------
static bool vbl_reached(void *a) { return (int32_t)(g_vbl_seq - *(uint32_t *)a) >= 0; }

static int ioctl_wait_vblank(drm_file_t *f, uint64_t arg) {
  struct wait_vblank_u v;
  int rc = drm_get_user(arg, &v, sizeof(v));
  if (rc)
    return rc;
  uint32_t cur = g_vbl_seq;
  uint32_t target = (v.type & 1) ? cur + v.sequence : v.sequence;
  if (v.type & 0x4000000u) { // _DRM_VBLANK_EVENT
    rc = queue_event(f, DRM_EVENT_VBLANK, v.signal_or_tval_sec);
    return rc;
  }
  if (v.sequence != 0 || !(v.type & 1)) {
    if (!vbl_reached(&target)) {
      g_vbl_waiters++;
      wake_up_all(&g_kick_wq);
      long w = wait_event_interruptible_timeout(&g_vbl_wq, vbl_reached, &target,
                                                1000);
      g_vbl_waiters--;
      if (w < 0)
        return (int)w;
    }
  }
  uint64_t ms = tick_count;
  v.sequence = g_vbl_seq;
  v.signal_or_tval_sec = ms / 1000;
  v.tval_usec = (ms % 1000) * 1000;
  return drm_put_user(arg, &v, sizeof(v));
}

// ---------------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------------
static int64_t drm_ioctl_locked(drm_file_t *f, unsigned long req, uint64_t arg) {
  if (((req >> 8) & 0xFF) != 'd')
    return -ENOTTY;
  unsigned nr = req & 0xFF;

  // Ioctls permitidos en nodos render.
  switch (nr) {
  case 0x00: return ioctl_version(arg);
  case 0x01: { // GET_UNIQUE
    struct drm_unique_u u;
    int rc = drm_get_user(arg, &u, sizeof(u));
    if (rc)
      return rc;
    u.len = 0;
    return drm_put_user(arg, &u, sizeof(u));
  }
  case 0x02: { // GET_MAGIC
    struct drm_auth_u au = {.magic = ++g_magic_ctr};
    return drm_put_user(arg, &au, sizeof(au));
  }
  case 0x07: { // SET_VERSION
    struct drm_set_version_u sv;
    int rc = drm_get_user(arg, &sv, sizeof(sv));
    if (rc)
      return rc;
    sv.di_major = 1;
    sv.di_minor = 4;
    sv.dd_major = 1;
    sv.dd_minor = 0;
    return drm_put_user(arg, &sv, sizeof(sv));
  }
  case 0x09: { // GEM_CLOSE
    struct drm_gem_close_u gc;
    int rc = drm_get_user(arg, &gc, sizeof(gc));
    if (rc)
      return rc;
    return drm_handle_close(f, gc.handle);
  }
  case 0x0c: return ioctl_get_cap(arg);
  case 0x0d: return ioctl_set_client_cap(f, arg);
  case 0x11: return 0; // AUTH_MAGIC: sin autenticacion
  case 0x2d: return drm_ioctl_prime_handle_to_fd(f, arg);
  case 0x2e: return drm_ioctl_prime_fd_to_handle(f, arg);
  case 0xB2: return drm_ioctl_create_dumb(f, arg);
  case 0xB3: return drm_ioctl_map_dumb(f, arg);
  case 0xB4: return drm_ioctl_destroy_dumb(f, arg);
  }

  if (f->render)
    return -EACCES;

  switch (nr) {
  case 0x0a: return drm_ioctl_gem_flink(f, arg);
  case 0x0b: return drm_ioctl_gem_open(f, arg);
  case 0x1e: // SET_MASTER
    if (g_master == f)
      return 0;
    if (g_master)
      return -EBUSY;
    g_master = f;
    f->master = true;
    return 0;
  case 0x1f: // DROP_MASTER
    if (g_master != f)
      return -EINVAL;
    g_master = NULL;
    f->master = false;
    return 0;
  case 0xA0: return ioctl_get_resources(f, arg);
  case 0xA1: return ioctl_get_crtc(arg);
  case 0xA2: return ioctl_set_crtc(f, arg);
  case 0xA3: case 0xBB: return -ENXIO; // sin plano de cursor
  case 0xA4: case 0xA5: return -EINVAL; // gamma_size = 0
  case 0xA6: return ioctl_get_encoder(arg);
  case 0xA7: return ioctl_get_connector(arg);
  case 0xAA: return ioctl_get_property(arg);
  case 0xAB: { // SETPROPERTY (conector, legacy): solo DPMS
    struct mode_connector_set_property sp;
    int rc = drm_get_user(arg, &sp, sizeof(sp));
    if (rc)
      return rc;
    if (sp.connector_id != DRM_CONNECTOR_ID)
      return -ENOENT;
    return sp.prop_id == P_CONN_DPMS ? 0 : -EINVAL;
  }
  case 0xAC: return ioctl_get_blob(arg);
  case 0xAD: return -EINVAL; // GETFB: no soportado
  case 0xAE: return ioctl_add_fb(f, arg);
  case 0xAF: return ioctl_rm_fb(f, arg);
  case 0xB0: return ioctl_page_flip(f, arg);
  case 0xB1: return ioctl_dirty_fb(f, arg);
  case 0xB5: return ioctl_get_plane_res(f, arg);
  case 0xB6: return ioctl_get_plane(arg);
  case 0xB7: return ioctl_set_plane(f, arg);
  case 0xB8: return ioctl_add_fb2(f, arg);
  case 0xB9: return ioctl_obj_get_properties(arg);
  case 0xBA: { // OBJ_SETPROPERTY: solo DPMS
    struct mode_obj_set_property sp;
    int rc = drm_get_user(arg, &sp, sizeof(sp));
    if (rc)
      return rc;
    return (sp.obj_type == OBJ_CONNECTOR && sp.prop_id == P_CONN_DPMS) ? 0
                                                                       : -EINVAL;
  }
  case 0xBC: return ioctl_atomic(f, arg);
  case 0xBD: return ioctl_create_blob(f, arg);
  case 0xBE: return ioctl_destroy_blob(arg);
  }
  return -ENOTTY;
}

static drm_file_t *node_file(vfs_node_t *node) {
  drm_file_t *f = (drm_file_t *)node->priv;
  return (f && f->magic == DRM_FILE_MAGIC) ? f : NULL;
}

static int64_t drm_ioctl(vfs_node_t *node, unsigned long req, uint64_t arg) {
  drm_file_t *f = node_file(node);
  if (!f)
    return -ENODEV;
  if (!f->render && (req & 0xFF) == 0x3a && ((req >> 8) & 0xFF) == 'd')
    return ioctl_wait_vblank(f, arg); // bloquea: no retiene g_drm_lock
  if (((req >> 8) & 0xFF) == 'd' && (req & 0xFF) >= 0xBF && (req & 0xFF) <= 0xCD)
    return drm_syncobj_ioctl(f, req, arg); // SYNCOBJ_*: bloquea, sin g_drm_lock
  mutex_lock(&g_drm_lock);
  int64_t rc = drm_ioctl_locked(f, req, arg);
  mutex_unlock(&g_drm_lock);
  return rc;
}

// ---------------------------------------------------------------------------
// open / close / read / poll / mmap
// ---------------------------------------------------------------------------
typedef struct {
  bool render;
} drm_node_priv_t;
static drm_node_priv_t g_card_priv = {false}, g_render_priv = {true};

static int drm_open(vfs_node_t *node, int flags) {
  drm_node_priv_t *np = (drm_node_priv_t *)node->priv;
  if (!np || (np != &g_card_priv && np != &g_render_priv))
    return -ENODEV;
  drm_file_t *f = (drm_file_t *)kzalloc(sizeof(*f));
  if (!f)
    return -ENOMEM;
  f->magic = DRM_FILE_MAGIC;
  f->render = np->render;
  f->nonblock = (flags & O_NONBLOCK_L) != 0;
  wait_queue_init(&f->wq);
  mutex_lock(&g_drm_lock);
  if (!f->render && !g_master) { // el primero en abrir card0 es master
    g_master = f;
    f->master = true;
  }
  unsigned long fl = spin_lock_irqsave(&g_ev_lock);
  f->next = g_files;
  g_files = f;
  spin_unlock_irqrestore(&g_ev_lock, fl);
  mutex_unlock(&g_drm_lock);
  node->priv = f;
  return 0;
}

static int drm_close(vfs_node_t *node) {
  drm_file_t *f = node_file(node);
  if (!f)
    return 0;
  mutex_lock(&g_drm_lock);
  // Fbs del fichero: si el CRTC muestra uno, se apaga la salida.
  drm_fb_t *fb = g_fbs;
  while (fb) {
    drm_fb_t *nx = fb->next;
    if (fb->owner == f) {
      if (g_crtc.fb == fb)
        crtc_commit(NULL, 0, 0, NULL, false);
      fb_unlink(fb);
      drm_fb_put(fb);
    }
    fb = nx;
  }
  for (int i = 0; i < MAX_BLOBS; i++)
    if (g_blobs[i].data && g_blobs[i].owner == f)
      blob_free(&g_blobs[i]);
  unsigned long fl = spin_lock_irqsave(&g_ev_lock);
  for (int i = 0; i < MAX_PEND; i++)
    if (g_pend[i].used && g_pend[i].f == f)
      g_pend[i].used = false;
  drm_file_t **pp = &g_files;
  while (*pp && *pp != f)
    pp = &(*pp)->next;
  if (*pp)
    *pp = f->next;
  spin_unlock_irqrestore(&g_ev_lock, fl);
  if (g_master == f)
    g_master = NULL;
  drm_file_release_gems(f);
  drm_file_release_syncobjs(f);
  mutex_unlock(&g_drm_lock);
  node->priv = NULL;
  f->magic = 0;
  kfree(f);
  return 0;
}

static bool ev_ready(void *a) {
  drm_file_t *f = (drm_file_t *)a;
  return f->head != f->tail;
}

static int64_t drm_read(vfs_node_t *node, uint64_t off, size_t size,
                        void *buf) {
  (void)off;
  drm_file_t *f = node_file(node);
  if (!f)
    return -ENODEV;
  if (size < sizeof(drm_event_rec_t))
    return -EINVAL;
  for (;;) {
    drm_event_rec_t tmp[8];
    size_t n = 0, max = size / sizeof(drm_event_rec_t);
    if (max > 8)
      max = 8;
    unsigned long fl = spin_lock_irqsave(&g_ev_lock);
    while (n < max && f->head != f->tail)
      tmp[n++] = f->ring[f->tail++ % DRM_EVENT_RING];
    spin_unlock_irqrestore(&g_ev_lock, fl);
    if (n) {
      memcpy(buf, tmp, n * sizeof(tmp[0]));
      return (int64_t)(n * sizeof(tmp[0]));
    }
    if (f->nonblock)
      return -EAGAIN;
    int rc = wait_event_interruptible(&f->wq, ev_ready, f);
    if (rc < 0)
      return rc;
  }
}

static int drm_poll(vfs_node_t *node, short events) {
  drm_file_t *f = node_file(node);
  if (!f)
    return 0;
  return ((events & 1) && f->head != f->tail) ? 1 : 0;
}

static wait_queue_t *drm_poll_wq(vfs_node_t *node) {
  drm_file_t *f = node_file(node);
  return f ? &f->wq : NULL;
}

static int drm_get_page_op(vfs_node_t *node, uint64_t pgoff, uint64_t *phys) {
  drm_file_t *f = node_file(node);
  if (!f || !phys)
    return -EINVAL;
  return drm_gem_get_page(f, pgoff * PAGE_SIZE, phys);
}

static vfs_ops_t drm_ops = {
    .read = drm_read,
    .open = drm_open,
    .close = drm_close,
    .poll = drm_poll,
    .poll_wq = drm_poll_wq,
    .ioctl = drm_ioctl,
    .get_page = drm_get_page_op,
};



// Nodos de /dev/dri y /sys del backend nativo (el de Linux, lkpi_drm.kur, los
// releva mientras tenga algun minor: drm_backend_claim/release).
static void native_nodes_register(void) {
  devtmpfs_register("dri/card0", DEVT_MKDEV(DRM_MAJOR, DRM_CARD_MINOR), 0660, 0,
                    0, &drm_ops, &g_card_priv);
  devtmpfs_register("dri/renderD128",
                    DEVT_MKDEV(DRM_MAJOR, DRM_RENDER_MINOR), 0666, 0, 0,
                    &drm_ops, &g_render_priv);
  drm_sysfs_publish();
}

static int g_claims;
static bool g_native_down;

void drm_backend_claim(void) {
  mutex_lock(&g_drm_lock);
  if (g_claims++ == 0 && !g_native_down) {
    g_native_down = true;
    devtmpfs_unregister("dri/card0");
    devtmpfs_unregister("dri/renderD128");
    drm_sysfs_unpublish();
    scanout_close();
    LOG_INFO("[DRM] backend nativo cede /dev/dri al driver de Linux");
  }
  mutex_unlock(&g_drm_lock);
}

void drm_backend_release(void) {
  mutex_lock(&g_drm_lock);
  if (g_claims > 0 && --g_claims == 0 && g_native_down) {
    g_native_down = false;
    native_nodes_register();
    LOG_INFO("[DRM] backend nativo recupera /dev/dri");
  }
  mutex_unlock(&g_drm_lock);
}

void drm_init(void) {
  mutex_init(&g_drm_lock);
  wait_queue_init(&g_kick_wq);
  wait_queue_init(&g_tick_wq);
  wait_queue_init(&g_vbl_wq);
  native_nodes_register();
  kthread_run(vbl_thread, NULL, "drm-vblank");
  LOG_INFO("[DRM] /dev/dri/card0 y renderD128 listos (KMS atomic, dumb)");
}
