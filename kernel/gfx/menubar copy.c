// kernel/gfx/menubar.c
#include "menubar.h"
#include "../heap.h"
#include "font_manager.h"
#include "taskbar.h"
#include "theme.h"
#include <stddef.h>

#define ORB_X 14
#define ORB_D 14
#define ORB_HIT_W 34 /* zona de clic del orbe, desde x=4 */
#define MENU_W 232
#define MENU_PAD 6
#define ITEM_H 24
#define SEP_H 9

typedef struct {
  const char *label; /* NULL = separador */
  menu_action_t act;
} menu_item_t;

static const menu_item_t k_menu[] = {
    {"Acerca de AuroraOS", MENU_ACT_ABOUT},
    {NULL, MENU_ACT_NONE},
    {"Minimizar ventana", MENU_ACT_MINIMIZE},
    {"Cerrar ventana", MENU_ACT_CLOSE},
};
#define MENU_N ((int)(sizeof(k_menu) / sizeof(k_menu[0])))

static struct {
  int sw, sh;
  int open;
  int hover; /* índice en k_menu o -1 */
  char app[64];
  int dirty;
  rect_t pending;
  uint32_t *scratch;
  size_t scratch_px;
} M;

static inline int imin(int a, int b) { return a < b ? a : b; }

static rect_t bar_rect(void) { return (rect_t){0, 0, M.sw, MENUBAR_H}; }

static int menu_height(void) {
  int h = MENU_PAD * 2;
  for (int i = 0; i < MENU_N; i++)
    h += k_menu[i].label ? ITEM_H : SEP_H;
  return h;
}

static rect_t menu_rect(void) {
  return (rect_t){6, MENUBAR_H + 2, MENU_W, menu_height()};
}

static void mark(rect_t r) {
  if (r.w <= 0 || r.h <= 0)
    return;
  M.pending = M.dirty ? rect_bounding_box(M.pending, r) : r;
  M.dirty = 1;
}

void menubar_init(int screen_w, int screen_h) {
  M.sw = screen_w;
  M.sh = screen_h;
  M.open = 0;
  M.hover = -1;
  M.app[0] = '\0';
  M.scratch_px = (size_t)screen_w * MENUBAR_H;
  M.scratch = (uint32_t *)kmalloc(M.scratch_px * sizeof(uint32_t));
  if (!M.scratch)
    M.scratch_px = 0;
  mark(bar_rect());
}

void menubar_set_app_name(const char *name) {
  char tmp[64];
  int i = 0;
  while (name && name[i] && i < 63) {
    tmp[i] = name[i];
    i++;
  }
  tmp[i] = '\0';
  int same = 1;
  for (int k = 0; k < 64; k++) {
    if (tmp[k] != M.app[k]) {
      same = 0;
      break;
    }
    if (!tmp[k])
      break;
  }
  if (same)
    return;
  for (int k = 0; k < 64; k++) {
    M.app[k] = tmp[k];
    if (!tmp[k])
      break;
  }
  mark(bar_rect());
}

void menubar_clock_changed(void) { mark(bar_rect()); }

rect_t menubar_get_damage_rect(void) {
  rect_t r = bar_rect();
  if (M.open)
    r = rect_bounding_box(r, menu_rect());
  return r;
}

int menubar_take_damage(rect_t *out) {
  if (!M.dirty)
    return 0;
  M.dirty = 0;
  *out = M.pending;
  return 1;
}

int menubar_is_open(void) { return M.open; }

void menubar_close(void) {
  if (!M.open)
    return;
  M.open = 0;
  M.hover = -1;
  mark(bar_rect());
  mark(menu_rect()); /* repintar donde estaba el menú */
}

static int item_at(int x, int y) {
  rect_t mr = menu_rect();
  if (x < mr.x || x >= mr.x + mr.w || y < mr.y + MENU_PAD ||
      y >= mr.y + mr.h - MENU_PAD)
    return -1;
  int cy = mr.y + MENU_PAD;
  for (int i = 0; i < MENU_N; i++) {
    int h = k_menu[i].label ? ITEM_H : SEP_H;
    if (y >= cy && y < cy + h)
      return k_menu[i].label ? i : -1;
    cy += h;
  }
  return -1;
}

int menubar_mouse_move(int x, int y) {
  if (M.open) {
    int h = item_at(x, y);
    if (h != M.hover) {
      M.hover = h;
      mark(menu_rect());
    }
  }
  rect_t mr = menu_rect();
  int in_menu = M.open && x >= mr.x && x < mr.x + mr.w && y >= mr.y &&
                y < mr.y + mr.h;
  return (y >= 0 && y < MENUBAR_H) || in_menu;
}

