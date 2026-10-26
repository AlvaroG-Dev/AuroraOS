// kernel/gfx/compositor.c
//
// Modelo de concurrencia
// ----------------------
// Solo el hilo del compositor toca window_stack, los items de la taskbar y el
// estado del Dock/barra de menús. Cualquier otro contexto (syscalls de winsrv
// en otras CPUs) publica comandos en una cola FIFO con lock (cmd_post) y el
// compositor los ejecuta al inicio de cada iteración. Lo único compartido con
// otras CPUs son: la cola de comandos, la lista de damage (dmg_lock) y los
// flags de despertar.
//
// Frame
// -----
// Para cada rectángulo dañado: se compone el fondo y las ventanas en el
// backbuffer (empezando por la ventana opaca más alta que lo tapa entero, si
// la hay), se pintan Dock y barra de menús encima y se vuelca a VRAM con el
// cursor mezclado. El volcado se hace con la preempción desactivada para que
// una ventana en movimiento nunca se quede a medio copiar en pantalla.
#include "compositor.h"
#include "../bmp.h"
#include "../boot_console.h"
#include "../heap.h"
#include "../input.h"
#include "../klog.h"
#include "../sched.h"
#include "../serial.h"
#include "../simd.h"
#include "../spinlock.h"
#include "../tarfs.h"
#include "../time.h"
#include "../wait.h"
#include "about.h"
#include "cursor.h"
#include "dock.h"
#include "font_manager.h"
#include "gfx.h"
#include "gfx_filter.h"
#include "menubar.h"
#include "../gpu_proxy.h"
#include "taskbar.h"
#include "theme.h"
#include "window.h"
#include "winsrv.h"
#include <emmintrin.h>
#include <stddef.h>
#include <stdint.h>

extern uint32_t *fb_ptr;
extern uint32_t fb_width;
extern uint32_t fb_height;
extern uint32_t fb_pitch;
extern uint32_t fb_bpp;
extern void fb_flush_rect(const uint32_t *src, uint32_t src_pitch, uint32_t x,
                          uint32_t y, uint32_t w, uint32_t h);

#define SH AURORA_SHADOW_SIZE

static uint32_t *backbuffer = NULL;
static uint32_t *wallpaper_cache = NULL;
static window_t *window_stack = NULL; /* solo hilo del compositor */
static task_t *compositor_task_ref = NULL;
static tar_node_t *bg_wallpaper_node = NULL;

static int cursor_x = 400, cursor_y = 300;
static cursor_shape_t cursor_shape = CURSOR_ARROW;
static rect_t cursor_drawn = {0, 0, 0, 0}; /* donde está pintado el cursor */

static wait_queue_t compositor_wq = WAIT_QUEUE_INIT(compositor_wq);
static volatile int compositor_has_input = 0;
static volatile int compositor_needs_clock = 0;
static volatile int compositor_dock_dirty = 0;
static volatile int compositor_animating = 0; /* lo lee compositor_frame_tick */
static volatile int compositor_frame_due = 0;

static window_t *drag_window = NULL;
static int drag_offset_x = 0;
static int drag_offset_y = 0;

static window_t *pressed_win = NULL;
static window_btn_state_t pressed_btn = WIN_BTN_NONE;
static int last_mouse_buttons = 0;

/* Icono del Dock pulsado: como en macOS, la acción ocurre al soltar sobre él. */
static taskbar_item_t *dock_pressed_item = NULL;

/* Ventana "Acerca de AuroraOS" (una sola instancia). */
static window_t *about_win = NULL;

/* Doble clic en la barra de título = Zoom (como macOS). */
static window_t *title_click_win = NULL;
static uint64_t title_click_tsc = 0;

// Estado del resize en curso (se aplica una sola vez al soltar; mientras
// tanto se dibuja un marco fantasma).
static window_t *resize_window = NULL;
static int resize_edge = 0;
static int resize_origin_x = 0;
static int resize_origin_y = 0;
static rect_t resize_from = {0, 0, 0, 0}; /* geometría al empezar */
static rect_t resize_to = {0, 0, 0, 0};   /* geometría propuesta */

#define RESIZE_EDGE_NONE 0
#define RESIZE_EDGE_RIGHT (1 << 0)
#define RESIZE_EDGE_BOTTOM (1 << 1)
#define RESIZE_EDGE_LEFT (1 << 2)
#define RESIZE_EDGE_TOP (1 << 3)
#define RESIZE_MIN_W 200
#define RESIZE_MIN_H 150
#define RESIZE_MAX 2048

static inline int imin(int a, int b) { return a < b ? a : b; }
static inline int imax(int a, int b) { return a > b ? a : b; }

static inline void sse_memset32(uint32_t *dest, uint32_t val, size_t count) {
  simd_fill_row(dest, val, count);
}

static inline uint64_t comp_rdtsc(void) {
  uint32_t lo, hi;
  __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
  return ((uint64_t)hi << 32) | lo;
}
extern uint64_t klog_get_tsc_freq(void);

static inline rect_t win_damage_rect(const window_t *w) {
  return (rect_t){w->x - SH, w->y - SH, w->width + SH * 2, w->height + SH * 2};
}

static inline rect_t screen_rect(void) {
  return (rect_t){0, 0, (int)fb_width, (int)fb_height};
}

/* =========================================================================
 * Lista de damage (hasta DAMAGE_MAX rectángulos, protegida por dmg_lock).
 * Reemplaza al único bounding box global: el reloj de arriba y el hover del
 * Dock de abajo ya no obligan a repintar media pantalla.
 * =======================================================================*/
#define DAMAGE_MAX 12
static rect_t dmg[DAMAGE_MAX];
static int dmg_n = 0;
static spinlock_t dmg_lock;
static volatile int dmg_pending = 0;

static inline int rect_area(rect_t r) { return r.w * r.h; }

void compositor_invalidate_rect(rect_t d) {
  d = rect_clip(d, screen_rect());
  if (d.w <= 0 || d.h <= 0)
    return;

  unsigned long fl = spin_lock_irqsave(&dmg_lock);
  for (int i = 0; i < dmg_n;) {
    rect_t u = rect_bounding_box(dmg[i], d);
    int sum = rect_area(dmg[i]) + rect_area(d);
    if (rect_area(u) <= sum + sum / 4) { /* solapan o casi: fusionar */
      d = u;
      dmg[i] = dmg[--dmg_n];
      i = 0; /* reintentar con el rect ampliado */
    } else {
      i++;
    }
  }
  if (dmg_n == DAMAGE_MAX) { /* desborde: colapsar todo en un rect */
    for (int i = 0; i < dmg_n; i++)
      d = rect_bounding_box(d, dmg[i]);
    dmg_n = 0;
  }
  dmg[dmg_n++] = d;
  dmg_pending = 1;
  spin_unlock_irqrestore(&dmg_lock, fl);

  wake_up_all(&compositor_wq);
}

static int damage_take(rect_t *out) {
  unsigned long fl = spin_lock_irqsave(&dmg_lock);
  int n = dmg_n;
  for (int i = 0; i < n; i++)
    out[i] = dmg[i];
  dmg_n = 0;
  dmg_pending = 0;
  spin_unlock_irqrestore(&dmg_lock, fl);
  return n;
}

void compositor_invalidate_dock(void) {
  compositor_dock_dirty = 1;
  wake_up_all(&compositor_wq);
}

/* Invalida lo que la ventana ocupaba en el último frame y lo que ocupa
 * ahora (animación, movimiento o cambio de aspecto). Hilo del compositor. */
static void invalidate_window(window_t *w) {
  rect_t now = window_visual_rect(w);
  compositor_invalidate_rect(rect_bounding_box(w->last_vis, now));
  w->last_vis = now;
}

/* =========================================================================
 * Cola de comandos (cualquier CPU -> hilo del compositor)
 * =======================================================================*/
typedef enum {
  CMD_ATTACH,
  CMD_CLOSE,
  CMD_MINIMIZE,
  CMD_FOCUS,
  CMD_ICON_BMP,
  CMD_TITLE,
  CMD_MAXIMIZE, /* value: 1 = maximizar, 0 = restaurar */
  CMD_DOCK_PIN, /* title = nombre, app_id, symbol, color, value = launch_id */
  CMD_WIN_APP,  /* win + app_id: la ventana pasa al lanzador de esa app */
  CMD_WIN_GRAB, /* win + value: 0 = mover, otro = RESIZE_EDGE_* (peticion del cliente) */
  CMD_WIN_RESIZE /* win + value = (ancho << 16) | alto exteriores (peticion del cliente) */
} cmd_type_t;

typedef struct {
  uint8_t type;
  window_t *win;
  tar_node_t *node;
  char title[64]; /* CMD_TITLE */
  int value;      /* CMD_MAXIMIZE / CMD_DOCK_PIN */
  char app_id[48];
  char symbol[8];
  uint32_t color;
} comp_cmd_t;

#define CMDQ_SIZE 128
static comp_cmd_t cmdq[CMDQ_SIZE];
static int cq_head = 0, cq_tail = 0;
static volatile int cq_count = 0;
static spinlock_t cmd_lock;

static int cmd_try_post(const comp_cmd_t *c) {
  unsigned long fl = spin_lock_irqsave(&cmd_lock);
  if (cq_count >= CMDQ_SIZE) {
    spin_unlock_irqrestore(&cmd_lock, fl);
    return -1;
  }
  cmdq[cq_head] = *c;
  cq_head = (cq_head + 1) % CMDQ_SIZE;
  cq_count++;
  spin_unlock_irqrestore(&cmd_lock, fl);
  wake_up_all(&compositor_wq);
  return 0;
}

