// kernel/drm/drm_linux_scanout.c
//
// Scanout del backend de Linux (lkpi_drm.kur): una ventana del compositor por
// CRTC. El driver de Linux hace su modeset y su GEM completos pero no
// programa la pantalla real (el compositor es dueño del GOP); tras cada commit
// la capa llama a drm_lx_scanout_present() con los píxeles del plano
// primario. Mismo criterio que el scanout nativo (drm_core.c), con varias
// salidas.

#include "drm.h"
#include "../uaccess.h"
#include "../gfx/compositor.h"
#include "../gfx/window.h"
#include "../klog.h"
#include "../mutex.h"
#include "../spinlock.h"
#include "../string.h"

#define LX_OUTPUTS 4

// DRM_FORMAT_* (fourcc)
#define LX_XRGB8888 0x34325258u
#define LX_ARGB8888 0x34325241u
#define LX_XBGR8888 0x34324258u
#define LX_ABGR8888 0x34324241u
#define LX_RGB565 0x36314752u

static struct {
  window_t *win;
  int w, h;
} g_out[LX_OUTPUTS];
static mutex_t g_lx_lock;
static bool g_lx_init;

static void lx_init(void) {
  if (!g_lx_init) {
    mutex_init(&g_lx_lock);
    g_lx_init = true;
  }
}

void drm_lx_scanout_close(int idx) {
  if (idx < 0 || idx >= LX_OUTPUTS)
    return;
  lx_init();
  mutex_lock(&g_lx_lock);
  if (g_out[idx].win) {
    compositor_close_window(g_out[idx].win);
    g_out[idx].win = NULL;
  }
  g_out[idx].w = g_out[idx].h = 0;
  mutex_unlock(&g_lx_lock);
}

int drm_lx_scanout_open(int idx, int w, int h) {
  if (idx < 0 || idx >= LX_OUTPUTS || w <= 0 || h <= 0)
    return -EINVAL;
  lx_init();
  mutex_lock(&g_lx_lock);
  if (g_out[idx].win && g_out[idx].w == w && g_out[idx].h == h) {
    mutex_unlock(&g_lx_lock);
    return 0;
  }
  if (g_out[idx].win) {
    compositor_close_window(g_out[idx].win);
    g_out[idx].win = NULL;
  }
  rect_t wa = compositor_work_area();
  int x = wa.x + (wa.w - w) / 2 + idx * 40;
  int y = wa.y + (wa.h - h) / 2 + idx * 40;
  if (x < 0)
    x = 0;
  if (y < wa.y)
    y = wa.y;
  window_t *win = compositor_create_window(
      x, y, w, h, "AuroraOS DRM", WIN_FLAGS_NODECORATION | WIN_FLAGS_FIXED_SIZE);
  if (!win) {
    mutex_unlock(&g_lx_lock);
    return -ENOMEM;
  }
  g_out[idx].win = win;
  g_out[idx].w = w;
  g_out[idx].h = h;
  if (idx == 0)
    compositor_focus_window(win);
  mutex_unlock(&g_lx_lock);
  return 0;
}

// Copia un fotograma (w x h, pitch bytes por fila) a la ventana de la salida.
int drm_lx_scanout_present(int idx, const void *src, uint32_t pitch,
                           uint32_t fourcc, int w, int h) {
  if (idx < 0 || idx >= LX_OUTPUTS || !src)
    return -EINVAL;
  lx_init();
  mutex_lock(&g_lx_lock);
  window_t *win = g_out[idx].win;
  if (!win) {
    mutex_unlock(&g_lx_lock);
    return -ENODEV;
  }
  unsigned long fl = spin_lock_irqsave(&win->content_lock);
  uint32_t *buf = win->content_buffer;
  if (buf) {
    int cw = win->content_w, ch = win->content_h;
    int rows = h < ch ? h : ch;
    int cols = w < cw ? w : cw;
    for (int y = 0; y < rows; y++) {
      uint32_t *dst = buf + (size_t)y * cw;
      const uint8_t *s = (const uint8_t *)src + (size_t)y * pitch;
      switch (fourcc) {
      case LX_XRGB8888:
        for (int x = 0; x < cols; x++)
          dst[x] = ((const uint32_t *)s)[x] | 0xFF000000u;
        break;
      case LX_ARGB8888:
        memcpy(dst, s, (size_t)cols * 4);
        break;
      case LX_XBGR8888:
      case LX_ABGR8888:
        for (int x = 0; x < cols; x++) {
          uint32_t p = ((const uint32_t *)s)[x];
          dst[x] = 0xFF000000u | ((p & 0xFF) << 16) | (p & 0xFF00) |
                   ((p >> 16) & 0xFF);
        }
        break;
      case LX_RGB565:
        for (int x = 0; x < cols; x++) {
          uint16_t p = ((const uint16_t *)s)[x];
          uint32_t r = (p >> 11) & 0x1F, g = (p >> 5) & 0x3F, b = p & 0x1F;
          dst[x] = 0xFF000000u | (((r << 3) | (r >> 2)) << 16) |
                   (((g << 2) | (g >> 4)) << 8) | ((b << 3) | (b >> 2));
        }
        break;
      default:
        break;
      }
    }
    window_mark_content_locked(win, (rect_t){0, 0, cw, ch});
  }
  int ix = win->x, iy = win->y + window_titlebar_h(win);
  int iw = win->content_w, ih = win->content_h;
  spin_unlock_irqrestore(&win->content_lock, fl);
  mutex_unlock(&g_lx_lock);
  compositor_invalidate_rect((rect_t){ix, iy, iw, ih});
  return 0;
}
