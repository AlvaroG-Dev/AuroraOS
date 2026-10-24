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
#include "compositor.h"
#include "../bmp.h"
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
#include "dock.h"
#include "font_manager.h"
#include "gfx.h"
#include "gfx_filter.h"
#include "menubar.h"
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

// Estado del resize en curso (se aplica una sola vez al soltar).
static window_t *resize_window = NULL;
static int resize_edge = 0;
static int resize_origin_x = 0;
static int resize_origin_y = 0;
static int resize_win_w = 0;
static int resize_win_h = 0;
static int resize_new_w = 0;
static int resize_new_h = 0;

#define RESIZE_EDGE_NONE 0
#define RESIZE_EDGE_RIGHT (1 << 0)
#define RESIZE_EDGE_BOTTOM (1 << 1)

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

/* =========================================================================
 * Cola de comandos (cualquier CPU -> hilo del compositor)
 * =======================================================================*/
typedef enum {
  CMD_ATTACH,
  CMD_CLOSE,
  CMD_MINIMIZE,
  CMD_FOCUS,
  CMD_ICON_BMP
} cmd_type_t;

typedef struct {
  uint8_t type;
  window_t *win;
  tar_node_t *node;
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

/* Outline del resize en curso (línea de acento de 1 px). */
static void draw_resize_outline(uint32_t *dst, int stride, rect_t clip) {
  if (!resize_window || resize_new_w <= 0 || resize_new_h <= 0)
    return;
  rect_t r = {resize_window->x, resize_window->y, resize_new_w, resize_new_h};
  gfx_draw_rounded_border(dst, stride, clip, r, AURORA_CORNER_RADIUS,
                          0xE0FFFFFF);
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

static void taskbar_item_on_click(taskbar_item_t *item) {
  if (!item)
    return;
  for (window_t *win = window_stack; win; win = win->next) {
    if (win->taskbar_item != item)
      continue;
    if (window_is_visible(win) && (win->flags & WIN_FLAGS_FOCUSED)) {
      do_minimize_window(win);
      taskbar_set_active_item(NULL);
      dock_invalidate_layout();
    } else {
      do_focus_window(win);
    }
    break;
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

static void run_menu_action(menu_action_t act) {
  window_t *w = focused_window();
  switch (act) {
  case MENU_ACT_ABOUT:
    LOG_INFO("[COMPOSITOR] Acerca de AuroraOS");
    break;
  case MENU_ACT_MINIMIZE:
    if (w) {
      do_minimize_window(w);
      taskbar_set_active_item(NULL);
    }
    break;
  case MENU_ACT_CLOSE:
    if (w)
      request_close(w);
    break;
  default:
    break;
  }
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

  win->taskbar_item =
      taskbar_add_item(TASKBAR_ITEM_APP_ICON, win->title, win->icon_symbol,
                       win->icon_bg_color, taskbar_item_on_click);
  if (win->taskbar_item && win->icon_bmp_node)
    win->taskbar_item->icon_bmp_node = win->icon_bmp_node;

  compositor_invalidate_rect(win_damage_rect(win));
  dock_invalidate_layout();
}

static void do_close_begin(window_t *win) {
  if (!win)
    return;
  window_start_close_animation(win);
  win->pending_destroy = 1;

  if (drag_window == win)
    drag_window = NULL;
  if (pressed_win == win) {
    pressed_win = NULL;
    pressed_btn = WIN_BTN_NONE;
  }
  if (resize_window == win)
    resize_window = NULL;

  compositor_invalidate_rect(
      (rect_t){win->x - SH, win->y - SH + win->anim_dy, win->width + SH * 2,
               win->height + SH * 2});
}

static void do_minimize_window(window_t *win) {
  if (!win)
    return;
  window_start_minimize_animation(win);
  win->pending_destroy = 0;

  if (win->flags & WIN_FLAGS_FOCUSED) {
    win->flags &= ~WIN_FLAGS_FOCUSED;
    win->flags |= WIN_FLAGS_INACTIVE;
    win->dirty = 1;
    winsrv_post_event(win, WINSRV_EV_BLUR, 0, 0, 0);
    dock_invalidate_layout();
  }

  if (drag_window == win)
    drag_window = NULL;
  if (pressed_win == win) {
    pressed_win = NULL;
    pressed_btn = WIN_BTN_NONE;
  }
  update_menubar_title();
  compositor_invalidate_rect(
      (rect_t){win->x - SH, win->y - SH + win->anim_dy, win->width + SH * 2,
               win->height + SH * 2});
}

static void do_focus_window(window_t *win) {
  if (!win)
    return;
  if (win->anim_state == WIN_ANIM_CLOSING)
    return;

  int need_restore = (win->flags & WIN_FLAGS_HIDDEN) ||
                     (win->anim_state == WIN_ANIM_MINIMIZING);
  if (need_restore) {
    win->flags &= ~WIN_FLAGS_HIDDEN;
    win->flags &= ~WIN_FLAGS_INACTIVE;
    if (win->anim_state == WIN_ANIM_MINIMIZING)
      win->anim_state = WIN_ANIM_NONE;
    if (win->anim_state != WIN_ANIM_OPENING)
      window_start_open_animation(win);
    win->dirty = 1;
    compositor_invalidate_rect(win_damage_rect(win));
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

  rect_t damage = win_damage_rect(win);
  if (old_focus)
    damage = rect_bounding_box(damage, win_damage_rect(old_focus));
  compositor_invalidate_rect(damage);

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
      compositor_invalidate_rect(win_damage_rect(c));
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

  if (drag_window == win)
    drag_window = NULL;
  if (pressed_win == win) {
    pressed_win = NULL;
    pressed_btn = WIN_BTN_NONE;
  }
  if (resize_window == win) {
    resize_window = NULL;
    resize_new_w = resize_new_h = 0;
  }

  if (win->prev)
    win->prev->next = win->next;
  else
    window_stack = win->next;
  if (win->next)
    win->next->prev = win->prev;

  compositor_invalidate_rect(win_damage_rect(win));
  window_destroy(win); /* también quita el item de la taskbar */
  dock_invalidate_layout();

  /* Como en macOS: al cerrar la ventana activa, pasa el foco a la siguiente
   * visible que no se esté cerrando. */
  if (was_focused) {
    for (window_t *c = window_stack; c; c = c->next) {
      if (!(c->flags & WIN_FLAGS_HIDDEN) && c->anim_state != WIN_ANIM_CLOSING &&
          c->anim_state != WIN_ANIM_MINIMIZING) {
        do_focus_window(c);
        break;
      }
    }
  }
  update_menubar_title();
}

static void do_toggle_maximize(window_t *win) {
  if (!win || win->anim_state != WIN_ANIM_NONE ||
      (win->flags & WIN_FLAGS_NODECORATION))
    return;

  rect_t old_r = win_damage_rect(win);
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
    nx = wa.x;
    ny = wa.y;
    nw = imin(wa.w, 2048);
    nh = imin(wa.h, 2048);
  }

  window_resize(win, nw, nh);
  if (win->width != nw || win->height != nh)
    return; /* falló la reserva: no cambiar nada más */

  win->x = nx;
  win->y = ny;
  win->maximized = !win->maximized;
  win->dirty = 1;
  compositor_invalidate_rect(rect_bounding_box(old_r, win_damage_rect(win)));
  winsrv_post_event(win, WINSRV_EV_RESIZE, win->content_w, win->content_h, 0);
  winsrv_post_event(win, WINSRV_EV_MOVE, nx, ny, 0);
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
void compositor_set_icon_bmp(window_t *win, tar_node_t *node) {
  comp_cmd_t c = {.type = CMD_ICON_BMP, .win = win, .node = node};
  cmd_post(&c);
}

/* =========================================================================
 * Cursor estilo macOS: flecha negra con borde blanco.
 * =======================================================================*/
#define CURSOR_W 12
#define CURSOR_H 19

static const uint8_t g_cursor_arrow[19][12] = {
    {2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, {2, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {2, 1, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0}, {2, 1, 1, 2, 0, 0, 0, 0, 0, 0, 0, 0},
    {2, 1, 1, 1, 2, 0, 0, 0, 0, 0, 0, 0}, {2, 1, 1, 1, 1, 2, 0, 0, 0, 0, 0, 0},
    {2, 1, 1, 1, 1, 1, 2, 0, 0, 0, 0, 0}, {2, 1, 1, 1, 1, 1, 1, 2, 0, 0, 0, 0},
    {2, 1, 1, 1, 1, 1, 1, 1, 2, 0, 0, 0}, {2, 1, 1, 1, 1, 1, 1, 1, 1, 2, 0, 0},
    {2, 1, 1, 1, 2, 2, 2, 2, 2, 2, 0, 0}, {2, 1, 1, 2, 1, 1, 1, 2, 0, 0, 0, 0},
    {2, 1, 2, 0, 2, 1, 1, 1, 2, 0, 0, 0}, {2, 2, 0, 0, 2, 1, 1, 1, 2, 0, 0, 0},
    {0, 0, 0, 0, 0, 2, 1, 1, 1, 2, 0, 0}, {0, 0, 0, 0, 0, 2, 1, 1, 1, 2, 0, 0},
    {0, 0, 0, 0, 0, 0, 2, 1, 1, 2, 0, 0}, {0, 0, 0, 0, 0, 0, 2, 1, 1, 2, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 2, 2, 0, 0, 0},
};

static void draw_cursor_overlay(uint32_t *dst, int stride, rect_t clip) {
  for (int y = 0; y < CURSOR_H; y++) {
    int py = cursor_y + y;
    if (py < clip.y || py >= clip.y + clip.h)
      continue;
    for (int x = 0; x < CURSOR_W; x++) {
      uint8_t v = g_cursor_arrow[y][x];
      if (v == 0)
        continue;
      int px = cursor_x + x;
      if (px < clip.x || px >= clip.x + clip.w)
        continue;
      /* Relleno negro, borde blanco (al revés que el cursor estilo Windows).
       * Se escribe directo en VRAM sin leerla. */
      dst[py * stride + px] = (v == 1) ? 0xFF111111 : 0xFFFFFFFF;
    }
  }
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

static void compositor_handle_mouse(int dx, int dy, uint8_t buttons) {
  int old_x = cursor_x, old_y = cursor_y;

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
        !menubar_is_open()) {
      if (window_in_controls(w, cursor_x, cursor_y)) {
        w->hover_controls = 1;
        win_hit_t h = window_hit_test(w, cursor_x, cursor_y);
        w->hover_btn = (h == WIN_HIT_MINIMIZE)   ? 1
                       : (h == WIN_HIT_MAXIMIZE) ? 2
                       : (h == WIN_HIT_CLOSE)    ? 3
                                                 : 0;
      }
    }
    if (w->hover_btn != old_btn || w->hover_controls != old_ctrl) {
      w->dirty = 1;
      compositor_invalidate_rect(win_damage_rect(w));
    }
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
        if (item->on_click)
          item->on_click(item);
      } else {
        int hit_any = 0;
        for (window_t *win = window_stack; win; win = win->next) {
          if ((win->flags & WIN_FLAGS_HIDDEN) || win->anim_state != WIN_ANIM_NONE)
            continue;
          win_hit_t h = window_hit_test(win, cursor_x, cursor_y);
          if (h == WIN_HIT_NONE)
            continue;
          hit_any = 1;

          switch (h) {
          case WIN_HIT_RESIZE_R:
          case WIN_HIT_RESIZE_B:
          case WIN_HIT_RESIZE_BR:
            resize_window = win;
            resize_edge = (h == WIN_HIT_RESIZE_R)   ? RESIZE_EDGE_RIGHT
                          : (h == WIN_HIT_RESIZE_B) ? RESIZE_EDGE_BOTTOM
                                                    : (RESIZE_EDGE_RIGHT |
                                                       RESIZE_EDGE_BOTTOM);
            resize_origin_x = cursor_x;
            resize_origin_y = cursor_y;
            resize_win_w = win->width;
            resize_win_h = win->height;
            do_focus_window(win);
            break;
          case WIN_HIT_CLOSE:
            pressed_win = win;
            pressed_btn = WIN_BTN_CLOSE_PRESSED;
            break;
          case WIN_HIT_MINIMIZE:
            pressed_win = win;
            pressed_btn = WIN_BTN_MINIMIZE_PRESSED;
            break;
          case WIN_HIT_MAXIMIZE:
            pressed_win = win;
            pressed_btn = WIN_BTN_MAXIMIZE_PRESSED;
            break;
          case WIN_HIT_TITLEBAR:
            do_focus_window(win);
            drag_window = win;
            drag_offset_x = cursor_x - win->x;
            drag_offset_y = cursor_y - win->y;
            break;
          case WIN_HIT_CLIENT:
            do_focus_window(win);
            post_client_mouse(win, MOUSE_BUTTON_LEFT);
            break;
          default:
            break;
          }
          break;
        }
        if (!hit_any)
          do_unfocus_all();
      }
    }
  }

  /* ---------- 2. Release ---------- */
  if (released) {
    /* Resize: se aplica una sola vez al soltar. */
    if (resize_window) {
      int final_w = resize_new_w, final_h = resize_new_h;
      if (final_w > 0 && final_h > 0 &&
          (final_w != resize_window->width || final_h != resize_window->height)) {
        rect_t old_r = win_damage_rect(resize_window);
        window_resize(resize_window, final_w, final_h);
        if (resize_window->width == final_w &&
            resize_window->height == final_h) {
          resize_window->maximized = 0;
          compositor_invalidate_rect(
              rect_bounding_box(old_r, win_damage_rect(resize_window)));
          winsrv_post_event(resize_window, WINSRV_EV_RESIZE,
                            resize_window->content_w, resize_window->content_h,
                            0);
        } else {
          compositor_invalidate_rect(old_r);
        }
      } else {
        compositor_invalidate_rect(
            (rect_t){resize_window->x - 2, resize_window->y - 2,
                     resize_new_w + 4, resize_new_h + 4});
      }
      resize_window = NULL;
      resize_edge = 0;
      resize_new_w = resize_new_h = 0;
    }

    /* Semáforos: la acción solo se ejecuta si se suelta sobre el mismo. */
    if (pressed_win && pressed_btn != WIN_BTN_NONE) {
      win_hit_t h = window_hit_test(pressed_win, cursor_x, cursor_y);
      if (pressed_btn == WIN_BTN_CLOSE_PRESSED && h == WIN_HIT_CLOSE) {
        request_close(pressed_win);
      } else if (pressed_btn == WIN_BTN_MINIMIZE_PRESSED &&
                 h == WIN_HIT_MINIMIZE) {
        do_minimize_window(pressed_win);
        taskbar_set_active_item(NULL);
        dock_invalidate_layout();
      } else if (pressed_btn == WIN_BTN_MAXIMIZE_PRESSED &&
                 h == WIN_HIT_MAXIMIZE) {
        do_toggle_maximize(pressed_win);
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

  /* ---------- 3. Resize en curso: solo outline hasta soltar ---------- */
  if (pressed && resize_window) {
    int new_w = resize_win_w, new_h = resize_win_h;
    if (resize_edge & RESIZE_EDGE_RIGHT)
      new_w += cursor_x - resize_origin_x;
    if (resize_edge & RESIZE_EDGE_BOTTOM)
      new_h += cursor_y - resize_origin_y;
    new_w = imax(200, imin(new_w, 2048));
    new_h = imax(150, imin(new_h, 2048));

    if (new_w != resize_new_w || new_h != resize_new_h) {
      int ow = imax(resize_new_w, resize_window->width);
      int oh = imax(resize_new_h, resize_window->height);
      compositor_invalidate_rect(
          (rect_t){resize_window->x - 2, resize_window->y - 2,
                   imax(ow, new_w) + 4, imax(oh, new_h) + 4});
      resize_new_w = new_w;
      resize_new_h = new_h;
    }
  }

  /* ---------- 4. Drag por la barra de título ---------- */
  if (pressed && drag_window) {
    int new_x = cursor_x - drag_offset_x;
    int new_y = cursor_y - drag_offset_y;
    new_y = imax(new_y, AURORA_MENUBAR_H);            /* no bajo la barra */
    new_y = imin(new_y, (int)fb_height - 40);         /* siempre agarrable */
    if (new_x != drag_window->x || new_y != drag_window->y) {
      rect_t old_r = win_damage_rect(drag_window);
      drag_window->x = new_x;
      drag_window->y = new_y;
      drag_window->maximized = 0;
      compositor_invalidate_rect(
          rect_bounding_box(old_r, win_damage_rect(drag_window)));
      winsrv_post_event(drag_window, WINSRV_EV_MOVE, new_x, new_y, 0);
    }
  }

  /* ---------- 5. Motion sobre cliente con botón pulsado ---------- */
  if (pressed && mouse_moved && !drag_window && !resize_window) {
    for (window_t *win = window_stack; win; win = win->next) {
      if (win->flags & WIN_FLAGS_HIDDEN)
        continue;
      if (window_hit_test(win, cursor_x, cursor_y) == WIN_HIT_CLIENT) {
        post_client_mouse(win, MOUSE_BUTTON_LEFT | MOUSE_BIT_MOTION);
        break;
      }
    }
  }

  last_mouse_buttons = buttons;

  /* ---------- 6. Refresco del cursor ---------- */
  if (mouse_moved && (old_x != cursor_x || old_y != cursor_y)) {
    compositor_invalidate_rect(
        (rect_t){old_x - 2, old_y - 2, CURSOR_W + 4, CURSOR_H + 4});
    compositor_invalidate_rect(
        (rect_t){cursor_x, cursor_y, CURSOR_W, CURSOR_H});
  }
}

/* Procesa hasta EVENT_BUDGET eventos por iteración y funde el movimiento
 * consecutivo del ratón (misma pulsación) en uno solo. Un cambio de botones
 * nunca se funde: se procesa por separado y en orden. */
#define EVENT_BUDGET 64

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
static void render_rect(rect_t clip) {
  uint64_t t0 = comp_rdtsc();

  /* El Dock y la barra de menús desenfocan lo que hay debajo: si el daño los
   * toca, se repinta entera su zona para que el desenfoque lea un fondo
   * recién compuesto (y no restos del frame anterior). */
  rect_t dd = dock_get_damage_rect();
  if (dd.w > 0 && rect_intersects(clip, dd))
    clip = rect_clip(rect_bounding_box(clip, dd), screen_rect());
  rect_t md = menubar_get_damage_rect();
  if (rect_intersects(clip, md))
    clip = rect_clip(rect_bounding_box(clip, md), screen_rect());
  if (clip.w <= 0 || clip.h <= 0)
    return;

  /* 1. Fondo */
  if (wallpaper_cache) {
    for (int y = clip.y; y < clip.y + clip.h; y++)
      simd_copy_normal(&backbuffer[y * fb_width + clip.x],
                       &wallpaper_cache[y * fb_width + clip.x], clip.w);
  } else {
    for (int y = clip.y; y < clip.y + clip.h; y++)
      simd_fill_row(&backbuffer[y * fb_width + clip.x], 0xFF1E1E2E, clip.w);
  }
  uint64_t t1 = comp_rdtsc();

  /* 2. Ventanas, de atrás hacia delante */
  window_t *curr = window_stack;
  while (curr && curr->next)
    curr = curr->next;
  while (curr) {
    if (!(curr->flags & WIN_FLAGS_HIDDEN)) {
      rect_t wb = {curr->x - SH, curr->y - SH + curr->anim_dy,
                   curr->width + SH * 2, curr->height + SH * 2};
      if (rect_intersects(clip, wb))
        window_render_frame(curr, backbuffer, fb_width, clip);
    }
    curr = curr->prev;
  }
  uint64_t t2 = comp_rdtsc();

  /* 3. Dock, barra de menús y outline de resize */
  dock_render(backbuffer, fb_width, clip);
  menubar_render(backbuffer, fb_width, clip);
  if (resize_window)
    draw_resize_outline(backbuffer, fb_width, clip);
  uint64_t t3 = comp_rdtsc();

  /* 4. Blit a VRAM (non-temporal) + cursor + flush */
  if (fb_ptr) {
    for (int y = clip.y; y < clip.y + clip.h; y++) {
      if (fb_bpp == 24) /* fb_ptr es un buffer en RAM; fb_flush_rect empaqueta */
        simd_copy_normal(&fb_ptr[y * fb_pitch + clip.x],
                         &backbuffer[y * fb_width + clip.x], clip.w);
      else
        simd_copy_to_vram(&fb_ptr[y * fb_pitch + clip.x],
                          &backbuffer[y * fb_width + clip.x], clip.w);
    }
    rect_t c_clip =
        rect_clip(clip, (rect_t){cursor_x, cursor_y, CURSOR_W, CURSOR_H});
    if (c_clip.w > 0 && c_clip.h > 0)
      draw_cursor_overlay(fb_ptr, fb_pitch, c_clip);
    fb_flush_rect(fb_ptr, fb_pitch, (uint32_t)clip.x, (uint32_t)clip.y,
                  (uint32_t)clip.w, (uint32_t)clip.h);
  }
  uint64_t t4 = comp_rdtsc();

  g_ph_cur[PH_BG] += t1 - t0;
  g_ph_cur[PH_WIN] += t2 - t1;
  g_ph_cur[PH_UI] += t3 - t2;
  g_ph_cur[PH_BLIT] += t4 - t3;
}

static void compositor_render(void) {
  rect_t list[DAMAGE_MAX];
  int n = damage_take(list);
  if (n == 0)
    return;

  uint64_t t0 = comp_rdtsc();
  for (int i = 0; i < n; i++) {
    rect_t r = rect_clip(list[i], screen_rect());
    if (r.w > 0 && r.h > 0)
      render_rect(r);
  }
  compositor_report_frame_stats(comp_rdtsc() - t0);

  /* Overlay de logs del kernel (solo dibuja si está activado). */
  extern void boot_console_draw_overlay(void);
  boot_console_draw_overlay();
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
      window_advance_animation(w, dt_ms);

      rect_t d1 = {w->x - SH, w->y - SH + w->anim_dy, w->width + SH * 2,
                   w->height + SH * 2};
      compositor_invalidate_rect(rect_bounding_box(d1, win_damage_rect(w)));

      if (w->anim_state == WIN_ANIM_NONE) {
        if (w->pending_destroy) {
          w->pending_destroy = 0;
          do_destroy(w);
        } else if (w->anim_alpha == 0 && !(w->flags & WIN_FLAGS_HIDDEN)) {
          w->flags |= WIN_FLAGS_HIDDEN;
          w->flags &= ~WIN_FLAGS_FOCUSED;
          w->flags |= WIN_FLAGS_INACTIVE;
          w->dirty = 1;
        }
      }
    }
    w = next;
  }
  return compositor_has_active_animations();
}

/* =========================================================================
 * Despertar y bucle principal
 * =======================================================================*/
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

  compositor_has_input = 0;
  compositor_needs_clock = 0;

  size_t size = (size_t)fb_width * fb_height * sizeof(uint32_t);
  backbuffer = (uint32_t *)kmalloc(size);
  if (!backbuffer) {
    LOG_ERR("[COMPOSITOR] ERROR CRITICO: No se pudo alojar el Backbuffer");
    return;
  }

  LOG_INFO("[COMPOSITOR] Motor grafico con aceleracion SIMD SSE2 inicializado.");
  taskbar_init();
  winsrv_init();
  dock_init((int)fb_width, (int)fb_height);
  menubar_init((int)fb_width, (int)fb_height);
  taskbar_refresh_clock();

  bg_wallpaper_node = tar_find_file("system/wallpapers/wallpaper-main.bmp");

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

    /* 5. Animaciones, Dock y barra de menús */
    int win_anim = compositor_tick_animations(dt);
    int dock_anim = dock_update(dt);

    rect_t r;
    if (dock_take_damage(&r))
      compositor_invalidate_rect(r);
    if (menubar_take_damage(&r))
      compositor_invalidate_rect(r);

    /* 6. Render */
    compositor_render();

    compositor_animating = (win_anim || dock_anim) ? 1 : 0;
  }
}
