// aurora-wayland: servidor Wayland nativo de Aurora.
//
// Proceso de usuario (ABI Linux/glibc) enlazado con la libwayland-server de la
// distro, sin tocar. Cada xdg_toplevel se convierte en una ventana mas del
// compositor de Aurora (winsrv): barra de titulo, sombras, animaciones, Dock y
// foco son los nativos. Aqui solo se traduce:
//
//   wl_surface.commit  ->  sys_win_blit() al content_buffer de la ventana
//   WINSRV_EV_*        ->  eventos wl_pointer / wl_keyboard / xdg_toplevel
//   /dev/input/event0  ->  wl_keyboard (teclas con release y mapa xkb)
//
// No es Weston ni un escritorio aparte: no hay scanout propio (no toca DRM).
#define _GNU_SOURCE
#include <errno.h>
#include <fnmatch.h>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <linux/input.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <wayland-server.h>
#include <xkbcommon/xkbcommon.h>

#include "org-kde-kwin-server-decoration-server-protocol.h"
#include "xdg-decoration-unstable-v1-server-protocol.h"
#include "xdg-shell-server-protocol.h"

// ---------------------------------------------------------------------------
// ABI de winsrv (ver kernel/gfx/winsrv.h y user/syscall.h)
// ---------------------------------------------------------------------------
#define ASYS_BASE 0x1000
#define ASYS_WIN_CREATE (ASYS_BASE + 0x10)
#define ASYS_WIN_DESTROY (ASYS_BASE + 0x11)
#define ASYS_WIN_BLIT (ASYS_BASE + 0x12)
#define ASYS_WIN_POLL_EVENT (ASYS_BASE + 0x13)
#define ASYS_WIN_SET_RAW_INPUT (ASYS_BASE + 0x16)

#define WINSRV_EV_CLOSE 1
#define WINSRV_EV_FOCUS 2
#define WINSRV_EV_BLUR 3
#define WINSRV_EV_MOVE 4
#define WINSRV_EV_RESIZE 9
#define WINSRV_EV_PTR 10
#define WINSRV_EV_PTR_LEAVE 11

#define TITLEBAR_H 32 /* alto de la barra de titulo de Aurora */
#define WIN_MIN 64
#define WIN_MAX 2048

typedef struct {
  uint32_t type;
  int32_t x, y;
  uint32_t data;
} winsrv_event_t;

typedef struct {
  int32_t win_id;
  int32_t x, y, w, h;
  int32_t src_x, src_y, src_stride;
  uint32_t *pixels;
} winsrv_blit_args_t;

#define WIN_FLAGS_NODECORATION (1 << 2)
#define WIN_FLAGS_FIXED_SIZE (1 << 5)
#define WIN_FLAGS_RAW_INPUT (1 << 6)
#define WIN_FLAGS_POPUP (1 << 7)
#define ASYS_WIN_CREATE_EX (ASYS_BASE + 0x17)
static int win_create_ex(int x, int y, int w, int h, const char *title,
                         uint32_t flags) {
  struct {
    uint32_t flags;
    char title[64];
  } req;
  memset(&req, 0, sizeof(req));
  req.flags = flags;
  snprintf(req.title, sizeof(req.title), "%s", title);
  return (int)syscall(ASYS_WIN_CREATE_EX, (long)x, (long)y, (long)w, (long)h,
                      &req);
}
#define ASYS_WIN_SET_TITLE (ASYS_BASE + 0x18)
#define ASYS_WIN_SET_STATE (ASYS_BASE + 0x19)
#define WINSRV_STATE_MINIMIZE 1
#define WINSRV_STATE_MAXIMIZE 2
#define WINSRV_STATE_RESTORE 3
#define ASYS_DOCK_PIN (ASYS_BASE + 0x1A)
#define ASYS_DOCK_POLL_LAUNCH (ASYS_BASE + 0x1B)
#define ASYS_WIN_SET_APP_ID (ASYS_BASE + 0x1C)
static int win_set_app_id(int id, const char *app_id) {
  return (int)syscall(ASYS_WIN_SET_APP_ID, (long)id, app_id);
}
static int win_set_title(int id, const char *title) {
  return (int)syscall(ASYS_WIN_SET_TITLE, (long)id, title);
}
static int win_set_state(int id, int state) {
  return (int)syscall(ASYS_WIN_SET_STATE, (long)id, (long)state);
}
static int win_destroy(int id) { return (int)syscall(ASYS_WIN_DESTROY, (long)id); }
static int win_poll(int id, winsrv_event_t *ev) {
  return (int)syscall(ASYS_WIN_POLL_EVENT, (long)id, ev, 0L);
}
// Copia el rectangulo (sx, sy, w, h) del buffer `pixels` (ancho `stride`) a
// (dx, dy) del contenido de la ventana.
static int win_blit(int id, int dx, int dy, int sx, int sy, int w, int h,
                    int stride, uint32_t *pixels) {
  winsrv_blit_args_t a = {.win_id = id,
                          .x = dx,
                          .y = dy,
                          .w = w,
                          .h = h,
                          .src_x = sx,
                          .src_y = sy,
                          .src_stride = stride,
                          .pixels = pixels};
  return (int)syscall(ASYS_WIN_BLIT, &a);
}

// ---------------------------------------------------------------------------
// Estado global
// ---------------------------------------------------------------------------
#define OUT_W 1280
#define OUT_H 800

struct surface;
struct toplevel;

static struct wl_display *g_display;
static struct wl_event_loop *g_loop;
static struct wl_event_source *g_pump_timer;
static struct wl_list g_toplevels;  /* struct toplevel.link */
static struct wl_list g_pointers;   /* struct seat_res.link */
static struct wl_list g_keyboards;  /* struct seat_res.link */
static struct wl_list g_datadevs;   /* struct seat_res.link */
static struct toplevel *g_kbd_focus;
static struct xsurf *g_ptr_focus;
static struct wl_list g_wins; /* struct xsurf.wlink: ventanas mapeadas */
static int g_win_count;
static int g_debug; /* AW_DEBUG=1: traza de eventos */

static struct xkb_context *g_xkb_ctx;
static struct xkb_keymap *g_xkb_map;
static struct xkb_state *g_xkb_state;
static int g_keymap_fd = -1;
static size_t g_keymap_size;

static struct data_source *g_selection;

static uint32_t now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

struct seat_res {
  struct wl_resource *res;
  struct wl_list link;
};

struct surface {
  struct wl_resource *res;
  struct wl_resource *buffer;     /* buffer pendiente (attach) */
  int has_attach;                 /* hubo attach desde el ultimo commit */
  int dmg_x0, dmg_y0, dmg_x1, dmg_y1; /* bbox de daño pendiente; x1<=x0 = ninguno */
  struct wl_list frames;          /* frame callbacks pendientes (wl_resource) */
  struct xsurf *xs;
  struct wl_listener buffer_destroy;
  uint32_t *scratch;              /* copia ARGB opaca del buffer */
  size_t scratch_px;
  int buf_w, buf_h;               /* tamaño del ultimo buffer volcado */
  int kde_ssd;                    /* org_kde_kwin_server_decoration creado */
};

struct xsurf {
  struct wl_resource *res;
  struct surface *surface;
  struct toplevel *tl;
  int is_popup;
  int configured_sent;
  uint32_t last_serial;
  int acked;
  int has_geom, gx, gy, gw, gh; /* set_window_geometry */
  int vx, vy; /* origen de la geometria visible dentro del buffer (ultimo commit) */
  // Ventana de Aurora que lo muestra (toplevel o popup).
  int win_id; /* -1 = aun no mapeada */
  struct wl_list wlink; /* en g_wins mientras esta mapeada */
  int cw, ch; /* tamano del contenido de la ventana */
  int wx, wy; /* origen del contenido en pantalla */
  int last_buttons;
  // Popups
  struct xsurf *parent;
  struct wl_resource *popup_res;
  int px, py;   /* posicion relativa a la geometria del padre */
  int pw, ph;   /* tamano pedido por el positioner */
  int grabbed;
};

struct toplevel {
  struct wl_resource *res;
  struct xsurf *xs;
  struct wl_list link;
  char title[64];
  int activated;
  int maximized;   /* estado anunciado en configure */
  int pending_max; /* set_maximized antes de existir la ventana de Aurora */
  int pending_min; /* idem para set_minimized */
  char app_id[64];
  int min_w, min_h, max_w, max_h; /* set_min_size / set_max_size (0 = sin limite) */
  // Politica de decoraciones: la unica barra de titulo debe ser la de Aurora.
  int ssd;          /* el cliente negocio xdg-decoration / KDE server-decoration */
  int inset_state;  /* 0 sin decidir, 1 recorte CSD activo, 2 sin recorte */
  int in_l, in_t, in_r, in_b; /* recorte aplicado en el ultimo commit */
  const struct csd_rule *rule;
};

struct data_source {
  struct wl_resource *res;
  struct wl_array mimes; /* cadenas contiguas separadas por NUL */
};

// ---------------------------------------------------------------------------
// Politica de decoraciones (CSD)
//
// Aurora siempre dibuja la barra de titulo, semaforos y sombras. Para los
// clientes que negocian xdg-decoration (o el protocolo de KDE) basta con
// contestar "server-side". Los que NO lo negocian (weston-terminal, GTK/Qt
// antiguos, SDL+libdecor, Electron...) dibujan su propio marco; contra eso hay
// tres capas, de menos a mas agresiva:
//   1. configure con estados TILED_*: los toolkits quitan sombras y esquinas
//      redondeadas y no pintan bordes de redimension.
//   2. set_window_geometry: lo que queda fuera (sombra) no se vuelca.
//   3. recorte ("inset") de la barra/bordes propios segun reglas por app_id o
//      nombre de proceso (csd-rules.conf), solo si el cliente muestra margenes
//      de CSD (geometria menor que el buffer).
// ---------------------------------------------------------------------------
#define CSD_RULES_PATH "/usr/share/aurora/csd-rules.conf"
struct csd_rule {
  struct wl_list link;
  char pat[64];
  int l, t, r, b;
};
static struct wl_list g_csd_rules; /* struct csd_rule.link */

static void csd_rule_add(const char *pat, int l, int t, int r, int b) {
  struct csd_rule *cr = calloc(1, sizeof(*cr));
  if (!cr)
    return;
  snprintf(cr->pat, sizeof(cr->pat), "%s", pat);
  cr->l = l; cr->t = t; cr->r = r; cr->b = b;
  wl_list_insert(g_csd_rules.prev, &cr->link);
}

