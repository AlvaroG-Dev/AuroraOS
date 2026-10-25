// tests/wayland/aw_sub_test.c
//
// Cliente de prueba de aurora-wayland (servidor Wayland nativo de Aurora):
//   - toplevel xdg_shell con buffer wl_shm opaco (cuadricula),
//   - subsuperficie A ARGB semitransparente (modo desync, se mueve con el
//     commit del padre),
//   - subsuperficie B 8x8 en modo sync, ampliada a 128x128 con wp_viewporter,
//   - subsuperficie C ARGB con degradado de alfa, colocada encima de A,
//   - cursor de texto / mano segun la superficie bajo el puntero
//     (wp_cursor_shape_v1) y traza de los eventos de puntero.
// Si aurora-wayland compone bien se ven las tres capas mezcladas sobre la
// cuadricula; el log serie imprime "[aw_sub_test] ..." para cada paso.
//
// Compilar con tests/wayland/build.sh (cabeceras de libwayland-dev y
// protocolos generados con wayland-scanner).
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

#include "cursor-shape-v1-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#define ROOT_W 480
#define ROOT_H 320
#define POOL_SIZE (2 * 1024 * 1024)

static struct wl_compositor *g_comp;
static struct wl_subcompositor *g_subc;
static struct wl_shm *g_shm;
static struct xdg_wm_base *g_wm;
static struct wp_viewporter *g_vpm;
static struct wl_seat *g_seat;
static struct wp_cursor_shape_manager_v1 *g_csm;
static struct wl_pointer *g_ptr;
static struct wp_cursor_shape_device_v1 *g_csdev;
static struct wl_shm_pool *g_pool;
static uint8_t *g_pool_data;
static size_t g_pool_off;

static struct wl_surface *g_root, *g_sa, *g_sb, *g_sc;
static struct wl_subsurface *g_ssa, *g_ssb, *g_ssc;
static int g_configured;

static void reg_global(void *d, struct wl_registry *reg, uint32_t name,
                       const char *iface, uint32_t ver) {
  (void)d;
  if (!strcmp(iface, "wl_compositor"))
    g_comp = wl_registry_bind(reg, name, &wl_compositor_interface, 4);
  else if (!strcmp(iface, "wl_subcompositor"))
    g_subc = wl_registry_bind(reg, name, &wl_subcompositor_interface, 1);
  else if (!strcmp(iface, "wl_shm"))
    g_shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
  else if (!strcmp(iface, "xdg_wm_base"))
    g_wm = wl_registry_bind(reg, name, &xdg_wm_base_interface, 1);
  else if (!strcmp(iface, "wp_viewporter"))
    g_vpm = wl_registry_bind(reg, name, &wp_viewporter_interface, 1);
  else if (!strcmp(iface, "wl_seat"))
    g_seat = wl_registry_bind(reg, name, &wl_seat_interface, ver < 5 ? ver : 5);
  else if (!strcmp(iface, "wp_cursor_shape_manager_v1"))
    g_csm = wl_registry_bind(reg, name, &wp_cursor_shape_manager_v1_interface, 1);
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t n) {
  (void)d; (void)r; (void)n;
}
static const struct wl_registry_listener reg_l = {reg_global, reg_remove};

static void wm_ping(void *d, struct xdg_wm_base *wm, uint32_t s) {
  (void)d;
  xdg_wm_base_pong(wm, s);
}
static const struct xdg_wm_base_listener wm_l = {wm_ping};

static void xs_configure(void *d, struct xdg_surface *xs, uint32_t serial) {
  (void)d;
  xdg_surface_ack_configure(xs, serial);
  g_configured = 1;
}
static const struct xdg_surface_listener xs_l = {xs_configure};
static void tl_configure(void *d, struct xdg_toplevel *t, int32_t w, int32_t h,
                         struct wl_array *s) {
  (void)d; (void)t; (void)s;
  printf("[aw_sub_test] toplevel.configure %dx%d\n", w, h);
}
static void tl_close(void *d, struct xdg_toplevel *t) {
  (void)d; (void)t;
  printf("[aw_sub_test] close\n");
  exit(0);
}
static const struct xdg_toplevel_listener tl_l = {tl_configure, tl_close};