/* Bloqueante: solo para tareas normales (nunca desde el hilo del compositor
 * ni desde IRQ). */
static void cmd_post(const comp_cmd_t *c) {
  while (cmd_try_post(c) < 0)
    sched_yield();
}

static int cmd_pop(comp_cmd_t *out) {
  unsigned long fl = spin_lock_irqsave(&cmd_lock);
  if (cq_count == 0) {
    spin_unlock_irqrestore(&cmd_lock, fl);
    return 0;
  }
  *out = cmdq[cq_tail];
  cq_tail = (cq_tail + 1) % CMDQ_SIZE;
  cq_count--;
  spin_unlock_irqrestore(&cmd_lock, fl);
  return 1;
}

/* =========================================================================
 * Diagnóstico de rendimiento por fase
 * =======================================================================*/
#define COMP_FRAME_LOG_INTERVAL 120
enum { PH_BG, PH_WIN, PH_UI, PH_BLIT, PH_N };
static uint64_t g_ph_sum[PH_N];
static uint64_t g_ph_cur[PH_N];
static uint64_t g_frame_cycles_max = 0;
static uint32_t g_frame_count = 0;

static void compositor_report_frame_stats(uint64_t total_cycles) {
  for (int i = 0; i < PH_N; i++) {
    g_ph_sum[i] += g_ph_cur[i];
    g_ph_cur[i] = 0;
  }
  if (total_cycles > g_frame_cycles_max)
    g_frame_cycles_max = total_cycles;
  if (++g_frame_count < COMP_FRAME_LOG_INTERVAL)
    return;

  uint64_t freq = klog_get_tsc_freq();
  if (freq > 0) {
    uint64_t us[PH_N];
    for (int i = 0; i < PH_N; i++)
      us[i] = (g_ph_sum[i] / g_frame_count) * 1000000ULL / freq;
    uint64_t max_us = g_frame_cycles_max * 1000000ULL / freq;
    LOG_DEBUG("[COMP-PERF] avg us: bg=%u win=%u ui=%u blit=%u | max=%u us "
              "(%u frames)",
              (unsigned)us[PH_BG], (unsigned)us[PH_WIN], (unsigned)us[PH_UI],
              (unsigned)us[PH_BLIT], (unsigned)max_us,
              (unsigned)g_frame_count);
  }
  for (int i = 0; i < PH_N; i++)
    g_ph_sum[i] = 0;
  g_frame_cycles_max = 0;
  g_frame_count = 0;
}

/* Marco fantasma del resize en curso: tinte de acento y borde. */
static rect_t resize_outline_rect(void) {
  return rect_grow(resize_to, 2);
}

static void draw_resize_outline(uint32_t *dst, int stride, rect_t clip) {
  if (!resize_window || resize_to.w <= 0 || resize_to.h <= 0)
    return;
  rect_t r = resize_to;
  gfx_fill_rounded_rect(dst, stride, clip, r, AURORA_CORNER_RADIUS,
                        0x160A84FF);
  gfx_draw_rounded_border(dst, stride, clip, r, AURORA_CORNER_RADIUS,
                          0xD8FFFFFF);
  gfx_draw_rounded_border(dst, stride, clip,
                          (rect_t){r.x + 1, r.y + 1, r.w - 2, r.h - 2},
                          AURORA_CORNER_RADIUS - 1, AURORA_ACCENT);
}

/* =========================================================================
 * Zona útil
 * =======================================================================*/
rect_t compositor_work_area(void) {
  int top = AURORA_MENUBAR_H;
  int h = (int)fb_height - top - dock_reserved_height();
  return (rect_t){0, top, (int)fb_width, h > 0 ? h : 0};
}

/* =========================================================================
 * Helpers de ventanas (hilo del compositor)
 * =======================================================================*/
static int stack_contains(const window_t *w) {
  for (window_t *x = window_stack; x; x = x->next)
    if (x == w)
      return 1;
  return 0;
}

static void update_menubar_title(void) {
  const char *name = NULL;
  for (window_t *w = window_stack; w; w = w->next) {
    if ((w->flags & WIN_FLAGS_FOCUSED) && !(w->flags & WIN_FLAGS_HIDDEN)) {
      name = w->title;
      break;
    }
  }
  menubar_set_app_name(name);
}

static void do_focus_window(window_t *win);
static void do_minimize_window(window_t *win);
static void do_close_begin(window_t *win);
static void do_toggle_maximize(window_t *win);
static void open_about_window(void);

/* Clic en un icono del Dock, como en macOS: activa la app (trae su ventana
 * al frente) o la restaura si estaba minimizada. Nunca minimiza. */
/* Peticiones de lanzamiento (clic en un lanzador sin ventanas) para el
 * proceso de userspace que gestiona las apps Linux (aurora-wayland). */
#define LAUNCHQ_SIZE 8
static uint32_t launchq[LAUNCHQ_SIZE];
static int lq_head, lq_count;
static spinlock_t launch_lock;

static void launch_request(uint32_t id) {
  unsigned long fl = spin_lock_irqsave(&launch_lock);
  if (lq_count < LAUNCHQ_SIZE) {
    launchq[(lq_head + lq_count) % LAUNCHQ_SIZE] = id;
    lq_count++;
  }
  spin_unlock_irqrestore(&launch_lock, fl);
}

uint32_t compositor_pop_launch(void) {
  uint32_t id = 0;
  unsigned long fl = spin_lock_irqsave(&launch_lock);
  if (lq_count > 0) {
    id = launchq[lq_head];
    lq_head = (lq_head + 1) % LAUNCHQ_SIZE;
    lq_count--;
  }
  spin_unlock_irqrestore(&launch_lock, fl);
  return id;
}

static void taskbar_item_on_click(taskbar_item_t *item) {
  if (!item)
    return;
  for (window_t *win = window_stack; win; win = win->next) {
    if (win->taskbar_item == item) {
      do_focus_window(win);
      return;
    }
  }
  /* Lanzador fijado sin ventanas: abrir la app. */
  if (item->pinned && item->launch_id && !item->launching) {
    item->launching = 8000; /* hasta que aparezca su ventana (o 8 s) */
    launch_request(item->launch_id);
    dock_bounce(item);
  }
}

static void request_close(window_t *win) {
  if (winsrv_has_window(win))
    winsrv_post_event(win, WINSRV_EV_CLOSE, 0, 0, 0);
  else
    do_close_begin(win);
}

static window_t *focused_window(void) {
  for (window_t *w = window_stack; w; w = w->next)
    if ((w->flags & WIN_FLAGS_FOCUSED) && !(w->flags & WIN_FLAGS_HIDDEN))
      return w;
  return NULL;
}

/* Ventana visible más alta que puede recibir el foco (ni cerrándose ni
 * minimizada), distinta de `except`. */
static window_t *next_focus_candidate(const window_t *except) {
  for (window_t *c = window_stack; c; c = c->next) {
    if (c == except || (c->flags & WIN_FLAGS_HIDDEN) || c->minimized ||
        c->anim_state == WIN_ANIM_CLOSING || c->pending_destroy)
      continue;
    return c;
  }
  return NULL;
}

/* ---- Datos de las ventanas para la barra de menús (orden de creación) ---- */
static window_t *win_by_index(int idx) {
  int i = 0;
  for (taskbar_item_t *it = taskbar_items_head(); it; it = it->next) {
    if (it->type != TASKBAR_ITEM_APP_ICON)
      continue;
    /* Un lanzador fijado puede tener varias ventanas (o ninguna). */
    for (window_t *w = window_stack; w; w = w->next)
      if (w->taskbar_item == it && i++ == idx)
        return w;
  }
  return NULL;
}
static int prov_count(void) {
  int n = 0;
  for (window_t *w = window_stack; w; w = w->next)
    if (w->taskbar_item && w->taskbar_item->type == TASKBAR_ITEM_APP_ICON)
      n++;
  return n;
}
static const char *prov_title(int i) {
  window_t *w = win_by_index(i);
  return w ? w->title : "";
}
static int prov_focused(int i) {
  window_t *w = win_by_index(i);
  return w && (w->flags & WIN_FLAGS_FOCUSED);
}
static int prov_has_focused(void) { return focused_window() != NULL; }
static const menubar_provider_t k_menu_provider = {
    prov_count, prov_title, prov_focused, prov_has_focused};

static void run_menu_action(menu_action_t act) {
  window_t *w = focused_window();
  if ((int)act >= MENU_ACT_FOCUS_BASE) {
    window_t *t = win_by_index((int)act - MENU_ACT_FOCUS_BASE);
    if (t)
      do_focus_window(t);
    return;
  }
  switch (act) {
  case MENU_ACT_ZOOM:
    if (w)
      do_toggle_maximize(w);
    break;
  case MENU_ACT_ABOUT:
    open_about_window();
    break;
  case MENU_ACT_MINIMIZE:
    if (w)
      do_minimize_window(w);
    break;
  case MENU_ACT_CLOSE:
    if (w)
      request_close(w);
    break;
  default:
    break;
  }
}