// Formato: "patron [left=N] [top=N] [right=N] [bottom=N] [border=N]" (glob sobre
// app_id o nombre de proceso; '#' comenta). "none" desactiva el recorte.
static void csd_rules_load(void) {
  // Valores por defecto: toolkit de Weston (marco de 6 px y barra de 27 px).
  csd_rule_add("org.freedesktop.weston.*", 6, 27, 6, 6);
  csd_rule_add("weston-*", 6, 27, 6, 6);
  FILE *f = fopen(CSD_RULES_PATH, "r");
  if (!f)
    return;
  char line[256];
  while (fgets(line, sizeof(line), f)) {
    char *h = strchr(line, '#');
    if (h)
      *h = 0;
    char pat[64];
    int off = 0;
    if (sscanf(line, "%63s%n", pat, &off) < 1)
      continue;
    int l = 0, t = 0, r = 0, b = 0;
    for (char *tok = strtok(line + off, " \t\r\n"); tok;
         tok = strtok(NULL, " \t\r\n")) {
      int v;
      if (sscanf(tok, "left=%d", &v) == 1) l = v;
      else if (sscanf(tok, "top=%d", &v) == 1) t = v;
      else if (sscanf(tok, "right=%d", &v) == 1) r = v;
      else if (sscanf(tok, "bottom=%d", &v) == 1) b = v;
      else if (sscanf(tok, "border=%d", &v) == 1) l = r = b = v;
    }
    // Una regla del fichero va delante de las predeterminadas.
    struct csd_rule *cr = calloc(1, sizeof(*cr));
    if (!cr)
      continue;
    snprintf(cr->pat, sizeof(cr->pat), "%s", pat);
    cr->l = l; cr->t = t; cr->r = r; cr->b = b;
    wl_list_insert(&g_csd_rules, &cr->link);
  }
  fclose(f);
}

static const struct csd_rule *csd_rule_find(struct toplevel *tl) {
  char comm[32] = "";
  pid_t pid = 0;
  wl_client_get_credentials(wl_resource_get_client(tl->res), &pid, NULL, NULL);
  if (pid > 0) {
    char path[48];
    snprintf(path, sizeof(path), "/proc/%d/comm", (int)pid);
    FILE *f = fopen(path, "r");
    if (f) {
      if (fgets(comm, sizeof(comm), f))
        comm[strcspn(comm, "\n")] = 0;
      fclose(f);
    }
  }
  struct csd_rule *cr;
  wl_list_for_each(cr, &g_csd_rules, link) {
    if ((tl->app_id[0] && !fnmatch(cr->pat, tl->app_id, 0)) ||
        (comm[0] && !fnmatch(cr->pat, comm, 0)))
      return cr;
  }
  return NULL;
}

static int tl_is_ssd(const struct toplevel *tl) {
  return tl->ssd || (tl->xs && tl->xs->surface && tl->xs->surface->kde_ssd);
}

static void toplevel_send_configure(struct toplevel *tl, int w, int h) {
  // El cliente cuenta su propia decoracion dentro de la geometria: si se
  // recorta, hay que pedirle esa parte de mas.
  if (w > 0 && h > 0 && tl->inset_state == 1) {
    w += tl->in_l + tl->in_r;
    h += tl->in_t + tl->in_b;
  }
  struct wl_array states;
  wl_array_init(&states);
  if (!tl_is_ssd(tl) && wl_resource_get_version(tl->res) >= 2) {
    static const uint32_t edges[4] = {
        XDG_TOPLEVEL_STATE_TILED_LEFT, XDG_TOPLEVEL_STATE_TILED_RIGHT,
        XDG_TOPLEVEL_STATE_TILED_TOP, XDG_TOPLEVEL_STATE_TILED_BOTTOM};
    for (int i = 0; i < 4; i++) {
      uint32_t *e = wl_array_add(&states, sizeof(uint32_t));
      if (e)
        *e = edges[i];
    }
  }
  if (tl->activated) {
    uint32_t *s = wl_array_add(&states, sizeof(uint32_t));
    *s = XDG_TOPLEVEL_STATE_ACTIVATED;
  }
  if (tl->maximized) {
    uint32_t *s = wl_array_add(&states, sizeof(uint32_t));
    *s = XDG_TOPLEVEL_STATE_MAXIMIZED;
  }
  xdg_toplevel_send_configure(tl->res, w, h, &states);
  wl_array_release(&states);
  uint32_t serial = wl_display_next_serial(g_display);
  tl->xs->last_serial = serial;
  xdg_surface_send_configure(tl->xs->res, serial);
  tl->xs->configured_sent = 1;
}

// ---------------------------------------------------------------------------
// Portapapeles (wl_data_device_manager)
// ---------------------------------------------------------------------------
static void offer_receive(struct wl_client *c, struct wl_resource *r,
                          const char *mime, int32_t fd) {
  (void)c;
  struct data_source *src = wl_resource_get_user_data(r);
  if (src && src == g_selection) /* solo se compara: la fuente puede haber muerto */
    wl_data_source_send_send(src->res, mime, fd);
  close(fd);
}
static void noop_destroy(struct wl_client *c, struct wl_resource *r) {
  (void)c;
  wl_resource_destroy(r);
}
static void offer_accept(struct wl_client *c, struct wl_resource *r,
                         uint32_t serial, const char *mime) {
  (void)c; (void)r; (void)serial; (void)mime;
}
static void offer_finish(struct wl_client *c, struct wl_resource *r) {
  (void)c; (void)r;
}
static void offer_set_actions(struct wl_client *c, struct wl_resource *r,
                              uint32_t a, uint32_t p) {
  (void)c; (void)r; (void)a; (void)p;
}
static const struct wl_data_offer_interface offer_impl = {
    .accept = offer_accept,
    .receive = offer_receive,
    .destroy = noop_destroy,
    .finish = offer_finish,
    .set_actions = offer_set_actions,
};

// Envia la seleccion actual a los data_device del cliente `client`.
static void selection_send_to(struct wl_client *client) {
  struct seat_res *d;
  wl_list_for_each(d, &g_datadevs, link) {
    if (wl_resource_get_client(d->res) != client)
      continue;
    if (!g_selection) {
      wl_data_device_send_selection(d->res, NULL);
      continue;
    }
    struct wl_resource *offer = wl_resource_create(
        client, &wl_data_offer_interface, wl_resource_get_version(d->res), 0);
    if (!offer)
      continue;
    wl_resource_set_implementation(offer, &offer_impl, g_selection, NULL);
    wl_data_device_send_data_offer(d->res, offer);
    const char *p = g_selection->mimes.data;
    size_t left = g_selection->mimes.size;
    while (left > 0) {
      size_t n = strnlen(p, left);
      wl_data_offer_send_offer(offer, p);
      p += n + 1;
      left -= (n + 1 > left) ? left : n + 1;
    }
    wl_data_device_send_selection(d->res, offer);
  }
}
static void selection_send_to_focus(void) {
  if (g_kbd_focus)
    selection_send_to(wl_resource_get_client(g_kbd_focus->res));
}

static void ds_offer(struct wl_client *c, struct wl_resource *r,
                     const char *mime) {
  (void)c;
  struct data_source *s = wl_resource_get_user_data(r);
  size_t n = strlen(mime) + 1;
  char *p = wl_array_add(&s->mimes, n);
  if (p)
    memcpy(p, mime, n);
}
static void ds_set_actions(struct wl_client *c, struct wl_resource *r,
                           uint32_t a) {
  (void)c; (void)r; (void)a;
}
static const struct wl_data_source_interface ds_impl = {
    .offer = ds_offer,
    .destroy = noop_destroy,
    .set_actions = ds_set_actions,
};
static void ds_resource_destroy(struct wl_resource *r) {
  struct data_source *s = wl_resource_get_user_data(r);
  if (g_selection == s) {
    g_selection = NULL;
    selection_send_to_focus();
  }
  wl_array_release(&s->mimes);
  free(s);
}
static void dm_create_source(struct wl_client *c, struct wl_resource *r,
                             uint32_t id) {
  struct data_source *s = calloc(1, sizeof(*s));
  if (!s) {
    wl_client_post_no_memory(c);
    return;
  }
  wl_array_init(&s->mimes);
  s->res = wl_resource_create(c, &wl_data_source_interface,
                              wl_resource_get_version(r), id);
  if (!s->res) {
    free(s);
    wl_client_post_no_memory(c);
    return;
  }
  wl_resource_set_implementation(s->res, &ds_impl, s, ds_resource_destroy);
}
static void dd_start_drag(struct wl_client *c, struct wl_resource *r,
                          struct wl_resource *src, struct wl_resource *origin,
                          struct wl_resource *icon, uint32_t serial) {
  (void)c; (void)r; (void)src; (void)origin; (void)icon; (void)serial;
}
static void dd_set_selection(struct wl_client *c, struct wl_resource *r,
                             struct wl_resource *src, uint32_t serial) {
  (void)r; (void)serial;
  struct data_source *s = src ? wl_resource_get_user_data(src) : NULL;
  g_selection = s;
  (void)c;
  selection_send_to_focus();
}
static const struct wl_data_device_interface dd_impl = {
    .start_drag = dd_start_drag,
    .set_selection = dd_set_selection,
    .release = noop_destroy,
};
static void seat_res_destroy(struct wl_resource *r) {
  struct seat_res *s = wl_resource_get_user_data(r);
  wl_list_remove(&s->link);
  free(s);
}
static struct seat_res *seat_res_add(struct wl_list *list,
                                     struct wl_resource *r) {
  struct seat_res *s = calloc(1, sizeof(*s));
  if (!s)
    return NULL;
  s->res = r;
  wl_list_insert(list, &s->link);
  return s;
}
static void dm_get_device(struct wl_client *c, struct wl_resource *r,
                          uint32_t id, struct wl_resource *seat) {
  (void)seat;
  struct wl_resource *dd = wl_resource_create(
      c, &wl_data_device_interface, wl_resource_get_version(r), id);
  if (!dd) {
    wl_client_post_no_memory(c);
    return;
  }
  struct seat_res *s = seat_res_add(&g_datadevs, dd);
  wl_resource_set_implementation(dd, &dd_impl, s, seat_res_destroy);
  if (g_kbd_focus && wl_resource_get_client(g_kbd_focus->res) == c)
    selection_send_to(c);
}
static const struct wl_data_device_manager_interface dm_impl = {
    .create_data_source = dm_create_source,
    .get_data_device = dm_get_device,
};
static void dm_bind(struct wl_client *c, void *d, uint32_t v, uint32_t id) {
  (void)d;
  struct wl_resource *r =
      wl_resource_create(c, &wl_data_device_manager_interface, v, id);
  if (!r) {
    wl_client_post_no_memory(c);
    return;
  }
  wl_resource_set_implementation(r, &dm_impl, NULL, NULL);
}

// ---------------------------------------------------------------------------
// wl_region (no se usa: aceptamos y olvidamos)
// ---------------------------------------------------------------------------
static void region_rect(struct wl_client *c, struct wl_resource *r, int32_t x,
                        int32_t y, int32_t w, int32_t h) {
  (void)c; (void)r; (void)x; (void)y; (void)w; (void)h;
}
static const struct wl_region_interface region_impl = {
    .destroy = noop_destroy,
    .add = region_rect,
    .subtract = region_rect,
};