static const char *surf_name(struct wl_surface *s) {
  if (s == g_root) return "root";
  if (s == g_sa) return "A";
  if (s == g_sb) return "B";
  if (s == g_sc) return "C";
  return "?";
}
static void ptr_enter(void *d, struct wl_pointer *p, uint32_t serial,
                      struct wl_surface *s, wl_fixed_t x, wl_fixed_t y) {
  (void)d; (void)p;
  printf("[aw_sub_test] ptr enter %s (%d,%d)\n", surf_name(s), wl_fixed_to_int(x),
         wl_fixed_to_int(y));
  if (g_csdev)
    wp_cursor_shape_device_v1_set_shape(
        g_csdev, serial,
        s == g_sb ? WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER
                  : WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT);
}
static void ptr_leave(void *d, struct wl_pointer *p, uint32_t serial,
                      struct wl_surface *s) {
  (void)d; (void)p; (void)serial;
  printf("[aw_sub_test] ptr leave %s\n", surf_name(s));
}
static void ptr_motion(void *d, struct wl_pointer *p, uint32_t t, wl_fixed_t x,
                       wl_fixed_t y) {
  (void)d; (void)p; (void)t; (void)x; (void)y;
}
static void ptr_button(void *d, struct wl_pointer *p, uint32_t s, uint32_t t,
                       uint32_t b, uint32_t st) {
  (void)d; (void)p; (void)s; (void)t;
  printf("[aw_sub_test] button %u state %u\n", b, st);
}
static void ptr_axis(void *d, struct wl_pointer *p, uint32_t t, uint32_t a,
                     wl_fixed_t v) {
  (void)d; (void)p; (void)t; (void)a; (void)v;
}
static void ptr_frame(void *d, struct wl_pointer *p) { (void)d; (void)p; }
static void ptr_axis_source(void *d, struct wl_pointer *p, uint32_t s) {
  (void)d; (void)p; (void)s;
}
static void ptr_axis_stop(void *d, struct wl_pointer *p, uint32_t t, uint32_t a) {
  (void)d; (void)p; (void)t; (void)a;
}
static void ptr_axis_discrete(void *d, struct wl_pointer *p, uint32_t a, int32_t v) {
  (void)d; (void)p; (void)a; (void)v;
}
static const struct wl_pointer_listener ptr_l = {
    ptr_enter, ptr_leave, ptr_motion, ptr_button, ptr_axis,
    ptr_frame, ptr_axis_source, ptr_axis_stop, ptr_axis_discrete};

static struct wl_buffer *mkbuf(int w, int h, uint32_t fmt, uint32_t **px) {
  size_t size = (size_t)w * h * 4;
  if (g_pool_off + size > POOL_SIZE) {
    fprintf(stderr, "pool agotado\n");
    exit(1);
  }
  *px = (uint32_t *)(g_pool_data + g_pool_off);
  struct wl_buffer *b =
      wl_shm_pool_create_buffer(g_pool, (int32_t)g_pool_off, w, h, w * 4, fmt);
  g_pool_off += (size + 63) & ~(size_t)63;
  return b;
}

static void fill_root(uint32_t *p) {
  for (int y = 0; y < ROOT_H; y++)
    for (int x = 0; x < ROOT_W; x++) {
      uint32_t g = 0x30 + y * 0x50 / ROOT_H;
      uint32_t c = 0xFF000000u | (g << 16) | (g << 8) | 0xA0;
      if (x % 40 == 0 || y % 40 == 0)
        c = 0xFFFFFFFFu;
      p[y * ROOT_W + x] = c;
    }
}