/* Olvida cualquier interacción en curso con `win` (drag, resize, botón). */
static void drop_interaction(window_t *win) {
  if (drag_window == win)
    drag_window = NULL;
  if (pressed_win == win) {
    pressed_win = NULL;
    pressed_btn = WIN_BTN_NONE;
  }
  if (resize_window == win) {
    compositor_invalidate_rect(resize_outline_rect());
    resize_window = NULL;
    resize_to = (rect_t){0, 0, 0, 0};
  }
  if (title_click_win == win)
    title_click_win = NULL;
}

static void do_attach(window_t *win) {
  if (!win)
    return;
  /* La barra de título nunca debe quedar bajo la barra de menús. */
  if (win->y < AURORA_MENUBAR_H)
    win->y = AURORA_MENUBAR_H;

  win->next = window_stack;
  win->prev = NULL;
  if (window_stack)
    window_stack->prev = win;
  window_stack = win;

  if (!(win->flags & WIN_FLAGS_POPUP))
    win->taskbar_item =
        taskbar_add_item(TASKBAR_ITEM_APP_ICON, win->title, win->icon_symbol,
                         win->icon_bg_color, taskbar_item_on_click);
  if (win->taskbar_item && win->icon_bmp_node)
    win->taskbar_item->icon_bmp_node = win->icon_bmp_node;

  invalidate_window(win);
  dock_invalidate_layout();
  dock_bounce(win->taskbar_item); /* la app "se abre" en el Dock */
}

/* "Acerca de AuroraOS": ventana del propio compositor, centrada en la zona
 * útil. Si ya está abierta, se trae al frente. */
static void open_about_window(void) {
  if (about_win && stack_contains(about_win) && !about_win->pending_destroy) {
    do_focus_window(about_win);
    return;
  }
  rect_t wa = compositor_work_area();
  int x = wa.x + (wa.w - ABOUT_W) / 2;
  int y = wa.y + imax(0, (wa.h - ABOUT_H) / 3);
  window_t *w = window_create(x, y, ABOUT_W, ABOUT_H, "Acerca de AuroraOS",
                              WIN_FLAGS_FIXED_SIZE);
  if (!w)
    return;
  win_set_icon_text(w, "A", AURORA_ACCENT);
  about_paint(w);
  about_win = w;
  do_attach(w);
  do_focus_window(w);
}

static void do_close_begin(window_t *win) {
  if (!win)
    return;
  window_start_close_animation(win);
  win->pending_destroy = 1;
  drop_interaction(win);
  invalidate_window(win);
}

/* Icono del Dock de la ventana: destino del genie. */
static rect_t dock_target(const window_t *win) {
  rect_t r = {0, 0, 0, 0};
  if (win->taskbar_item && !dock_item_rect(win->taskbar_item, &r))
    r = (rect_t){0, 0, 0, 0};
  return r;
}

static void do_minimize_window(window_t *win) {
  if (!win || win->anim_state == WIN_ANIM_CLOSING || win->pending_destroy)
    return;
  window_start_minimize_animation(win, dock_target(win));
  drop_interaction(win);

  int was_focused = (win->flags & WIN_FLAGS_FOCUSED) != 0;
  if (was_focused) {
    win->flags &= ~WIN_FLAGS_FOCUSED;
    win->flags |= WIN_FLAGS_INACTIVE;
    win->dirty = 1;
    winsrv_post_event(win, WINSRV_EV_BLUR, 0, 0, 0);
    taskbar_set_active_item(NULL);
  }
  invalidate_window(win);
  dock_invalidate_layout();

  /* Como macOS: el foco pasa a la siguiente ventana visible. */
  if (was_focused) {
    window_t *next = next_focus_candidate(win);
    if (next)
      do_focus_window(next);
  }
  update_menubar_title();
}

static void do_focus_window(window_t *win) {
  if (!win)
    return;
  if (win->anim_state == WIN_ANIM_CLOSING || win->pending_destroy)
    return;

  if (win->minimized || (win->flags & WIN_FLAGS_HIDDEN)) {
    /* Vuelve del Dock con el genie inverso. */
    win->flags &= ~(WIN_FLAGS_HIDDEN | WIN_FLAGS_INACTIVE);
    window_start_restore_animation(win, dock_target(win));
    win->dirty = 1;
    invalidate_window(win);
  }

  if (window_stack == win && (win->flags & WIN_FLAGS_FOCUSED))
    return;

  window_t *old_focus = NULL;
  for (window_t *c = window_stack; c; c = c->next) {
    if (c != win && (c->flags & WIN_FLAGS_FOCUSED)) {
      old_focus = c;
      c->flags &= ~WIN_FLAGS_FOCUSED;
      c->flags |= WIN_FLAGS_INACTIVE;
      c->dirty = 1;
    }
  }

  if (window_stack != win) {
    if (win->prev)
      win->prev->next = win->next;
    if (win->next)
      win->next->prev = win->prev;
    win->next = window_stack;
    win->prev = NULL;
    if (window_stack)
      window_stack->prev = win;
    window_stack = win;
  }

  win->flags |= WIN_FLAGS_FOCUSED;
  win->flags &= ~WIN_FLAGS_INACTIVE;
  win->dirty = 1;

  if (win->taskbar_item)
    taskbar_set_active_item(win->taskbar_item);

  invalidate_window(win);
  if (old_focus)
    invalidate_window(old_focus);

  if (old_focus)
    winsrv_post_event(old_focus, WINSRV_EV_BLUR, 0, 0, 0);
  winsrv_post_event(win, WINSRV_EV_FOCUS, 0, 0, 0);

  dock_invalidate_layout();
  update_menubar_title();
}

static void do_unfocus_all(void) {
  int changed = 0;
  for (window_t *c = window_stack; c; c = c->next) {
    if (c->flags & WIN_FLAGS_FOCUSED) {
      c->flags &= ~WIN_FLAGS_FOCUSED;
      c->flags |= WIN_FLAGS_INACTIVE;
      c->dirty = 1;
      invalidate_window(c);
      winsrv_post_event(c, WINSRV_EV_BLUR, 0, 0, 0);
      changed = 1;
    }
  }
  if (changed) {
    taskbar_set_active_item(NULL);
    dock_invalidate_layout();
    update_menubar_title();
  }
}

/* Destrucción final (tras la animación de cierre). */
static void do_destroy(window_t *win) {
  if (!win)
    return;
  int was_focused = (win->flags & WIN_FLAGS_FOCUSED) != 0;
  drop_interaction(win);

  if (win->prev)
    win->prev->next = win->next;
  else
    window_stack = win->next;
  if (win->next)
    win->next->prev = win->prev;

  compositor_invalidate_rect(
      rect_bounding_box(win->last_vis, win_damage_rect(win)));
  if (win == about_win)
    about_win = NULL;
  if (win->taskbar_item && win->taskbar_item->pinned) {
    /* El lanzador fijado se queda; solo se desengancha la ventana. */
    taskbar_item_t *pin = win->taskbar_item;
    if (pin->win_count > 0)
      pin->win_count--;
    if (pin->win_count == 0)
      pin->is_active = 0;
    win->taskbar_item = NULL;
  }
  if (win->taskbar_item) {
    dock_forget(win->taskbar_item);
    if (dock_pressed_item == win->taskbar_item)
      dock_pressed_item = NULL;
  }
  window_destroy(win); /* también quita el item de la taskbar */
  dock_invalidate_layout();

  /* Como en macOS: al cerrar la ventana activa, pasa el foco a la siguiente
   * visible que no se esté cerrando. */
  if (was_focused) {
    window_t *next = next_focus_candidate(NULL);
    if (next)
      do_focus_window(next);
  }
  update_menubar_title();
}

static void do_toggle_maximize(window_t *win) {
  if (!win || win->anim_state != WIN_ANIM_NONE ||
      (win->flags & (WIN_FLAGS_NODECORATION | WIN_FLAGS_FIXED_SIZE)))
    return;

  rect_t wa = compositor_work_area();
  int nx, ny, nw, nh;
  if (win->maximized) {
    nx = win->restore_x;
    ny = win->restore_y;
    nw = win->restore_w;
    nh = win->restore_h;
  } else {
    win->restore_x = win->x;
    win->restore_y = win->y;
    win->restore_w = win->width;
    win->restore_h = win->height;
    /* Como el Zoom de macOS: la zona útil con un pequeño margen, para que la
     * sombra y las esquinas sigan viéndose. */
    nx = wa.x + 6;
    ny = wa.y + 4;
    nw = imin(wa.w - 12, RESIZE_MAX);
    nh = imin(wa.h - 8, RESIZE_MAX);
  }

  window_resize(win, nw, nh);
  if (win->width != nw || win->height != nh)
    return; /* falló la reserva: no cambiar nada más */

  win->x = nx;
  win->y = ny;
  win->maximized = !win->maximized;
  win->dirty = 1;
  invalidate_window(win);
  winsrv_post_event(win, WINSRV_EV_RESIZE, win->content_w, win->content_h, 0);
  winsrv_post_event(win, WINSRV_EV_MOVE, nx, ny, 0);
  winsrv_post_event(win, WINSRV_EV_STATE, win->maximized, 0, 0);
}