// ---------------------------------------------------------------------------
// wl_surface
// ---------------------------------------------------------------------------
static void surface_clear_buffer(struct surface *s) {
  if (s->buffer) {
    wl_list_remove(&s->buffer_destroy.link);
    wl_list_init(&s->buffer_destroy.link);
    s->buffer = NULL;
  }
}
static void surface_buffer_gone(struct wl_listener *l, void *data) {
  (void)data;
  struct surface *s = wl_container_of(l, s, buffer_destroy);
  wl_list_remove(&s->buffer_destroy.link);
  wl_list_init(&s->buffer_destroy.link);
  s->buffer = NULL;
}
static void surface_destroy_req(struct wl_client *c, struct wl_resource *r) {
  (void)c;
  wl_resource_destroy(r);
}
static void surface_attach(struct wl_client *c, struct wl_resource *r,
                           struct wl_resource *buf, int32_t x, int32_t y) {
  (void)c; (void)x; (void)y;
  struct surface *s = wl_resource_get_user_data(r);
  surface_clear_buffer(s);
  s->buffer = buf;
  s->has_attach = 1;
  if (buf) {
    s->buffer_destroy.notify = surface_buffer_gone;
    wl_resource_add_destroy_listener(buf, &s->buffer_destroy);
  }
}
static void surface_add_damage(struct surface *s, int x, int y, int w, int h) {
  if (w <= 0 || h <= 0)
    return;
  if (s->dmg_x1 <= s->dmg_x0) {
    s->dmg_x0 = x; s->dmg_y0 = y; s->dmg_x1 = x + w; s->dmg_y1 = y + h;
    return;
  }
  if (x < s->dmg_x0) s->dmg_x0 = x;
  if (y < s->dmg_y0) s->dmg_y0 = y;
  if (x + w > s->dmg_x1) s->dmg_x1 = x + w;
  if (y + h > s->dmg_y1) s->dmg_y1 = y + h;
}
static void surface_damage(struct wl_client *c, struct wl_resource *r,
                           int32_t x, int32_t y, int32_t w, int32_t h) {
  (void)c;
  surface_add_damage(wl_resource_get_user_data(r), x, y, w, h);
}
static void surface_damage_buffer(struct wl_client *c, struct wl_resource *r,
                                  int32_t x, int32_t y, int32_t w, int32_t h) {
  (void)c;
  surface_add_damage(wl_resource_get_user_data(r), x, y, w, h);
}
static void callback_destroy(struct wl_resource *r) {
  wl_list_remove(wl_resource_get_link(r));
}
static void surface_frame(struct wl_client *c, struct wl_resource *r,
                          uint32_t id) {
  struct surface *s = wl_resource_get_user_data(r);
  struct wl_resource *cb = wl_resource_create(c, &wl_callback_interface, 1, id);
  if (!cb) {
    wl_client_post_no_memory(c);
    return;
  }
  wl_resource_set_implementation(cb, NULL, NULL, callback_destroy);
  wl_list_insert(s->frames.prev, wl_resource_get_link(cb));
}
static void surface_noop_region(struct wl_client *c, struct wl_resource *r,
                                struct wl_resource *region) {
  (void)c; (void)r; (void)region;
}
static void surface_set_transform(struct wl_client *c, struct wl_resource *r,
                                  int32_t t) {
  (void)c; (void)r; (void)t;
}
static void surface_set_scale(struct wl_client *c, struct wl_resource *r,
                              int32_t sc) {
  (void)c; (void)r; (void)sc;
}
static void surface_offset(struct wl_client *c, struct wl_resource *r,
                           int32_t x, int32_t y) {
  (void)c; (void)r; (void)x; (void)y;
}

static void xs_unmap(struct xsurf *xs) {
  if (xs->win_id < 0)
    return;
  if (g_ptr_focus == xs)
    g_ptr_focus = NULL;
  win_destroy(xs->win_id);
  xs->win_id = -1;
  wl_list_remove(&xs->wlink);
}

// Crea la ventana de Aurora de un toplevel (ventana normal con barra de
// titulo) o de un popup (sin decoracion, sin Dock, sin foco).
static void xs_map(struct xsurf *xs, int w, int h) {
  int id;
  if (xs->is_popup) {
    if (w < 8) w = 8;
    if (h < 8) h = 8;
    if (!xs->parent || xs->parent->win_id < 0)
      return;
    xs->wx = xs->parent->wx + xs->px;
    xs->wy = xs->parent->wy + xs->py;
    // La posicion del popup se limita a la pantalla (slide).
    if (xs->wx + w > OUT_W) xs->wx = OUT_W - w;
    if (xs->wy + h > OUT_H) xs->wy = OUT_H - h;
    if (xs->wx < 0) xs->wx = 0;
    if (xs->wy < 0) xs->wy = 0;
    id = win_create_ex(xs->wx, xs->wy, w, h, "popup",
                       WIN_FLAGS_NODECORATION | WIN_FLAGS_FIXED_SIZE |
                           WIN_FLAGS_POPUP | WIN_FLAGS_RAW_INPUT);
  } else {
    if (w < WIN_MIN) w = WIN_MIN;
    if (h < WIN_MIN - TITLEBAR_H) h = WIN_MIN - TITLEBAR_H;
    if (w > WIN_MAX || h + TITLEBAR_H > WIN_MAX) {
      fprintf(stderr, "[aurora-wayland] ventana demasiado grande (%dx%d)\n", w, h);
      return;
    }
    // Cascada ancha: las barras de titulo de las ventanas que se van abriendo
    // quedan a la vista (y se distingue cada app).
    int n = g_win_count % 5;
    xs->wx = 80 + 260 * n;
    xs->wy = 60 + 50 * n + TITLEBAR_H;
    const char *title = xs->tl && xs->tl->title[0] ? xs->tl->title : "Wayland";
    struct toplevel *t = xs->tl;
    // min == max: el cliente no admite redimensionar (dialogos, calculadoras).
    int fixed = t && t->min_w > 0 && t->min_w == t->max_w && t->min_h > 0 &&
                t->min_h == t->max_h;
    id = win_create_ex(xs->wx, xs->wy - TITLEBAR_H, w, h + TITLEBAR_H, title,
                       WIN_FLAGS_RAW_INPUT | (fixed ? WIN_FLAGS_FIXED_SIZE : 0));
    g_win_count++;
  }
  if (id < 0) {
    fprintf(stderr, "[aurora-wayland] win_create fallo\n");
    return;
  }
  xs->win_id = id;
  if (xs->tl && xs->tl->app_id[0])
    win_set_app_id(id, xs->tl->app_id); /* agrupa la ventana en el Dock */
  xs->cw = w;
  xs->ch = h;
  wl_list_insert(&g_wins, &xs->wlink);
  wl_event_source_timer_update(g_pump_timer, 4);
  fprintf(stderr, "[aurora-wayland] %s '%s' -> ventana %d (%dx%d)\n",
          xs->is_popup ? "popup" : "toplevel",
          xs->tl ? xs->tl->title : "", id, w, h);
  if (xs->tl) {
    // Peticiones del cliente anteriores a la ventana.
    if (xs->tl->pending_max) {
      xs->tl->pending_max = 0;
      xs->tl->maximized = 1;
      int rc = win_set_state(id, WINSRV_STATE_MAXIMIZE);
      fprintf(stderr, "[aurora-wayland] maximize pendiente -> rc=%d\n", rc);
    }
    if (xs->tl->pending_min) {
      xs->tl->pending_min = 0;
      win_set_state(id, WINSRV_STATE_MINIMIZE);
    }
  }
}

static void surface_commit(struct wl_client *c, struct wl_resource *r) {
  (void)c;
  struct surface *s = wl_resource_get_user_data(r);
  struct xsurf *xs = s->xs;
  struct toplevel *tl = xs ? xs->tl : NULL;

  // Primer commit de un xdg_surface: hay que configurar antes de aceptar buffer.
  if (xs && !xs->configured_sent) {
    if (tl)
      toplevel_send_configure(tl, 0, 0);
    else if (xs->is_popup)
      xs->configured_sent = 1;
  }

  if (g_debug)
    fprintf(stderr, "[aurora-wayland] commit xs=%p attach=%d buf=%p dmg=(%d,%d)-(%d,%d)\n",
            (void *)xs, s->has_attach, (void *)s->buffer, s->dmg_x0, s->dmg_y0,
            s->dmg_x1, s->dmg_y1);
  if (s->has_attach) {
    s->has_attach = 0;
    if (s->buffer) {
      struct wl_shm_buffer *shm = wl_shm_buffer_get(s->buffer);
      if (shm && xs && (tl || xs->is_popup)) {
        int w = wl_shm_buffer_get_width(shm);
        int h = wl_shm_buffer_get_height(shm);
        int stride = wl_shm_buffer_get_stride(shm);
        // Area visible: la geometria de ventana (sin sombras CSD) o el buffer.
        int gx = 0, gy = 0, gw = w, gh = h;
        if (xs->has_geom) {
          gx = xs->gx; gy = xs->gy; gw = xs->gw; gh = xs->gh;
          if (gx < 0) { gw += gx; gx = 0; }
          if (gy < 0) { gh += gy; gy = 0; }
          if (gx + gw > w) gw = w - gx;
          if (gy + gh > h) gh = h - gy;
          if (gw <= 0 || gh <= 0) { gx = gy = 0; gw = w; gh = h; }
        }
        if (tl && !tl_is_ssd(tl)) {
          // Cliente que dibuja su propio marco: recortar barra/bordes.
          if (tl->inset_state == 0 && xs->has_geom) {
            int margins = gx > 0 || gy > 0 || gw < w || gh < h;
            tl->rule = csd_rule_find(tl);
            tl->inset_state = (tl->rule && margins) ? 1 : 2;
            fprintf(stderr, "[aurora-wayland] '%s' (app_id '%s') sin xdg-decoration: %s\n",
                    tl->title, tl->app_id,
                    tl->inset_state == 1 ? "recorte de su marco" : "se deja tal cual");
          }
          if (tl->inset_state == 1) {
            // Maximizada, el toolkit quita bordes laterales/inferior pero deja la barra.
            int l = tl->maximized ? 0 : tl->rule->l, r = tl->maximized ? 0 : tl->rule->r;
            int b = tl->maximized ? 0 : tl->rule->b, t = tl->rule->t;
            if (gw - l - r >= WIN_MIN && gh - t - b >= WIN_MIN - TITLEBAR_H) {
              gx += l; gy += t; gw -= l + r; gh -= t + b;
              tl->in_l = l; tl->in_t = t; tl->in_r = r; tl->in_b = b;
            }
          }
        }
        xs->vx = gx;
        xs->vy = gy;
        if (xs->win_id < 0)
          xs_map(xs, gw, gh);
        if (xs->win_id >= 0) {
          int full = (w != s->buf_w || h != s->buf_h || s->dmg_x1 <= s->dmg_x0);
          int x0 = full ? 0 : s->dmg_x0, y0 = full ? 0 : s->dmg_y0;
          int x1 = full ? w : s->dmg_x1, y1 = full ? h : s->dmg_y1;
          if (x0 < gx) x0 = gx;
          if (y0 < gy) y0 = gy;
          if (x1 > gx + gw) x1 = gx + gw;
          if (y1 > gy + gh) y1 = gy + gh;
          if (x1 > x0 && y1 > y0) {
            size_t need = (size_t)w * h;
            if (s->scratch_px < need) {
              free(s->scratch);
              s->scratch = malloc(need * 4);
              s->scratch_px = s->scratch ? need : 0;
              x0 = 0; y0 = 0; x1 = w; y1 = h; /* scratch nuevo: todo */
            }
            if (s->scratch) {
              wl_shm_buffer_begin_access(shm);
              const uint8_t *src = wl_shm_buffer_get_data(shm);
              for (int y = y0; y < y1; y++) {
                const uint32_t *row = (const uint32_t *)(src + (size_t)y * stride);
                uint32_t *dst = s->scratch + (size_t)y * w;
                for (int x = x0; x < x1; x++)
                  dst[x] = row[x] | 0xFF000000u;
              }
              wl_shm_buffer_end_access(shm);
              win_blit(xs->win_id, x0 - gx, y0 - gy, x0, y0, x1 - x0, y1 - y0, w,
                       s->scratch);
            }
          }
          s->buf_w = w;
          s->buf_h = h;
        }
      }
      // Ya copiamos el contenido: el cliente puede reutilizar el buffer.
      wl_buffer_send_release(s->buffer);
    }
  }
  s->dmg_x0 = s->dmg_y0 = s->dmg_x1 = s->dmg_y1 = 0;

  // Frame callbacks: todo lo pedido hasta aqui se considera presentado.
  uint32_t t = now_ms();
  struct wl_resource *cb, *tmp;
  wl_resource_for_each_safe(cb, tmp, &s->frames) {
    wl_callback_send_done(cb, t);
    wl_resource_destroy(cb);
  }
}

