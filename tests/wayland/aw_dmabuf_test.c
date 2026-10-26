// tests/wayland/aw_dmabuf_test.c
//
// Cliente de prueba de zwp_linux_dmabuf_v1 en aurora-wayland: crea un dumb
// buffer en /dev/dri/renderD128, lo pinta (degradado + barras de color), lo
// exporta con PRIME (dma-buf), lo entrega al servidor con linux-dmabuf
// (create_immed) y lo muestra en un toplevel. Si todo va bien aparece una
// ventana con el patron y el log serie imprime "[aw_dmabuf_test] ... OK".
#define _GNU_SOURCE
#include <drm/drm.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_mode.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

#include "linux-dmabuf-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#define W 320
#define H 200

static struct wl_compositor *g_comp;
static struct xdg_wm_base *g_wm;
static struct zwp_linux_dmabuf_v1 *g_dmabuf;
static int g_configured, g_xrgb, g_argb, g_linear;

static void dmabuf_format(void *d, struct zwp_linux_dmabuf_v1 *o, uint32_t f) {
  (void)d; (void)o;
  if (f == DRM_FORMAT_XRGB8888) g_xrgb = 1;
  if (f == DRM_FORMAT_ARGB8888) g_argb = 1;
}
static void dmabuf_modifier(void *d, struct zwp_linux_dmabuf_v1 *o, uint32_t f,
                            uint32_t hi, uint32_t lo) {
  (void)d; (void)o;
  if (hi == 0 && lo == 0) g_linear = 1;
  dmabuf_format(d, o, f);
}
static const struct zwp_linux_dmabuf_v1_listener dmabuf_l = {dmabuf_format, dmabuf_modifier};

static void reg_global(void *d, struct wl_registry *reg, uint32_t name,
                       const char *iface, uint32_t ver) {
  (void)d;
  if (!strcmp(iface, "wl_compositor"))
    g_comp = wl_registry_bind(reg, name, &wl_compositor_interface, 4);
  else if (!strcmp(iface, "xdg_wm_base"))
    g_wm = wl_registry_bind(reg, name, &xdg_wm_base_interface, 1);
  else if (!strcmp(iface, "zwp_linux_dmabuf_v1")) {
    g_dmabuf = wl_registry_bind(reg, name, &zwp_linux_dmabuf_v1_interface, ver < 3 ? ver : 3);
    zwp_linux_dmabuf_v1_add_listener(g_dmabuf, &dmabuf_l, NULL);
  }
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t n) { (void)d; (void)r; (void)n; }
static const struct wl_registry_listener reg_l = {reg_global, reg_remove};

static void wm_ping(void *d, struct xdg_wm_base *wm, uint32_t s) { (void)d; xdg_wm_base_pong(wm, s); }
static const struct xdg_wm_base_listener wm_l = {wm_ping};
static void xs_conf(void *d, struct xdg_surface *xs, uint32_t s) {
  (void)d; xdg_surface_ack_configure(xs, s); g_configured = 1;
}
static const struct xdg_surface_listener xs_l = {xs_conf};
static void tl_conf(void *d, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *s) {
  (void)d; (void)t; (void)w; (void)h; (void)s;
}
static void tl_close(void *d, struct xdg_toplevel *t) { (void)d; (void)t; exit(0); }
static const struct xdg_toplevel_listener tl_l = {tl_conf, tl_close, NULL, NULL};