/* Fija (o actualiza) un lanzador en el Dock. */
static void do_dock_pin(const comp_cmd_t *c) {
  taskbar_item_t *it = NULL;
  for (taskbar_item_t *i = taskbar_items_head(); i; i = i->next)
    if (i->pinned && !strcmp(i->app_id, c->app_id)) {
      it = i;
      break;
    }
  if (!it) {
    it = taskbar_add_item(TASKBAR_ITEM_APP_ICON, c->title, c->symbol, c->color,
                          taskbar_item_on_click);
    if (!it)
      return;
    it->pinned = 1;
    strncpy(it->app_id, c->app_id, sizeof(it->app_id) - 1);
  }
  it->launch_id = (uint32_t)c->value;
  if (c->node) {
    it->icon_bmp_node = c->node;
    it->dock_icon_state = 0; /* regenerar con el icono real */
  }
  dock_invalidate_layout();
}

/* La ventana `win` pertenece a la app `app_id`: si hay un lanzador fijado
 * para ella, pasa a colgar de él (un solo icono por app). */
static void do_win_app(window_t *win, const char *app_id) {
  taskbar_item_t *pin = NULL;
  for (taskbar_item_t *i = taskbar_items_head(); i; i = i->next)
    if (i->pinned && !strcmp(i->app_id, app_id)) {
      pin = i;
      break;
    }
  if (!pin || win->taskbar_item == pin)
    return;
  taskbar_item_t *old = win->taskbar_item;
  win->taskbar_item = pin;
  if (old) {
    dock_forget(old);
    if (dock_pressed_item == old)
      dock_pressed_item = NULL;
    taskbar_remove_item_ptr(old);
  }
  pin->win_count++;
  pin->launching = 0;
  if (win->flags & WIN_FLAGS_FOCUSED)
    taskbar_set_active_item(pin);
  dock_invalidate_layout();
  dock_bounce(pin);
}

static void exec_cmd(const comp_cmd_t *c) {
  switch (c->type) {
  case CMD_ATTACH:
    do_attach(c->win);
    break;
  case CMD_CLOSE:
    if (stack_contains(c->win))
      do_close_begin(c->win);
    break;
  case CMD_MINIMIZE:
    if (stack_contains(c->win))
      do_minimize_window(c->win);
    break;
  case CMD_FOCUS:
    if (stack_contains(c->win))
      do_focus_window(c->win);
    break;
  case CMD_ICON_BMP:
    if (stack_contains(c->win))
      win_set_icon_bmp(c->win, c->node);
    break;
  case CMD_TITLE:
    if (stack_contains(c->win)) {
      memcpy(c->win->title, c->title, sizeof(c->win->title));
      c->win->title[sizeof(c->win->title) - 1] = 0;
      if (c->win->taskbar_item && !c->win->taskbar_item->pinned) {
        strncpy(c->win->taskbar_item->label, c->win->title,
                sizeof(c->win->taskbar_item->label) - 1);
        dock_invalidate_layout();
      }
      c->win->dirty = 1; /* redibuja la barra de titulo */
      invalidate_window(c->win);
      update_menubar_title();
    }
    break;
  case CMD_MAXIMIZE:
    /* do_toggle_maximize alterna y no hace nada durante una animacion (p. ej.
     * la de apertura): se deja pendiente y se aplica al terminar. */
    if (stack_contains(c->win)) {
      if (c->win->anim_state != WIN_ANIM_NONE)
        c->win->pending_max = c->value ? 1 : 2;
      else if (!!c->win->maximized != !!c->value)
        do_toggle_maximize(c->win);
    }
    break;
  case CMD_DOCK_PIN:
    do_dock_pin(c);
    break;
  case CMD_WIN_APP:
    if (stack_contains(c->win))
      do_win_app(c->win, c->app_id);
    break;
  case CMD_WIN_RESIZE:
    /* El cliente necesita mas espacio (p. ej. xdg_toplevel.set_min_size). */
    if (stack_contains(c->win) && c->win->anim_state == WIN_ANIM_NONE &&
        !c->win->maximized && !(c->win->flags & WIN_FLAGS_FIXED_SIZE)) {
      window_t *win = c->win;
      int nw = (c->value >> 16) & 0xFFFF, nh = c->value & 0xFFFF;
      window_resize(win, nw, nh);
      if (win->width == nw && win->height == nh) {
        invalidate_window(win);
        winsrv_post_event(win, WINSRV_EV_RESIZE, win->content_w, win->content_h,
                          0);
      }
    }
    break;
  case CMD_WIN_GRAB:
    /* xdg_toplevel.move/resize: solo mientras siga pulsado el boton izquierdo
     * (el cliente lo pide con el serial de ese clic). */
    if (stack_contains(c->win) && (last_mouse_buttons & 0x01) && !drag_window &&
        !resize_window && c->win->anim_state == WIN_ANIM_NONE &&
        !(c->win->flags & (WIN_FLAGS_NODECORATION | WIN_FLAGS_POPUP))) {
      window_t *win = c->win;
      if (c->value == 0) {
        drag_window = win;
        drag_offset_x = cursor_x - win->x;
        drag_offset_y = cursor_y - win->y;
      } else if (!(win->flags & WIN_FLAGS_FIXED_SIZE)) {
        resize_window = win;
        resize_edge = c->value;
        resize_origin_x = cursor_x;
        resize_origin_y = cursor_y;
        resize_from = (rect_t){win->x, win->y, win->width, win->height};
        resize_to = resize_from;
      }
    }
    break;
  }
}

/* API pública: encola y despierta. */
window_t *compositor_create_window(int x, int y, int w, int h,
                                   const char *title, uint32_t flags) {
  window_t *win = window_create(x, y, w, h, title, flags);
  if (!win)
    return NULL;
  comp_cmd_t c = {.type = CMD_ATTACH, .win = win, .node = NULL};
  cmd_post(&c);
  return win;
}
void compositor_close_window(window_t *win) {
  comp_cmd_t c = {.type = CMD_CLOSE, .win = win, .node = NULL};
  cmd_post(&c);
}
void compositor_minimize_window(window_t *win) {
  comp_cmd_t c = {.type = CMD_MINIMIZE, .win = win, .node = NULL};
  cmd_post(&c);
}
void compositor_focus_window(window_t *win) {
  comp_cmd_t c = {.type = CMD_FOCUS, .win = win, .node = NULL};
  cmd_post(&c);
}
void compositor_set_title(window_t *win, const char *title) {
  comp_cmd_t c = {.type = CMD_TITLE, .win = win, .node = NULL};
  strncpy(c.title, title, sizeof(c.title) - 1);
  cmd_post(&c);
}
void compositor_set_maximized(window_t *win, int maximized) {
  comp_cmd_t c = {.type = CMD_MAXIMIZE, .win = win, .node = NULL,
                  .value = maximized};
  cmd_post(&c);
}
void compositor_dock_pin(const char *name, const char *app_id,
                         const char *symbol, uint32_t color, uint32_t launch_id,
                         tar_node_t *icon) {
  comp_cmd_t c = {.type = CMD_DOCK_PIN, .node = icon, .color = color,
                  .value = (int)launch_id};
  strncpy(c.title, name, sizeof(c.title) - 1);
  strncpy(c.app_id, app_id, sizeof(c.app_id) - 1);
  strncpy(c.symbol, symbol, sizeof(c.symbol) - 1);
  cmd_post(&c);
}
void compositor_set_app_id(window_t *win, const char *app_id) {
  comp_cmd_t c = {.type = CMD_WIN_APP, .win = win};
  strncpy(c.app_id, app_id, sizeof(c.app_id) - 1);
  cmd_post(&c);
}
void compositor_request_resize(window_t *win, int w, int h) {
  comp_cmd_t c = {.type = CMD_WIN_RESIZE, .win = win, .value = (w << 16) | h};
  cmd_post(&c);
}
void compositor_begin_grab(window_t *win, int edges) {
  comp_cmd_t c = {.type = CMD_WIN_GRAB, .win = win, .value = edges};
  cmd_post(&c);
}
void compositor_set_icon_bmp(window_t *win, tar_node_t *node) {
  comp_cmd_t c = {.type = CMD_ICON_BMP, .win = win, .node = node};
  cmd_post(&c);
}

/* =========================================================================
 * Cursor (imágenes de cursor.c, mezcladas sobre el backbuffer al volcar)
 * =======================================================================*/
static rect_t cursor_rect(void) {
  const cursor_img_t *c = cursor_get(cursor_shape);
  return (rect_t){cursor_x - c->hot_x, cursor_y - c->hot_y, c->w, c->h};
}

/* El cursor cambió de sitio o de forma: repintar donde estaba y donde está. */
static void cursor_moved(void) {
  rect_t now = cursor_rect();
  if (now.x == cursor_drawn.x && now.y == cursor_drawn.y &&
      now.w == cursor_drawn.w)
    return;
  compositor_invalidate_rect(cursor_drawn);
  compositor_invalidate_rect(now);
  cursor_drawn = now;
}

static void set_cursor_shape(cursor_shape_t s) {
  if (s == cursor_shape)
    return;
  cursor_shape = s;
  compositor_invalidate_rect(cursor_drawn);
  cursor_drawn = (rect_t){0, 0, 0, 0};
  cursor_moved();
}

/* Mezcla el cursor sobre `row` (píxeles x0..x0+n de la fila y, ya copiados del
 * backbuffer). Nunca se lee VRAM. */
static void draw_cursor_row(uint32_t *row, int x0, int n, int y) {
  const cursor_img_t *c = cursor_get(cursor_shape);
  rect_t cr = cursor_rect();
  rect_t r = rect_clip((rect_t){x0, y, n, 1}, cr);
  if (r.w <= 0)
    return;
  const uint32_t *src = &c->px[(y - cr.y) * c->w + (r.x - cr.x)];
  uint32_t *out = &row[r.x - x0];
  for (int x = 0; x < r.w; x++) {
    if (src[x] >> 24)
      out[x] = blend_pixel_fast(src[x], out[x]);
  }
}