static const struct wl_surface_interface surface_impl = {
    .destroy = surface_destroy_req,
    .attach = surface_attach,
    .damage = surface_damage,
    .frame = surface_frame,
    .set_opaque_region = surface_noop_region,
    .set_input_region = surface_noop_region,
    .commit = surface_commit,
    .set_buffer_transform = surface_set_transform,
    .set_buffer_scale = surface_set_scale,
    .damage_buffer = surface_damage_buffer,
    .offset = surface_offset,
};

static void surface_resource_destroy(struct wl_resource *r) {
  struct surface *s = wl_resource_get_user_data(r);
  surface_clear_buffer(s);
  struct wl_resource *cb, *tmp;
  wl_resource_for_each_safe(cb, tmp, &s->frames) wl_resource_destroy(cb);
  if (s->xs)
    s->xs->surface = NULL;
  free(s->scratch);
  free(s);
}

static void comp_create_surface(struct wl_client *c, struct wl_resource *r,
                                uint32_t id) {
  struct surface *s = calloc(1, sizeof(*s));
  if (!s) {
    wl_client_post_no_memory(c);
    return;
  }
  s->res = wl_resource_create(c, &wl_surface_interface,
                              wl_resource_get_version(r), id);
  if (!s->res) {
    free(s);
    wl_client_post_no_memory(c);
    return;
  }
  wl_list_init(&s->frames);
  wl_list_init(&s->buffer_destroy.link);
  wl_resource_set_implementation(s->res, &surface_impl, s,
                                 surface_resource_destroy);
}
static void comp_create_region(struct wl_client *c, struct wl_resource *r,
                               uint32_t id) {
  struct wl_resource *reg = wl_resource_create(c, &wl_region_interface,
                                               wl_resource_get_version(r), id);
  if (!reg) {
    wl_client_post_no_memory(c);
    return;
  }
  wl_resource_set_implementation(reg, &region_impl, NULL, NULL);
}
static const struct wl_compositor_interface comp_impl = {
    .create_surface = comp_create_surface,
    .create_region = comp_create_region,
};
static void comp_bind(struct wl_client *c, void *d, uint32_t v, uint32_t id) {
  (void)d;
  struct wl_resource *r = wl_resource_create(c, &wl_compositor_interface, v, id);
  if (!r) {
    wl_client_post_no_memory(c);
    return;
  }
  wl_resource_set_implementation(r, &comp_impl, NULL, NULL);
}

// ---------------------------------------------------------------------------
// wl_subcompositor: aceptado; las subsuperficies aun no se componen.
// ---------------------------------------------------------------------------
static void sub_set_position(struct wl_client *c, struct wl_resource *r,
                             int32_t x, int32_t y) {
  (void)c; (void)r; (void)x; (void)y;
}
static void sub_place(struct wl_client *c, struct wl_resource *r,
                      struct wl_resource *sib) {
  (void)c; (void)r; (void)sib;
}
static void sub_mode(struct wl_client *c, struct wl_resource *r) {
  (void)c; (void)r;
}
static const struct wl_subsurface_interface subsurface_impl = {
    .destroy = noop_destroy,
    .set_position = sub_set_position,
    .place_above = sub_place,
    .place_below = sub_place,
    .set_sync = sub_mode,
    .set_desync = sub_mode,
};
static void subc_get_subsurface(struct wl_client *c, struct wl_resource *r,
                                uint32_t id, struct wl_resource *surf,
                                struct wl_resource *parent) {
  (void)surf; (void)parent;
  struct wl_resource *ss = wl_resource_create(c, &wl_subsurface_interface,
                                              wl_resource_get_version(r), id);
  if (!ss) {
    wl_client_post_no_memory(c);
    return;
  }
  wl_resource_set_implementation(ss, &subsurface_impl, NULL, NULL);
}
static const struct wl_subcompositor_interface subc_impl = {
    .destroy = noop_destroy,
    .get_subsurface = subc_get_subsurface,
};
static void subc_bind(struct wl_client *c, void *d, uint32_t v, uint32_t id) {
  (void)d;
  struct wl_resource *r =
      wl_resource_create(c, &wl_subcompositor_interface, v, id);
  if (!r) {
    wl_client_post_no_memory(c);
    return;
  }
  wl_resource_set_implementation(r, &subc_impl, NULL, NULL);
}

// ---------------------------------------------------------------------------
// wl_output
// ---------------------------------------------------------------------------
static void output_release(struct wl_client *c, struct wl_resource *r) {
  (void)c;
  wl_resource_destroy(r);
}
static const struct wl_output_interface output_impl = {.release = output_release};
static void output_bind(struct wl_client *c, void *d, uint32_t v, uint32_t id) {
  (void)d;
  struct wl_resource *r = wl_resource_create(c, &wl_output_interface, v, id);
  if (!r) {
    wl_client_post_no_memory(c);
    return;
  }
  wl_resource_set_implementation(r, &output_impl, NULL, NULL);
  wl_output_send_geometry(r, 0, 0, 340, 210, WL_OUTPUT_SUBPIXEL_UNKNOWN,
                          "Aurora", "Display", WL_OUTPUT_TRANSFORM_NORMAL);
  wl_output_send_mode(r, WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED,
                      OUT_W, OUT_H, 60000);
  if (v >= 2) {
    wl_output_send_scale(r, 1);
    wl_output_send_done(r);
  }
}

// ---------------------------------------------------------------------------
// wl_seat: puntero y teclado
// ---------------------------------------------------------------------------
static void kbd_focus_set(struct toplevel *tl);

static void send_modifiers_to(struct wl_client *client) {
  uint32_t dep = xkb_state_serialize_mods(g_xkb_state, XKB_STATE_MODS_DEPRESSED);
  uint32_t lat = xkb_state_serialize_mods(g_xkb_state, XKB_STATE_MODS_LATCHED);
  uint32_t lck = xkb_state_serialize_mods(g_xkb_state, XKB_STATE_MODS_LOCKED);
  uint32_t grp = xkb_state_serialize_layout(g_xkb_state, XKB_STATE_LAYOUT_EFFECTIVE);
  uint32_t serial = wl_display_next_serial(g_display);
  struct seat_res *k;
  wl_list_for_each(k, &g_keyboards, link) {
    if (wl_resource_get_client(k->res) == client)
      wl_keyboard_send_modifiers(k->res, serial, dep, lat, lck, grp);
  }
}

static void kbd_focus_set(struct toplevel *tl) {
  if (g_debug)
    fprintf(stderr, "[aurora-wayland] kbd focus -> %s (kbds=%d)\n",
            tl ? tl->title : "(nadie)", wl_list_length(&g_keyboards));
  if (tl == g_kbd_focus)
    return;
  if (g_kbd_focus) {
    struct wl_client *oc = wl_resource_get_client(g_kbd_focus->res);
    struct wl_resource *surf = g_kbd_focus->xs->surface ? g_kbd_focus->xs->surface->res : NULL;
    uint32_t serial = wl_display_next_serial(g_display);
    struct seat_res *k;
    if (surf)
      wl_list_for_each(k, &g_keyboards, link)
        if (wl_resource_get_client(k->res) == oc)
          wl_keyboard_send_leave(k->res, serial, surf);
    g_kbd_focus->activated = 0;
    toplevel_send_configure(g_kbd_focus, g_kbd_focus->xs->cw, g_kbd_focus->xs->ch);
  }
  g_kbd_focus = tl;
  if (!tl)
    return;
  struct wl_client *nc = wl_resource_get_client(tl->res);
  struct wl_resource *surf = tl->xs->surface ? tl->xs->surface->res : NULL;
  tl->activated = 1;
  toplevel_send_configure(tl, tl->xs->cw, tl->xs->ch);
  if (surf) {
    uint32_t serial = wl_display_next_serial(g_display);
    struct wl_array keys;
    wl_array_init(&keys);
    struct seat_res *k;
    wl_list_for_each(k, &g_keyboards, link)
      if (wl_resource_get_client(k->res) == nc)
        wl_keyboard_send_enter(k->res, serial, surf, &keys);
    wl_array_release(&keys);
    send_modifiers_to(nc);
  }
  selection_send_to(nc);
}

