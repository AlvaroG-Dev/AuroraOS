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
#include <dirent.h>
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
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <wayland-server.h>
#include <xcb/composite.h>
#include <xcb/xcb.h>
#include <xkbcommon/xkbcommon.h>

#include "cursor-shape-v1-server-protocol.h"
#include "fractional-scale-v1-server-protocol.h"
#include "org-kde-kwin-server-decoration-server-protocol.h"
#include "primary-selection-unstable-v1-server-protocol.h"
#include "viewporter-server-protocol.h"
#include "idle-inhibit-unstable-v1-server-protocol.h"
#include "linux-dmabuf-unstable-v1-server-protocol.h"
#include "presentation-time-server-protocol.h"
#include "xdg-activation-v1-server-protocol.h"
#include "xdg-output-unstable-v1-server-protocol.h"
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

// AW_MOCK=1: sin winsrv (host de desarrollo). Las ventanas viven en memoria y
// SIGUSR1 las vuelca a /tmp/aw-mock-<id>.ppm; sirve para depurar clientes
// Wayland reales (GTK, Qt...) en el PC de desarrollo con WAYLAND_DEBUG=1.
static int g_mock;
#define MOCK_MAX_WIN 32
static struct { int used, w, h; uint32_t *px; char title[64]; } g_mw[MOCK_MAX_WIN];
static int mock_dump(int sig, void *data);

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
  if (g_mock) {
    for (int i = 0; i < MOCK_MAX_WIN; i++)
      if (!g_mw[i].used) {
        g_mw[i].used = 1;
        g_mw[i].w = w;
        g_mw[i].h = h;
        g_mw[i].px = calloc((size_t)w * h, 4);
        snprintf(g_mw[i].title, sizeof(g_mw[i].title), "%s", title);
        return i;
      }
    return -1;
  }
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
#define ASYS_WIN_BEGIN_GRAB (ASYS_BASE + 0x1D)
#define ASYS_WIN_RESIZE (ASYS_BASE + 0x1E)
#define ASYS_WIN_SET_CURSOR (ASYS_BASE + 0x1F)
#define ASYS_WIN_SET_ICON (ASYS_BASE + 0x15)
#define WINSRV_EV_STATE 12 /* x = 1: la ventana quedo maximizada */
// Bordes para ASYS_WIN_BEGIN_GRAB (RESIZE_EDGE_* del compositor); 0 = mover.
// Tamano exterior pedido por el cliente (con la barra de titulo de Aurora).
static int win_request_resize(int id, int w, int h) {
  if (g_mock)
    return 0;
  return (int)syscall(ASYS_WIN_RESIZE, (long)id, (long)w, (long)h);
}
static int win_begin_grab(int id, int edges) {
  if (g_mock)
    return 0;
  return (int)syscall(ASYS_WIN_BEGIN_GRAB, (long)id, (long)edges);
}
static int win_set_app_id(int id, const char *app_id) {
  if (g_mock)
    return 0;
  return (int)syscall(ASYS_WIN_SET_APP_ID, (long)id, app_id);
}
// Formas de cursor de Aurora (cursor_shape_t de kernel/gfx/cursor.h).
enum {
  AUR_CURSOR_ARROW = 0,
  AUR_CURSOR_RESIZE_EW,
  AUR_CURSOR_RESIZE_NS,
  AUR_CURSOR_RESIZE_NWSE,
  AUR_CURSOR_RESIZE_NESW,
  AUR_CURSOR_POINTER,
  AUR_CURSOR_NOT_ALLOWED,
  AUR_CURSOR_TEXT,
  AUR_CURSOR_GRABBING,
  AUR_CURSOR_WAIT,
};
static int win_set_cursor(int id, int shape) {
  if (g_mock)
    return 0;
  return (int)syscall(ASYS_WIN_SET_CURSOR, (long)id, (long)shape);
}
static int win_set_title(int id, const char *title) {
  if (g_mock)
    return 0;
  return (int)syscall(ASYS_WIN_SET_TITLE, (long)id, title);
}
static int win_set_state(int id, int state) {
  if (g_mock)
    return 0;
  return (int)syscall(ASYS_WIN_SET_STATE, (long)id, (long)state);
}
static int win_destroy(int id) {
  if (g_mock) {
    if (id >= 0 && id < MOCK_MAX_WIN) {
      free(g_mw[id].px);
      memset(&g_mw[id], 0, sizeof(g_mw[id]));
    }
    return 0;
  }
  return (int)syscall(ASYS_WIN_DESTROY, (long)id);
}
static int win_poll(int id, winsrv_event_t *ev) {
  if (g_mock)
    return 0;
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
  if (g_mock) {
    if (id < 0 || id >= MOCK_MAX_WIN || !g_mw[id].used)
      return -1;
    for (int j = 0; j < h; j++)
      for (int i = 0; i < w; i++)
        if (dx + i >= 0 && dx + i < g_mw[id].w && dy + j >= 0 && dy + j < g_mw[id].h)
          g_mw[id].px[(size_t)(dy + j) * g_mw[id].w + dx + i] =
              pixels[(size_t)(sy + j) * stride + sx + i];
    return 0;
  }
  return (int)syscall(ASYS_WIN_BLIT, &a);
}

// ---------------------------------------------------------------------------
// Estado global
// ---------------------------------------------------------------------------
#define OUT_W 1280
#define OUT_H 800

struct surface;
struct toplevel;

// Rectangulo [x0,x1)x[y0,y1); vacio si x1<=x0.
typedef struct { int x0, y0, x1, y1; } rc_t;
static inline int rc_empty(rc_t r) { return r.x1 <= r.x0 || r.y1 <= r.y0; }
static inline void rc_union(rc_t *a, rc_t b) {
  if (rc_empty(b))
    return;
  if (rc_empty(*a)) {
    *a = b;
    return;
  }
  if (b.x0 < a->x0) a->x0 = b.x0;
  if (b.y0 < a->y0) a->y0 = b.y0;
  if (b.x1 > a->x1) a->x1 = b.x1;
  if (b.y1 > a->y1) a->y1 = b.y1;
}
static inline rc_t rc_move(rc_t r, int dx, int dy) {
  return (rc_t){r.x0 + dx, r.y0 + dy, r.x1 + dx, r.y1 + dy};
}
static inline rc_t rc_clip(rc_t r, rc_t c) {
  if (r.x0 < c.x0) r.x0 = c.x0;
  if (r.y0 < c.y0) r.y0 = c.y0;
  if (r.x1 > c.x1) r.x1 = c.x1;
  if (r.y1 > c.y1) r.y1 = c.y1;
  return r;
}

// Nodo de la pila de subsuperficies de un padre (de abajo a arriba). El padre
// se representa a si mismo con el nodo `self` (res == NULL).
struct subsurf {
  struct wl_resource *res;
  struct surface *surface;
  struct surface *parent;
  struct wl_list link; /* en parent->stack */
  int x, y;            /* posicion aplicada, respecto al padre */
  int px, py, pos_pending;
  int sync;
  rc_t pdmg; /* dano diferido (modo sync), en coordenadas de la subsuperficie */
};

static struct wl_display *g_display;
static struct wl_event_loop *g_loop;
static const char *g_sockname = "wayland-0";
static struct wl_event_source *g_pump_timer;
static struct wl_list g_toplevels;  /* struct toplevel.link */
static struct wl_list g_pointers;   /* struct seat_res.link */
static struct wl_list g_keyboards;  /* struct seat_res.link */
static struct wl_list g_datadevs;   /* struct seat_res.link */
static struct toplevel *g_kbd_focus;
static struct xsurf *g_ptr_focus;
static struct surface *g_ptr_surf; /* superficie (raiz o subsuperficie) con el foco de puntero */
static struct wl_list g_wins; /* struct xsurf.wlink: ventanas mapeadas */
static int g_win_count;
static int g_debug; /* AW_DEBUG=1: traza de eventos */

static struct xkb_context *g_xkb_ctx;
static struct xkb_keymap *g_xkb_map;
static struct xkb_state *g_xkb_state;
static int g_keymap_fd = -1;
static size_t g_keymap_size;

static struct data_source *g_selection;
static struct data_source *g_primary; /* seleccion primaria (boton central) */
static struct wl_list g_psdevs;       /* struct seat_res.link: primary_selection_device */
static void primary_send_to(struct wl_client *client);
static void surface_send_presented(struct surface *s);
static struct wl_list g_frame_q;           /* superficies con frame callbacks pendientes */
static struct wl_event_source *g_frame_timer;
#define FRAME_MS 16                        /* ~60 Hz */

static uint32_t now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

struct seat_res {
  struct wl_resource *res;
  struct wl_list link;
};

struct region;
struct surface {
  struct wl_resource *res;
  struct region *in_reg;  /* region de entrada (NULL = toda la superficie) */
  struct wl_list pfb;     /* wp_presentation_feedback pendientes (del siguiente commit) */
  struct wl_list flink;   /* en g_frame_q mientras espera su "vsync" */
  int fq;                 /* 1 = en g_frame_q */
  // Subsuperficies
  struct wl_list stack;   /* struct subsurf.link; incluye `self` */
  struct subsurf self;
  struct subsurf *sub;    /* rol wl_subsurface (NULL si no lo tiene) */
  int stack_dirty;        /* place_above/below desde el ultimo commit */
  // Contenido: `scratch` guarda el ultimo buffer (alfa respetado solo en
  // subsuperficies; la raiz se fuerza opaca).
  int has_content;
  int store_alpha;        /* scratch puede tener alfa */
  int scale, pend_scale;  /* wl_surface.set_buffer_scale */
  // wp_viewport (pendiente / aplicado); src en px de superficie
  struct wl_resource *vp;
  int vp_src_set, vp_sx, vp_sy, vp_sw, vp_sh;
  int vp_dst_w, vp_dst_h; /* -1 = sin destino */
  int pvp_src_set, pvp_sx, pvp_sy, pvp_sw, pvp_sh;
  int pvp_dst_w, pvp_dst_h;
  int dw, dh;             /* tamano de la superficie (coords de superficie) */
  // Composicion (solo raices con subsuperficies/escalado)
  uint32_t *canvas;
  size_t canvas_px;
  int canvas_valid;       /* el canvas tiene el ultimo compuesto */
  int pres_w, pres_h;     /* tamano de lo ultimo presentado a la ventana */
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
  int x_or; /* ventana X11 override-redirect (menus, tooltips): posicion absoluta */
};