static int edges_for_hit(win_hit_t h) {
  switch (h) {
  case WIN_HIT_RESIZE_R:
    return RESIZE_EDGE_RIGHT;
  case WIN_HIT_RESIZE_B:
    return RESIZE_EDGE_BOTTOM;
  case WIN_HIT_RESIZE_BR:
    return RESIZE_EDGE_RIGHT | RESIZE_EDGE_BOTTOM;
  case WIN_HIT_RESIZE_L:
    return RESIZE_EDGE_LEFT;
  case WIN_HIT_RESIZE_T:
    return RESIZE_EDGE_TOP;
  case WIN_HIT_RESIZE_TL:
    return RESIZE_EDGE_TOP | RESIZE_EDGE_LEFT;
  case WIN_HIT_RESIZE_TR:
    return RESIZE_EDGE_TOP | RESIZE_EDGE_RIGHT;
  case WIN_HIT_RESIZE_BL:
    return RESIZE_EDGE_BOTTOM | RESIZE_EDGE_LEFT;
  default:
    return RESIZE_EDGE_NONE;
  }
}

static cursor_shape_t shape_for_edges(int e) {
  int h = e & (RESIZE_EDGE_LEFT | RESIZE_EDGE_RIGHT);
  int v = e & (RESIZE_EDGE_TOP | RESIZE_EDGE_BOTTOM);
  if (h && v) {
    int nwse = (e & RESIZE_EDGE_LEFT) ? (e & RESIZE_EDGE_TOP) != 0
                                      : (e & RESIZE_EDGE_BOTTOM) != 0;
    return nwse ? CURSOR_RESIZE_NWSE : CURSOR_RESIZE_NESW;
  }
  if (h)
    return CURSOR_RESIZE_EW;
  if (v)
    return CURSOR_RESIZE_NS;
  return CURSOR_ARROW;
}

/* Ventana interactiva más alta bajo el cursor y la zona tocada. */
static window_t *window_at_cursor(win_hit_t *hit) {
  for (window_t *w = window_stack; w; w = w->next) {
    if ((w->flags & WIN_FLAGS_HIDDEN) || w->anim_state != WIN_ANIM_NONE)
      continue;
    win_hit_t h = window_hit_test(w, cursor_x, cursor_y);
    if (h != WIN_HIT_NONE) {
      *hit = h;
      return w;
    }
  }
  *hit = WIN_HIT_NONE;
  return NULL;
}

static void update_cursor_shape(void) {
  cursor_shape_t s = CURSOR_ARROW;
  if (resize_window) {
    s = shape_for_edges(resize_edge);
  } else if (!drag_window && !pressed_win && !menubar_is_open() &&
             cursor_y >= AURORA_MENUBAR_H &&
             !dock_hit_test(cursor_x, cursor_y)) {
    win_hit_t h;
    if (window_at_cursor(&h))
      s = shape_for_edges(edges_for_hit(h));
  }
  set_cursor_shape(s);
}

/* =========================================================================
 * Entrada
 * =======================================================================*/
static void compositor_handle_key(uint8_t scancode, int pressed) {
  if (!pressed)
    return;
  if (menubar_is_open() && scancode == 0x01) { /* Esc cierra el menú */
    menubar_close();
    return;
  }
  if (scancode == 0x58) { /* F12: overlay de logs del kernel */
    boot_console_overlay_enable(!boot_console_overlay_enabled());
    compositor_invalidate_rect(screen_rect());
    return;
  }
  window_t *w = focused_window();
  if (w)
    winsrv_post_event(w, WINSRV_EV_KEY, (int32_t)scancode, 1, 0);
}

// Formato de winsrv_event_t::data para MOUSE:
//   bits 0-7  : botón (0=izq 1=medio 2=der 64=rueda arriba 65=rueda abajo)
//   bit  8    : release (1 = soltar)
//   bit  9    : motion  (1 = movimiento con botón pulsado)
#define MOUSE_BUTTON_LEFT 0u
#define MOUSE_BIT_RELEASE (1u << 8)
#define MOUSE_BIT_MOTION (1u << 9)

static void post_client_mouse(window_t *win, uint32_t data) {
  int tb = window_titlebar_h(win);
  winsrv_post_event(win, WINSRV_EV_MOUSE, cursor_x - win->x,
                    cursor_y - win->y - tb, data);
}

/* Id de semáforo (1 min, 2 max, 3 cerrar) para hover/pressed. */
static int light_id(win_hit_t h) {
  return (h == WIN_HIT_MINIMIZE)   ? 1
         : (h == WIN_HIT_MAXIMIZE) ? 2
         : (h == WIN_HIT_CLOSE)    ? 3
                                   : 0;
}

static void set_pressed_light(window_t *w, int id) {
  if (!w || w->pressed_btn == id)
    return;
  w->pressed_btn = id;
  compositor_invalidate_rect(window_invalidate_controls(w));
}

/* Geometría propuesta del resize según el cursor (sin tocar la ventana). */
static rect_t resize_proposal(void) {
  rect_t f = resize_from;
  int dx = cursor_x - resize_origin_x, dy = cursor_y - resize_origin_y;
  int w = f.w, h = f.h;
  if (resize_edge & RESIZE_EDGE_RIGHT)
    w = f.w + dx;
  if (resize_edge & RESIZE_EDGE_LEFT)
    w = f.w - dx;
  if (resize_edge & RESIZE_EDGE_BOTTOM)
    h = f.h + dy;
  if (resize_edge & RESIZE_EDGE_TOP)
    h = f.h - dy;
  w = imax(RESIZE_MIN_W, imin(w, RESIZE_MAX));
  h = imax(RESIZE_MIN_H, imin(h, RESIZE_MAX));
  int x = (resize_edge & RESIZE_EDGE_LEFT) ? f.x + f.w - w : f.x;
  int y = f.y;
  if (resize_edge & RESIZE_EDGE_TOP) {
    y = f.y + f.h - h;
    if (y < AURORA_MENUBAR_H) { /* el borde superior no pasa la barra */
      y = AURORA_MENUBAR_H;
      h = f.y + f.h - y;
    }
  }
  return (rect_t){x, y, w, h};
}

static void finish_resize(void) {
  window_t *w = resize_window;
  rect_t to = resize_to;
  compositor_invalidate_rect(resize_outline_rect());
  resize_window = NULL;
  resize_edge = 0;
  resize_to = (rect_t){0, 0, 0, 0};
  if (!w || to.w <= 0 || to.h <= 0)
    return;
  if (to.w == w->width && to.h == w->height && to.x == w->x && to.y == w->y)
    return;

  if (to.w != w->width || to.h != w->height) {
    window_resize(w, to.w, to.h);
    if (w->width != to.w || w->height != to.h)
      return; /* sin memoria: se queda como estaba */
    winsrv_post_event(w, WINSRV_EV_RESIZE, w->content_w, w->content_h, 0);
  }
  w->x = to.x;
  w->y = to.y;
  int was_max = w->maximized;
  w->maximized = 0;
  invalidate_window(w);
  winsrv_post_event(w, WINSRV_EV_MOVE, w->x, w->y, 0);
  if (was_max)
    winsrv_post_event(w, WINSRV_EV_STATE, 0, 0, 0);
}

/* Puntero "crudo" para clientes con WIN_FLAGS_RAW_INPUT (p. ej. el servidor
 * Wayland): movimiento y botones sin filtrar sobre el area de cliente. Con un
 * boton pulsado el cliente que lo recibio conserva el puntero (grab). */
static window_t *raw_ptr_win;

static void route_raw_pointer(int mouse_moved, uint8_t buttons) {
  window_t *target = NULL;
  int grab = buttons && raw_ptr_win && stack_contains(raw_ptr_win);
  if (grab) {
    target = raw_ptr_win;
  } else if (!buttons && !drag_window && !resize_window && !pressed_win &&
             !menubar_is_open()) {
    win_hit_t h;
    window_t *w = window_at_cursor(&h);
    if (w && h == WIN_HIT_CLIENT && (w->flags & WIN_FLAGS_RAW_INPUT))
      target = w;
  }

  if (raw_ptr_win && target != raw_ptr_win) {
    winsrv_post_event(raw_ptr_win, WINSRV_EV_PTR_LEAVE, 0, 0, 0);
    raw_ptr_win = NULL;
  }
  if (!target)
    return;
  int entered = (raw_ptr_win != target);
  if (entered || mouse_moved || buttons != (uint8_t)last_mouse_buttons) {
    raw_ptr_win = target;
    winsrv_post_event(target, WINSRV_EV_PTR, cursor_x - target->x,
                      cursor_y - target->y - window_titlebar_h(target),
                      buttons & 7u);
  }
}