static void ptr_set_cursor(struct wl_client *c, struct wl_resource *r,
                           uint32_t serial, struct wl_resource *surf,
                           int32_t hx, int32_t hy) {
  (void)c; (void)r; (void)serial; (void)surf; (void)hx; (void)hy;
}
static const struct wl_pointer_interface pointer_impl = {
    .set_cursor = ptr_set_cursor,
    .release = noop_destroy,
};
static const struct wl_keyboard_interface keyboard_impl = {
    .release = noop_destroy,
};
static void seat_get_pointer(struct wl_client *c, struct wl_resource *r,
                             uint32_t id) {
  struct wl_resource *p = wl_resource_create(c, &wl_pointer_interface,
                                             wl_resource_get_version(r), id);
  if (!p) {
    wl_client_post_no_memory(c);
    return;
  }
  struct seat_res *s = seat_res_add(&g_pointers, p);
  wl_resource_set_implementation(p, &pointer_impl, s, seat_res_destroy);
}
static void seat_get_keyboard(struct wl_client *c, struct wl_resource *r,
                              uint32_t id) {
  struct wl_resource *k = wl_resource_create(c, &wl_keyboard_interface,
                                             wl_resource_get_version(r), id);
  if (!k) {
    wl_client_post_no_memory(c);
    return;
  }
  struct seat_res *s = seat_res_add(&g_keyboards, k);
  wl_resource_set_implementation(k, &keyboard_impl, s, seat_res_destroy);
  if (g_keymap_fd >= 0)
    wl_keyboard_send_keymap(k, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, g_keymap_fd,
                            (uint32_t)g_keymap_size);
  if (wl_resource_get_version(k) >= 4)
    wl_keyboard_send_repeat_info(k, 25, 400);
  // Si ya habia foco en la ventana de este cliente, entra ahora tambien.
  if (g_kbd_focus && wl_resource_get_client(g_kbd_focus->res) == c &&
      g_kbd_focus->xs->surface) {
    struct wl_array keys;
    wl_array_init(&keys);
    wl_keyboard_send_enter(k, wl_display_next_serial(g_display),
                           g_kbd_focus->xs->surface->res, &keys);
    wl_array_release(&keys);
    send_modifiers_to(c);
  }
}
static void seat_get_touch(struct wl_client *c, struct wl_resource *r,
                           uint32_t id) {
  (void)c; (void)r; (void)id;
  wl_resource_post_error(r, WL_SEAT_ERROR_MISSING_CAPABILITY, "sin touch");
}
static const struct wl_seat_interface seat_impl = {
    .get_pointer = seat_get_pointer,
    .get_keyboard = seat_get_keyboard,
    .get_touch = seat_get_touch,
    .release = noop_destroy,
};
static void seat_bind(struct wl_client *c, void *d, uint32_t v, uint32_t id) {
  (void)d;
  struct wl_resource *r = wl_resource_create(c, &wl_seat_interface, v, id);
  if (!r) {
    wl_client_post_no_memory(c);
    return;
  }
  wl_resource_set_implementation(r, &seat_impl, NULL, NULL);
  wl_seat_send_capabilities(r, WL_SEAT_CAPABILITY_POINTER |
                                   WL_SEAT_CAPABILITY_KEYBOARD);
  if (v >= 2)
    wl_seat_send_name(r, "seat0");
}

// ---------------------------------------------------------------------------
// xdg-shell
// ---------------------------------------------------------------------------
struct positioner {
  int w, h;
  int ax, ay, aw, ah; /* anchor rect */
  int offx, offy;
  uint32_t anchor, gravity;
};
static void pos_destroy(struct wl_client *c, struct wl_resource *r) { noop_destroy(c, r); }
// Posicion del popup respecto a la geometria del padre (xdg_positioner).
static void popup_place(const struct positioner *p, int *x, int *y) {
  int ax, ay;
  switch (p->anchor) {
  case XDG_POSITIONER_ANCHOR_TOP_LEFT: case XDG_POSITIONER_ANCHOR_LEFT:
  case XDG_POSITIONER_ANCHOR_BOTTOM_LEFT: ax = p->ax; break;
  case XDG_POSITIONER_ANCHOR_TOP_RIGHT: case XDG_POSITIONER_ANCHOR_RIGHT:
  case XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT: ax = p->ax + p->aw; break;
  default: ax = p->ax + p->aw / 2; break;
  }
  switch (p->anchor) {
  case XDG_POSITIONER_ANCHOR_TOP_LEFT: case XDG_POSITIONER_ANCHOR_TOP:
  case XDG_POSITIONER_ANCHOR_TOP_RIGHT: ay = p->ay; break;
  case XDG_POSITIONER_ANCHOR_BOTTOM_LEFT: case XDG_POSITIONER_ANCHOR_BOTTOM:
  case XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT: ay = p->ay + p->ah; break;
  default: ay = p->ay + p->ah / 2; break;
  }
  int gx, gy;
  switch (p->gravity) {
  case XDG_POSITIONER_GRAVITY_TOP_LEFT: case XDG_POSITIONER_GRAVITY_LEFT:
  case XDG_POSITIONER_GRAVITY_BOTTOM_LEFT: gx = ax - p->w; break;
  case XDG_POSITIONER_GRAVITY_TOP_RIGHT: case XDG_POSITIONER_GRAVITY_RIGHT:
  case XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT: gx = ax; break;
  default: gx = ax - p->w / 2; break;
  }
  switch (p->gravity) {
  case XDG_POSITIONER_GRAVITY_TOP_LEFT: case XDG_POSITIONER_GRAVITY_TOP:
  case XDG_POSITIONER_GRAVITY_TOP_RIGHT: gy = ay - p->h; break;
  case XDG_POSITIONER_GRAVITY_BOTTOM_LEFT: case XDG_POSITIONER_GRAVITY_BOTTOM:
  case XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT: gy = ay; break;
  default: gy = ay - p->h / 2; break;
  }
  *x = gx + p->offx;
  *y = gy + p->offy;
}
static void pos_set_size(struct wl_client *c, struct wl_resource *r, int32_t w, int32_t h) {
  (void)c; struct positioner *p = wl_resource_get_user_data(r); p->w = w; p->h = h;
}
static void pos_set_anchor_rect(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y, int32_t w, int32_t h) {
  (void)c; struct positioner *p = wl_resource_get_user_data(r);
  p->ax = x; p->ay = y; p->aw = w; p->ah = h;
}
static void pos_set_anchor(struct wl_client *c, struct wl_resource *r, uint32_t a) {
  (void)c; ((struct positioner *)wl_resource_get_user_data(r))->anchor = a;
}
static void pos_set_gravity(struct wl_client *c, struct wl_resource *r, uint32_t g) {
  (void)c; ((struct positioner *)wl_resource_get_user_data(r))->gravity = g;
}
static void pos_set_constraint(struct wl_client *c, struct wl_resource *r, uint32_t a) {
  (void)c; (void)r; (void)a;
}
static void pos_set_offset(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y) {
  (void)c; struct positioner *p = wl_resource_get_user_data(r); p->offx = x; p->offy = y;
}
static void pos_set_reactive(struct wl_client *c, struct wl_resource *r) { (void)c; (void)r; }
static void pos_set_parent_size(struct wl_client *c, struct wl_resource *r, int32_t w, int32_t h) {
  (void)c; (void)r; (void)w; (void)h;
}
static void pos_set_parent_configure(struct wl_client *c, struct wl_resource *r, uint32_t s) {
  (void)c; (void)r; (void)s;
}
static const struct xdg_positioner_interface positioner_impl = {
    .destroy = pos_destroy,
    .set_size = pos_set_size,
    .set_anchor_rect = pos_set_anchor_rect,
    .set_anchor = pos_set_anchor,
    .set_gravity = pos_set_gravity,
    .set_constraint_adjustment = pos_set_constraint,
    .set_offset = pos_set_offset,
    .set_reactive = pos_set_reactive,
    .set_parent_size = pos_set_parent_size,
    .set_parent_configure = pos_set_parent_configure,
};
static void pos_resource_destroy(struct wl_resource *r) {
  free(wl_resource_get_user_data(r));
}

// xdg_toplevel
static void tl_set_title(struct wl_client *c, struct wl_resource *r, const char *t) {
  (void)c;
  struct toplevel *tl = wl_resource_get_user_data(r);
  snprintf(tl->title, sizeof(tl->title), "%s", t);
  if (tl->xs && tl->xs->win_id >= 0)
    win_set_title(tl->xs->win_id, tl->title);
}
static void tl_set_app_id(struct wl_client *c, struct wl_resource *r, const char *t) {
  (void)c;
  struct toplevel *tl = wl_resource_get_user_data(r);
  snprintf(tl->app_id, sizeof(tl->app_id), "%s", t);
  if (tl->xs && tl->xs->win_id >= 0)
    win_set_app_id(tl->xs->win_id, tl->app_id);
}
static void tl_set_parent(struct wl_client *c, struct wl_resource *r, struct wl_resource *p) {
  (void)c; (void)r; (void)p;
}
static void tl_show_menu(struct wl_client *c, struct wl_resource *r, struct wl_resource *seat, uint32_t serial, int32_t x, int32_t y) {
  (void)c; (void)r; (void)seat; (void)serial; (void)x; (void)y;
}
static void tl_move(struct wl_client *c, struct wl_resource *r, struct wl_resource *seat, uint32_t serial) {
  (void)c; (void)r; (void)seat; (void)serial;
}
static void tl_resize(struct wl_client *c, struct wl_resource *r, struct wl_resource *seat, uint32_t serial, uint32_t edges) {
  (void)c; (void)r; (void)seat; (void)serial; (void)edges;
}
static void tl_set_min_size(struct wl_client *c, struct wl_resource *r, int32_t w, int32_t h) {
  (void)c;
  struct toplevel *tl = wl_resource_get_user_data(r);
  tl->min_w = w; tl->min_h = h;
}
static void tl_set_max_size(struct wl_client *c, struct wl_resource *r, int32_t w, int32_t h) {
  (void)c;
  struct toplevel *tl = wl_resource_get_user_data(r);
  tl->max_w = w; tl->max_h = h;
}
static void tl_nop(struct wl_client *c, struct wl_resource *r) { (void)c; (void)r; }
static void tl_set_maximized(struct wl_client *c, struct wl_resource *r) {
  (void)c;
  struct toplevel *tl = wl_resource_get_user_data(r);
  fprintf(stderr, "[aurora-wayland] set_maximized win=%d\n",
          tl->xs ? tl->xs->win_id : -2);
  if (tl->xs && tl->xs->win_id >= 0) {
    tl->maximized = 1;
    win_set_state(tl->xs->win_id, WINSRV_STATE_MAXIMIZE);
    // El configure con el nuevo tamano llega con WINSRV_EV_RESIZE; si la
    // ventana ya estaba maximizada no hay resize, asi que se anuncia ya.
    toplevel_send_configure(tl, tl->xs->cw, tl->xs->ch);
  } else {
    tl->pending_max = 1;
  }
}
static void tl_unset_maximized(struct wl_client *c, struct wl_resource *r) {
  (void)c;
  struct toplevel *tl = wl_resource_get_user_data(r);
  tl->pending_max = 0;
  if (tl->xs && tl->xs->win_id >= 0 && tl->maximized) {
    tl->maximized = 0;
    win_set_state(tl->xs->win_id, WINSRV_STATE_RESTORE);
  }
}
static void tl_set_minimized(struct wl_client *c, struct wl_resource *r) {
  (void)c;
  struct toplevel *tl = wl_resource_get_user_data(r);
  if (tl->xs && tl->xs->win_id >= 0)
    win_set_state(tl->xs->win_id, WINSRV_STATE_MINIMIZE);
  else
    tl->pending_min = 1;
}
static void tl_set_fullscreen(struct wl_client *c, struct wl_resource *r, struct wl_resource *o) {
  (void)c; (void)r; (void)o;
}
static const struct xdg_toplevel_interface toplevel_impl = {
    .destroy = noop_destroy,
    .set_parent = tl_set_parent,
    .set_title = tl_set_title,
    .set_app_id = tl_set_app_id,
    .show_window_menu = tl_show_menu,
    .move = tl_move,
    .resize = tl_resize,
    .set_max_size = tl_set_max_size,
    .set_min_size = tl_set_min_size,
    .set_maximized = tl_set_maximized,
    .unset_maximized = tl_unset_maximized,
    .set_fullscreen = tl_set_fullscreen,
    .unset_fullscreen = tl_nop,
    .set_minimized = tl_set_minimized,
};
static void toplevel_resource_destroy(struct wl_resource *r) {
  struct toplevel *tl = wl_resource_get_user_data(r);
  if (g_kbd_focus == tl)
    g_kbd_focus = NULL;
  if (tl->xs) {
    xs_unmap(tl->xs);
    tl->xs->tl = NULL;
  }
  wl_list_remove(&tl->link);
  free(tl);
}

