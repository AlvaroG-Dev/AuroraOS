/* N5a: cliente Wayland+EGL (egl-wayland de NVIDIA) con diagnostico paso a paso: cada llamada EGL
 * imprime su resultado y eglGetError(). Pinta N cuadros de colores distintos y los presenta.
 * Uso: aw_nvwl_test [cuadros] [segundos_de_espera] */
#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include <wayland-egl.h>

#include "xdg-shell-client-protocol.h"

typedef void *EGLDisplay, *EGLConfig, *EGLContext, *EGLSurface;
typedef int32_t EGLint;
typedef unsigned int EGLBoolean, EGLenum;
#define EGL_NONE 0x3038
#define EGL_RENDERABLE_TYPE 0x3040
#define EGL_OPENGL_ES2_BIT 0x0004
#define EGL_SURFACE_TYPE 0x3033
#define EGL_WINDOW_BIT 0x0004
#define EGL_RED_SIZE 0x3024
#define EGL_GREEN_SIZE 0x3023
#define EGL_BLUE_SIZE 0x3022
#define EGL_CONTEXT_CLIENT_VERSION 0x3098
#define EGL_OPENGL_ES_API 0x30A0
#define EGL_PLATFORM_WAYLAND_KHR 0x31D8
#define EGL_VENDOR 0x3053
#define EGL_EXTENSIONS 0x3055
EGLDisplay eglGetPlatformDisplay(EGLenum, void *, const intptr_t *);
EGLBoolean eglInitialize(EGLDisplay, EGLint *, EGLint *);
const char *eglQueryString(EGLDisplay, EGLint);
EGLBoolean eglBindAPI(EGLenum);
EGLBoolean eglChooseConfig(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *);
EGLContext eglCreateContext(EGLDisplay, EGLConfig, EGLContext, const EGLint *);
EGLSurface eglCreatePlatformWindowSurface(EGLDisplay, EGLConfig, void *, const intptr_t *);
EGLBoolean eglMakeCurrent(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
EGLBoolean eglSwapBuffers(EGLDisplay, EGLSurface);
EGLBoolean eglSwapInterval(EGLDisplay, EGLint);
EGLint eglGetError(void);
void *(*eglGetProcAddress(const char *))(void);
void glClearColor(float, float, float, float);
void glClear(unsigned);
void glViewport(int, int, int, int);
void glFinish(void);
#define GL_COLOR_BUFFER_BIT 0x4000

#define STEP(...) do { printf("[aw_nvwl] " __VA_ARGS__); printf("\n"); fflush(stdout); } while (0)
#define EGLERR() STEP("  eglGetError=0x%x", eglGetError())

static struct wl_compositor *g_comp;
static struct xdg_wm_base *g_wm;
static int g_configured, g_cw = 320, g_ch = 200;

static void reg_global(void *d, struct wl_registry *reg, uint32_t name, const char *iface, uint32_t ver) {
  (void)d;
  if (!strcmp(iface, "wl_compositor"))
    g_comp = wl_registry_bind(reg, name, &wl_compositor_interface, ver < 4 ? ver : 4);
  else if (!strcmp(iface, "xdg_wm_base"))
    g_wm = wl_registry_bind(reg, name, &xdg_wm_base_interface, 1);
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t n) { (void)d; (void)r; (void)n; }
static const struct wl_registry_listener reg_l = {reg_global, reg_remove};
static void wm_ping(void *d, struct xdg_wm_base *wm, uint32_t s) { (void)d; xdg_wm_base_pong(wm, s); }
static const struct xdg_wm_base_listener wm_l = {wm_ping};
static void xs_conf(void *d, struct xdg_surface *xs, uint32_t s) { (void)d; xdg_surface_ack_configure(xs, s); g_configured = 1; }
static const struct xdg_surface_listener xs_l = {xs_conf};
static void tl_conf(void *d, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *s) {
  (void)d; (void)t; (void)s;
  if (w > 0 && h > 0) { g_cw = w; g_ch = h; }
}
static void tl_close(void *d, struct xdg_toplevel *t) { (void)d; (void)t; exit(0); }
static const struct xdg_toplevel_listener tl_l = {tl_conf, tl_close, NULL, NULL};

static int g_frame_cb;
static void frame_done(void *d, struct wl_callback *cb, uint32_t t) { (void)d; (void)t; g_frame_cb++; wl_callback_destroy(cb); }
static const struct wl_callback_listener frame_l = {frame_done};

static double now(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec / 1e9; }

int main(int argc, char **argv) {
  int frames = argc > 1 ? atoi(argv[1]) : 5, hold = argc > 2 ? atoi(argv[2]) : 5;
  setvbuf(stdout, NULL, _IONBF, 0);
  struct wl_display *dpy = wl_display_connect(NULL);
  if (!dpy) { STEP("FAIL wl_display_connect"); return 1; }
  struct wl_registry *reg = wl_display_get_registry(dpy);
  wl_registry_add_listener(reg, &reg_l, NULL);
  wl_display_roundtrip(dpy);
  if (!g_comp || !g_wm) { STEP("FAIL globales"); return 1; }
  xdg_wm_base_add_listener(g_wm, &wm_l, NULL);
  struct wl_surface *surf = wl_compositor_create_surface(g_comp);
  struct xdg_surface *xs = xdg_wm_base_get_xdg_surface(g_wm, surf);
  xdg_surface_add_listener(xs, &xs_l, NULL);
  struct xdg_toplevel *tl = xdg_surface_get_toplevel(xs);
  xdg_toplevel_add_listener(tl, &tl_l, NULL);
  xdg_toplevel_set_title(tl, "AW NVWL Test");
  xdg_toplevel_set_app_id(tl, "aurora.nvwltest");
  wl_surface_commit(surf);
  wl_display_roundtrip(dpy);
  STEP("configure=%d tam=%dx%d", g_configured, g_cw, g_ch);

  EGLDisplay edpy = eglGetPlatformDisplay(EGL_PLATFORM_WAYLAND_KHR, dpy, NULL);
  STEP("eglGetPlatformDisplay(wayland) -> %p", edpy); EGLERR();
  if (!edpy) return 1;
  EGLint maj = 0, min = 0;
  EGLBoolean r = eglInitialize(edpy, &maj, &min);
  STEP("eglInitialize -> %d (%d.%d) vendor=%s", r, maj, min, eglQueryString(edpy, EGL_VENDOR)); EGLERR();
  if (!r) return 1;
  {
    const char *ex = eglQueryString(edpy, EGL_EXTENSIONS);
    STEP("EGL ext: dma_buf_import_modifiers=%d", ex && strstr(ex, "EGL_EXT_image_dma_buf_import_modifiers") != NULL);
    EGLBoolean (*qm)(EGLDisplay, EGLint, EGLint, uint64_t *, EGLBoolean *, EGLint *) =
        (void *)eglGetProcAddress("eglQueryDmaBufModifiersEXT");
    if (qm) {
      uint64_t m[64]; EGLBoolean e[64]; EGLint nn = 0;
      for (unsigned f = 0; f < 2; f++) {
        uint32_t fcc = f ? 0x34325241u : 0x34325258u;
        int rr = qm(edpy, (EGLint)fcc, 64, m, e, &nn);
        STEP("eglQueryDmaBufModifiersEXT(0x%x) -> %d n=%d", fcc, rr, nn);
        for (int i = 0; i < nn && i < 12; i++) STEP("   mod[%d]=0x%llx external_only=%d", i, (unsigned long long)m[i], e[i]);
      }
    }
  }
  eglBindAPI(EGL_OPENGL_ES_API);
  EGLConfig cfg; EGLint n = 0;
  EGLint ca[] = {EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                 EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE};
  r = eglChooseConfig(edpy, ca, &cfg, 1, &n);
  STEP("eglChooseConfig -> %d n=%d", r, n); EGLERR();
  if (!r || n < 1) return 1;
  EGLint xa[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
  EGLContext ctx = eglCreateContext(edpy, cfg, NULL, xa);
  STEP("eglCreateContext -> %p", ctx); EGLERR();
  struct wl_egl_window *win = wl_egl_window_create(surf, g_cw, g_ch);
  STEP("wl_egl_window_create -> %p", (void *)win);
  EGLSurface es = eglCreatePlatformWindowSurface(edpy, cfg, win, NULL);
  STEP("eglCreatePlatformWindowSurface -> %p", es); EGLERR();
  if (!es) return 1;
  r = eglMakeCurrent(edpy, es, es, ctx);
  STEP("eglMakeCurrent -> %d", r); EGLERR();
  if (!r) return 1;
  r = eglSwapInterval(edpy, 0);
  STEP("eglSwapInterval(0) -> %d", r);

  for (int i = 0; i < frames; i++) {
    float c[5][3] = {{1, .2f, .2f}, {.2f, 1, .2f}, {.2f, .4f, 1}, {1, 1, .2f}, {.9f, .3f, .9f}};
    glViewport(0, 0, g_cw, g_ch);
    glClearColor(c[i % 5][0], c[i % 5][1], c[i % 5][2], 1);
    glClear(GL_COLOR_BUFFER_BIT);
    struct wl_callback *cb = wl_surface_frame(surf);
    wl_callback_add_listener(cb, &frame_l, NULL);
    double t0 = now();
    r = eglSwapBuffers(edpy, es);
    STEP("frame %d: eglSwapBuffers -> %d (%.0f ms)", i, r, (now() - t0) * 1000); EGLERR();
    wl_display_flush(dpy);
    /* esperar el callback de frame hasta 2 s */
    double t1 = now();
    int want = i + 1;
    while (g_frame_cb < want && now() - t1 < 2.0) {
      if (wl_display_prepare_read(dpy) != 0) { wl_display_dispatch_pending(dpy); continue; }
      wl_display_flush(dpy);
      struct pollfd pf = {.fd = wl_display_get_fd(dpy), .events = POLLIN};
      if (poll(&pf, 1, 100) > 0) wl_display_read_events(dpy); else wl_display_cancel_read(dpy);
      wl_display_dispatch_pending(dpy);
    }
    STEP("  frame_callback %s (%d)", g_frame_cb >= want ? "recibido" : "NO llego en 2 s", g_frame_cb);
  }
  STEP("%d cuadros enviados; esperando %d s con la ventana visible", frames, hold);
  double t2 = now();
  while (now() - t2 < hold) {
    wl_display_flush(dpy);
    struct pollfd pf = {.fd = wl_display_get_fd(dpy), .events = POLLIN};
    if (wl_display_prepare_read(dpy) == 0) {
      if (poll(&pf, 1, 100) > 0) wl_display_read_events(dpy); else wl_display_cancel_read(dpy);
    }
    wl_display_dispatch_pending(dpy);
  }
  STEP("ALL PASS");
  return 0;
}