static void compositor_handle_mouse(int dx, int dy, uint8_t buttons) {
  cursor_x += dx;
  cursor_y += dy;
  if (cursor_x < 0)
    cursor_x = 0;
  if (cursor_x >= (int)fb_width)
    cursor_x = (int)fb_width - 1;
  if (cursor_y < 0)
    cursor_y = 0;
  if (cursor_y >= (int)fb_height)
    cursor_y = (int)fb_height - 1;

  int mouse_moved = (dx != 0 || dy != 0);
  int pressed = buttons & 0x01;
  int released = (last_mouse_buttons & 0x01) && !pressed;

  dock_set_cursor(cursor_x, cursor_y);

  /* ---------- 0. Hover: barra de menús ---------- */
  if (mouse_moved)
    menubar_mouse_move(cursor_x, cursor_y);

  /* ---------- 0.5 Hover de los semáforos ---------- */
  for (window_t *w = window_stack; w; w = w->next) {
    int old_btn = w->hover_btn, old_ctrl = w->hover_controls;
    w->hover_btn = 0;
    w->hover_controls = 0;
    if (!(w->flags & WIN_FLAGS_HIDDEN) && w->anim_state == WIN_ANIM_NONE &&
        !menubar_is_open() && !drag_window && !resize_window) {
      if (window_in_controls(w, cursor_x, cursor_y)) {
        w->hover_controls = 1;
        w->hover_btn = light_id(window_hit_test(w, cursor_x, cursor_y));
      }
    }
    if (w->hover_btn != old_btn || w->hover_controls != old_ctrl)
      compositor_invalidate_rect(window_invalidate_controls(w));
  }

  /* ---------- 1. Press izquierdo ---------- */
  if (pressed && !(last_mouse_buttons & 0x01)) {
    menu_action_t act = MENU_ACT_NONE;
    if (menubar_mouse_press(cursor_x, cursor_y, &act)) {
      if (act != MENU_ACT_NONE)
        run_menu_action(act);
    } else {
      taskbar_item_t *item = dock_hit_test(cursor_x, cursor_y);
      if (item) {
        dock_pressed_item = item;
        dock_set_pressed(item);
      } else {
        win_hit_t h;
        window_t *win = window_at_cursor(&h);
        if (!win)
          do_unfocus_all();
        else if (win_hit_is_resize(h)) {
          resize_window = win;
          resize_edge = edges_for_hit(h);
          resize_origin_x = cursor_x;
          resize_origin_y = cursor_y;
          resize_from = (rect_t){win->x, win->y, win->width, win->height};
          resize_to = resize_from;
          do_focus_window(win);
        } else {
          switch (h) {
          case WIN_HIT_CLOSE:
          case WIN_HIT_MINIMIZE:
          case WIN_HIT_MAXIMIZE:
            pressed_win = win;
            pressed_btn = (h == WIN_HIT_CLOSE)      ? WIN_BTN_CLOSE_PRESSED
                          : (h == WIN_HIT_MINIMIZE) ? WIN_BTN_MINIMIZE_PRESSED
                                                    : WIN_BTN_MAXIMIZE_PRESSED;
            set_pressed_light(win, light_id(h));
            break;
          case WIN_HIT_TITLEBAR: {
            uint64_t now = comp_rdtsc();
            uint64_t freq = klog_get_tsc_freq();
            uint64_t dbl = freq ? freq * WIN_DOUBLE_CLICK_MS / 1000 : 0;
            do_focus_window(win);
            if (title_click_win == win && now - title_click_tsc < dbl) {
              title_click_win = NULL;
              do_toggle_maximize(win);
            } else {
              title_click_win = win;
              title_click_tsc = now;
              drag_window = win;
              drag_offset_x = cursor_x - win->x;
              drag_offset_y = cursor_y - win->y;
            }
            break;
          }
          case WIN_HIT_CLIENT:
            if (!(win->flags & WIN_FLAGS_POPUP))
              do_focus_window(win);
            post_client_mouse(win, MOUSE_BUTTON_LEFT);
            break;
          default:
            break;
          }
        }
      }
    }
  }

  /* ---------- 2. Release ---------- */
  if (released) {
    /* Dock: la acción ocurre al soltar, si se suelta sobre el mismo icono. */
    if (dock_pressed_item) {
      taskbar_item_t *it = dock_pressed_item;
      dock_pressed_item = NULL;
      dock_set_pressed(NULL);
      if (dock_hit_test(cursor_x, cursor_y) == it && it->on_click)
        it->on_click(it);
    }

    /* Resize: se aplica una sola vez al soltar. */
    if (resize_window)
      finish_resize();

    /* Semáforos: la acción solo se ejecuta si se suelta sobre el mismo. */
    if (pressed_win && pressed_btn != WIN_BTN_NONE) {
      window_t *pw = pressed_win;
      win_hit_t h = window_hit_test(pw, cursor_x, cursor_y);
      set_pressed_light(pw, 0);
      if (pressed_btn == WIN_BTN_CLOSE_PRESSED && h == WIN_HIT_CLOSE) {
        request_close(pw);
      } else if (pressed_btn == WIN_BTN_MINIMIZE_PRESSED &&
                 h == WIN_HIT_MINIMIZE) {
        do_minimize_window(pw);
      } else if (pressed_btn == WIN_BTN_MAXIMIZE_PRESSED &&
                 h == WIN_HIT_MAXIMIZE) {
        do_toggle_maximize(pw);
      }
    }

    /* Release al cliente si el cursor está sobre un área de cliente. */
    for (window_t *win = window_stack; win; win = win->next) {
      if (win->flags & WIN_FLAGS_HIDDEN)
        continue;
      if (window_hit_test(win, cursor_x, cursor_y) == WIN_HIT_CLIENT) {
        post_client_mouse(win, MOUSE_BUTTON_LEFT | MOUSE_BIT_RELEASE);
        break;
      }
    }

    drag_window = NULL;
    pressed_win = NULL;
    pressed_btn = WIN_BTN_NONE;
  }

  /* Icono del Dock pulsado: oscurecido solo mientras el cursor siga encima. */
  if (pressed && dock_pressed_item && mouse_moved)
    dock_set_pressed(dock_hit_test(cursor_x, cursor_y) == dock_pressed_item
                         ? dock_pressed_item
                         : NULL);

  /* ---------- 2.5 Semáforo pulsado: oscurecido solo mientras el cursor
   * siga encima, como en macOS ---------- */
  if (pressed && pressed_win && mouse_moved) {
    win_hit_t h = window_hit_test(pressed_win, cursor_x, cursor_y);
    int want = (pressed_btn == WIN_BTN_CLOSE_PRESSED && h == WIN_HIT_CLOSE) ||
                       (pressed_btn == WIN_BTN_MINIMIZE_PRESSED &&
                        h == WIN_HIT_MINIMIZE) ||
                       (pressed_btn == WIN_BTN_MAXIMIZE_PRESSED &&
                        h == WIN_HIT_MAXIMIZE)
                   ? light_id(h)
                   : 0;
    set_pressed_light(pressed_win, want);
  }

  /* ---------- 3. Resize en curso: solo el marco fantasma hasta soltar ---- */
  if (pressed && resize_window) {
    rect_t to = resize_proposal();
    if (to.x != resize_to.x || to.y != resize_to.y || to.w != resize_to.w ||
        to.h != resize_to.h) {
      rect_t old = resize_outline_rect();
      resize_to = to;
      compositor_invalidate_rect(rect_bounding_box(old, resize_outline_rect()));
    }
  }

  /* ---------- 4. Drag por la barra de título ---------- */
  if (pressed && drag_window) {
    int new_x = cursor_x - drag_offset_x;
    int new_y = cursor_y - drag_offset_y;
    new_y = imax(new_y, AURORA_MENUBAR_H);    /* no bajo la barra */
    new_y = imin(new_y, (int)fb_height - 40); /* siempre agarrable */
    if (new_x != drag_window->x || new_y != drag_window->y) {
      drag_window->x = new_x;
      drag_window->y = new_y;
      if (drag_window->maximized)
        winsrv_post_event(drag_window, WINSRV_EV_STATE, 0, 0, 0);
      drag_window->maximized = 0;
      title_click_win = NULL; /* arrastrar no cuenta como clic */
      invalidate_window(drag_window);
      winsrv_post_event(drag_window, WINSRV_EV_MOVE, new_x, new_y, 0);
    }
  }

  /* ---------- 5. Motion sobre cliente con botón pulsado ---------- */
  if (pressed && mouse_moved && !drag_window && !resize_window &&
      !pressed_win) {
    for (window_t *win = window_stack; win; win = win->next) {
      if (win->flags & WIN_FLAGS_HIDDEN)
        continue;
      if (window_hit_test(win, cursor_x, cursor_y) == WIN_HIT_CLIENT) {
        post_client_mouse(win, MOUSE_BUTTON_LEFT | MOUSE_BIT_MOTION);
        break;
      }
    }
  }

  route_raw_pointer(mouse_moved, buttons);

  last_mouse_buttons = buttons;

  /* ---------- 6. Cursor: forma y posición ---------- */
  update_cursor_shape();
  cursor_moved();
}

/* Procesa hasta EVENT_BUDGET eventos por iteración y funde el movimiento
 * consecutivo del ratón (misma pulsación) en uno solo. Un cambio de botones
 * nunca se funde: se procesa por separado y en orden. Con un ratón USB a
 * 1000 Hz el presupuesto alcanza para varios frames de eventos acumulados. */
#define EVENT_BUDGET 256

/* Duración mínima de un frame con volcado (ms). */
#define COMP_FRAME_MS 16