int main(void) {
  struct wl_display *dpy = wl_display_connect(NULL);
  if (!dpy) {
    fprintf(stderr, "[aw_sub_test] sin servidor Wayland\n");
    return 1;
  }
  struct wl_registry *reg = wl_display_get_registry(dpy);
  wl_registry_add_listener(reg, &reg_l, NULL);
  wl_display_roundtrip(dpy);
  if (!g_comp || !g_subc || !g_shm || !g_wm) {
    fprintf(stderr, "[aw_sub_test] faltan globales\n");
    return 1;
  }
  printf("[aw_sub_test] globales: viewporter=%d cursor_shape=%d seat=%d\n",
         g_vpm != NULL, g_csm != NULL, g_seat != NULL);
  xdg_wm_base_add_listener(g_wm, &wm_l, NULL);

  int fd = memfd_create("aw-sub", MFD_CLOEXEC);
  if (fd < 0 || ftruncate(fd, POOL_SIZE) < 0) {
    perror("memfd");
    return 1;
  }
  g_pool_data = mmap(NULL, POOL_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  g_pool = wl_shm_create_pool(g_shm, fd, POOL_SIZE);

  g_root = wl_compositor_create_surface(g_comp);
  struct xdg_surface *xs = xdg_wm_base_get_xdg_surface(g_wm, g_root);
  xdg_surface_add_listener(xs, &xs_l, NULL);
  struct xdg_toplevel *tl = xdg_surface_get_toplevel(xs);
  xdg_toplevel_add_listener(tl, &tl_l, NULL);
  xdg_toplevel_set_title(tl, "AW Sub Test");
  xdg_toplevel_set_app_id(tl, "aurora.subtest");

  if (g_seat) {
    g_ptr = wl_seat_get_pointer(g_seat);
    wl_pointer_add_listener(g_ptr, &ptr_l, NULL);
    if (g_csm)
      g_csdev = wp_cursor_shape_manager_v1_get_pointer(g_csm, g_ptr);
  }

  // Subsuperficies: se crean antes del primer commit del padre.
  g_sa = wl_compositor_create_surface(g_comp);
  g_ssa = wl_subcompositor_get_subsurface(g_subc, g_sa, g_root);
  wl_subsurface_set_position(g_ssa, 60, 50);
  wl_subsurface_set_desync(g_ssa);

  g_sb = wl_compositor_create_surface(g_comp);
  g_ssb = wl_subcompositor_get_subsurface(g_subc, g_sb, g_root); /* sync */
  wl_subsurface_set_position(g_ssb, 300, 150);

  g_sc = wl_compositor_create_surface(g_comp);
  g_ssc = wl_subcompositor_get_subsurface(g_subc, g_sc, g_root);
  wl_subsurface_set_position(g_ssc, 110, 70);
  wl_subsurface_place_above(g_ssc, g_sa);
  wl_subsurface_set_desync(g_ssc);

  wl_surface_commit(g_root); /* pide configure */
  wl_display_roundtrip(dpy);
  if (!g_configured) {
    fprintf(stderr, "[aw_sub_test] sin configure\n");
    return 1;
  }

  // Contenido
  uint32_t *p;
  struct wl_buffer *b = mkbuf(ROOT_W, ROOT_H, WL_SHM_FORMAT_XRGB8888, &p);
  fill_root(p);

  uint32_t *pa;
  struct wl_buffer *ba = mkbuf(140, 90, WL_SHM_FORMAT_ARGB8888, &pa);
  for (int i = 0; i < 140 * 90; i++)
    pa[i] = 0x80800000u; /* rojo al 50% (premultiplicado) */
  wl_surface_attach(g_sa, ba, 0, 0);
  wl_surface_damage(g_sa, 0, 0, 140, 90);
  wl_surface_commit(g_sa);

  uint32_t *pb;
  struct wl_buffer *bb = mkbuf(8, 8, WL_SHM_FORMAT_XRGB8888, &pb);
  for (int y = 0; y < 8; y++)
    for (int x = 0; x < 8; x++)
      pb[y * 8 + x] = ((x / 4) ^ (y / 4)) ? 0xFFFF00FFu : 0xFF00FFFFu;
  if (g_vpm) {
    struct wp_viewport *vp = wp_viewporter_get_viewport(g_vpm, g_sb);
    wp_viewport_set_destination(vp, 128, 128);
  }
  wl_surface_attach(g_sb, bb, 0, 0);
  wl_surface_damage(g_sb, 0, 0, 8, 8);
  wl_surface_commit(g_sb); /* sync: no se ve hasta el commit del padre */

  uint32_t *pc;
  struct wl_buffer *bc = mkbuf(100, 60, WL_SHM_FORMAT_ARGB8888, &pc);
  for (int y = 0; y < 60; y++)
    for (int x = 0; x < 100; x++) {
      uint32_t a = (uint32_t)(x * 255 / 99);
      pc[y * 100 + x] = (a << 24) | (a << 8); /* verde con alfa creciente */
    }
  wl_surface_attach(g_sc, bc, 0, 0);
  wl_surface_damage(g_sc, 0, 0, 100, 60);
  wl_surface_commit(g_sc);

  wl_surface_attach(g_root, b, 0, 0);
  wl_surface_damage(g_root, 0, 0, ROOT_W, ROOT_H);
  wl_surface_commit(g_root);
  wl_display_flush(dpy);
  printf("[aw_sub_test] mapped\n");
  fflush(stdout);

  // Mueve A cada segundo con el commit del padre (A es desync: su posicion
  // solo cambia al hacer commit el padre).
  int step = 0;
  for (;;) {
    wl_display_flush(dpy);
    if (wl_display_prepare_read(dpy) != 0) {
      wl_display_dispatch_pending(dpy);
      continue;
    }
    struct pollfd pfd = {.fd = wl_display_get_fd(dpy), .events = POLLIN};
    int r = poll(&pfd, 1, 1000);
    if (r > 0)
      wl_display_read_events(dpy);
    else
      wl_display_cancel_read(dpy);
    wl_display_dispatch_pending(dpy);
    if (r == 0 && step < 5) {
      step++;
      wl_subsurface_set_position(g_ssa, 60 + 15 * step, 50 + 8 * step);
      wl_surface_damage(g_root, 0, 0, ROOT_W, ROOT_H);
      wl_surface_commit(g_root);
      printf("[aw_sub_test] mueve A paso %d\n", step);
      fflush(stdout);
    }
  }
}