int menubar_mouse_press(int x, int y, menu_action_t *act) {
  if (act)
    *act = MENU_ACT_NONE;

  if (M.open) {
    int i = item_at(x, y);
    if (i >= 0 && act)
      *act = k_menu[i].act;
    menubar_close();
    return 1; /* un clic con el menú abierto nunca llega a las ventanas */
  }

  if (y >= 0 && y < MENUBAR_H) {
    if (x >= 4 && x < 4 + ORB_HIT_W) {
      M.open = 1;
      M.hover = -1;
      mark(bar_rect());
      mark(menu_rect());
    }
    return 1;
  }
  return 0;
}

static void join_datetime(char *out, size_t cap) {
  const char *d = taskbar_clock_date_str();
  const char *t = taskbar_clock_time_str();
  size_t n = 0;
  while (d && *d && n < cap - 1)
    out[n++] = *d++;
  if (n && t && *t) {
    if (n < cap - 1)
      out[n++] = ' ';
    if (n < cap - 1)
      out[n++] = ' ';
  }
  while (t && *t && n < cap - 1)
    out[n++] = *t++;
  out[n] = '\0';
}

void menubar_render(uint32_t *dst, int stride, rect_t clip) {
  rect_t bar = bar_rect();
  if (!rect_intersects(clip, menubar_get_damage_rect()))
    return;

  /* ---- Barra: fondo desenfocado + tinte + línea inferior ---- */
  if (M.scratch)
    gfx_backdrop_blur_rounded(dst, stride, clip, bar, 0, 10, M.scratch,
                              M.scratch_px);
  gfx_blend_rect(dst, stride, clip, bar, AURORA_MENUBAR_TINT);
  gfx_blend_rect(dst, stride, clip,
                 (rect_t){0, MENUBAR_H - 1, M.sw, 1}, AURORA_MENUBAR_LINE);

  /* ---- Orbe de Aurora ---- */
  int oy = (MENUBAR_H - ORB_D) / 2;
  if (M.open)
    gfx_fill_rounded_rect(dst, stride, clip,
                          (rect_t){ORB_X - 7, 3, ORB_D + 14, MENUBAR_H - 6}, 6,
                          0x30FFFFFF);
  gfx_fill_rounded_rect_gradient_v(dst, stride, clip,
                                   (rect_t){ORB_X, oy, ORB_D, ORB_D}, ORB_D / 2,
                                   AURORA_ACCENT_HOVER, AURORA_ACCENT);
  gfx_draw_rounded_border(dst, stride, clip, (rect_t){ORB_X, oy, ORB_D, ORB_D},
                          ORB_D / 2, 0x40FFFFFF);

  /* ---- Nombre de la app activa (semibold) ---- */
  const char *name = M.app[0] ? M.app : "Aurora";
  int tx = ORB_X + ORB_D + 14;
  int ty = gfx_text_center_y(FONT_ID_MAIN_BOLD, 0, MENUBAR_H - 1);
  gfx_draw_string(dst, stride, clip, tx, ty, name, AURORA_TEXT_PRIMARY,
                  FONT_ID_MAIN_BOLD);

  /* ---- Fecha y hora a la derecha ---- */
  char buf[48];
  join_datetime(buf, sizeof(buf));
  if (buf[0]) {
    int w = gfx_text_width(buf, FONT_ID_MAIN_REGULAR);
    gfx_draw_string(dst, stride, clip, M.sw - w - 14,
                    gfx_text_center_y(FONT_ID_MAIN_REGULAR, 0, MENUBAR_H - 1),
                    buf, AURORA_TEXT_SECONDARY, FONT_ID_MAIN_REGULAR);
  }

  /* ---- Menú desplegable ---- */
  if (!M.open)
    return;
  rect_t mr = menu_rect();
  if (M.scratch && (size_t)mr.w * mr.h <= M.scratch_px)
    gfx_backdrop_blur_rounded(dst, stride, clip, mr, 8, 10, M.scratch,
                              M.scratch_px);
  gfx_fill_rounded_rect(dst, stride, clip, mr, 8, AURORA_MENU_BG);
  gfx_draw_rounded_border(dst, stride, clip, mr, 8, 0x38FFFFFF);

  int cy = mr.y + MENU_PAD;
  for (int i = 0; i < MENU_N; i++) {
    if (!k_menu[i].label) {
      gfx_blend_rect(dst, stride, clip,
                     (rect_t){mr.x + 12, cy + SEP_H / 2, mr.w - 24, 1},
                     0x28FFFFFF);
      cy += SEP_H;
      continue;
    }
    rect_t ir = {mr.x + 5, cy, mr.w - 10, ITEM_H};
    if (i == M.hover)
      gfx_fill_rounded_rect(dst, stride, clip, ir, 5, AURORA_MENU_HILITE);
    gfx_draw_string(dst, stride, clip, mr.x + 14,
                    gfx_text_center_y(FONT_ID_MAIN_REGULAR, cy, ITEM_H),
                    k_menu[i].label, AURORA_TEXT_PRIMARY, FONT_ID_MAIN_REGULAR);
    cy += ITEM_H;
  }
}