static void compositor_process_events(void) {
  input_event_t ev;
  int budget = EVENT_BUDGET;
  int mdx = 0, mdy = 0, have_m = 0;

  while (budget-- > 0 && input_pop(&ev)) {
    if (ev.type == INPUT_EV_MOUSE) {
      if (ev.code != (uint8_t)last_mouse_buttons) {
        if (have_m) {
          compositor_handle_mouse(mdx, mdy, (uint8_t)last_mouse_buttons);
          mdx = mdy = have_m = 0;
        }
        compositor_handle_mouse(ev.value, ev.value2, ev.code);
      } else {
        mdx += ev.value;
        mdy += ev.value2;
        have_m = 1;
      }
    } else if (ev.type == INPUT_EV_KEY) {
      if (have_m) {
        compositor_handle_mouse(mdx, mdy, (uint8_t)last_mouse_buttons);
        mdx = mdy = have_m = 0;
      }
      compositor_handle_key(ev.code, ev.value);
    }
  }
  if (have_m)
    compositor_handle_mouse(mdx, mdy, (uint8_t)last_mouse_buttons);

  if (input_pending() > 0) { /* quedan eventos: seguir en la próxima vuelta */
    compositor_has_input = 1;
  }
}

/* =========================================================================
 * Render
 * =======================================================================*/

/* Fondo + ventanas de atrás hacia delante en `e`. Si una ventana opaca tapa
 * `e` entero, se empieza por ella: ni el fondo ni lo que hay debajo se ven
 * (p. ej. el cursor del terminal parpadeando solo recompone el terminal). */
static void compose_scene(rect_t e) {
  window_t *cover = NULL;
  for (window_t *w = window_stack; w; w = w->next) {
    if (window_covers(w, e)) {
      cover = w;
      break;
    }
  }

  window_t *curr = cover;
  if (!curr) {
    if (wallpaper_cache) {
      for (int y = e.y; y < e.y + e.h; y++)
        simd_copy_normal(&backbuffer[y * fb_width + e.x],
                         &wallpaper_cache[y * fb_width + e.x], e.w);
    } else {
      for (int y = e.y; y < e.y + e.h; y++)
        simd_fill_row(&backbuffer[y * fb_width + e.x], 0xFF1E1E2E, e.w);
    }
    curr = window_stack;
    while (curr && curr->next)
      curr = curr->next;
  }
  for (; curr; curr = curr->prev) {
    if (!(curr->flags & WIN_FLAGS_HIDDEN) &&
        rect_intersects(e, window_visual_rect(curr)))
      window_render_frame(curr, backbuffer, fb_width, e);
  }
}

/* Backbuffer -> VRAM del rectángulo `clip`, con el cursor encima. Sin
 * preempción: así nunca queda en pantalla media ventana en la posición
 * nueva y media en la vieja durante un quantum entero de otra tarea. */
/* Copia de lo que hay en VRAM (cursor incluido) y fila temporal. Sin vsync, el
 * escaneo de la pantalla (o el refresco de QEMU/KVM) puede caer a mitad del
 * volcado y mostrar media ventana en la posición vieja y media en la nueva
 * (tearing). Solo se escriben en VRAM los píxeles que cambian de verdad: al
 * mover una ventana la mayor parte del rect dañado es idéntica y el volcado
 * es mucho más corto. */
static uint32_t *front_shadow = NULL;
static uint32_t *present_line = NULL;
#define FRONT_UNKNOWN 0x01234567u /* alpha != 0xFF: nunca sale del compositor */
#define PRESENT_GAP 16 /* píxeles iguales que se escriben igual para no partir */

static void present_row_delta(int y, int x0, int n) {
  const uint32_t *src = &backbuffer[y * fb_width + x0];
  uint32_t *line = present_line;
  uint32_t *front = &front_shadow[y * fb_width + x0];
  uint32_t *vram = &fb_ptr[y * fb_pitch + x0];

  rect_t cr = cursor_rect();
  if (y >= cr.y && y < cr.y + cr.h && cr.x < x0 + n && cr.x + cr.w > x0) {
    simd_copy_normal(line, src, (size_t)n);
    draw_cursor_row(line, x0, n, y);
    src = line;
  }

  int i = 0;
  while (i < n) {
    while (i < n && src[i] == front[i])
      i++;
    if (i >= n)
      break;
    int start = i, last = i, same = 0;
    for (; i < n; i++) {
      if (src[i] != front[i]) {
        last = i;
        same = 0;
      } else if (++same >= PRESENT_GAP) {
        break;
      }
    }
    int len = last - start + 1;
    simd_copy_to_vram(&vram[start], &src[start], (size_t)len);
    simd_copy_normal(&front[start], &src[start], (size_t)len);
  }
}

static void present_rect(rect_t clip) {
  if (!fb_ptr)
    return;
  preempt_disable();
  if (fb_bpp == 24 || !front_shadow || !present_line) {
    for (int y = clip.y; y < clip.y + clip.h; y++) {
      uint32_t *dst = &fb_ptr[y * fb_pitch + clip.x];
      const uint32_t *src = &backbuffer[y * fb_width + clip.x];
      if (fb_bpp == 24) /* fb_ptr es un buffer en RAM; fb_flush_rect empaqueta */
        simd_copy_normal(dst, src, clip.w);
      else
        simd_copy_to_vram(dst, src, clip.w);
      draw_cursor_row(dst, clip.x, clip.w, y);
    }
    fb_flush_rect(fb_ptr, fb_pitch, (uint32_t)clip.x, (uint32_t)clip.y,
                  (uint32_t)clip.w, (uint32_t)clip.h);
  } else {
    for (int y = clip.y; y < clip.y + clip.h; y++)
      present_row_delta(y, clip.x, clip.w);
  }
  preempt_enable();
}

static void render_rect(rect_t clip) {
  uint64_t t0 = comp_rdtsc();

  /* Los paneles de cristal (Dock, tooltip, menús) desenfocan lo que tienen
   * debajo: para la parte de cada uno que cae en el clip, el fondo debe estar
   * recién compuesto también en un margen del radio de desenfoque. Solo se
   * compone ese margen extra; el cristal y la UI se pintan y vuelcan solo
   * dentro del clip. */
  glass_rect_t glass[4];
  int ng = dock_glass_rects(glass, 4);
  ng += menubar_glass_rects(glass + ng, 4 - ng);
  rect_t e = clip;
  for (int i = 0; i < ng; i++) {
    rect_t s = gfx_glass_source_rect(clip, glass[i].r, glass[i].blur);
    if (s.w > 0 && s.h > 0)
      e = rect_bounding_box(e, s);
  }
  e = rect_clip(e, screen_rect());

  /* 1-2. Fondo y ventanas */
  uint64_t t1 = comp_rdtsc();
  compose_scene(e);
  uint64_t t2 = comp_rdtsc();

  /* 3. Dock, barra de menús y marco de resize */
  dock_render(backbuffer, fb_width, clip);
  menubar_render(backbuffer, fb_width, clip);
  if (resize_window)
    draw_resize_outline(backbuffer, fb_width, clip);
  /* Overlay de logs del kernel, sobre todo lo demás (solo si está
   * activado: arranque sin splash). */
  boot_console_draw_overlay(backbuffer, fb_width, clip.x, clip.y, clip.w,
                            clip.h);
  uint64_t t3 = comp_rdtsc();

  /* 4. Volcado a VRAM + cursor */
  present_rect(clip);
  gpup_notify_dirty(clip.x, clip.y, clip.w, clip.h);
  uint64_t t4 = comp_rdtsc();

  g_ph_cur[PH_BG] += t1 - t0;
  g_ph_cur[PH_WIN] += t2 - t1;
  g_ph_cur[PH_UI] += t3 - t2;
  g_ph_cur[PH_BLIT] += t4 - t3;
}

/* Devuelve 1 si se pintó algo. */
static int compositor_render(void) {
  rect_t list[DAMAGE_MAX];
  int n = damage_take(list);
  if (n == 0)
    return 0;

  uint64_t t0 = comp_rdtsc();
  for (int i = 0; i < n; i++) {
    rect_t r = rect_clip(list[i], screen_rect());
    if (r.w > 0 && r.h > 0)
      render_rect(r);
  }
  compositor_report_frame_stats(comp_rdtsc() - t0);
  return 1;
}

/* =========================================================================
 * Animaciones
 * =======================================================================*/
static int compositor_has_active_animations(void) {
  for (window_t *w = window_stack; w; w = w->next)
    if (w->anim_state != WIN_ANIM_NONE)
      return 1;
  return 0;
}

static int compositor_tick_animations(int dt_ms) {
  window_t *w = window_stack;
  while (w) {
    window_t *next = w->next;
    if (w->anim_state != WIN_ANIM_NONE) {
      /* El destino del genie sigue al icono (el Dock puede estar animando la
       * magnificación). */
      if (w->anim_state == WIN_ANIM_MINIMIZING ||
          w->anim_state == WIN_ANIM_RESTORING) {
        rect_t t = dock_target(w);
        if (t.w > 0)
          w->genie_to = t;
      }
      window_advance_animation(w, dt_ms);

      if (w->anim_state == WIN_ANIM_NONE) {
        if (w->pending_destroy) {
          w->pending_destroy = 0;
          do_destroy(w);
          w = next;
          continue;
        }
        if (w->anim_alpha == 0 && !(w->flags & WIN_FLAGS_HIDDEN)) {
          w->flags |= WIN_FLAGS_HIDDEN; /* fin del minimizado */
          w->flags &= ~WIN_FLAGS_FOCUSED;
          w->flags |= WIN_FLAGS_INACTIVE;
          w->dirty = 1;
        }
        if (w->pending_max) {
          int want = w->pending_max == 1;
          w->pending_max = 0;
          if (!(w->flags & WIN_FLAGS_HIDDEN) && !!w->maximized != want)
            do_toggle_maximize(w);
        }
      }
      invalidate_window(w);
    }
    w = next;
  }
  return compositor_has_active_animations();
}