// xdg_popup: ventana sin decoracion de Aurora, posicionada sobre su padre.
static void popup_grab(struct wl_client *c, struct wl_resource *r, struct wl_resource *seat, uint32_t serial) {
  (void)c; (void)seat; (void)serial;
  struct xsurf *xs = wl_resource_get_user_data(r);
  xs->grabbed = 1;
}
static void popup_resource_destroy(struct wl_resource *r) {
  struct xsurf *xs = wl_resource_get_user_data(r);
  xs->popup_res = NULL;
  xs->is_popup = 0;
  xs_unmap(xs);
}
static void popup_reposition(struct wl_client *c, struct wl_resource *r, struct wl_resource *pos, uint32_t token) {
  (void)c; (void)r; (void)pos; (void)token;
}
static const struct xdg_popup_interface popup_impl = {
    .destroy = noop_destroy,
    .grab = popup_grab,
    .reposition = popup_reposition,
};

// xdg_surface
static void xs_get_toplevel(struct wl_client *c, struct wl_resource *r, uint32_t id) {
  struct xsurf *xs = wl_resource_get_user_data(r);
  struct toplevel *tl = calloc(1, sizeof(*tl));
  if (!tl) {
    wl_client_post_no_memory(c);
    return;
  }
  tl->res = wl_resource_create(c, &xdg_toplevel_interface,
                               wl_resource_get_version(r), id);
  if (!tl->res) {
    free(tl);
    wl_client_post_no_memory(c);
    return;
  }
  tl->xs = xs;
  xs->tl = tl;
  wl_list_insert(&g_toplevels, &tl->link);
  wl_resource_set_implementation(tl->res, &toplevel_impl, tl,
                                 toplevel_resource_destroy);
}
static void xs_get_popup(struct wl_client *c, struct wl_resource *r, uint32_t id,
                         struct wl_resource *parent, struct wl_resource *pos) {
  struct xsurf *xs = wl_resource_get_user_data(r);
  struct positioner *p = wl_resource_get_user_data(pos);
  struct wl_resource *pop = wl_resource_create(c, &xdg_popup_interface,
                                               wl_resource_get_version(r), id);
  if (!pop) {
    wl_client_post_no_memory(c);
    return;
  }
  wl_resource_set_implementation(pop, &popup_impl, xs, popup_resource_destroy);
  xs->is_popup = 1;
  xs->popup_res = pop;
  xs->parent = parent ? wl_resource_get_user_data(parent) : NULL;
  popup_place(p, &xs->px, &xs->py);
  xs->pw = p->w;
  xs->ph = p->h;
  xdg_popup_send_configure(pop, xs->px, xs->py, p->w, p->h);
  uint32_t serial = wl_display_next_serial(g_display);
  xs->last_serial = serial;
  xdg_surface_send_configure(r, serial);
  xs->configured_sent = 1;
}
static void xs_set_geometry(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y, int32_t w, int32_t h) {
  (void)c;
  struct xsurf *xs = wl_resource_get_user_data(r);
  if (w <= 0 || h <= 0)
    return;
  xs->has_geom = 1;
  xs->gx = x; xs->gy = y; xs->gw = w; xs->gh = h;
}
static void xs_ack_configure(struct wl_client *c, struct wl_resource *r, uint32_t serial) {
  (void)c;
  struct xsurf *xs = wl_resource_get_user_data(r);
  if (serial == xs->last_serial)
    xs->acked = 1;
}
static const struct xdg_surface_interface xdg_surface_impl = {
    .destroy = noop_destroy,
    .get_toplevel = xs_get_toplevel,
    .get_popup = xs_get_popup,
    .set_window_geometry = xs_set_geometry,
    .ack_configure = xs_ack_configure,
};
static void xs_resource_destroy(struct wl_resource *r) {
  struct xsurf *xs = wl_resource_get_user_data(r);
  xs_unmap(xs);
  struct xsurf *o;
  wl_list_for_each(o, &g_wins, wlink)
    if (o->parent == xs)
      o->parent = NULL;
  if (xs->surface)
    xs->surface->xs = NULL;
  if (xs->tl)
    xs->tl->xs = NULL;
  free(xs);
}

// xdg_wm_base
static void wm_create_positioner(struct wl_client *c, struct wl_resource *r, uint32_t id) {
  struct positioner *p = calloc(1, sizeof(*p));
  struct wl_resource *pr = p ? wl_resource_create(c, &xdg_positioner_interface,
                                                  wl_resource_get_version(r), id)
                             : NULL;
  if (!pr) {
    free(p);
    wl_client_post_no_memory(c);
    return;
  }
  wl_resource_set_implementation(pr, &positioner_impl, p, pos_resource_destroy);
}
static void wm_get_xdg_surface(struct wl_client *c, struct wl_resource *r,
                               uint32_t id, struct wl_resource *surf) {
  struct surface *s = wl_resource_get_user_data(surf);
  struct xsurf *xs = calloc(1, sizeof(*xs));
  if (!xs) {
    wl_client_post_no_memory(c);
    return;
  }
  xs->res = wl_resource_create(c, &xdg_surface_interface,
                               wl_resource_get_version(r), id);
  if (!xs->res) {
    free(xs);
    wl_client_post_no_memory(c);
    return;
  }
  xs->surface = s;
  xs->win_id = -1;
  s->xs = xs;
  wl_resource_set_implementation(xs->res, &xdg_surface_impl, xs,
                                 xs_resource_destroy);
}
static void wm_pong(struct wl_client *c, struct wl_resource *r, uint32_t serial) {
  (void)c; (void)r; (void)serial;
}
static const struct xdg_wm_base_interface wm_impl = {
    .destroy = noop_destroy,
    .create_positioner = wm_create_positioner,
    .get_xdg_surface = wm_get_xdg_surface,
    .pong = wm_pong,
};
static void wm_bind(struct wl_client *c, void *d, uint32_t v, uint32_t id) {
  (void)d;
  struct wl_resource *r = wl_resource_create(c, &xdg_wm_base_interface, v, id);
  if (!r) {
    wl_client_post_no_memory(c);
    return;
  }
  wl_resource_set_implementation(r, &wm_impl, NULL, NULL);
}