int main(void) {
  struct wl_display *dpy = wl_display_connect(NULL);
  if (!dpy) { fprintf(stderr, "[aw_dmabuf_test] sin display\n"); return 1; }
  struct wl_registry *reg = wl_display_get_registry(dpy);
  wl_registry_add_listener(reg, &reg_l, NULL);
  wl_display_roundtrip(dpy);
  wl_display_roundtrip(dpy);
  printf("[aw_dmabuf_test] linux_dmabuf=%d xrgb=%d argb=%d linear=%d\n",
         g_dmabuf != NULL, g_xrgb, g_argb, g_linear);
  if (!g_comp || !g_wm || !g_dmabuf || !g_xrgb || !g_linear) {
    printf("[aw_dmabuf_test] FAIL globales\n");
    return 1;
  }
  xdg_wm_base_add_listener(g_wm, &wm_l, NULL);

  int drm = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
  struct drm_mode_create_dumb cd = {.width = W, .height = H, .bpp = 32};
  if (drm < 0 || ioctl(drm, DRM_IOCTL_MODE_CREATE_DUMB, &cd) < 0) {
    printf("[aw_dmabuf_test] FAIL create_dumb (errno=%d)\n", errno);
    return 1;
  }
  struct drm_mode_map_dumb md = {.handle = cd.handle};
  uint32_t *px = MAP_FAILED;
  if (ioctl(drm, DRM_IOCTL_MODE_MAP_DUMB, &md) == 0)
    px = mmap(NULL, cd.size, PROT_READ | PROT_WRITE, MAP_SHARED, drm, md.offset);
  if (px == MAP_FAILED) { printf("[aw_dmabuf_test] FAIL mmap dumb\n"); return 1; }
  for (int y = 0; y < H; y++)
    for (int x = 0; x < W; x++) {
      uint32_t r = x * 255 / (W - 1), g = y * 255 / (H - 1), b = 0x60;
      if (y > H - 40) { // barras de color
        int bar = x * 6 / W;
        static const uint32_t c[6] = {0xFF3B30, 0xFF9500, 0xFFCC00, 0x34C759, 0x0A84FF, 0xBF5AF2};
        px[y * (cd.pitch / 4) + x] = c[bar];
      } else {
        px[y * (cd.pitch / 4) + x] = (r << 16) | (g << 8) | b;
      }
    }
  struct drm_prime_handle ph = {.handle = cd.handle, .flags = DRM_CLOEXEC | DRM_RDWR};
  if (ioctl(drm, DRM_IOCTL_PRIME_HANDLE_TO_FD, &ph) < 0) {
    printf("[aw_dmabuf_test] FAIL prime export (errno=%d)\n", errno);
    return 1;
  }
  printf("[aw_dmabuf_test] dmabuf fd=%d pitch=%u size=%llu\n", ph.fd, cd.pitch,
         (unsigned long long)cd.size);

  struct wl_surface *surf = wl_compositor_create_surface(g_comp);
  struct xdg_surface *xs = xdg_wm_base_get_xdg_surface(g_wm, surf);
  xdg_surface_add_listener(xs, &xs_l, NULL);
  struct xdg_toplevel *tl = xdg_surface_get_toplevel(xs);
  xdg_toplevel_add_listener(tl, &tl_l, NULL);
  xdg_toplevel_set_title(tl, "AW DMABUF Test");
  xdg_toplevel_set_app_id(tl, "aurora.dmabuftest");
  wl_surface_commit(surf);
  wl_display_roundtrip(dpy);
  if (!g_configured) { printf("[aw_dmabuf_test] FAIL sin configure\n"); return 1; }

  struct zwp_linux_buffer_params_v1 *params = zwp_linux_dmabuf_v1_create_params(g_dmabuf);
  zwp_linux_buffer_params_v1_add(params, ph.fd, 0, 0, cd.pitch, 0, 0);
  struct wl_buffer *buf = zwp_linux_buffer_params_v1_create_immed(
      params, W, H, DRM_FORMAT_XRGB8888, 0);
  zwp_linux_buffer_params_v1_destroy(params);
  wl_surface_attach(surf, buf, 0, 0);
  wl_surface_damage(surf, 0, 0, W, H);
  wl_surface_commit(surf);
  if (wl_display_roundtrip(dpy) < 0) {
    printf("[aw_dmabuf_test] FAIL el servidor rechazo el buffer\n");
    return 1;
  }
  printf("[aw_dmabuf_test] mapped OK\n");
  fflush(stdout);
  // Segundo cuadro: cambia un pixel en el dumb y vuelve a confirmar.
  px[0] = 0xFFFFFF;
  wl_surface_attach(surf, buf, 0, 0);
  wl_surface_damage(surf, 0, 0, W, H);
  wl_surface_commit(surf);
  wl_display_roundtrip(dpy);
  printf("[aw_dmabuf_test] recommit OK\n");
  fflush(stdout);
  for (;;) {
    wl_display_flush(dpy);
    struct pollfd pfd = {.fd = wl_display_get_fd(dpy), .events = POLLIN};
    if (poll(&pfd, 1, 1000) > 0 && wl_display_dispatch(dpy) < 0)
      break;
  }
  return 0;
}