/* =========================================================================
 * Despertar y bucle principal
 * =======================================================================*/
static bool cond_never(void *arg) {
  (void)arg;
  return false;
}

static bool cond_work(void *arg) {
  (void)arg;
  return compositor_has_input || compositor_needs_clock ||
         compositor_dock_dirty || cq_count > 0 || dmg_pending;
}

/* Con animaciones en curso no despertamos por el daño que genera el propio
 * compositor, solo por entrada/comandos o por el siguiente frame. */
static bool cond_urgent_or_frame(void *arg) {
  (void)arg;
  return compositor_has_input || compositor_needs_clock ||
         compositor_dock_dirty || cq_count > 0 || compositor_frame_due;
}

void compositor_notify_event(void) {
  compositor_has_input = 1;
  wake_up_all(&compositor_wq);
}

void compositor_notify_clock_tick(void) {
  compositor_needs_clock = 1;
  wake_up_all(&compositor_wq);
}

void compositor_frame_tick(void) {
  if (!compositor_animating)
    return;
  if (tick_count & 15) /* cada 16 ms ≈ 62 fps */
    return;
  compositor_frame_due = 1;
  wake_up_all(&compositor_wq);
}

void compositor_init(void) {
  spin_init(&dmg_lock);
  spin_init(&cmd_lock);
  font_manager_init();
  cursor_init();

  compositor_has_input = 0;
  compositor_needs_clock = 0;

  size_t size = (size_t)fb_width * fb_height * sizeof(uint32_t);
  backbuffer = (uint32_t *)kmalloc(size);
  if (!backbuffer) {
    LOG_ERR("[COMPOSITOR] ERROR CRITICO: No se pudo alojar el Backbuffer");
    return;
  }

  if (fb_bpp == 32) {
    front_shadow = (uint32_t *)kmalloc(size);
    present_line = (uint32_t *)kmalloc((size_t)fb_width * sizeof(uint32_t));
    if (!present_line)
      front_shadow = NULL;
    if (front_shadow)
      for (size_t i = 0; i < (size_t)fb_width * fb_height; i++)
        front_shadow[i] = FRONT_UNKNOWN;
  }

  LOG_INFO("[COMPOSITOR] Motor grafico con aceleracion SIMD SSE2 inicializado.");
  taskbar_init();
  winsrv_init();
  dock_init((int)fb_width, (int)fb_height);
  menubar_init((int)fb_width, (int)fb_height);
  menubar_set_provider(&k_menu_provider);
  taskbar_refresh_clock();

  bg_wallpaper_node = tar_find_file("system/wallpapers/wallpaper-aurora-blue.bmp");

  wallpaper_cache = (uint32_t *)kmalloc(size);
  if (wallpaper_cache) {
    /* [FIX] kmalloc no zeroa. Si el BMP tiene alpha=0 en algunos píxeles
     * (típico de BMPs generados desde JPEG), blend_pixel devuelve el dst
     * sin tocarlo, y ese dst es basura del heap. En QEMU suele ser cero,
     * en VirtualBox es ruido. Inicializamos a negro opaco: los píxeles
     * con alpha=0 quedan negros en vez de basura. */
    for (size_t i = 0; i < (size_t)fb_width * fb_height; i++)
      wallpaper_cache[i] = 0xFF000000;

    int drawn = 0;
    if (bg_wallpaper_node) {
      rect_t full_clip = {0, 0, (int)fb_width, (int)fb_height};
      if (bmp_draw_scaled(bg_wallpaper_node, wallpaper_cache, fb_width,
                          full_clip, 0, 0, fb_width, fb_height) == 0) {
        drawn = 1;
      }
    }
    if (!drawn) {
      for (int y = 0; y < (int)fb_height; y++) {
        int t = (y * 256) / (int)fb_height;
        uint32_t top = 0xFF060612;
        uint32_t bot = 0xFF12102A;
        uint32_t base = ((uint32_t)(((top >> 24) & 0xFF) * (256 - t) / 256 +
                                    ((bot >> 24) & 0xFF) * t / 256)
                         << 24) |
                        ((uint32_t)(((top >> 16) & 0xFF) * (256 - t) / 256 +
                                    ((bot >> 16) & 0xFF) * t / 256)
                         << 16) |
                        ((uint32_t)(((top >> 8) & 0xFF) * (256 - t) / 256 +
                                    ((bot >> 8) & 0xFF) * t / 256)
                         << 8) |
                        (uint32_t)(((top & 0xFF) * (256 - t) / 256 +
                                    (bot & 0xFF) * t / 256));
        sse_memset32(&wallpaper_cache[y * fb_width], base, fb_width);
      }

      struct {
        int yc;
        int amp;
        uint32_t color;
        int thick;
      } bands[] = {
          {(int)fb_height * 28 / 100, 40, 0x1800D4FF, 120},
          {(int)fb_height * 42 / 100, 55, 0x147C4DFF, 140},
          {(int)fb_height * 55 / 100, 70, 0x10FF4DD2, 160},
      };
      for (int b = 0; b < 3; b++) {
        for (int y = 0; y < (int)fb_height; y++) {
          int dy = y - bands[b].yc;
          if (dy < -bands[b].thick || dy > bands[b].thick)
            continue;
          for (int x = 0; x < (int)fb_width; x++) {
            int wave = (x * 6 / (int)fb_width) * 2;
            int offset = (dy + ((x / 32) % 3) * 3 - 3 + wave);
            int ad = offset < 0 ? -offset : offset;
            if (ad > bands[b].thick)
              continue;
            int a = (bands[b].thick - ad) * 255 / bands[b].thick;
            int alpha = ((bands[b].color >> 24) & 0xFF) * a / 255;
            if (alpha <= 1)
              continue;
            uint32_t c =
                ((uint32_t)alpha << 24) | (bands[b].color & 0x00FFFFFF);
            wallpaper_cache[y * fb_width + x] =
                blend_pixel_fast(c, wallpaper_cache[y * fb_width + x]);
          }
        }
      }
    }
  }

  cursor_drawn = cursor_rect();
  compositor_invalidate_rect(screen_rect());
}

void compositor_thread(void) {
  compositor_task_ref = sched_current();
  LOG_INFO("[COMP] Compositor thread started");

  uint64_t tsc_freq = klog_get_tsc_freq();
  uint64_t last_tsc = comp_rdtsc();

  while (1) {
    if (!compositor_animating) {
      wait_event(&compositor_wq, cond_work, NULL);
      last_tsc = comp_rdtsc();
    } else {
      /* El timeout (20 ticks) es solo una red de seguridad por si el hook
       * compositor_frame_tick() no estuviera instalado en time_tick(). */
      wait_event_timeout(&compositor_wq, cond_urgent_or_frame, NULL, 20);
    }
    compositor_frame_due = 0;
    uint64_t frame_tsc = comp_rdtsc();

    /* 1. Comandos de otras CPUs (crear/cerrar/enfocar/icono) */
    comp_cmd_t cmd;
    while (cmd_pop(&cmd))
      exec_cmd(&cmd);

    /* 2. Entrada */
    if (compositor_has_input) {
      compositor_has_input = 0;
      compositor_process_events();
    }

    /* 3. Reloj */
    if (compositor_needs_clock) {
      compositor_needs_clock = 0;
      taskbar_refresh_clock();
      menubar_clock_changed();
    }
    if (compositor_dock_dirty) {
      compositor_dock_dirty = 0;
      dock_invalidate_layout();
    }

    /* 4. Tiempo monotónico (TSC), independiente del ritmo de eventos */
    uint64_t now_tsc = comp_rdtsc();
    int dt = tsc_freq > 0 ? (int)(((now_tsc - last_tsc) * 1000) / tsc_freq) : 1;
    last_tsc = now_tsc;
    dt = imax(1, imin(dt, 33));

    /* 5. Animaciones, Dock y barra de menús. El Dock va después: si una
     * ventana se destruye aquí, su item desaparece y dock_update rehace los
     * slots antes de pintar (el genie usa los del frame anterior, que solo
     * compara por puntero). */
    int win_anim = compositor_tick_animations(dt);
    int dock_anim = dock_update(dt);

    rect_t r;
    if (dock_take_damage(&r))
      compositor_invalidate_rect(r);
    if (menubar_take_damage(&r))
      compositor_invalidate_rect(r);

    /* 6. Render */
    int rendered = compositor_render();

    compositor_animating = (win_anim || dock_anim) ? 1 : 0;

    /* Ritmo de frames (~62 fps). Sin esto, con un ratón a 1000 Hz cada
     * evento provoca un volcado a VRAM y, si el frame tarda, el compositor
     * escribe VRAM sin pausa: el escaneo de pantalla siempre pilla una
     * ventana a medias (cortes al arrastrar). Durante la pausa los eventos se
     * acumulan en la cola y se funden en un solo movimiento. */
    if (rendered && tsc_freq > 0) {
      uint64_t spent_ms = (comp_rdtsc() - frame_tsc) * 1000 / tsc_freq;
      if (spent_ms < COMP_FRAME_MS)
        wait_event_timeout(&compositor_wq, cond_never, NULL,
                           COMP_FRAME_MS - spent_ms);
    }
  }
}