// xdg-decoration: siempre del lado del servidor (la decoracion es la de Aurora)
static void deco_set_mode(struct wl_client *c, struct wl_resource *r, uint32_t m) {
  (void)c; (void)m;
  zxdg_toplevel_decoration_v1_send_configure(
      r, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
}
static void deco_unset_mode(struct wl_client *c, struct wl_resource *r) {
  (void)c;
  zxdg_toplevel_decoration_v1_send_configure(
      r, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
}
static const struct zxdg_toplevel_decoration_v1_interface deco_impl = {
    .destroy = noop_destroy,
    .set_mode = deco_set_mode,
    .unset_mode = deco_unset_mode,
};
static void dm2_get_toplevel_decoration(struct wl_client *c, struct wl_resource *r,
                                        uint32_t id, struct wl_resource *tlr) {
  struct toplevel *tl = wl_resource_get_user_data(tlr);
  if (tl)
    tl->ssd = 1;
  struct wl_resource *d = wl_resource_create(
      c, &zxdg_toplevel_decoration_v1_interface, wl_resource_get_version(r), id);
  if (!d) {
    wl_client_post_no_memory(c);
    return;
  }
  wl_resource_set_implementation(d, &deco_impl, NULL, NULL);
  zxdg_toplevel_decoration_v1_send_configure(
      d, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
}
static const struct zxdg_decoration_manager_v1_interface deco_mgr_impl = {
    .destroy = noop_destroy,
    .get_toplevel_decoration = dm2_get_toplevel_decoration,
};
static void deco_mgr_bind(struct wl_client *c, void *d, uint32_t v, uint32_t id) {
  (void)d;
  struct wl_resource *r =
      wl_resource_create(c, &zxdg_decoration_manager_v1_interface, v, id);
  if (!r) {
    wl_client_post_no_memory(c);
    return;
  }
  wl_resource_set_implementation(r, &deco_mgr_impl, NULL, NULL);
}

// org_kde_kwin_server_decoration: mismo efecto para toolkits que solo conocen
// el protocolo de KDE.
static void kde_release(struct wl_client *c, struct wl_resource *r) {
  (void)c;
  wl_resource_destroy(r);
}
static void kde_request_mode(struct wl_client *c, struct wl_resource *r, uint32_t m) {
  (void)c; (void)m;
  org_kde_kwin_server_decoration_send_mode(
      r, ORG_KDE_KWIN_SERVER_DECORATION_MANAGER_MODE_SERVER);
}
static const struct org_kde_kwin_server_decoration_interface kde_deco_impl = {
    .release = kde_release,
    .request_mode = kde_request_mode,
};
static void kde_mgr_create(struct wl_client *c, struct wl_resource *r,
                           uint32_t id, struct wl_resource *surf) {
  struct wl_resource *d = wl_resource_create(
      c, &org_kde_kwin_server_decoration_interface, wl_resource_get_version(r), id);
  if (!d) {
    wl_client_post_no_memory(c);
    return;
  }
  wl_resource_set_implementation(d, &kde_deco_impl, NULL, NULL);
  struct surface *s = wl_resource_get_user_data(surf);
  if (s)
    s->kde_ssd = 1;
  org_kde_kwin_server_decoration_send_mode(
      d, ORG_KDE_KWIN_SERVER_DECORATION_MANAGER_MODE_SERVER);
}
static const struct org_kde_kwin_server_decoration_manager_interface kde_mgr_impl = {
    .create = kde_mgr_create,
};
static void kde_mgr_bind(struct wl_client *c, void *d, uint32_t v, uint32_t id) {
  (void)d;
  struct wl_resource *r = wl_resource_create(
      c, &org_kde_kwin_server_decoration_manager_interface, v, id);
  if (!r) {
    wl_client_post_no_memory(c);
    return;
  }
  wl_resource_set_implementation(r, &kde_mgr_impl, NULL, NULL);
  org_kde_kwin_server_decoration_manager_send_default_mode(
      r, ORG_KDE_KWIN_SERVER_DECORATION_MANAGER_MODE_SERVER);
}

// ---------------------------------------------------------------------------
// Eventos de Aurora -> Wayland
// ---------------------------------------------------------------------------
static void ptr_send_to(struct xsurf *xs, void (*fn)(struct wl_resource *, void *), void *arg) {
  struct wl_client *c = wl_resource_get_client(xs->res);
  struct seat_res *p;
  wl_list_for_each(p, &g_pointers, link)
    if (wl_resource_get_client(p->res) == c)
      fn(p->res, arg);
}

struct ptr_ev { struct xsurf *xs; int x, y; uint32_t serial; uint32_t time; uint32_t button; uint32_t state; };
static void do_enter(struct wl_resource *p, void *a) {
  struct ptr_ev *e = a;
  if (e->xs->surface)
    wl_pointer_send_enter(p, e->serial, e->xs->surface->res,
                          wl_fixed_from_int(e->x), wl_fixed_from_int(e->y));
}
static void do_leave(struct wl_resource *p, void *a) {
  struct ptr_ev *e = a;
  if (e->xs->surface)
    wl_pointer_send_leave(p, e->serial, e->xs->surface->res);
}
static void do_motion(struct wl_resource *p, void *a) {
  struct ptr_ev *e = a;
  wl_pointer_send_motion(p, e->time, wl_fixed_from_int(e->x), wl_fixed_from_int(e->y));
}
static void do_button(struct wl_resource *p, void *a) {
  struct ptr_ev *e = a;
  wl_pointer_send_button(p, e->serial, e->time, e->button, e->state);
}
static void do_frame(struct wl_resource *p, void *a) {
  (void)a;
  if (wl_resource_get_version(p) >= 5)
    wl_pointer_send_frame(p);
}

static void pointer_leave(struct xsurf *xs) {
  if (g_ptr_focus != xs)
    return;
  struct ptr_ev e = {.xs = xs, .serial = wl_display_next_serial(g_display)};
  ptr_send_to(xs, do_leave, &e);
  ptr_send_to(xs, do_frame, &e);
  g_ptr_focus = NULL;
  xs->last_buttons = 0;
}

// Raiz (toplevel) de la cadena de padres de un popup.
static struct xsurf *xs_root(struct xsurf *xs) {
  while (xs && xs->is_popup && xs->parent)
    xs = xs->parent;
  return xs;
}

// Cierra los popups con grab de la ventana `root` (clic fuera / perdida de foco).
static void popups_dismiss(struct xsurf *root, struct xsurf *except) {
  struct xsurf *o;
  wl_list_for_each(o, &g_wins, wlink)
    if (o != except && o->is_popup && o->grabbed && o->popup_res &&
        xs_root(o) == root)
      xdg_popup_send_popup_done(o->popup_res);
}

static void pointer_event(struct xsurf *xs, int x, int y, uint32_t buttons) {
  // Aurora da coordenadas de la ventana (= geometria); Wayland, de la superficie.
  struct ptr_ev e = {.xs = xs, .x = x + xs->vx, .y = y + xs->vy,
                     .time = now_ms()};
  if (g_debug)
    fprintf(stderr, "[aurora-wayland] ptr win=%d (%d,%d) btn=%u\n", xs->win_id,
            x, y, buttons);
  if (g_ptr_focus != xs) {
    if (g_ptr_focus)
      pointer_leave(g_ptr_focus);
    g_ptr_focus = xs;
    xs->last_buttons = 0;
    e.serial = wl_display_next_serial(g_display);
    ptr_send_to(xs, do_enter, &e);
  }
  ptr_send_to(xs, do_motion, &e);
  static const uint32_t codes[3] = {BTN_LEFT, BTN_RIGHT, BTN_MIDDLE};
  for (int b = 0; b < 3; b++) {
    int was = (xs->last_buttons >> b) & 1, is = (buttons >> b) & 1;
    if (was != is) {
      e.serial = wl_display_next_serial(g_display);
      e.button = codes[b];
      e.state = is ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED;
      ptr_send_to(xs, do_button, &e);
      // Pulsar sobre la ventana de la que cuelga un popup (no sobre el propio
      // popup) lo cierra, como en cualquier compositor con grab de popups.
      if (is && !xs->is_popup)
        popups_dismiss(xs, NULL);
    }
  }
  xs->last_buttons = (int)buttons;
  ptr_send_to(xs, do_frame, &e);
}

static void handle_win_event(struct xsurf *xs, const winsrv_event_t *ev) {
  struct toplevel *tl = xs->tl;
  switch (ev->type) {
  case WINSRV_EV_CLOSE:
    if (tl)
      xdg_toplevel_send_close(tl->res);
    else if (xs->popup_res)
      xdg_popup_send_popup_done(xs->popup_res);
    break;
  case WINSRV_EV_FOCUS:
    if (tl)
      kbd_focus_set(tl);
    break;
  case WINSRV_EV_BLUR:
    if (tl) {
      if (g_kbd_focus == tl)
        kbd_focus_set(NULL);
      popups_dismiss(xs, NULL);
    }
    break;
  case WINSRV_EV_RESIZE:
    if (tl && (ev->x != xs->cw || ev->y != xs->ch)) {
      xs->cw = ev->x;
      xs->ch = ev->y;
      toplevel_send_configure(tl, xs->cw, xs->ch);
    }
    break;
  case WINSRV_EV_MOVE:
    if (tl) {
      xs->wx = ev->x;
      xs->wy = ev->y + TITLEBAR_H;
    }
    break;
  case WINSRV_EV_PTR:
    pointer_event(xs, ev->x, ev->y, ev->data);
    break;
  case WINSRV_EV_PTR_LEAVE:
    pointer_leave(xs);
    break;
  default:
    break;
  }
}

// Vacia las colas de eventos de winsrv. winsrv no ofrece un fd esperable, asi
// que se sondea con un temporizador mientras haya ventanas.
static int pump_events(void *data) {
  (void)data;
  struct xsurf *xs, *tmp;
  int mapped = 0;
  wl_list_for_each_safe(xs, tmp, &g_wins, wlink) {
    mapped++;
    winsrv_event_t ev;
    int budget = 64;
    int id = xs->win_id;
    while (budget-- > 0 && xs->win_id == id && win_poll(id, &ev) > 0)
      handle_win_event(xs, &ev);
  }
  wl_display_flush_clients(g_display);
  wl_event_source_timer_update(g_pump_timer, mapped ? 4 : 0);
  return 0;
}

// ---------------------------------------------------------------------------
// Teclado: /dev/input/event0 -> wl_keyboard
// ---------------------------------------------------------------------------
static int kbd_input(int fd, uint32_t mask, void *data) {
  (void)mask; (void)data;
  struct input_event ev[32];
  ssize_t n;
  while ((n = read(fd, ev, sizeof(ev))) > 0) {
    for (size_t i = 0; i < (size_t)n / sizeof(ev[0]); i++) {
      if (ev[i].type != EV_KEY || ev[i].value == 2)
        continue;
      int down = ev[i].value == 1;
      uint32_t code = ev[i].code;
      uint32_t before = xkb_state_serialize_mods(g_xkb_state, XKB_STATE_MODS_EFFECTIVE);
      uint32_t lbefore = xkb_state_serialize_mods(g_xkb_state, XKB_STATE_MODS_LOCKED);
      xkb_state_update_key(g_xkb_state, code + 8, down ? XKB_KEY_DOWN : XKB_KEY_UP);
      if (g_debug)
        fprintf(stderr, "[aurora-wayland] key %u %s focus=%s\n", code,
                down ? "down" : "up", g_kbd_focus ? g_kbd_focus->title : "-");
      if (!g_kbd_focus)
        continue;
      struct wl_client *c = wl_resource_get_client(g_kbd_focus->res);
      uint32_t serial = wl_display_next_serial(g_display);
      uint32_t t = now_ms();
      struct seat_res *k;
      wl_list_for_each(k, &g_keyboards, link)
        if (wl_resource_get_client(k->res) == c)
          wl_keyboard_send_key(k->res, serial, t, code,
                               down ? WL_KEYBOARD_KEY_STATE_PRESSED
                                    : WL_KEYBOARD_KEY_STATE_RELEASED);
      if (before != xkb_state_serialize_mods(g_xkb_state, XKB_STATE_MODS_EFFECTIVE) ||
          lbefore != xkb_state_serialize_mods(g_xkb_state, XKB_STATE_MODS_LOCKED))
        send_modifiers_to(c);
    }
  }
  wl_display_flush_clients(g_display);
  return 0;
}

// Rueda: /dev/input/event1 (REL_WHEEL/REL_HWHEEL) -> wl_pointer.axis hacia la
// superficie que tiene el foco de puntero. El resto de eventos del raton ya
// llegan por winsrv (WINSRV_EV_PTR).
struct axis_ev { uint32_t time, axis; int notches; };
static void do_axis(struct wl_resource *p, void *a) {
  struct axis_ev *e = a;
  // 15 px por muesca; en Wayland un valor positivo es hacia abajo/derecha.
  if (wl_resource_get_version(p) >= 5)
    wl_pointer_send_axis_discrete(p, e->axis, e->notches);
  wl_pointer_send_axis(p, e->time, e->axis, wl_fixed_from_int(e->notches * 15));
}
static int wheel_input(int fd, uint32_t mask, void *data) {
  (void)mask; (void)data;
  struct input_event ev[32];
  ssize_t n;
  while ((n = read(fd, ev, sizeof(ev))) > 0) {
    for (size_t i = 0; i < (size_t)n / sizeof(ev[0]); i++) {
      if (ev[i].type != EV_REL ||
          (ev[i].code != REL_WHEEL && ev[i].code != REL_HWHEEL))
        continue;
      if (g_debug)
        fprintf(stderr, "[aurora-wayland] wheel code=%u value=%d focus=%d\n",
                ev[i].code, ev[i].value, g_ptr_focus ? g_ptr_focus->win_id : -1);
      if (!g_ptr_focus)
        continue;
      struct axis_ev e = {
          .time = now_ms(),
          .axis = ev[i].code == REL_WHEEL ? WL_POINTER_AXIS_VERTICAL_SCROLL
                                          : WL_POINTER_AXIS_HORIZONTAL_SCROLL,
          // evdev: + = arriba/izquierda; Wayland: + = abajo/derecha
          .notches = ev[i].code == REL_WHEEL ? -ev[i].value : ev[i].value};
      ptr_send_to(g_ptr_focus, do_axis, &e);
      ptr_send_to(g_ptr_focus, do_frame, &e);
    }
  }
  wl_display_flush_clients(g_display);
  return 0;
}

static int keymap_init(void) {
  g_xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
  if (!g_xkb_ctx)
    return -1;
  struct xkb_rule_names names = {.layout = getenv("AURORA_XKB_LAYOUT"),
                                 .variant = getenv("AURORA_XKB_VARIANT")};
  if (!names.layout) names.layout = "us";
  g_xkb_map = xkb_keymap_new_from_names(g_xkb_ctx, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
  if (!g_xkb_map) {
    fprintf(stderr, "[aurora-wayland] xkb: no se pudo compilar el mapa '%s'\n", names.layout);
    return -1;
  }
  g_xkb_state = xkb_state_new(g_xkb_map);
  char *str = xkb_keymap_get_as_string(g_xkb_map, XKB_KEYMAP_FORMAT_TEXT_V1);
  if (!str)
    return -1;
  g_keymap_size = strlen(str) + 1;
  g_keymap_fd = memfd_create("xkb-keymap", MFD_CLOEXEC);
  if (g_keymap_fd < 0 || ftruncate(g_keymap_fd, (off_t)g_keymap_size) < 0) {
    free(str);
    return -1;
  }
  void *m = mmap(NULL, g_keymap_size, PROT_READ | PROT_WRITE, MAP_SHARED, g_keymap_fd, 0);
  if (m == MAP_FAILED) {
    free(str);
    return -1;
  }
  memcpy(m, str, g_keymap_size);
  munmap(m, g_keymap_size);
  free(str);
  return 0;
}

// ---------------------------------------------------------------------------
// Lanzadores del Dock: /usr/share/aurora/dock.conf
//   Nombre | app_id | simbolo | color RRGGBB | orden de arranque (ruta args...)
// aurora-wayland los fija en el Dock; cuando el usuario pulsa uno sin ventanas
// abiertas, el compositor encola una peticion y aqui se lanza el proceso (con
// WAYLAND_DISPLAY/XDG_RUNTIME_DIR ya preparados) y se recoge al terminar.
// ---------------------------------------------------------------------------
#define DOCK_CONF_PATH "/usr/share/aurora/dock.conf"
#define MAX_LAUNCHERS 16
static struct {
  char name[32], app_id[48], symbol[8], cmd[192];
  uint32_t color;
} g_launchers[MAX_LAUNCHERS];
static int g_nlaunchers;
static struct wl_event_source *g_launch_timer;

static char *trim(char *s) {
  while (*s == ' ' || *s == '\t')
    s++;
  char *e = s + strlen(s);
  while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\n' || e[-1] == '\r'))
    *--e = 0;
  return s;
}

static void launchers_load(void) {
  FILE *f = fopen(DOCK_CONF_PATH, "r");
  if (!f)
    return;
  char line[512];
  while (g_nlaunchers < MAX_LAUNCHERS && fgets(line, sizeof(line), f)) {
    char *h = strchr(line, '#');
    if (h)
      *h = 0;
    char *fld[5];
    int n = 0;
    for (char *p = line; n < 5;) {
      fld[n++] = p;
      char *bar = n < 5 ? strchr(p, '|') : NULL;
      if (!bar)
        break;
      *bar = 0;
      p = bar + 1;
    }
    if (n < 5)
      continue;
    int i = g_nlaunchers;
    snprintf(g_launchers[i].name, sizeof(g_launchers[i].name), "%s", trim(fld[0]));
    snprintf(g_launchers[i].app_id, sizeof(g_launchers[i].app_id), "%s", trim(fld[1]));
    snprintf(g_launchers[i].symbol, sizeof(g_launchers[i].symbol), "%s", trim(fld[2]));
    g_launchers[i].color = (uint32_t)strtoul(trim(fld[3]), NULL, 16) | 0xFF000000u;
    snprintf(g_launchers[i].cmd, sizeof(g_launchers[i].cmd), "%s", trim(fld[4]));
    if (g_launchers[i].app_id[0] && g_launchers[i].cmd[0])
      g_nlaunchers++;
  }
  fclose(f);
}

static void launchers_pin(void) {
  for (int i = 0; i < g_nlaunchers; i++) {
    struct {
      uint32_t launch_id, color;
      char name[32], symbol[8], app_id[48];
    } r;
    memset(&r, 0, sizeof(r));
    r.launch_id = (uint32_t)i + 1;
    r.color = g_launchers[i].color;
    snprintf(r.name, sizeof(r.name), "%s", g_launchers[i].name);
    snprintf(r.symbol, sizeof(r.symbol), "%s", g_launchers[i].symbol);
    snprintf(r.app_id, sizeof(r.app_id), "%s", g_launchers[i].app_id);
    syscall(ASYS_DOCK_PIN, &r);
  }
  fprintf(stderr, "[aurora-wayland] %d lanzador(es) fijados en el Dock\n", g_nlaunchers);
}

static void launch_app(int idx) {
  const char *cmd = g_launchers[idx].cmd;
  fprintf(stderr, "[aurora-wayland] lanzando '%s': %s\n", g_launchers[idx].name, cmd);
  pid_t pid = fork();
  if (pid != 0) {
    if (pid < 0)
      fprintf(stderr, "[aurora-wayland] fork fallo: %s\n", strerror(errno));
    return;
  }
  // Hijo: argv por espacios (sin comillas; las apps con ruta + args simples).
  char buf[192];
  snprintf(buf, sizeof(buf), "%s", cmd);
  char *argv[16];
  int ac = 0;
  for (char *tok = strtok(buf, " "); tok && ac < 15; tok = strtok(NULL, " "))
    argv[ac++] = tok;
  argv[ac] = NULL;
  int fd = open("/dev/null", O_RDWR);
  if (fd >= 0) {
    dup2(fd, 0);
    dup2(fd, 1);
    dup2(fd, 2);
    if (fd > 2)
      close(fd);
  }
  signal(SIGPIPE, SIG_DFL);
  setenv("XDG_RUNTIME_DIR", "/run/user/0", 0);
  setenv("WAYLAND_DISPLAY", "wayland-0", 0);
  execv(argv[0], argv);
  _exit(127);
}

static int launch_poll(void *data) {
  (void)data;
  long id;
  while ((id = syscall(ASYS_DOCK_POLL_LAUNCH)) > 0)
    if (id <= g_nlaunchers)
      launch_app((int)id - 1);
  wl_event_source_timer_update(g_launch_timer, 100);
  return 0;
}

static int on_chld(int sig, void *data) {
  (void)sig; (void)data;
  while (waitpid(-1, NULL, WNOHANG) > 0)
    ;
  return 0;
}

static int on_term(int sig, void *data) {
  (void)sig; (void)data;
  wl_display_terminate(g_display);
  return 0;
}

int main(int argc, char **argv) {
  const char *sock = argc > 1 ? argv[1] : "wayland-0";
  signal(SIGPIPE, SIG_IGN);
  g_debug = getenv("AW_DEBUG") != NULL;

  g_display = wl_display_create();
  if (!g_display) {
    fprintf(stderr, "[aurora-wayland] wl_display_create fallo\n");
    return 1;
  }
  g_loop = wl_display_get_event_loop(g_display);
  wl_list_init(&g_toplevels);
  wl_list_init(&g_csd_rules);
  csd_rules_load();
  wl_list_init(&g_wins);
  wl_list_init(&g_pointers);
  wl_list_init(&g_keyboards);
  wl_list_init(&g_datadevs);

  if (keymap_init() < 0)
    fprintf(stderr, "[aurora-wayland] aviso: sin teclado xkb\n");

  wl_display_init_shm(g_display);
  wl_global_create(g_display, &wl_compositor_interface, 4, NULL, comp_bind);
  wl_global_create(g_display, &wl_subcompositor_interface, 1, NULL, subc_bind);
  wl_global_create(g_display, &wl_output_interface, 3, NULL, output_bind);
  wl_global_create(g_display, &wl_seat_interface, 5, NULL, seat_bind);
  wl_global_create(g_display, &wl_data_device_manager_interface, 3, NULL, dm_bind);
  wl_global_create(g_display, &xdg_wm_base_interface, 3, NULL, wm_bind);
  wl_global_create(g_display, &zxdg_decoration_manager_v1_interface, 1, NULL,
                   deco_mgr_bind);
  wl_global_create(g_display, &org_kde_kwin_server_decoration_manager_interface, 1,
                   NULL, kde_mgr_bind);

  g_pump_timer = wl_event_loop_add_timer(g_loop, pump_events, NULL);
  wl_event_loop_add_signal(g_loop, SIGCHLD, on_chld, NULL);
  wl_event_loop_add_signal(g_loop, SIGTERM, on_term, NULL);
  wl_event_loop_add_signal(g_loop, SIGINT, on_term, NULL);

  int kfd = open("/dev/input/event0", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
  if (kfd >= 0)
    wl_event_loop_add_fd(g_loop, kfd, WL_EVENT_READABLE, kbd_input, NULL);
  else
    fprintf(stderr, "[aurora-wayland] aviso: sin /dev/input/event0 (%s)\n", strerror(errno));

  int wfd = open("/dev/input/event1", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
  if (wfd >= 0)
    wl_event_loop_add_fd(g_loop, wfd, WL_EVENT_READABLE, wheel_input, NULL);
  else
    fprintf(stderr, "[aurora-wayland] aviso: sin /dev/input/event1 (%s)\n", strerror(errno));

  if (wl_display_add_socket(g_display, sock) < 0) {
    fprintf(stderr, "[aurora-wayland] no se pudo crear el socket '%s' (XDG_RUNTIME_DIR=%s)\n",
            sock, getenv("XDG_RUNTIME_DIR") ? getenv("XDG_RUNTIME_DIR") : "(null)");
    return 1;
  }
  launchers_load();
  launchers_pin();
  g_launch_timer = wl_event_loop_add_timer(g_loop, launch_poll, NULL);
  wl_event_source_timer_update(g_launch_timer, 100);
  fprintf(stderr, "[aurora-wayland] READY socket=%s\n", sock);
  wl_display_run(g_display);

  fprintf(stderr, "[aurora-wayland] terminado\n");
  wl_display_destroy_clients(g_display);
  wl_display_destroy(g_display);
  return 0;
}