struct toplevel {
  struct wl_resource *res; /* NULL en ventanas X11 (Xwayland) */
  struct wl_client *client; /* cliente Wayland dueno de la superficie */
  int is_x;                 /* ventana X11 gestionada por el WM interno */
  uint32_t xwin;            /* id de la ventana X11 */
  struct xsurf *xs;
  struct wl_list link;
  char title[64];
  int activated;
  int maximized;   /* estado anunciado en configure */
  int fullscreen;  /* set_fullscreen: se trata como maximizada */
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
  wl_client_get_credentials(tl->client, &pid, NULL, NULL);
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

static struct wl_client *xs_client(const struct xsurf *xs) {
  return xs->surface && xs->surface->res ? wl_resource_get_client(xs->surface->res)
                                         : NULL;
}
static void xwm_toplevel_configure(struct toplevel *tl, int w, int h);
static void xwm_close(struct toplevel *tl);
static void xwm_sync_pos(struct xsurf *xs);
static void xwm_title(struct toplevel *tl);

static int tl_is_ssd(const struct toplevel *tl) {
  return tl->ssd || tl->is_x || (tl->xs && tl->xs->surface && tl->xs->surface->kde_ssd);
}

static void toplevel_send_configure(struct toplevel *tl, int w, int h) {
  if (tl->is_x) { /* X11: se traduce a ConfigureWindow / SetInputFocus */
    xwm_toplevel_configure(tl, w, h);
    return;
  }
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
  if (tl->fullscreen) {
    uint32_t *s = wl_array_add(&states, sizeof(uint32_t));
    *s = XDG_TOPLEVEL_STATE_FULLSCREEN;
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
    selection_send_to(g_kbd_focus->client);
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
  if (g_kbd_focus && g_kbd_focus->client == c)
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
// wl_region: lista de operaciones add/subtract evaluadas en orden. Solo se usa
// la region de entrada (hit-test del puntero); la opaca se ignora.
// ---------------------------------------------------------------------------
#define REGION_MAX_OPS 64
struct region {
  int n;
  struct { int x, y, w, h, add; } op[REGION_MAX_OPS];
};
static void region_op(struct wl_resource *r, int add, int x, int y, int w, int h) {
  struct region *rg = wl_resource_get_user_data(r);
  if (rg->n >= REGION_MAX_OPS)
    return;
  rg->op[rg->n].x = x; rg->op[rg->n].y = y;
  rg->op[rg->n].w = w; rg->op[rg->n].h = h;
  rg->op[rg->n].add = add;
  rg->n++;
}
static void region_add(struct wl_client *c, struct wl_resource *r, int32_t x,
                       int32_t y, int32_t w, int32_t h) {
  (void)c;
  region_op(r, 1, x, y, w, h);
}
static void region_sub(struct wl_client *c, struct wl_resource *r, int32_t x,
                       int32_t y, int32_t w, int32_t h) {
  (void)c;
  region_op(r, 0, x, y, w, h);
}
static int region_contains(const struct region *rg, int x, int y) {
  if (!rg)
    return 1;
  int in = 0;
  for (int i = 0; i < rg->n; i++)
    if (x >= rg->op[i].x && y >= rg->op[i].y && x < rg->op[i].x + rg->op[i].w &&
        y < rg->op[i].y + rg->op[i].h)
      in = rg->op[i].add;
  return in;
}
static const struct wl_region_interface region_impl = {
    .destroy = noop_destroy,
    .add = region_add,
    .subtract = region_sub,
};
static void region_resource_destroy(struct wl_resource *r) {
  free(wl_resource_get_user_data(r));
}

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
static void surface_set_input_region(struct wl_client *c, struct wl_resource *r,
                                     struct wl_resource *region) {
  (void)c;
  struct surface *s = wl_resource_get_user_data(r);
  free(s->in_reg);
  s->in_reg = NULL;
  if (region) {
    struct region *src = wl_resource_get_user_data(region);
    s->in_reg = malloc(sizeof(*src));
    if (s->in_reg)
      memcpy(s->in_reg, src, sizeof(*src));
  }
}
static void surface_set_transform(struct wl_client *c, struct wl_resource *r,
                                  int32_t t) {
  (void)c; (void)r; (void)t;
}
static void surface_set_scale(struct wl_client *c, struct wl_resource *r,
                              int32_t sc) {
  (void)c;
  struct surface *s = wl_resource_get_user_data(r);
  s->pend_scale = sc > 0 ? sc : 1;
}
static void surface_offset(struct wl_client *c, struct wl_resource *r,
                           int32_t x, int32_t y) {
  (void)c; (void)r; (void)x; (void)y;
}

static int has_launcher(const char *app_id);

static void xs_unmap(struct xsurf *xs) {
  if (xs->win_id < 0)
    return;
  if (g_ptr_focus == xs) {
    g_ptr_focus = NULL;
    g_ptr_surf = NULL;
  }
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
    if (xs->x_or) {
      /* override-redirect X11: wx/wy son sus coordenadas absolutas */
    } else if (!xs->parent || xs->parent->win_id < 0) {
      return;
    } else {
      xs->wx = xs->parent->wx + xs->px;
      xs->wy = xs->parent->wy + xs->py;
    }
    // La posicion del popup se limita a la pantalla (slide).
    if (xs->wx + w > OUT_W) xs->wx = OUT_W - w;
    if (xs->wy + h > OUT_H) xs->wy = OUT_H - h;
    if (xs->wx < 0) xs->wx = 0;
    if (xs->wy < 0) xs->wy = 0;
    id = win_create_ex(xs->wx, xs->wy, w, h, "popup",
                       WIN_FLAGS_NODECORATION | WIN_FLAGS_FIXED_SIZE |
                           WIN_FLAGS_POPUP | WIN_FLAGS_RAW_INPUT);
  } else {
    if (xs->tl && xs->tl->min_w > w) w = xs->tl->min_w; /* respeta set_min_size */
    if (xs->tl && xs->tl->min_h > h) h = xs->tl->min_h;
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
  if (xs->tl && !g_mock && !has_launcher(xs->tl->app_id))
    syscall(ASYS_WIN_SET_ICON, (long)id, "/system/icons/app-generic.bmp");
  xs->cw = w;
  xs->ch = h;
  wl_list_insert(&g_wins, &xs->wlink);
  if (xs->tl && xs->tl->is_x)
    xwm_sync_pos(xs);
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

// ---------------------------------------------------------------------------
// Arbol de superficies, composicion y presentacion en la ventana de Aurora
//
// Una superficie "plana" (sin subsuperficies, sin viewport ni escala) se vuelca
// tal cual a la ventana. Si la raiz tiene subsuperficies mapeadas (o escala /
// viewport) se compone en un canvas del tamano de la superficie raiz, en el
// orden de la pila de subsuperficies, y ese canvas es lo que se vuelca.
// ---------------------------------------------------------------------------
static void surface_calc_size(struct surface *s) {
  int sc = s->scale > 0 ? s->scale : 1;
  if (!s->has_content) {
    s->dw = s->dh = 0;
  } else if (s->vp_dst_w > 0 && s->vp_dst_h > 0) {
    s->dw = s->vp_dst_w;
    s->dh = s->vp_dst_h;
  } else if (s->vp_src_set) {
    s->dw = s->vp_sw;
    s->dh = s->vp_sh;
  } else {
    s->dw = s->buf_w / sc;
    s->dh = s->buf_h / sc;
  }
  if (s->has_content && (s->dw < 1 || s->dh < 1))
    s->dw = s->dh = 1;
}

// Rectangulo del buffer (px) que muestra la superficie.
static void surface_src_rect(const struct surface *s, int *sx, int *sy, int *sw,
                             int *sh) {
  int sc = s->scale > 0 ? s->scale : 1;
  *sx = 0; *sy = 0; *sw = s->buf_w; *sh = s->buf_h;
  if (s->vp_src_set) {
    *sx = s->vp_sx * sc; *sy = s->vp_sy * sc;
    *sw = s->vp_sw * sc; *sh = s->vp_sh * sc;
  }
  if (*sx < 0) *sx = 0;
  if (*sy < 0) *sy = 0;
  if (*sx >= s->buf_w) *sx = s->buf_w - 1;
  if (*sy >= s->buf_h) *sy = s->buf_h - 1;
  if (*sw < 1 || *sx + *sw > s->buf_w) *sw = s->buf_w - *sx;
  if (*sh < 1 || *sy + *sh > s->buf_h) *sh = s->buf_h - *sy;
}

static int surface_is_plain(const struct surface *s) {
  return s->dw == s->buf_w && s->dh == s->buf_h && !s->vp_src_set;
}

static struct surface *tree_root(struct surface *s) {
  while (s->sub && s->sub->parent)
    s = s->sub->parent;
  return s;
}

static void surface_origin(const struct surface *s, int *ox, int *oy) {
  int x = 0, y = 0;
  while (s->sub && s->sub->parent) {
    x += s->sub->x;
    y += s->sub->y;
    s = s->sub->parent;
  }
  *ox = x;
  *oy = y;
}

static int tree_has_subs(const struct surface *s) {
  const struct subsurf *n;
  wl_list_for_each(n, &s->stack, link)
    if (n != &s->self && n->surface && n->surface->has_content)
      return 1;
  return 0;
}

static void sub_unlink(struct subsurf *sb) {
  if (!sb->parent)
    return;
  wl_list_remove(&sb->link);
  wl_list_init(&sb->link);
  sb->parent = NULL;
}

// Mezcla `s` (ARGB premultiplicado) sobre `d`.
static inline uint32_t blend_premul(uint32_t s, uint32_t d) {
  uint32_t ia = 255 - (s >> 24), out = 0;
  for (int sh = 0; sh < 32; sh += 8) {
    uint32_t v = ((s >> sh) & 255) + ((((d >> sh) & 255) * ia + 127) / 255);
    if (v > 255)
      v = 255;
    out |= v << sh;
  }
  return out;
}

// Pinta la superficie `s` (origen (ox,oy) en el canvas de la raiz) en `clip`.
static void draw_surface(struct surface *root, struct surface *s, int ox, int oy,
                         rc_t clip) {
  if (!s->has_content || !s->scratch || s->dw <= 0 || s->dh <= 0)
    return;
  rc_t r = rc_clip((rc_t){ox, oy, ox + s->dw, oy + s->dh}, clip);
  if (rc_empty(r))
    return;
  int sx, sy, sw, sh;
  surface_src_rect(s, &sx, &sy, &sw, &sh);
  int plain = (sw == s->dw && sh == s->dh);
  int cw = root->dw;
  for (int y = r.y0; y < r.y1; y++) {
    uint32_t *dst = root->canvas + (size_t)y * cw;
    int ly = y - oy;
    int srow = sy + (plain ? ly : (int)((long)ly * sh / s->dh));
    const uint32_t *src = s->scratch + (size_t)srow * s->buf_w;
    if (plain && !s->store_alpha) {
      memcpy(dst + r.x0, src + sx + (r.x0 - ox), (size_t)(r.x1 - r.x0) * 4);
      continue;
    }
    for (int x = r.x0; x < r.x1; x++) {
      int lx = x - ox;
      uint32_t p = src[sx + (plain ? lx : (int)((long)lx * sw / s->dw))];
      if (!s->store_alpha || (p >> 24) == 255)
        dst[x] = p | (s->store_alpha ? 0 : 0xFF000000u);
      else if (p >> 24)
        dst[x] = blend_premul(p, dst[x]);
    }
  }
}

static void compose_node(struct surface *root, struct surface *s, int ox, int oy,
                         rc_t clip) {
  struct subsurf *n;
  wl_list_for_each(n, &s->stack, link) {
    if (n == &s->self)
      draw_surface(root, s, ox, oy, clip);
    else if (n->surface && n->surface->has_content)
      compose_node(root, n->surface, ox + n->x, oy + n->y, clip);
  }
}

// Vuelca `d` de `px` (ancho `w`, alto `h`) a la ventana de Aurora de la raiz,
// con la politica de geometria/recorte de decoraciones CSD.
static void present(struct surface *s, const uint32_t *px, int w, int h, rc_t d) {
  struct xsurf *xs = s->xs;
  struct toplevel *tl = xs->tl;
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
  int fresh = 0;
  if (xs->win_id < 0) {
    xs_map(xs, gw, gh);
    fresh = 1;
  }
  if (xs->win_id < 0)
    return;
  rc_t r = fresh ? (rc_t){0, 0, w, h} : d;
  r = rc_clip(r, (rc_t){gx, gy, gx + gw, gy + gh});
  if (!rc_empty(r))
    win_blit(xs->win_id, r.x0 - gx, r.y0 - gy, r.x0, r.y0, r.x1 - r.x0,
             r.y1 - r.y0, w, (uint32_t *)px);
}

// `d` (coordenadas de la superficie raiz) ha cambiado: componer y presentar.
static void root_update(struct surface *root, rc_t d) {
  struct xsurf *xs = root->xs;
  if (!xs || !(xs->tl || xs->is_popup) || !root->has_content)
    return;
  rc_t all = {0, 0, root->dw, root->dh};
  if (root->pres_w != root->dw || root->pres_h != root->dh)
    d = all;
  d = rc_clip(d, all);
  if (rc_empty(d))
    return;
  if (surface_is_plain(root) && !tree_has_subs(root)) {
    root->canvas_valid = 0;
    present(root, root->scratch, root->dw, root->dh, d);
  } else {
    size_t need = (size_t)root->dw * root->dh;
    if (root->canvas_px < need) {
      free(root->canvas);
      root->canvas = malloc(need * 4);
      root->canvas_px = root->canvas ? need : 0;
      root->canvas_valid = 0;
    }
    if (!root->canvas)
      return;
    if (!root->canvas_valid)
      d = all;
    for (int y = d.y0; y < d.y1; y++) {
      uint32_t *row = root->canvas + (size_t)y * root->dw;
      for (int x = d.x0; x < d.x1; x++)
        row[x] = 0xFF000000u;
    }
    compose_node(root, root, 0, 0, d);
    root->canvas_valid = 1;
    present(root, root->canvas, root->dw, root->dh, d);
  }
  root->pres_w = root->dw;
  root->pres_h = root->dh;
}

// El dano `d` (coords de `s`) sube por el arbol hasta la raiz. Una
// subsuperficie en modo sync retiene el dano hasta el commit de su padre.
static void surface_damage_up(struct surface *s, rc_t d) {
  while (s->sub && s->sub->parent) {
    struct subsurf *sb = s->sub;
    if (sb->sync) {
      rc_union(&sb->pdmg, d);
      return;
    }
    d = rc_move(d, sb->x, sb->y);
    s = sb->parent;
  }
  root_update(s, d);
}

// ---------------------------------------------------------------------------
// zwp_linux_dmabuf_v1: buffers dma-buf lineales XRGB8888/ARGB8888 de un plano
// (los que crean Mesa/GBM, Xwayland o los clientes con PRIME). El fd se mapea
// una vez (las paginas son las del GEM exportado: sin copia) y cada commit lo
// vuelca al almacen de la superficie igual que un wl_shm.
// ---------------------------------------------------------------------------
#define DRM_FMT_XRGB8888 0x34325258u
#define DRM_FMT_ARGB8888 0x34325241u

struct dmabuf_buf {
  int fd;
  int w, h, stride, offset;
  uint32_t format;
  uint8_t *map;
  size_t map_len;
};

struct dmabuf_params {
  int fd; /* -1 = plano 0 sin anadir */
  uint32_t offset, stride;
  int used;
};

static void dmabuf_buf_destroy_res(struct wl_resource *r);
static void dmabuf_buffer_destroy(struct wl_client *c, struct wl_resource *r) {
  (void)c;
  wl_resource_destroy(r);
}
static const struct wl_buffer_interface dmabuf_buffer_impl = {
    .destroy = dmabuf_buffer_destroy,
};
static struct dmabuf_buf *dmabuf_buf_from(struct wl_resource *buf) {
  if (!buf || !wl_resource_instance_of(buf, &wl_buffer_interface,
                                       &dmabuf_buffer_impl))
    return NULL;
  return wl_resource_get_user_data(buf);
}
static int dmabuf_buf_map(struct dmabuf_buf *b) {
  if (b->map)
    return 1;
  off_t len = lseek(b->fd, 0, SEEK_END);
  size_t need = (size_t)b->offset + (size_t)b->stride * b->h;
  if (len < 0 || (size_t)len < need)
    return 0;
  void *m = mmap(NULL, (size_t)len, PROT_READ, MAP_SHARED, b->fd, 0);
  if (m == MAP_FAILED)
    return 0;
  b->map = m;
  b->map_len = (size_t)len;
  return 1;
}
static void dmabuf_buf_destroy_res(struct wl_resource *r) {
  struct dmabuf_buf *b = wl_resource_get_user_data(r);
  if (!b)
    return;
  if (b->map)
    munmap(b->map, b->map_len);
  close(b->fd);
  free(b);
}

static struct wl_resource *dmabuf_make_buffer(struct wl_client *c,
                                              struct wl_resource *pr, uint32_t id,
                                              int w, int h, uint32_t format,
                                              uint32_t flags, int immed) {
  struct dmabuf_params *p = wl_resource_get_user_data(pr);
  if (p->used) {
    wl_resource_post_error(pr, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_ALREADY_USED,
                           "params ya usado");
    return NULL;
  }
  p->used = 1;
  int ok = p->fd >= 0 && w > 0 && h > 0 && w <= 8192 && h <= 8192 &&
           flags == 0 && (format == DRM_FMT_XRGB8888 || format == DRM_FMT_ARGB8888) &&
           p->stride >= (uint32_t)w * 4;
  if (p->fd < 0) {
    wl_resource_post_error(pr, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INCOMPLETE,
                           "falta el plano 0");
    return NULL;
  }
  struct dmabuf_buf *b = ok ? calloc(1, sizeof(*b)) : NULL;
  if (b) {
    b->fd = p->fd;
    p->fd = -1;
    b->w = w;
    b->h = h;
    b->stride = (int)p->stride;
    b->offset = (int)p->offset;
    b->format = format;
    if (!dmabuf_buf_map(b)) {
      close(b->fd);
      free(b);
      b = NULL;
    }
  }
  if (!b) {
    if (immed)
      wl_resource_post_error(pr, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_FORMAT,
                             "dmabuf no soportado");
    else
      zwp_linux_buffer_params_v1_send_failed(pr);
    return NULL;
  }
  struct wl_resource *buf = wl_resource_create(c, &wl_buffer_interface, 1, id);
  if (!buf) {
    munmap(b->map, b->map_len);
    close(b->fd);
    free(b);
    wl_client_post_no_memory(c);
    return NULL;
  }
  wl_resource_set_implementation(buf, &dmabuf_buffer_impl, b, dmabuf_buf_destroy_res);
  if (!immed)
    zwp_linux_buffer_params_v1_send_created(pr, buf);
  return buf;
}

static void dparams_destroy(struct wl_client *c, struct wl_resource *r) {
  (void)c;
  wl_resource_destroy(r);
}
static void dparams_add(struct wl_client *c, struct wl_resource *r, int32_t fd,
                        uint32_t plane, uint32_t offset, uint32_t stride,
                        uint32_t mod_hi, uint32_t mod_lo) {
  (void)c;
  struct dmabuf_params *p = wl_resource_get_user_data(r);
  if (p->used) {
    close(fd);
    wl_resource_post_error(r, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_ALREADY_USED,
                           "params ya usado");
    return;
  }
  if (plane != 0) {
    close(fd);
    wl_resource_post_error(r, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_PLANE_IDX,
                           "solo hay un plano");
    return;
  }
  if (p->fd >= 0) {
    close(fd);
    wl_resource_post_error(r, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_PLANE_SET,
                           "plano 0 ya anadido");
    return;
  }
  if (mod_hi != 0 || mod_lo != 0) { /* solo DRM_FORMAT_MOD_LINEAR */
    close(fd);
    return; /* el create posterior fallara por plano ausente */
  }
  p->fd = fd;
  p->offset = offset;
  p->stride = stride;
}
static void dparams_create(struct wl_client *c, struct wl_resource *r, int32_t w,
                           int32_t h, uint32_t format, uint32_t flags) {
  dmabuf_make_buffer(c, r, 0, w, h, format, flags, 0);
}
static void dparams_create_immed(struct wl_client *c, struct wl_resource *r,
                                 uint32_t id, int32_t w, int32_t h,
                                 uint32_t format, uint32_t flags) {
  dmabuf_make_buffer(c, r, id, w, h, format, flags, 1);
}
static const struct zwp_linux_buffer_params_v1_interface dparams_impl = {
    .destroy = dparams_destroy,
    .add = dparams_add,
    .create = dparams_create,
    .create_immed = dparams_create_immed,
};
static void dparams_free(struct wl_resource *r) {
  struct dmabuf_params *p = wl_resource_get_user_data(r);
  if (p && p->fd >= 0)
    close(p->fd);
  free(p);
}
static void dmabuf_create_params(struct wl_client *c, struct wl_resource *r,
                                 uint32_t id) {
  struct dmabuf_params *p = calloc(1, sizeof(*p));
  struct wl_resource *pr =
      p ? wl_resource_create(c, &zwp_linux_buffer_params_v1_interface,
                             wl_resource_get_version(r), id)
        : NULL;
  if (!pr) {
    free(p);
    wl_client_post_no_memory(c);
    return;
  }
  p->fd = -1;
  wl_resource_set_implementation(pr, &dparams_impl, p, dparams_free);
}
static const struct zwp_linux_dmabuf_v1_interface dmabuf_impl = {
    .destroy = noop_destroy,
    .create_params = dmabuf_create_params,
};
static void dmabuf_bind(struct wl_client *c, void *d, uint32_t v, uint32_t id) {
  (void)d;
  struct wl_resource *r = wl_resource_create(c, &zwp_linux_dmabuf_v1_interface, v, id);
  if (!r) {
    wl_client_post_no_memory(c);
    return;
  }
  wl_resource_set_implementation(r, &dmabuf_impl, NULL, NULL);
  static const uint32_t fmts[] = {DRM_FMT_XRGB8888, DRM_FMT_ARGB8888};
  for (unsigned i = 0; i < sizeof(fmts) / sizeof(fmts[0]); i++) {
    if (v >= 3)
      zwp_linux_dmabuf_v1_send_modifier(r, fmts[i], 0, 0); /* LINEAR */
    else
      zwp_linux_dmabuf_v1_send_format(r, fmts[i]);
  }
}

// Copia el buffer shm al almacen de la superficie; devuelve el dano en px de
// buffer. En subsuperficies se conserva el alfa; la raiz se fuerza opaca.
static rc_t surface_ingest(struct surface *s, const uint8_t *src, int w, int h,
                           int stride, int argb) {
  int keep_alpha = s->sub != NULL && argb;
  rc_t all = {0, 0, w, h};
  rc_t d = all;
  if (w == s->buf_w && h == s->buf_h && s->has_content &&
      s->store_alpha == keep_alpha && s->dmg_x1 > s->dmg_x0)
    d = rc_clip((rc_t){s->dmg_x0, s->dmg_y0, s->dmg_x1, s->dmg_y1}, all);
  size_t need = (size_t)w * h;
  if (s->scratch_px < need) {
    free(s->scratch);
    s->scratch = malloc(need * 4);
    s->scratch_px = s->scratch ? need : 0;
    d = all;
  }
  if (!s->scratch) {
    s->has_content = 0;
    return (rc_t){0, 0, 0, 0};
  }
  uint32_t force = keep_alpha ? 0 : 0xFF000000u;
  for (int y = d.y0; y < d.y1; y++) {
    const uint32_t *row = (const uint32_t *)(src + (size_t)y * stride);
    uint32_t *dst = s->scratch + (size_t)y * w;
    for (int x = d.x0; x < d.x1; x++)
      dst[x] = row[x] | force;
  }
  s->store_alpha = keep_alpha;
  s->buf_w = w;
  s->buf_h = h;
  s->has_content = 1;
  return d;
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
    fprintf(stderr, "[aurora-wayland] commit s=%p xs=%p sub=%p attach=%d buf=%p dmg=(%d,%d)-(%d,%d)\n",
            (void *)s, (void *)xs, (void *)s->sub, s->has_attach, (void *)s->buffer,
            s->dmg_x0, s->dmg_y0, s->dmg_x1, s->dmg_y1);

  // Estado pendiente: escala y viewport.
  int need_all = 0;
  if (s->scale != s->pend_scale) {
    s->scale = s->pend_scale;
    need_all = 1;
  }
  if (s->vp_src_set != s->pvp_src_set || s->vp_sx != s->pvp_sx ||
      s->vp_sy != s->pvp_sy || s->vp_sw != s->pvp_sw || s->vp_sh != s->pvp_sh ||
      s->vp_dst_w != s->pvp_dst_w || s->vp_dst_h != s->pvp_dst_h) {
    s->vp_src_set = s->pvp_src_set;
    s->vp_sx = s->pvp_sx; s->vp_sy = s->pvp_sy;
    s->vp_sw = s->pvp_sw; s->vp_sh = s->pvp_sh;
    s->vp_dst_w = s->pvp_dst_w; s->vp_dst_h = s->pvp_dst_h;
    need_all = 1;
  }

  int old_dw = s->dw, old_dh = s->dh;
  int tree_member = s->sub || (xs && (tl || xs->is_popup));
  int ingested = 0;
  rc_t ingest_dmg = {0, 0, 0, 0};
  if (s->has_attach) {
    s->has_attach = 0;
    if (s->buffer) {
      struct wl_shm_buffer *shm = wl_shm_buffer_get(s->buffer);
      struct dmabuf_buf *db = dmabuf_buf_from(s->buffer);
      if (shm && tree_member) {
        wl_shm_buffer_begin_access(shm);
        ingest_dmg = surface_ingest(s, wl_shm_buffer_get_data(shm),
                                    wl_shm_buffer_get_width(shm),
                                    wl_shm_buffer_get_height(shm),
                                    wl_shm_buffer_get_stride(shm),
                                    wl_shm_buffer_get_format(shm) ==
                                        WL_SHM_FORMAT_ARGB8888);
        wl_shm_buffer_end_access(shm);
        ingested = 1;
      } else if (db && tree_member && dmabuf_buf_map(db)) {
        ingest_dmg = surface_ingest(s, db->map + db->offset, db->w, db->h,
                                    db->stride, db->format == DRM_FMT_ARGB8888);
        ingested = 1;
      }
      // Ya copiamos el contenido: el cliente puede reutilizar el buffer.
      wl_buffer_send_release(s->buffer);
    } else if (s->has_content) {
      s->has_content = 0; /* attach(NULL): la superficie se desmapea */
      need_all = 1;
    }
  }
  surface_calc_size(s);
  if (s->dw != old_dw || s->dh != old_dh)
    need_all = 1;

  rc_t area = {0, 0, old_dw > s->dw ? old_dw : s->dw,
               old_dh > s->dh ? old_dh : s->dh};
  rc_t dmg = {0, 0, 0, 0};
  if (need_all || s->stack_dirty)
    dmg = area;
  else if (ingested)
    dmg = surface_is_plain(s) ? ingest_dmg : area;
  s->stack_dirty = 0;

  // Posiciones pendientes y dano diferido de las subsuperficies (modo sync).
  struct subsurf *n;
  wl_list_for_each(n, &s->stack, link) {
    if (n == &s->self)
      continue;
    struct surface *cs = n->surface;
    if (n->pos_pending) {
      n->pos_pending = 0;
      if (cs && cs->has_content)
        rc_union(&dmg, rc_move((rc_t){0, 0, cs->dw, cs->dh}, n->x, n->y));
      n->x = n->px;
      n->y = n->py;
      if (cs && cs->has_content)
        rc_union(&dmg, rc_move((rc_t){0, 0, cs->dw, cs->dh}, n->x, n->y));
    }
    if (!rc_empty(n->pdmg)) {
      rc_union(&dmg, rc_move(n->pdmg, n->x, n->y));
      n->pdmg = (rc_t){0, 0, 0, 0};
    }
  }
  if (!rc_empty(dmg))
    surface_damage_up(s, dmg);
  s->dmg_x0 = s->dmg_y0 = s->dmg_x1 = s->dmg_y1 = 0;

  // Frame callbacks y feedback de presentacion: como en un vsync a 60 Hz, no al
  // instante (un cliente que anima y repinta al recibir el frame saturaria la
  // CPU en un bucle commit -> frame -> commit sin fin).
  if (!s->fq && (!wl_list_empty(&s->frames) || !wl_list_empty(&s->pfb))) {
    s->fq = 1;
    wl_list_insert(g_frame_q.prev, &s->flink);
    if (g_frame_timer)
      wl_event_source_timer_update(g_frame_timer, FRAME_MS);
  }
}

static int frame_tick(void *data) {
  (void)data;
  struct surface *s, *tmp;
  wl_list_for_each_safe(s, tmp, &g_frame_q, flink) {
    wl_list_remove(&s->flink);
    s->fq = 0;
    surface_send_presented(s);
    uint32_t t = now_ms();
    struct wl_resource *cb, *ctmp;
    wl_resource_for_each_safe(cb, ctmp, &s->frames) {
      wl_callback_send_done(cb, t);
      wl_resource_destroy(cb);
    }
  }
  wl_display_flush_clients(g_display);
  return 0;
}

static const struct wl_surface_interface surface_impl = {
    .destroy = surface_destroy_req,
    .attach = surface_attach,
    .damage = surface_damage,
    .frame = surface_frame,
    .set_opaque_region = surface_noop_region,
    .set_input_region = surface_set_input_region,
    .commit = surface_commit,
    .set_buffer_transform = surface_set_transform,
    .set_buffer_scale = surface_set_scale,
    .damage_buffer = surface_damage_buffer,
    .offset = surface_offset,
};

static void surface_resource_destroy(struct wl_resource *r) {
  struct surface *s = wl_resource_get_user_data(r);
  if (s->fq) {
    wl_list_remove(&s->flink);
    s->fq = 0;
  }
  if (g_ptr_surf == s)
    g_ptr_surf = NULL;
  // Si era una subsuperficie, desaparece de su padre.
  if (s->sub) {
    struct surface *p = s->sub->parent;
    rc_t d = rc_move((rc_t){0, 0, s->dw, s->dh}, s->sub->x, s->sub->y);
    s->sub->surface = NULL;
    sub_unlink(s->sub);
    s->sub = NULL;
    if (p && !rc_empty(d))
      surface_damage_up(p, d);
  }
  // Sus hijas quedan huerfanas (los objetos wl_subsurface siguen existiendo).
  struct subsurf *n, *tmpn;
  wl_list_for_each_safe(n, tmpn, &s->stack, link) {
    if (n == &s->self)
      continue;
    wl_list_remove(&n->link);
    wl_list_init(&n->link);
    n->parent = NULL;
  }
  if (s->vp)
    wl_resource_set_user_data(s->vp, NULL);
  {
    struct wl_resource *fb, *ftmp;
    wl_resource_for_each_safe(fb, ftmp, &s->pfb) {
      wp_presentation_feedback_send_discarded(fb);
      wl_resource_destroy(fb);
    }
  }
  surface_clear_buffer(s);
  struct wl_resource *cb, *tmp;
  wl_resource_for_each_safe(cb, tmp, &s->frames) wl_resource_destroy(cb);
  if (s->xs)
    s->xs->surface = NULL;
  free(s->scratch);
  free(s->canvas);
  free(s->in_reg);
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
  wl_list_init(&s->pfb);
  wl_list_init(&s->buffer_destroy.link);
  wl_list_init(&s->stack);
  s->self.surface = s;
  wl_list_insert(&s->stack, &s->self.link);
  s->scale = s->pend_scale = 1;
  s->vp_dst_w = s->vp_dst_h = s->pvp_dst_w = s->pvp_dst_h = -1;
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
  struct region *rg = calloc(1, sizeof(*rg));
  if (!rg) {
    wl_resource_destroy(reg);
    wl_client_post_no_memory(c);
    return;
  }
  wl_resource_set_implementation(reg, &region_impl, rg, region_resource_destroy);
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
// wl_subcompositor: las subsuperficies se componen dentro de la ventana de su
// superficie raiz (ver "Arbol de superficies"). Posicion y orden se aplican en
// el commit del padre; en modo sync el contenido tambien.
// ---------------------------------------------------------------------------
static void sub_set_position(struct wl_client *c, struct wl_resource *r,
                             int32_t x, int32_t y) {
  (void)c;
  struct subsurf *sb = wl_resource_get_user_data(r);
  sb->px = x;
  sb->py = y;
  sb->pos_pending = 1;
}
// Nodo de la pila de `sb->parent` que corresponde a la superficie `sibres`.
static struct subsurf *sub_sibling(struct subsurf *sb, struct wl_resource *sibres) {
  struct surface *sib = wl_resource_get_user_data(sibres);
  if (!sb->parent || !sib)
    return NULL;
  if (sib == sb->parent)
    return &sib->self;
  if (sib->sub && sib->sub->parent == sb->parent && sib->sub != sb)
    return sib->sub;
  return NULL;
}
static void sub_place_above(struct wl_client *c, struct wl_resource *r,
                            struct wl_resource *sibres) {
  (void)c;
  struct subsurf *sb = wl_resource_get_user_data(r);
  struct subsurf *sib = sub_sibling(sb, sibres);
  if (!sib) {
    wl_resource_post_error(r, WL_SUBSURFACE_ERROR_BAD_SURFACE,
                           "place_above: no es hermana ni padre");
    return;
  }
  wl_list_remove(&sb->link);
  wl_list_insert(&sib->link, &sb->link);
  sb->parent->stack_dirty = 1;
}
static void sub_place_below(struct wl_client *c, struct wl_resource *r,
                            struct wl_resource *sibres) {
  (void)c;
  struct subsurf *sb = wl_resource_get_user_data(r);
  struct subsurf *sib = sub_sibling(sb, sibres);
  if (!sib) {
    wl_resource_post_error(r, WL_SUBSURFACE_ERROR_BAD_SURFACE,
                           "place_below: no es hermana ni padre");
    return;
  }
  wl_list_remove(&sb->link);
  wl_list_insert(sib->link.prev, &sb->link);
  sb->parent->stack_dirty = 1;
}
static void sub_set_sync(struct wl_client *c, struct wl_resource *r) {
  (void)c;
  ((struct subsurf *)wl_resource_get_user_data(r))->sync = 1;
}
static void sub_set_desync(struct wl_client *c, struct wl_resource *r) {
  (void)c;
  struct subsurf *sb = wl_resource_get_user_data(r);
  sb->sync = 0;
  // El estado retenido pasa a ser efectivo.
  if (sb->surface && sb->parent && !rc_empty(sb->pdmg)) {
    rc_t d = sb->pdmg;
    sb->pdmg = (rc_t){0, 0, 0, 0};
    surface_damage_up(sb->surface, d);
  }
}
static const struct wl_subsurface_interface subsurface_impl = {
    .destroy = noop_destroy,
    .set_position = sub_set_position,
    .place_above = sub_place_above,
    .place_below = sub_place_below,
    .set_sync = sub_set_sync,
    .set_desync = sub_set_desync,
};
static void subsurface_resource_destroy(struct wl_resource *r) {
  struct subsurf *sb = wl_resource_get_user_data(r);
  struct surface *cs = sb->surface, *p = sb->parent;
  rc_t d = {0, 0, 0, 0};
  if (cs && p && cs->has_content)
    d = rc_move((rc_t){0, 0, cs->dw, cs->dh}, sb->x, sb->y);
  sub_unlink(sb);
  if (cs)
    cs->sub = NULL;
  if (p && !rc_empty(d))
    surface_damage_up(p, d);
  free(sb);
}
static void subc_get_subsurface(struct wl_client *c, struct wl_resource *r,
                                uint32_t id, struct wl_resource *surf,
                                struct wl_resource *parent) {
  struct surface *cs = wl_resource_get_user_data(surf);
  struct surface *ps = wl_resource_get_user_data(parent);
  if (cs == ps || cs->sub || cs->xs) {
    wl_resource_post_error(r, WL_SUBCOMPOSITOR_ERROR_BAD_SURFACE,
                           "la superficie ya tiene rol o es su propio padre");
    return;
  }
  for (struct surface *a = ps; a; a = a->sub ? a->sub->parent : NULL)
    if (a == cs) {
      wl_resource_post_error(r, WL_SUBCOMPOSITOR_ERROR_BAD_PARENT,
                             "ciclo de subsuperficies");
      return;
    }
  struct subsurf *sb = calloc(1, sizeof(*sb));
  if (!sb) {
    wl_client_post_no_memory(c);
    return;
  }
  sb->res = wl_resource_create(c, &wl_subsurface_interface,
                               wl_resource_get_version(r), id);
  if (!sb->res) {
    free(sb);
    wl_client_post_no_memory(c);
    return;
  }
  sb->surface = cs;
  sb->parent = ps;
  sb->sync = 1; /* por defecto una subsuperficie nace en modo sync */
  wl_list_insert(ps->stack.prev, &sb->link); /* encima de las demas */
  cs->sub = sb;
  wl_resource_set_implementation(sb->res, &subsurface_impl, sb,
                                 subsurface_resource_destroy);
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
    struct wl_client *oc = g_kbd_focus->client;
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
  struct wl_client *nc = tl->client;
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
  primary_send_to(nc);
}

// El cursor visible es siempre uno de Aurora (cursor_shape_t del compositor).
// Las imagenes que mande el cliente (wl_pointer.set_cursor con superficie, o
// sin ella para ocultarlo) nunca se muestran: se descartan y queda la flecha.
// La forma solo puede pedirse con wp_cursor_shape_v1 (cursor_shape_apply).
// Se fija en la ventana bajo el puntero de ese cliente; el compositor la usa
// solo sobre el area de cliente (bordes y barra de titulo siguen siendo suyos).
static void cursor_shape_apply(struct wl_client *c, int shape) {
  if (g_ptr_focus && g_ptr_focus->win_id >= 0 &&
      xs_client(g_ptr_focus) == c)
    win_set_cursor(g_ptr_focus->win_id, shape);
}
static void ptr_set_cursor(struct wl_client *c, struct wl_resource *r,
                           uint32_t serial, struct wl_resource *surf,
                           int32_t hx, int32_t hy) {
  (void)r; (void)serial; (void)surf; (void)hx; (void)hy;
  cursor_shape_apply(c, AUR_CURSOR_ARROW);
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
  if (g_kbd_focus && g_kbd_focus->client == c &&
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
// El cliente pide mover/redimensionar su ventana (arrastre de su propia barra o
// borde). Aurora lo hace con su arrastre nativo, solo si el boton sigue pulsado.
static void tl_move(struct wl_client *c, struct wl_resource *r, struct wl_resource *seat, uint32_t serial) {
  (void)c; (void)seat; (void)serial;
  struct toplevel *tl = wl_resource_get_user_data(r);
  if (tl->xs && tl->xs->win_id >= 0)
    win_begin_grab(tl->xs->win_id, 0);
}
static void tl_resize(struct wl_client *c, struct wl_resource *r, struct wl_resource *seat, uint32_t serial, uint32_t edges) {
  (void)c; (void)seat; (void)serial;
  struct toplevel *tl = wl_resource_get_user_data(r);
  // xdg: top=1 bottom=2 left=4 right=8; compositor: right=1 bottom=2 left=4 top=8
  int e = 0;
  if (edges & XDG_TOPLEVEL_RESIZE_EDGE_TOP) e |= 8;
  if (edges & XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM) e |= 2;
  if (edges & XDG_TOPLEVEL_RESIZE_EDGE_LEFT) e |= 4;
  if (edges & XDG_TOPLEVEL_RESIZE_EDGE_RIGHT) e |= 1;
  if (e && tl->xs && tl->xs->win_id >= 0)
    win_begin_grab(tl->xs->win_id, e);
}
static void tl_set_min_size(struct wl_client *c, struct wl_resource *r, int32_t w, int32_t h) {
  (void)c;
  struct toplevel *tl = wl_resource_get_user_data(r);
  tl->min_w = w; tl->min_h = h;
  // Si la ventana ya existe y es mas pequena que el minimo del cliente, se
  // agranda (la geometria de ventana excluye las sombras CSD; el recorte de
  // marcos se suma tambien).
  struct xsurf *xs = tl->xs;
  if (xs && xs->win_id >= 0 && !xs->is_popup && w > 0 && h > 0) {
    int mw = w, mh = h;
    if (tl->inset_state == 1) {
      mw -= tl->in_l + tl->in_r;
      mh -= tl->in_t + tl->in_b;
    }
    if (mw > xs->cw || mh > xs->ch) {
      int nw = mw > xs->cw ? mw : xs->cw, nh = mh > xs->ch ? mh : xs->ch;
      win_request_resize(xs->win_id, nw, nh + TITLEBAR_H);
    }
  }
}
static void tl_set_max_size(struct wl_client *c, struct wl_resource *r, int32_t w, int32_t h) {
  (void)c;
  struct toplevel *tl = wl_resource_get_user_data(r);
  tl->max_w = w; tl->max_h = h;
}
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
// Pantalla completa = maximizada (Aurora conserva menubar y Dock); el estado
// `fullscreen` se anuncia para que el cliente adapte su interfaz.
static void tl_set_fullscreen(struct wl_client *c, struct wl_resource *r, struct wl_resource *o) {
  (void)o;
  struct toplevel *tl = wl_resource_get_user_data(r);
  tl->fullscreen = 1;
  tl_set_maximized(c, r);
}
static void tl_unset_fullscreen(struct wl_client *c, struct wl_resource *r) {
  struct toplevel *tl = wl_resource_get_user_data(r);
  if (!tl->fullscreen)
    return;
  tl->fullscreen = 0;
  tl_unset_maximized(c, r);
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
    .unset_fullscreen = tl_unset_fullscreen,
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
  tl->client = c;
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
// El modo es siempre server-side. Se anuncia una sola vez por objeto: repetir
// el evento en cada peticion hace que algunos toolkits (GTK3) vuelvan a pedir
// el modo cliente en un bucle sin fin.
static void deco_set_mode(struct wl_client *c, struct wl_resource *r, uint32_t m) {
  (void)c; (void)m;
  (void)r; /* ya se anuncio al crear el objeto */
}
static void deco_unset_mode(struct wl_client *c, struct wl_resource *r) {
  (void)c; (void)r;
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
  (void)c; (void)r; (void)m;
  /* Siempre server; el modo ya se anuncio al crear (ver deco_set_mode). */
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
// Seleccion primaria (zwp_primary_selection_device_manager_v1): el texto
// seleccionado se pega con el boton central. Misma mecanica que el portapapeles.
// ---------------------------------------------------------------------------
static void psoffer_receive(struct wl_client *c, struct wl_resource *r,
                            const char *mime, int32_t fd) {
  (void)c;
  struct data_source *src = wl_resource_get_user_data(r);
  if (src && src == g_primary)
    zwp_primary_selection_source_v1_send_send(src->res, mime, fd);
  close(fd);
}
static const struct zwp_primary_selection_offer_v1_interface psoffer_impl = {
    .receive = psoffer_receive,
    .destroy = noop_destroy,
};
static void primary_send_to(struct wl_client *client) {
  struct seat_res *d;
  wl_list_for_each(d, &g_psdevs, link) {
    if (wl_resource_get_client(d->res) != client)
      continue;
    if (!g_primary) {
      zwp_primary_selection_device_v1_send_selection(d->res, NULL);
      continue;
    }
    struct wl_resource *offer = wl_resource_create(
        client, &zwp_primary_selection_offer_v1_interface,
        wl_resource_get_version(d->res), 0);
    if (!offer)
      continue;
    wl_resource_set_implementation(offer, &psoffer_impl, g_primary, NULL);
    zwp_primary_selection_device_v1_send_data_offer(d->res, offer);
    const char *p = g_primary->mimes.data;
    size_t left = g_primary->mimes.size;
    while (left > 0) {
      size_t n = strnlen(p, left);
      zwp_primary_selection_offer_v1_send_offer(offer, p);
      p += n + 1;
      left -= (n + 1 > left) ? left : n + 1;
    }
    zwp_primary_selection_device_v1_send_selection(d->res, offer);
  }
}
static void primary_send_to_focus(void) {
  if (g_kbd_focus)
    primary_send_to(g_kbd_focus->client);
}
static void pssrc_offer(struct wl_client *c, struct wl_resource *r, const char *mime) {
  ds_offer(c, r, mime);
}
static const struct zwp_primary_selection_source_v1_interface pssrc_impl = {
    .offer = pssrc_offer,
    .destroy = noop_destroy,
};
static void pssrc_resource_destroy(struct wl_resource *r) {
  struct data_source *s = wl_resource_get_user_data(r);
  if (g_primary == s) {
    g_primary = NULL;
    primary_send_to_focus();
  }
  wl_array_release(&s->mimes);
  free(s);
}
static void psm_create_source(struct wl_client *c, struct wl_resource *r, uint32_t id) {
  struct data_source *s = calloc(1, sizeof(*s));
  if (!s) {
    wl_client_post_no_memory(c);
    return;
  }
  wl_array_init(&s->mimes);
  s->res = wl_resource_create(c, &zwp_primary_selection_source_v1_interface,
                              wl_resource_get_version(r), id);
  if (!s->res) {
    free(s);
    wl_client_post_no_memory(c);
    return;
  }
  wl_resource_set_implementation(s->res, &pssrc_impl, s, pssrc_resource_destroy);
}
static void psdev_set_selection(struct wl_client *c, struct wl_resource *r,
                                struct wl_resource *src, uint32_t serial) {
  (void)c; (void)r; (void)serial;
  g_primary = src ? wl_resource_get_user_data(src) : NULL;
  primary_send_to_focus();
}
static const struct zwp_primary_selection_device_v1_interface psdev_impl = {
    .set_selection = psdev_set_selection,
    .destroy = noop_destroy,
};
static void psm_get_device(struct wl_client *c, struct wl_resource *r, uint32_t id,
                           struct wl_resource *seat) {
  (void)seat;
  struct wl_resource *dd = wl_resource_create(
      c, &zwp_primary_selection_device_v1_interface, wl_resource_get_version(r), id);
  if (!dd) {
    wl_client_post_no_memory(c);
    return;
  }
  struct seat_res *sr = seat_res_add(&g_psdevs, dd);
  wl_resource_set_implementation(dd, &psdev_impl, sr, seat_res_destroy);
  if (g_kbd_focus && g_kbd_focus->client == c)
    primary_send_to(c);
}
static const struct zwp_primary_selection_device_manager_v1_interface psm_impl = {
    .create_source = psm_create_source,
    .get_device = psm_get_device,
    .destroy = noop_destroy,
};
static void psm_bind(struct wl_client *c, void *d, uint32_t v, uint32_t id) {
  (void)d;
  struct wl_resource *r = wl_resource_create(
      c, &zwp_primary_selection_device_manager_v1_interface, v, id);
  if (!r) {
    wl_client_post_no_memory(c);
    return;
  }
  wl_resource_set_implementation(r, &psm_impl, NULL, NULL);
}

// ---------------------------------------------------------------------------
// wp_viewporter: recorte (set_source) y escalado (set_destination) del buffer.
// El estado es pendiente hasta el commit de la superficie.
// ---------------------------------------------------------------------------
static void vp_set_source(struct wl_client *c, struct wl_resource *r,
                          wl_fixed_t x, wl_fixed_t y, wl_fixed_t w, wl_fixed_t h) {
  (void)c;
  struct surface *s = wl_resource_get_user_data(r);
  if (!s) {
    wl_resource_post_error(r, WP_VIEWPORT_ERROR_NO_SURFACE, "superficie destruida");
    return;
  }
  if (x == wl_fixed_from_int(-1) && y == wl_fixed_from_int(-1) &&
      w == wl_fixed_from_int(-1) && h == wl_fixed_from_int(-1)) {
    s->pvp_src_set = 0;
    return;
  }
  if (x < 0 || y < 0 || w <= 0 || h <= 0) {
    wl_resource_post_error(r, WP_VIEWPORT_ERROR_BAD_VALUE, "origen/tamano invalidos");
    return;
  }
  s->pvp_src_set = 1;
  s->pvp_sx = wl_fixed_to_int(x);
  s->pvp_sy = wl_fixed_to_int(y);
  s->pvp_sw = (wl_fixed_to_int(w) < 1) ? 1 : wl_fixed_to_int(w);
  s->pvp_sh = (wl_fixed_to_int(h) < 1) ? 1 : wl_fixed_to_int(h);
}
static void vp_set_destination(struct wl_client *c, struct wl_resource *r,
                               int32_t w, int32_t h) {
  (void)c;
  struct surface *s = wl_resource_get_user_data(r);
  if (!s) {
    wl_resource_post_error(r, WP_VIEWPORT_ERROR_NO_SURFACE, "superficie destruida");
    return;
  }
  if (w == -1 && h == -1) {
    s->pvp_dst_w = s->pvp_dst_h = -1;
    return;
  }
  if (w <= 0 || h <= 0) {
    wl_resource_post_error(r, WP_VIEWPORT_ERROR_BAD_VALUE, "destino invalido");
    return;
  }
  s->pvp_dst_w = w;
  s->pvp_dst_h = h;
}
static const struct wp_viewport_interface viewport_impl = {
    .destroy = noop_destroy,
    .set_source = vp_set_source,
    .set_destination = vp_set_destination,
};
static void viewport_resource_destroy(struct wl_resource *r) {
  struct surface *s = wl_resource_get_user_data(r);
  if (!s)
    return;
  s->vp = NULL;
  s->pvp_src_set = 0; /* sin viewport: el siguiente commit lo deshace */
  s->pvp_dst_w = s->pvp_dst_h = -1;
}
static void vpm_get_viewport(struct wl_client *c, struct wl_resource *r,
                             uint32_t id, struct wl_resource *surf) {
  struct surface *s = wl_resource_get_user_data(surf);
  if (s->vp) {
    wl_resource_post_error(r, WP_VIEWPORTER_ERROR_VIEWPORT_EXISTS,
                           "la superficie ya tiene un viewport");
    return;
  }
  struct wl_resource *vp = wl_resource_create(c, &wp_viewport_interface,
                                              wl_resource_get_version(r), id);
  if (!vp) {
    wl_client_post_no_memory(c);
    return;
  }
  s->vp = vp;
  wl_resource_set_implementation(vp, &viewport_impl, s, viewport_resource_destroy);
}
static const struct wp_viewporter_interface viewporter_impl = {
    .destroy = noop_destroy,
    .get_viewport = vpm_get_viewport,
};
static void viewporter_bind(struct wl_client *c, void *d, uint32_t v, uint32_t id) {
  (void)d;
  struct wl_resource *r = wl_resource_create(c, &wp_viewporter_interface, v, id);
  if (!r) {
    wl_client_post_no_memory(c);
    return;
  }
  wl_resource_set_implementation(r, &viewporter_impl, NULL, NULL);
}

// ---------------------------------------------------------------------------
// wp_fractional_scale: la salida es 1.0x (120/120).
// ---------------------------------------------------------------------------
static const struct wp_fractional_scale_v1_interface fscale_impl = {
    .destroy = noop_destroy,
};
static void fsm_get_fractional_scale(struct wl_client *c, struct wl_resource *r,
                                     uint32_t id, struct wl_resource *surf) {
  (void)surf;
  struct wl_resource *f = wl_resource_create(c, &wp_fractional_scale_v1_interface,
                                             wl_resource_get_version(r), id);
  if (!f) {
    wl_client_post_no_memory(c);
    return;
  }
  wl_resource_set_implementation(f, &fscale_impl, NULL, NULL);
  wp_fractional_scale_v1_send_preferred_scale(f, 120);
}
static const struct wp_fractional_scale_manager_v1_interface fsm_impl = {
    .destroy = noop_destroy,
    .get_fractional_scale = fsm_get_fractional_scale,
};
static void fsm_bind(struct wl_client *c, void *d, uint32_t v, uint32_t id) {
  (void)d;
  struct wl_resource *r =
      wl_resource_create(c, &wp_fractional_scale_manager_v1_interface, v, id);
  if (!r) {
    wl_client_post_no_memory(c);
    return;
  }
  wl_resource_set_implementation(r, &fsm_impl, NULL, NULL);
}

// ---------------------------------------------------------------------------
// wp_cursor_shape_v1: cada forma estandar se traduce a la forma de Aurora mas
// parecida (cursor.c); lo que no tiene equivalente queda como flecha.
// ---------------------------------------------------------------------------
static int aur_cursor_for_shape(uint32_t shape) {
  switch (shape) {
  case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER:
  case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ALIAS:
  case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRAB:
    return AUR_CURSOR_POINTER;
  case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRABBING:
  case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_MOVE:
  case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ALL_SCROLL:
    return AUR_CURSOR_GRABBING;
  case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT:
  case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_VERTICAL_TEXT:
    return AUR_CURSOR_TEXT;
  case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_WAIT:
  case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_PROGRESS:
    return AUR_CURSOR_WAIT;
  case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NOT_ALLOWED:
  case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NO_DROP:
    return AUR_CURSOR_NOT_ALLOWED;
  case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_E_RESIZE:
  case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_W_RESIZE:
  case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_EW_RESIZE:
  case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_COL_RESIZE:
    return AUR_CURSOR_RESIZE_EW;
  case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_N_RESIZE:
  case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_S_RESIZE:
  case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NS_RESIZE:
  case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ROW_RESIZE:
    return AUR_CURSOR_RESIZE_NS;
  case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NE_RESIZE:
  case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_SW_RESIZE:
  case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NESW_RESIZE:
    return AUR_CURSOR_RESIZE_NESW;
  case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NW_RESIZE:
  case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_SE_RESIZE:
  case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NWSE_RESIZE:
    return AUR_CURSOR_RESIZE_NWSE;
  default:
    return AUR_CURSOR_ARROW;
  }
}
static void csdev_set_shape(struct wl_client *c, struct wl_resource *r,
                            uint32_t serial, uint32_t shape) {
  (void)serial;
  if (shape < WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT ||
      shape > WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ZOOM_OUT) {
    wl_resource_post_error(r, WP_CURSOR_SHAPE_DEVICE_V1_ERROR_INVALID_SHAPE,
                           "forma de cursor %u invalida", shape);
    return;
  }
  cursor_shape_apply(c, aur_cursor_for_shape(shape));
}
static const struct wp_cursor_shape_device_v1_interface csdev_impl = {
    .destroy = noop_destroy,
    .set_shape = csdev_set_shape,
};
static void csm_new_device(struct wl_client *c, struct wl_resource *r, uint32_t id) {
  struct wl_resource *d = wl_resource_create(c, &wp_cursor_shape_device_v1_interface,
                                             wl_resource_get_version(r), id);
  if (!d) {
    wl_client_post_no_memory(c);
    return;
  }
  wl_resource_set_implementation(d, &csdev_impl, NULL, NULL);
}
static void csm_get_pointer(struct wl_client *c, struct wl_resource *r, uint32_t id,
                            struct wl_resource *pointer) {
  (void)pointer;
  csm_new_device(c, r, id);
}
static void csm_get_tablet_tool(struct wl_client *c, struct wl_resource *r,
                                uint32_t id, struct wl_resource *tool) {
  (void)tool;
  csm_new_device(c, r, id);
}
static const struct wp_cursor_shape_manager_v1_interface csm_impl = {
    .destroy = noop_destroy,
    .get_pointer = csm_get_pointer,
    .get_tablet_tool_v2 = csm_get_tablet_tool,
};
static void csm_bind(struct wl_client *c, void *d, uint32_t v, uint32_t id) {
  (void)d;
  struct wl_resource *r =
      wl_resource_create(c, &wp_cursor_shape_manager_v1_interface, v, id);
  if (!r) {
    wl_client_post_no_memory(c);
    return;
  }
  wl_resource_set_implementation(r, &csm_impl, NULL, NULL);
}

// ---------------------------------------------------------------------------
// zxdg_output_manager_v1: tamano logico de la salida.
// ---------------------------------------------------------------------------
static const struct zxdg_output_v1_interface xdgout_impl = {.destroy = noop_destroy};
static void xom_get_xdg_output(struct wl_client *c, struct wl_resource *r,
                               uint32_t id, struct wl_resource *output) {
  (void)output;
  struct wl_resource *o = wl_resource_create(c, &zxdg_output_v1_interface,
                                             wl_resource_get_version(r), id);
  if (!o) {
    wl_client_post_no_memory(c);
    return;
  }
  wl_resource_set_implementation(o, &xdgout_impl, NULL, NULL);
  zxdg_output_v1_send_logical_position(o, 0, 0);
  zxdg_output_v1_send_logical_size(o, OUT_W, OUT_H);
  if (wl_resource_get_version(o) >= 2) {
    zxdg_output_v1_send_name(o, "Aurora-1");
    zxdg_output_v1_send_description(o, "Pantalla de Aurora");
  }
  zxdg_output_v1_send_done(o);
}
static const struct zxdg_output_manager_v1_interface xom_impl = {
    .destroy = noop_destroy,
    .get_xdg_output = xom_get_xdg_output,
};
static void xom_bind(struct wl_client *c, void *d, uint32_t v, uint32_t id) {
  (void)d;
  struct wl_resource *r = wl_resource_create(c, &zxdg_output_manager_v1_interface, v, id);
  if (!r) {
    wl_client_post_no_memory(c);
    return;
  }
  wl_resource_set_implementation(r, &xom_impl, NULL, NULL);
}

// ---------------------------------------------------------------------------
// wp_presentation: la "presentacion" ocurre en el commit (se vuelca a la
// ventana de forma sincrona), a 60 Hz nominales.
// ---------------------------------------------------------------------------
static void pfb_destroy(struct wl_resource *r) {
  wl_list_remove(wl_resource_get_link(r));
}
static void pres_feedback(struct wl_client *c, struct wl_resource *r,
                          struct wl_resource *surf, uint32_t id) {
  struct surface *s = wl_resource_get_user_data(surf);
  struct wl_resource *fb = wl_resource_create(c, &wp_presentation_feedback_interface,
                                              wl_resource_get_version(r), id);
  if (!fb) {
    wl_client_post_no_memory(c);
    return;
  }
  wl_resource_set_implementation(fb, NULL, NULL, pfb_destroy);
  wl_list_insert(s->pfb.prev, wl_resource_get_link(fb));
}
static const struct wp_presentation_interface pres_impl = {
    .destroy = noop_destroy,
    .feedback = pres_feedback,
};
static void pres_bind(struct wl_client *c, void *d, uint32_t v, uint32_t id) {
  (void)d;
  struct wl_resource *r = wl_resource_create(c, &wp_presentation_interface, v, id);
  if (!r) {
    wl_client_post_no_memory(c);
    return;
  }
  wl_resource_set_implementation(r, &pres_impl, NULL, NULL);
  wp_presentation_send_clock_id(r, CLOCK_MONOTONIC);
}
static void surface_send_presented(struct surface *s) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  static uint64_t seq;
  seq++;
  struct wl_resource *fb, *tmp;
  wl_resource_for_each_safe(fb, tmp, &s->pfb) {
    wp_presentation_feedback_send_presented(
        fb, (uint32_t)((uint64_t)ts.tv_sec >> 32), (uint32_t)ts.tv_sec,
        (uint32_t)ts.tv_nsec, 16666667, (uint32_t)(seq >> 32), (uint32_t)seq, 0);
    wl_resource_destroy(fb);
  }
}

// ---------------------------------------------------------------------------
// zwp_idle_inhibit_manager_v1 y xdg_activation_v1: se aceptan (no hay
// salvapantallas; la activacion de ventanas ajenas no se concede).
// ---------------------------------------------------------------------------
static const struct zwp_idle_inhibitor_v1_interface inhibitor_impl = {.destroy = noop_destroy};
static void iim_create(struct wl_client *c, struct wl_resource *r, uint32_t id,
                       struct wl_resource *surf) {
  (void)surf;
  struct wl_resource *i = wl_resource_create(c, &zwp_idle_inhibitor_v1_interface,
                                             wl_resource_get_version(r), id);
  if (!i) {
    wl_client_post_no_memory(c);
    return;
  }
  wl_resource_set_implementation(i, &inhibitor_impl, NULL, NULL);
}
static const struct zwp_idle_inhibit_manager_v1_interface iim_impl = {
    .destroy = noop_destroy,
    .create_inhibitor = iim_create,
};
static void iim_bind(struct wl_client *c, void *d, uint32_t v, uint32_t id) {
  (void)d;
  struct wl_resource *r = wl_resource_create(c, &zwp_idle_inhibit_manager_v1_interface, v, id);
  if (!r) {
    wl_client_post_no_memory(c);
    return;
  }
  wl_resource_set_implementation(r, &iim_impl, NULL, NULL);
}

static void act_tok_set_serial(struct wl_client *c, struct wl_resource *r, uint32_t s, struct wl_resource *seat) {
  (void)c; (void)r; (void)s; (void)seat;
}
static void act_tok_set_app_id(struct wl_client *c, struct wl_resource *r, const char *a) {
  (void)c; (void)r; (void)a;
}
static void act_tok_set_surface(struct wl_client *c, struct wl_resource *r, struct wl_resource *s) {
  (void)c; (void)r; (void)s;
}
static void act_tok_commit(struct wl_client *c, struct wl_resource *r) {
  (void)c;
  static unsigned n;
  char tok[32];
  snprintf(tok, sizeof(tok), "aurora-%u", ++n);
  xdg_activation_token_v1_send_done(r, tok);
}
static const struct xdg_activation_token_v1_interface act_tok_impl = {
    .set_serial = act_tok_set_serial,
    .set_app_id = act_tok_set_app_id,
    .set_surface = act_tok_set_surface,
    .commit = act_tok_commit,
    .destroy = noop_destroy,
};
static void act_get_token(struct wl_client *c, struct wl_resource *r, uint32_t id) {
  struct wl_resource *t = wl_resource_create(c, &xdg_activation_token_v1_interface,
                                             wl_resource_get_version(r), id);
  if (!t) {
    wl_client_post_no_memory(c);
    return;
  }
  wl_resource_set_implementation(t, &act_tok_impl, NULL, NULL);
}
static void act_activate(struct wl_client *c, struct wl_resource *r, const char *tok, struct wl_resource *surf) {
  (void)c; (void)r; (void)tok; (void)surf;
}
static const struct xdg_activation_v1_interface act_impl = {
    .destroy = noop_destroy,
    .get_activation_token = act_get_token,
    .activate = act_activate,
};
static void act_bind(struct wl_client *c, void *d, uint32_t v, uint32_t id) {
  (void)d;
  struct wl_resource *r = wl_resource_create(c, &xdg_activation_v1_interface, v, id);
  if (!r) {
    wl_client_post_no_memory(c);
    return;
  }
  wl_resource_set_implementation(r, &act_impl, NULL, NULL);
}

// ---------------------------------------------------------------------------
// Eventos de Aurora -> Wayland
// ---------------------------------------------------------------------------
static void ptr_send_to(struct wl_client *c, void (*fn)(struct wl_resource *, void *), void *arg) {
  struct seat_res *p;
  wl_list_for_each(p, &g_pointers, link)
    if (wl_resource_get_client(p->res) == c)
      fn(p->res, arg);
}

struct ptr_ev { struct surface *surf; int x, y; uint32_t serial; uint32_t time; uint32_t button; uint32_t state; };
static void do_enter(struct wl_resource *p, void *a) {
  struct ptr_ev *e = a;
  wl_pointer_send_enter(p, e->serial, e->surf->res, wl_fixed_from_int(e->x),
                        wl_fixed_from_int(e->y));
}
static void do_leave(struct wl_resource *p, void *a) {
  struct ptr_ev *e = a;
  wl_pointer_send_leave(p, e->serial, e->surf->res);
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

// El puntero sale de la superficie con foco (sin tocar g_ptr_focus).
static void pointer_surf_leave(void) {
  if (!g_ptr_surf)
    return;
  struct ptr_ev e = {.surf = g_ptr_surf, .serial = wl_display_next_serial(g_display)};
  struct wl_client *c = wl_resource_get_client(g_ptr_surf->res);
  ptr_send_to(c, do_leave, &e);
  ptr_send_to(c, do_frame, &e);
  g_ptr_surf = NULL;
}

static void pointer_leave(struct xsurf *xs) {
  if (g_ptr_focus != xs)
    return;
  pointer_surf_leave();
  g_ptr_focus = NULL;
  xs->last_buttons = 0;
}

// Superficie (raiz o subsuperficie) mas alta bajo (px,py), coordenadas de la
// superficie raiz. Las regiones de entrada no se tienen en cuenta.
static struct surface *surface_at(struct surface *s, int ox, int oy, int px, int py) {
  struct subsurf *n;
  wl_list_for_each_reverse(n, &s->stack, link) {
    if (n == &s->self) {
      if (s->has_content && px >= ox && py >= oy && px < ox + s->dw &&
          py < oy + s->dh && region_contains(s->in_reg, px - ox, py - oy))
        return s;
    } else if (n->surface && n->surface->has_content) {
      struct surface *r = surface_at(n->surface, ox + n->x, oy + n->y, px, py);
      if (r)
        return r;
    }
  }
  return NULL;
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
  int rx = x + xs->vx, ry = y + xs->vy;
  struct ptr_ev e = {.time = now_ms()};
  if (g_debug)
    fprintf(stderr, "[aurora-wayland] ptr win=%d (%d,%d) btn=%u\n", xs->win_id,
            x, y, buttons);
  if (!xs->surface)
    return;
  if (g_ptr_focus != xs) {
    if (g_ptr_focus)
      pointer_leave(g_ptr_focus);
    pointer_surf_leave();
    g_ptr_focus = xs;
    xs->last_buttons = 0;
  }
  // Con un boton pulsado el puntero queda "agarrado" a su superficie.
  struct surface *target = NULL;
  if (xs->last_buttons && g_ptr_surf && tree_root(g_ptr_surf) == xs->surface)
    target = g_ptr_surf;
  if (!target)
    target = surface_at(xs->surface, 0, 0, rx, ry);
  if (!target)
    target = xs->surface;
  int ox, oy;
  surface_origin(target, &ox, &oy);
  e.surf = target;
  e.x = rx - ox;
  e.y = ry - oy;
  struct wl_client *cl = wl_resource_get_client(target->res);
  if (g_ptr_surf != target) {
    pointer_surf_leave();
    g_ptr_surf = target;
    e.serial = wl_display_next_serial(g_display);
    ptr_send_to(cl, do_enter, &e);
  }
  ptr_send_to(cl, do_motion, &e);
  static const uint32_t codes[3] = {BTN_LEFT, BTN_RIGHT, BTN_MIDDLE};
  for (int b = 0; b < 3; b++) {
    int was = (xs->last_buttons >> b) & 1, is = (buttons >> b) & 1;
    if (was != is) {
      e.serial = wl_display_next_serial(g_display);
      e.button = codes[b];
      e.state = is ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED;
      ptr_send_to(cl, do_button, &e);
      // Pulsar sobre la ventana de la que cuelga un popup (no sobre el propio
      // popup) lo cierra, como en cualquier compositor con grab de popups.
      if (is && !xs->is_popup)
        popups_dismiss(xs, NULL);
    }
  }
  xs->last_buttons = (int)buttons;
  ptr_send_to(cl, do_frame, &e);
}

// ---------------------------------------------------------------------------
// Xwayland + gestor de ventanas X11 interno
//
// Las apps X11 corren sobre Xwayland (rootless, memoria compartida wl_shm). El
// servidor escucha en /tmp/.X11-unix/X0 (y el socket abstracto) y solo arranca
// Xwayland cuando llega el primer cliente X. Aurora hace de WM: Xwayland le
// pasa un socket (-wm) por el que este proceso habla X11 con xcb; cada ventana
// X11 gestionada es una ventana mas del compositor (barra de titulo, Dock,
// foco...) y las override-redirect (menus, tooltips) son popups sin marco en
// sus coordenadas de pantalla. La superficie Wayland de cada ventana X se
// asocia con el mensaje WL_SURFACE_ID.
// ---------------------------------------------------------------------------
struct xwin {
  struct wl_list link;
  xcb_window_t id;
  int x, y, w, h;
  int or_;       /* override-redirect */
  int mapped;
  int delete_proto;
  uint32_t surf_id;
  struct xsurf *xs;
};
static struct wl_list g_xwins;
static xcb_connection_t *g_xc;
static xcb_window_t g_xroot;
static pid_t g_xpid;
static struct wl_event_source *g_xwm_src;
static const char *g_xdisplay_num = "0";
static xcb_atom_t A_SURF_ID, A_WM_PROTOCOLS, A_DELETE, A_NET_WM_NAME, A_UTF8,
    A_WM_CLASS, A_ALLOW_COMMITS;

static xcb_atom_t xatom(const char *name) {
  xcb_intern_atom_cookie_t c = xcb_intern_atom(g_xc, 0, (uint16_t)strlen(name), name);
  xcb_intern_atom_reply_t *r = xcb_intern_atom_reply(g_xc, c, NULL);
  xcb_atom_t a = r ? r->atom : XCB_ATOM_NONE;
  free(r);
  return a;
}

static struct xwin *xwin_find(xcb_window_t id) {
  struct xwin *w;
  wl_list_for_each(w, &g_xwins, link)
    if (w->id == id)
      return w;
  return NULL;
}

static struct wl_client *xwayland_client(void) {
  if (g_xpid <= 0)
    return NULL;
  struct wl_list *list = wl_display_get_client_list(g_display);
  struct wl_list *l;
  for (l = list->next; l != list; l = l->next) {
    struct wl_client *c = wl_client_from_link(l);
    pid_t pid = 0;
    wl_client_get_credentials(c, &pid, NULL, NULL);
    if (pid == g_xpid)
      return c;
  }
  return NULL;
}

static char *xprop_str(xcb_window_t win, xcb_atom_t prop, xcb_atom_t type, int *len) {
  xcb_get_property_cookie_t c = xcb_get_property(g_xc, 0, win, prop, type, 0, 256);
  xcb_get_property_reply_t *r = xcb_get_property_reply(g_xc, c, NULL);
  char *out = NULL;
  if (r && xcb_get_property_value_length(r) > 0) {
    int n = xcb_get_property_value_length(r);
    out = malloc((size_t)n + 1);
    if (out) {
      memcpy(out, xcb_get_property_value(r), (size_t)n);
      out[n] = 0;
      if (len)
        *len = n;
    }
  }
  free(r);
  return out;
}

static void xwin_read_props(struct xwin *xw) {
  if (!xw->xs || !xw->xs->tl)
    return;
  struct toplevel *tl = xw->xs->tl;
  char *t = xprop_str(xw->id, A_NET_WM_NAME, A_UTF8, NULL);
  if (!t)
    t = xprop_str(xw->id, XCB_ATOM_WM_NAME, XCB_ATOM_STRING, NULL);
  if (t) {
    snprintf(tl->title, sizeof(tl->title), "%s", t);
    free(t);
    if (xw->xs->win_id >= 0)
      win_set_title(xw->xs->win_id, tl->title);
  }
  int n = 0;
  char *cls = xprop_str(xw->id, A_WM_CLASS, XCB_ATOM_STRING, &n);
  if (cls) {
    const char *klass = cls + strlen(cls) + 1; /* "instancia\0clase\0" */
    if (klass < cls + n && *klass)
      snprintf(tl->app_id, sizeof(tl->app_id), "%s", klass);
    else
      snprintf(tl->app_id, sizeof(tl->app_id), "%s", cls);
    free(cls);
    if (xw->xs->win_id >= 0 && tl->app_id[0])
      win_set_app_id(xw->xs->win_id, tl->app_id);
  }
  xcb_get_property_cookie_t pc =
      xcb_get_property(g_xc, 0, xw->id, A_WM_PROTOCOLS, XCB_ATOM_ATOM, 0, 32);
  xcb_get_property_reply_t *pr = xcb_get_property_reply(g_xc, pc, NULL);
  xw->delete_proto = 0;
  if (pr) {
    xcb_atom_t *at = xcb_get_property_value(pr);
    int na = xcb_get_property_value_length(pr) / (int)sizeof(xcb_atom_t);
    for (int i = 0; i < na; i++)
      if (at[i] == A_DELETE)
        xw->delete_proto = 1;
    free(pr);
  }
}

// Une la ventana X con su superficie Wayland (WL_SURFACE_ID).
static void xwin_bind(struct xwin *xw) {
  if (!xw->surf_id)
    return;
  struct wl_client *xc = xwayland_client();
  if (!xc)
    return;
  struct wl_resource *r = wl_client_get_object(xc, xw->surf_id);
  if (!r || !wl_resource_instance_of(r, &wl_surface_interface, &surface_impl))
    return;
  struct surface *s = wl_resource_get_user_data(r);
  if (s->xs && s->xs == xw->xs)
    return;
  if (s->xs)
    return; /* la superficie ya tiene otro rol */
  struct xsurf *xs = xw->xs;
  if (!xs) {
    xs = calloc(1, sizeof(*xs));
    struct toplevel *tl = xw->or_ ? NULL : calloc(1, sizeof(*tl));
    if (!xs || (!xw->or_ && !tl)) {
      free(xs);
      free(tl);
      return;
    }
    xs->win_id = -1;
    xs->configured_sent = 1;
    if (xw->or_) {
      xs->is_popup = 1;
      xs->x_or = 1;
    } else {
      tl->is_x = 1;
      tl->ssd = 1;
      tl->xwin = xw->id;
      tl->client = xc;
      tl->xs = xs;
      xs->tl = tl;
      snprintf(tl->title, sizeof(tl->title), "X11");
      wl_list_insert(&g_toplevels, &tl->link);
    }
    xw->xs = xs;
  }
  xs->surface = s;
  s->xs = xs;
  if (xw->or_) {
    xs->wx = xw->x;
    xs->wy = xw->y;
  }
  fprintf(stderr, "[aurora-wayland] X11 0x%x -> superficie %u (%s)\n", xw->id,
          xw->surf_id, xw->or_ ? "override-redirect" : "ventana");
  xwin_read_props(xw);
  if (s->has_content)
    root_update(s, (rc_t){0, 0, s->dw, s->dh});
}

static void xwin_unmap(struct xwin *xw) {
  xw->mapped = 0;
  xw->surf_id = 0;
  if (!xw->xs)
    return;
  xs_unmap(xw->xs);
  if (xw->xs->surface) {
    xw->xs->surface->xs = NULL;
    xw->xs->surface = NULL;
  }
}

static void xwin_free(struct xwin *xw) {
  xwin_unmap(xw);
  if (xw->xs) {
    if (xw->xs->tl) {
      wl_list_remove(&xw->xs->tl->link);
      if (g_kbd_focus == xw->xs->tl)
        g_kbd_focus = NULL;
      free(xw->xs->tl);
    }
    free(xw->xs);
  }
  wl_list_remove(&xw->link);
  free(xw);
}

// Pone la ventana X en su posicion de pantalla real (origen del contenido).
static void xwm_sync_pos(struct xsurf *xs) {
  if (!g_xc || !xs->tl)
    return;
  uint32_t v[2] = {(uint32_t)xs->wx, (uint32_t)xs->wy};
  xcb_configure_window(g_xc, xs->tl->xwin,
                       XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, v);
  struct xwin *xw = xwin_find(xs->tl->xwin);
  if (xw) {
    xw->x = xs->wx;
    xw->y = xs->wy;
  }
  xcb_flush(g_xc);
}

static void xwm_toplevel_configure(struct toplevel *tl, int w, int h) {
  if (!g_xc)
    return;
  struct xwin *xw = xwin_find(tl->xwin);
  if (!xw)
    return;
  if (w > 0 && h > 0 && (w != xw->w || h != xw->h)) {
    uint32_t v[2] = {(uint32_t)w, (uint32_t)h};
    xcb_configure_window(g_xc, xw->id,
                         XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, v);
    xw->w = w;
    xw->h = h;
  }
  if (tl->activated)
    xcb_set_input_focus(g_xc, XCB_INPUT_FOCUS_POINTER_ROOT, xw->id, XCB_CURRENT_TIME);
  xcb_flush(g_xc);
}

static void xwm_close(struct toplevel *tl) {
  if (!g_xc)
    return;
  struct xwin *xw = xwin_find(tl->xwin);
  if (!xw)
    return;
  if (xw->delete_proto) {
    xcb_client_message_event_t ev = {0};
    ev.response_type = XCB_CLIENT_MESSAGE;
    ev.format = 32;
    ev.window = xw->id;
    ev.type = A_WM_PROTOCOLS;
    ev.data.data32[0] = A_DELETE;
    ev.data.data32[1] = XCB_CURRENT_TIME;
    xcb_send_event(g_xc, 0, xw->id, XCB_EVENT_MASK_NO_EVENT, (const char *)&ev);
  } else {
    xcb_kill_client(g_xc, xw->id);
  }
  xcb_flush(g_xc);
}

static void xwm_stop(void) {
  if (g_xwm_src) {
    wl_event_source_remove(g_xwm_src);
    g_xwm_src = NULL;
  }
  struct xwin *w, *t;
  wl_list_for_each_safe(w, t, &g_xwins, link)
    xwin_free(w);
  if (g_xc) {
    xcb_disconnect(g_xc);
    g_xc = NULL;
  }
  if (g_xpid > 0) {
    kill(g_xpid, SIGTERM);
    waitpid(g_xpid, NULL, WNOHANG);
  }
  g_xpid = 0;
  fprintf(stderr, "[aurora-wayland] Xwayland termino; X11 desactivado\n");
}

static int xwm_event(int fd, uint32_t mask, void *data) {
  (void)fd; (void)data;
  if (!g_xc)
    return 0;
  if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) {
    xwm_stop();
    return 0;
  }
  xcb_generic_event_t *ev;
  while ((ev = xcb_poll_for_event(g_xc))) {
    if (g_debug)
      fprintf(stderr, "[aurora-wayland] xwm evento %d\n", ev->response_type & 0x7f);
    switch (ev->response_type & 0x7f) {
    case XCB_CREATE_NOTIFY: {
      xcb_create_notify_event_t *e = (void *)ev;
      if (e->parent != g_xroot || xwin_find(e->window))
        break;
      struct xwin *xw = calloc(1, sizeof(*xw));
      if (!xw)
        break;
      xw->id = e->window;
      xw->x = e->x; xw->y = e->y; xw->w = e->width; xw->h = e->height;
      xw->or_ = e->override_redirect;
      wl_list_insert(&g_xwins, &xw->link);
      uint32_t m = XCB_EVENT_MASK_PROPERTY_CHANGE;
      xcb_change_window_attributes(g_xc, xw->id, XCB_CW_EVENT_MASK, &m);
      break;
    }
    case XCB_CONFIGURE_REQUEST: {
      xcb_configure_request_event_t *e = (void *)ev;
      struct xwin *xw = xwin_find(e->window);
      uint32_t v[7];
      int n = 0;
      uint16_t mask = e->value_mask;
      int managed = xw && xw->xs && xw->xs->tl && xw->xs->win_id >= 0;
      if (managed)
        mask &= (uint16_t)~(XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y);
      if (mask & XCB_CONFIG_WINDOW_X) v[n++] = (uint32_t)(int32_t)e->x;
      if (mask & XCB_CONFIG_WINDOW_Y) v[n++] = (uint32_t)(int32_t)e->y;
      if (mask & XCB_CONFIG_WINDOW_WIDTH) v[n++] = e->width;
      if (mask & XCB_CONFIG_WINDOW_HEIGHT) v[n++] = e->height;
      if (mask & XCB_CONFIG_WINDOW_BORDER_WIDTH) v[n++] = e->border_width;
      if (mask & XCB_CONFIG_WINDOW_SIBLING) v[n++] = e->sibling;
      if (mask & XCB_CONFIG_WINDOW_STACK_MODE) v[n++] = e->stack_mode;
      xcb_configure_window(g_xc, e->window, mask, v);
      if (xw) {
        if (mask & XCB_CONFIG_WINDOW_X) xw->x = e->x;
        if (mask & XCB_CONFIG_WINDOW_Y) xw->y = e->y;
        if ((mask & XCB_CONFIG_WINDOW_WIDTH) && (mask & XCB_CONFIG_WINDOW_HEIGHT)) {
          xw->w = e->width;
          xw->h = e->height;
          if (managed && xw->xs->win_id >= 0)
            win_request_resize(xw->xs->win_id, e->width, e->height + TITLEBAR_H);
        }
        if (xw->or_ && xw->xs) {
          xw->xs->wx = xw->x;
          xw->xs->wy = xw->y;
        }
      }
      break;
    }
    case XCB_MAP_REQUEST: {
      xcb_map_request_event_t *e = (void *)ev;
      xcb_map_window(g_xc, e->window);
      /* Xwayland retiene los commits de la ventana hasta que el WM lo permite. */
      uint32_t one = 1;
      xcb_change_property(g_xc, XCB_PROP_MODE_REPLACE, e->window, A_ALLOW_COMMITS,
                          XCB_ATOM_CARDINAL, 32, 1, &one);
      break;
    }
    case XCB_MAP_NOTIFY: {
      xcb_map_notify_event_t *e = (void *)ev;
      struct xwin *xw = xwin_find(e->window);
      if (xw) {
        xw->mapped = 1;
        if (xw->or_) {
          uint32_t one = 1;
          xcb_change_property(g_xc, XCB_PROP_MODE_REPLACE, xw->id, A_ALLOW_COMMITS,
                              XCB_ATOM_CARDINAL, 32, 1, &one);
        }
        xwin_bind(xw);
      }
      break;
    }
    case XCB_UNMAP_NOTIFY: {
      xcb_unmap_notify_event_t *e = (void *)ev;
      struct xwin *xw = xwin_find(e->window);
      if (xw)
        xwin_unmap(xw);
      break;
    }
    case XCB_DESTROY_NOTIFY: {
      xcb_destroy_notify_event_t *e = (void *)ev;
      struct xwin *xw = xwin_find(e->window);
      if (xw)
        xwin_free(xw);
      break;
    }
    case XCB_CLIENT_MESSAGE: {
      xcb_client_message_event_t *e = (void *)ev;
      if (e->type == A_SURF_ID) {
        struct xwin *xw = xwin_find(e->window);
        if (xw) {
          xw->surf_id = e->data.data32[0];
          xwin_bind(xw);
        }
      }
      break;
    }
    case XCB_PROPERTY_NOTIFY: {
      xcb_property_notify_event_t *e = (void *)ev;
      struct xwin *xw = xwin_find(e->window);
      if (xw && xw->xs)
        xwin_read_props(xw);
      break;
    }
    default:
      break;
    }
    free(ev);
  }
  if (xcb_connection_has_error(g_xc))
    xwm_stop();
  else
    xcb_flush(g_xc);
  return 0;
}

static int g_xwm_fd = -1; /* extremo nuestro del socket -wm */
static int g_xwaiting;    /* Xwayland lanzado, a la espera de su SIGUSR1 */

// Xwayland avisa con SIGUSR1 cuando termina de arrancar (lo hace si el padre lo
// tenia ignorado al hacer exec): ya se puede hablar X11 por el socket -wm sin
// interbloqueo (durante el arranque Xwayland hace roundtrips contra este mismo
// proceso y xcb bloquearia el bucle de eventos).
static void xwm_ready(void) {
  g_xwaiting = 0;
  g_xc = xcb_connect_to_fd(g_xwm_fd, NULL);
  if (xcb_connection_has_error(g_xc)) {
    fprintf(stderr, "[aurora-wayland] xcb_connect_to_fd fallo\n");
    xcb_disconnect(g_xc);
    g_xc = NULL;
    g_xwm_fd = -1;
    if (g_xpid > 0)
      kill(g_xpid, SIGTERM);
    g_xpid = 0;
    return;
  }
  const xcb_setup_t *setup = xcb_get_setup(g_xc);
  g_xroot = xcb_setup_roots_iterator(setup).data->root;
  uint32_t m = XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY |
               XCB_EVENT_MASK_PROPERTY_CHANGE;
  xcb_void_cookie_t ck = xcb_change_window_attributes_checked(g_xc, g_xroot,
                                                              XCB_CW_EVENT_MASK, &m);
  xcb_generic_error_t *err = xcb_request_check(g_xc, ck);
  if (err) {
    fprintf(stderr, "[aurora-wayland] xwm: no se pudo tomar el control de la raiz X11\n");
    free(err);
  }
  /* Xwayland solo da superficie a las ventanas con redireccion Composite manual
   * (lo hace el WM sobre los hijos de la raiz, como el xwm de Weston). */
  {
    xcb_composite_query_version_cookie_t vc =
        xcb_composite_query_version(g_xc, 0, 4);
    free(xcb_composite_query_version_reply(g_xc, vc, NULL));
    xcb_composite_redirect_subwindows(g_xc, g_xroot, XCB_COMPOSITE_REDIRECT_MANUAL);
  }
  A_SURF_ID = xatom("WL_SURFACE_ID");
  A_WM_PROTOCOLS = xatom("WM_PROTOCOLS");
  A_DELETE = xatom("WM_DELETE_WINDOW");
  A_NET_WM_NAME = xatom("_NET_WM_NAME");
  A_UTF8 = xatom("UTF8_STRING");
  A_WM_CLASS = xatom("WM_CLASS");
  A_ALLOW_COMMITS = xatom("_XWAYLAND_ALLOW_COMMITS");
  xcb_flush(g_xc);
  g_xwm_src = wl_event_loop_add_fd(g_loop, g_xwm_fd, WL_EVENT_READABLE, xwm_event, NULL);
  fprintf(stderr, "[aurora-wayland] Xwayland pid %d, DISPLAY=:%s, WM listo\n", (int)g_xpid,
          g_xdisplay_num);
}

static void xwm_spawn(void) {
  int sv[2];
  if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) < 0) {
    perror("[aurora-wayland] xwm socketpair");
    return;
  }
  mkdir("/tmp/xkb", 0777); /* salida de xkbcomp (/var/lib/xkb -> /tmp/xkb) */
  char wmfd[16], disp[16];
  snprintf(disp, sizeof(disp), ":%s", g_xdisplay_num);
  pid_t pid = fork();
  if (pid < 0) {
    close(sv[0]);
    close(sv[1]);
    return;
  }
  if (pid == 0) {
    int keep[1] = {sv[1]};
    for (int i = 0; i < 1; i++) {
      int fl = fcntl(keep[i], F_GETFD);
      fcntl(keep[i], F_SETFD, fl & ~FD_CLOEXEC);
    }
    snprintf(wmfd, sizeof(wmfd), "%d", sv[1]);
    sigset_t none;
    sigemptyset(&none);
    sigprocmask(SIG_SETMASK, &none, NULL); /* wl_event_loop bloqueo SIGTERM... */
    signal(SIGUSR1, SIG_IGN); /* Xwayland avisara con SIGUSR1 al estar listo */
    setenv("XDG_RUNTIME_DIR", "/run/user/0", 0);
    setenv("WAYLAND_DISPLAY", g_sockname, 1);
    unsetenv("DISPLAY");
    if (getenv("AW_XSTRACE")) /* depuracion: traza de syscalls de Xwayland */
      execl("/usr/bin/strace", "strace", "-D", "-f", "-o", getenv("AW_XSTRACE"),
            "/usr/bin/Xwayland", disp, "-rootless", "-shm", "-noreset",
            "-wm", wmfd, (char *)NULL);
    execl("/usr/bin/Xwayland", "Xwayland", disp, "-rootless", "-shm", "-noreset",
          "-wm", wmfd, (char *)NULL);
    _exit(127);
  }
  close(sv[1]);
  g_xpid = pid;
  g_xwaiting = 1;
  g_xwm_fd = sv[0];
}

// Arranca Xwayland (sin -listenfd: con -wm no escucha en los sockets heredados)
// y, cuando avisa con SIGUSR1, se conecta como gestor de ventanas.
static void xwm_init(void) {
  const char *num = getenv("AW_XDISPLAY");
  if (num && *num)
    g_xdisplay_num = num;
  wl_list_init(&g_xwins);
  if (access("/usr/bin/Xwayland", X_OK) != 0)
    return;
  mkdir("/tmp/.X11-unix", 01777);
  chmod("/tmp/.X11-unix", 01777);
  xwm_spawn();
}

static void handle_win_event(struct xsurf *xs, const winsrv_event_t *ev) {
  struct toplevel *tl = xs->tl;
  switch (ev->type) {
  case WINSRV_EV_CLOSE:
    if (tl && tl->is_x)
      xwm_close(tl);
    else if (tl)
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
      if (tl->is_x)
        xwm_sync_pos(xs);
    }
    break;
  case WINSRV_EV_STATE:
    // Aurora maximizo/restauro la ventana (semaforo, doble clic, arrastre).
    if (tl && tl->maximized != (ev->x != 0)) {
      tl->maximized = ev->x != 0;
      if (!tl->maximized)
        tl->fullscreen = 0;
      toplevel_send_configure(tl, xs->cw, xs->ch);
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
      struct wl_client *c = g_kbd_focus->client;
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
      if (!g_ptr_focus || !g_ptr_surf)
        continue;
      struct axis_ev e = {
          .time = now_ms(),
          .axis = ev[i].code == REL_WHEEL ? WL_POINTER_AXIS_VERTICAL_SCROLL
                                          : WL_POINTER_AXIS_HORIZONTAL_SCROLL,
          // evdev: + = arriba/izquierda; Wayland: + = abajo/derecha
          .notches = ev[i].code == REL_WHEEL ? -ev[i].value : ev[i].value};
      struct wl_client *pc = wl_resource_get_client(g_ptr_surf->res);
      ptr_send_to(pc, do_axis, &e);
      ptr_send_to(pc, do_frame, &e);
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
//   Nombre | app_id | simbolo | color RRGGBB | icono BMP | orden (ruta args...)
// aurora-wayland los fija en el Dock; cuando el usuario pulsa uno sin ventanas
// abiertas, el compositor encola una peticion y aqui se lanza el proceso (con
// WAYLAND_DISPLAY/XDG_RUNTIME_DIR ya preparados) y se recoge al terminar.
// ---------------------------------------------------------------------------
#define DOCK_CONF_PATH "/usr/share/aurora/dock.conf"
#define MAX_LAUNCHERS 16
static struct {
  char name[32], app_id[48], symbol[8], icon[64], cmd[192];
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
    char *fld[6];
    int n = 0;
    for (char *p = line; n < 6;) {
      fld[n++] = p;
      char *bar = n < 6 ? strchr(p, '|') : NULL;
      if (!bar)
        break;
      *bar = 0;
      p = bar + 1;
    }
    if (n < 6)
      continue;
    int i = g_nlaunchers;
    snprintf(g_launchers[i].name, sizeof(g_launchers[i].name), "%s", trim(fld[0]));
    snprintf(g_launchers[i].app_id, sizeof(g_launchers[i].app_id), "%s", trim(fld[1]));
    snprintf(g_launchers[i].symbol, sizeof(g_launchers[i].symbol), "%s", trim(fld[2]));
    g_launchers[i].color = (uint32_t)strtoul(trim(fld[3]), NULL, 16) | 0xFF000000u;
    snprintf(g_launchers[i].icon, sizeof(g_launchers[i].icon), "%s", trim(fld[4]));
    snprintf(g_launchers[i].cmd, sizeof(g_launchers[i].cmd), "%s", trim(fld[5]));
    if (g_launchers[i].app_id[0] && g_launchers[i].cmd[0])
      g_nlaunchers++;
  }
  fclose(f);
}

static int has_launcher(const char *app_id) {
  for (int i = 0; i < g_nlaunchers; i++)
    if (app_id[0] && !strcmp(g_launchers[i].app_id, app_id))
      return 1;
  return 0;
}

// Aplicaciones instaladas: /usr/share/applications/*.desktop (freedesktop).
// Se fijan en el Dock tras las de dock.conf (hasta MAX_LAUNCHERS) con el icono
// generico de Aurora. app_id = StartupWMClass o, si falta, el nombre del
// ejecutable (lo que declaran GTK/Qt con xdg_toplevel.set_app_id).
#define APPS_DIR "/usr/share/applications"
static void desktop_exec_clean(char *dst, size_t n, const char *exec) {
  size_t o = 0;
  for (const char *p = exec; *p && o + 1 < n; p++) {
    if (*p == '%' && p[1]) { /* codigos de campo %f %F %u %U %i %c %k */
      p++;
      continue;
    }
    if (*p == '"')
      continue;
    dst[o++] = *p;
  }
  while (o && dst[o - 1] == ' ')
    o--;
  dst[o] = 0;
}

static void desktop_launchers_load(void) {
  DIR *d = opendir(APPS_DIR);
  if (!d)
    return;
  struct dirent *e;
  while (g_nlaunchers < MAX_LAUNCHERS && (e = readdir(d)) != NULL) {
    size_t nl = strlen(e->d_name);
    if (nl < 9 || strcmp(e->d_name + nl - 8, ".desktop") != 0)
      continue;
    char path[320];
    snprintf(path, sizeof(path), APPS_DIR "/%s", e->d_name);
    FILE *f = fopen(path, "r");
    if (!f)
      continue;
    char name[32] = "", exec[192] = "", wmclass[48] = "", line[512];
    int in_entry = 0, hidden = 0, is_app = 0;
    while (fgets(line, sizeof(line), f)) {
      char *t = trim(line);
      if (t[0] == '[') {
        in_entry = !strcmp(t, "[Desktop Entry]");
        continue;
      }
      if (!in_entry)
        continue;
      if (!strncmp(t, "Name=", 5) && !name[0])
        snprintf(name, sizeof(name), "%s", t + 5);
      else if (!strncmp(t, "Exec=", 5))
        desktop_exec_clean(exec, sizeof(exec), t + 5);
      else if (!strncmp(t, "StartupWMClass=", 15))
        snprintf(wmclass, sizeof(wmclass), "%s", t + 15);
      else if (!strcmp(t, "NoDisplay=true") || !strcmp(t, "Hidden=true") ||
               !strcmp(t, "Terminal=true"))
        hidden = 1;
      else if (!strcmp(t, "Type=Application"))
        is_app = 1;
    }
    fclose(f);
    if (!is_app || hidden || !name[0] || !exec[0])
      continue;
    // app_id: StartupWMClass o basename del ejecutable.
    char app_id[48];
    if (wmclass[0]) {
      snprintf(app_id, sizeof(app_id), "%s", wmclass);
    } else {
      char tmp[192];
      snprintf(tmp, sizeof(tmp), "%s", exec);
      char *sp = strchr(tmp, ' ');
      if (sp)
        *sp = 0;
      const char *base = strrchr(tmp, '/');
      snprintf(app_id, sizeof(app_id), "%s", base ? base + 1 : tmp);
    }
    int dup = 0;
    for (int i = 0; i < g_nlaunchers; i++)
      if (!strcmp(g_launchers[i].app_id, app_id))
        dup = 1;
    if (dup)
      continue;
    int i = g_nlaunchers;
    snprintf(g_launchers[i].name, sizeof(g_launchers[i].name), "%s", name);
    snprintf(g_launchers[i].app_id, sizeof(g_launchers[i].app_id), "%s", app_id);
    // Simbolo = iniciales de las dos primeras palabras (o las dos primeras
    // letras): las baldosas sin icono se distinguen por letras y color.
    const char *w2 = strchr(name, ' ');
    g_launchers[i].symbol[0] = name[0];
    g_launchers[i].symbol[1] = (w2 && w2[1]) ? w2[1] : (name[1] ? name[1] : 0);
    g_launchers[i].symbol[2] = 0;
    uint32_t h = 2166136261u; /* color estable por nombre (FNV-1a) */
    for (const char *c = app_id; *c; c++)
      h = (h ^ (uint8_t)*c) * 16777619u;
    g_launchers[i].color = 0xFF000000u | (0x30 + (h & 0x5F)) << 16 |
                           (0x30 + ((h >> 8) & 0x5F)) << 8 | (0x40 + ((h >> 16) & 0x7F));
    g_launchers[i].icon[0] = 0; /* sin icono: baldosa con simbolo y color */
    snprintf(g_launchers[i].cmd, sizeof(g_launchers[i].cmd), "%s", exec);
    g_nlaunchers++;
  }
  closedir(d);
}

static void launchers_pin(void) {
  for (int i = 0; i < g_nlaunchers; i++) {
    struct {
      uint32_t launch_id, color;
      char name[32], symbol[8], app_id[48], icon[64];
    } r;
    memset(&r, 0, sizeof(r));
    r.launch_id = (uint32_t)i + 1;
    r.color = g_launchers[i].color;
    snprintf(r.name, sizeof(r.name), "%s", g_launchers[i].name);
    snprintf(r.symbol, sizeof(r.symbol), "%s", g_launchers[i].symbol);
    snprintf(r.app_id, sizeof(r.app_id), "%s", g_launchers[i].app_id);
    snprintf(r.icon, sizeof(r.icon), "%s", g_launchers[i].icon);
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
  setenv("DISPLAY", ":0", 0);
  execvp(argv[0], argv); /* ruta absoluta o nombre en PATH (.desktop) */
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

static int mock_dump(int sig, void *data) {
  (void)sig; (void)data;
  for (int i = 0; i < MOCK_MAX_WIN; i++) {
    if (!g_mw[i].used)
      continue;
    char path[64];
    snprintf(path, sizeof(path), "/tmp/aw-mock-%d.ppm", i);
    FILE *f = fopen(path, "wb");
    if (!f)
      continue;
    fprintf(f, "P6\n%d %d\n255\n", g_mw[i].w, g_mw[i].h);
    for (size_t k = 0; k < (size_t)g_mw[i].w * g_mw[i].h; k++) {
      uint32_t p = g_mw[i].px[k];
      fputc((p >> 16) & 255, f);
      fputc((p >> 8) & 255, f);
      fputc(p & 255, f);
    }
    fclose(f);
    fprintf(stderr, "[aurora-wayland] mock: ventana %d '%s' %dx%d -> %s\n", i,
            g_mw[i].title, g_mw[i].w, g_mw[i].h, path);
  }
  return 0;
}

static int on_usr1(int sig, void *data) {
  if (g_xwaiting)
    xwm_ready();
  else if (g_mock)
    mock_dump(sig, data);
  return 0;
}

static int on_term(int sig, void *data) {
  (void)sig; (void)data;
  wl_display_terminate(g_display);
  return 0;
}

int main(int argc, char **argv) {
  const char *sock = argc > 1 ? argv[1] : "wayland-0";
  g_sockname = sock;
  signal(SIGPIPE, SIG_IGN);
  g_debug = getenv("AW_DEBUG") != NULL;
  g_mock = getenv("AW_MOCK") != NULL;

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
  wl_list_init(&g_psdevs);

  if (keymap_init() < 0)
    fprintf(stderr, "[aurora-wayland] aviso: sin teclado xkb\n");

  wl_display_init_shm(g_display);
  wl_global_create(g_display, &wl_compositor_interface, 4, NULL, comp_bind);
  wl_global_create(g_display, &wl_subcompositor_interface, 1, NULL, subc_bind);
  wl_global_create(g_display, &wl_output_interface, 3, NULL, output_bind);
  wl_global_create(g_display, &wl_seat_interface, 5, NULL, seat_bind);
  wl_global_create(g_display, &wl_data_device_manager_interface, 3, NULL, dm_bind);
  wl_global_create(g_display, &xdg_wm_base_interface, 3, NULL, wm_bind);
  wl_global_create(g_display, &zwp_primary_selection_device_manager_v1_interface, 1,
                   NULL, psm_bind);
  wl_global_create(g_display, &zwp_linux_dmabuf_v1_interface, 3, NULL, dmabuf_bind);
  wl_global_create(g_display, &wp_viewporter_interface, 1, NULL, viewporter_bind);
  wl_global_create(g_display, &wp_fractional_scale_manager_v1_interface, 1, NULL,
                   fsm_bind);
  wl_global_create(g_display, &wp_cursor_shape_manager_v1_interface, 1, NULL,
                   csm_bind);
  wl_global_create(g_display, &zxdg_output_manager_v1_interface, 2, NULL, xom_bind);
  wl_global_create(g_display, &wp_presentation_interface, 1, NULL, pres_bind);
  wl_global_create(g_display, &zwp_idle_inhibit_manager_v1_interface, 1, NULL,
                   iim_bind);
  wl_global_create(g_display, &xdg_activation_v1_interface, 1, NULL, act_bind);
  wl_global_create(g_display, &zxdg_decoration_manager_v1_interface, 1, NULL,
                   deco_mgr_bind);
  wl_global_create(g_display, &org_kde_kwin_server_decoration_manager_interface, 1,
                   NULL, kde_mgr_bind);

  wl_list_init(&g_frame_q);
  g_frame_timer = wl_event_loop_add_timer(g_loop, frame_tick, NULL);
  g_pump_timer = wl_event_loop_add_timer(g_loop, pump_events, NULL);
  wl_event_loop_add_signal(g_loop, SIGCHLD, on_chld, NULL);
  wl_event_loop_add_signal(g_loop, SIGTERM, on_term, NULL);
  wl_event_loop_add_signal(g_loop, SIGINT, on_term, NULL);
  wl_event_loop_add_signal(g_loop, SIGUSR1, on_usr1, NULL); /* Xwayland listo / mock */

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
  xwm_init();

  if (!g_mock) {
    launchers_load();
    desktop_launchers_load();
    launchers_pin();
  }
  g_launch_timer = wl_event_loop_add_timer(g_loop, launch_poll, NULL);
  wl_event_source_timer_update(g_launch_timer, 100);
  fprintf(stderr, "[aurora-wayland] READY socket=%s\n", sock);
  wl_display_run(g_display);

  fprintf(stderr, "[aurora-wayland] terminado\n");
  wl_display_destroy_clients(g_display);
  wl_display_destroy(g_display);
  return 0;
}
