// kernel/gfx/menubar.c
#include "menubar.h"
#include "../heap.h"
#include "font_manager.h"
#include "taskbar.h"
#include "theme.h"
#include <stddef.h>

#define ORB_X 16
#define ORB_D 14
#define TITLE_GAP 22   /* separación entre títulos de menú */
#define PILL_PAD 9     /* relleno del resaltado alrededor del título */
#define MENU_MIN_W 210
#define MENU_PAD 7
#define ITEM_H 26
#define SEP_H 11
#define MAX_DYN 16

typedef struct {
  const char *label; /* NULL = separador */
  menu_action_t act;
  int needs_focus; /* deshabilitado si no hay ventana activa */
} item_t;

typedef struct {
  const char *title; /* NULL = orbe */
  const item_t *items;
  int n;
  int dynamic; /* añade la lista de ventanas */
} menu_t;

static const item_t k_aurora[] = {{"Acerca de AuroraOS", MENU_ACT_ABOUT, 0}};
static const item_t k_app[] = {{"Minimizar", MENU_ACT_MINIMIZE, 1},
                               {"Cerrar", MENU_ACT_CLOSE, 1}};
static const item_t k_file[] = {{"Cerrar ventana", MENU_ACT_CLOSE, 1}};
static const item_t k_window[] = {{"Minimizar", MENU_ACT_MINIMIZE, 1},
                                  {"Zoom", MENU_ACT_ZOOM, 1}};
static const item_t k_help[] = {{"Acerca de AuroraOS", MENU_ACT_ABOUT, 0}};

/* 0 orbe, 1 app (negrita, su título es dinámico), 2.. menús fijos */
static const menu_t k_menus[] = {
    {NULL, k_aurora, 1, 0},   {NULL, k_app, 2, 0},
    {"Archivo", k_file, 1, 0}, {"Ventana", k_window, 2, 1},
    {"Ayuda", k_help, 1, 0},
};
#define NMENUS ((int)(sizeof(k_menus) / sizeof(k_menus[0])))

static struct {
  int sw, sh;
  int open;  /* índice de menú abierto o -1 */
  int hover; /* fila resaltada o -1 */
  char app[64];
  int dirty;
  rect_t pending;
  uint32_t *scratch;
  size_t scratch_px;
  const menubar_provider_t *prov;

  /* instantánea de la lista de ventanas, tomada al abrir "Ventana" */
  int dyn_n;
  char dyn_title[MAX_DYN][40];
  int dyn_focus[MAX_DYN];
} M;

static inline int imax(int a, int b) { return a > b ? a : b; }

static rect_t bar_rect(void) { return (rect_t){0, 0, M.sw, MENUBAR_H}; }

static void mark(rect_t r) {
  if (r.w <= 0 || r.h <= 0)
    return;
  M.pending = M.dirty ? rect_bounding_box(M.pending, r) : r;
  M.dirty = 1;
}

/* ---------------- Disposición de los títulos ---------------- */
static const char *menu_title(int m) {
  if (m == 1)
    return M.app[0] ? M.app : "Aurora";
  return k_menus[m].title;
}

static font_id_t menu_font(int m) {
  return m == 1 ? FONT_ID_MAIN_BOLD : FONT_ID_MAIN_REGULAR;
}

/* x y ancho del texto de cada título. El orbe es el 0. */
static void layout(int *tx, int *tw) {
  tx[0] = ORB_X;
  tw[0] = ORB_D;
  int x = ORB_X + ORB_D + TITLE_GAP;
  for (int m = 1; m < NMENUS; m++) {
    tx[m] = x;
    tw[m] = gfx_text_width(menu_title(m), menu_font(m));
    x += tw[m] + TITLE_GAP;
  }
}

static int title_at(int x, int y) {
  if (y < 0 || y >= MENUBAR_H)
    return -1;
  int tx[NMENUS], tw[NMENUS];
  layout(tx, tw);
  for (int m = 0; m < NMENUS; m++)
    if (x >= tx[m] - PILL_PAD && x < tx[m] + tw[m] + PILL_PAD)
      return m;
  return -1;
}

/* ---------------- Menú desplegable ---------------- */
static int has_focus(void) {
  return M.prov && M.prov->has_focused && M.prov->has_focused();
}

static int menu_rows(int m) {
  int n = k_menus[m].n;
  if (k_menus[m].dynamic && M.dyn_n > 0)
    n += 1 + M.dyn_n; /* separador + ventanas */
  return n;
}

static int menu_height(int m) {
  int h = MENU_PAD * 2;
  for (int i = 0; i < k_menus[m].n; i++)
    h += k_menus[m].items[i].label ? ITEM_H : SEP_H;
  if (k_menus[m].dynamic && M.dyn_n > 0)
    h += SEP_H + M.dyn_n * ITEM_H;
  return h;
}

static int menu_width(int m) {
  int w = MENU_MIN_W;
  for (int i = 0; i < k_menus[m].n; i++)
    if (k_menus[m].items[i].label)
      w = imax(w, gfx_text_width(k_menus[m].items[i].label,
                                 FONT_ID_MAIN_REGULAR) + 44);
  if (k_menus[m].dynamic)
    for (int i = 0; i < M.dyn_n; i++)
      w = imax(w, gfx_text_width(M.dyn_title[i], FONT_ID_MAIN_REGULAR) + 52);
  return w;
}

static rect_t menu_rect(int m) {
  int tx[NMENUS], tw[NMENUS];
  layout(tx, tw);
  int x = tx[m] - PILL_PAD - 4;
  int w = menu_width(m);
  if (x + w > M.sw - 8)
    x = M.sw - w - 8;
  if (x < 6)
    x = 6;
  return (rect_t){x, MENUBAR_H + 2, w, menu_height(m)};
}

/* Fila (índice lógico) bajo y; -1 si es separador o fuera. Las filas fijas van
 * primero; después, separador + ventanas (índice = n + 1 + i). */
static int row_at(int m, int x, int y) {
  rect_t mr = menu_rect(m);
  if (x < mr.x || x >= mr.x + mr.w || y < mr.y + MENU_PAD ||
      y >= mr.y + mr.h - MENU_PAD)
    return -1;
  int cy = mr.y + MENU_PAD, row = 0;
  for (int i = 0; i < k_menus[m].n; i++, row++) {
    int h = k_menus[m].items[i].label ? ITEM_H : SEP_H;
    if (y >= cy && y < cy + h)
      return k_menus[m].items[i].label ? row : -1;
    cy += h;
  }
  if (k_menus[m].dynamic && M.dyn_n > 0) {
    cy += SEP_H;
    row++; /* el separador ocupa un índice lógico */
    for (int i = 0; i < M.dyn_n; i++, row++) {
      if (y >= cy && y < cy + ITEM_H)
        return row;
      cy += ITEM_H;
    }
  }
  return -1;
}

static void snapshot_windows(void) {
  M.dyn_n = 0;
  if (!M.prov || !M.prov->window_count)
    return;
  int n = M.prov->window_count();
  if (n > MAX_DYN)
    n = MAX_DYN;
  for (int i = 0; i < n; i++) {
    const char *t = M.prov->window_title(i);
    int k = 0;
    while (t && t[k] && k < 39) {
      M.dyn_title[i][k] = t[k];
      k++;
    }
    M.dyn_title[i][k] = '\0';
    M.dyn_focus[i] = M.prov->window_is_focused(i);
  }
  M.dyn_n = n;
}

static void open_menu(int m) {
  if (M.open >= 0)
    mark(menu_rect(M.open));
  M.open = m;
  M.hover = -1;
  if (k_menus[m].dynamic)
    snapshot_windows();
  else
    M.dyn_n = 0;
  mark(bar_rect());
  mark(menu_rect(m));
}

/* ---------------- API ---------------- */
void menubar_init(int screen_w, int screen_h) {
  M.sw = screen_w;
  M.sh = screen_h;
  M.open = -1;
  M.hover = -1;
  M.app[0] = '\0';
  size_t bar_px = (size_t)screen_w * MENUBAR_H;
  M.scratch_px = bar_px > (size_t)(440 * 560) ? bar_px : (size_t)(440 * 560);
  M.scratch = (uint32_t *)kmalloc(M.scratch_px * sizeof(uint32_t));
  if (!M.scratch)
    M.scratch_px = 0; /* sin desenfoque: solo tinte */
  mark(bar_rect());
}

void menubar_set_provider(const menubar_provider_t *p) { M.prov = p; }

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
  if (M.open >= 0)
    menubar_close(); /* el layout cambió bajo el menú abierto */
}

void menubar_clock_changed(void) { mark(bar_rect()); }

int menubar_glass_rects(glass_rect_t *out, int max) {
  if (M.open < 0 || max < 1)
    return 0;
  out[0] = (glass_rect_t){menu_rect(M.open), AURORA_GLASS_BLUR};
  return 1;
}

rect_t menubar_get_damage_rect(void) {
  rect_t r = bar_rect();
  if (M.open >= 0)
    r = rect_bounding_box(r, menu_rect(M.open));
  return r;
}

int menubar_take_damage(rect_t *out) {
  if (!M.dirty)
    return 0;
  M.dirty = 0;
  *out = M.pending;
  return 1;
}

int menubar_is_open(void) { return M.open >= 0; }

void menubar_close(void) {
  if (M.open < 0)
    return;
  mark(menu_rect(M.open)); /* repintar donde estaba el menú */
  M.open = -1;
  M.hover = -1;
  mark(bar_rect());
}

int menubar_mouse_move(int x, int y) {
  if (M.open >= 0) {
    int t = title_at(x, y);
    if (t >= 0 && t != M.open) { /* como macOS: cambia de menú al pasar */
      open_menu(t);
    } else {
      int h = row_at(M.open, x, y);
      if (h != M.hover) {
        M.hover = h;
        mark(menu_rect(M.open));
      }
    }
  }
  rect_t mr = M.open >= 0 ? menu_rect(M.open) : (rect_t){0, 0, 0, 0};
  int in_menu = M.open >= 0 && x >= mr.x && x < mr.x + mr.w && y >= mr.y &&
                y < mr.y + mr.h;
  return (y >= 0 && y < MENUBAR_H) || in_menu;
}

/* Acción de la fila lógica `row` del menú m, o NONE si está deshabilitada. */
static menu_action_t row_action(int m, int row) {
  int fixed = k_menus[m].n;
  if (row < fixed) {
    const item_t *it = &k_menus[m].items[row];
    if (it->needs_focus && !has_focus())
      return MENU_ACT_NONE;
    return it->act;
  }
  int di = row - fixed - 1; /* -1 por el separador */
  if (di >= 0 && di < M.dyn_n)
    return (menu_action_t)(MENU_ACT_FOCUS_BASE + di);
  return MENU_ACT_NONE;
}

int menubar_mouse_press(int x, int y, menu_action_t *act) {
  if (act)
    *act = MENU_ACT_NONE;

  if (M.open >= 0) {
    int m = M.open;
    int row = row_at(m, x, y);
    menu_action_t a = row >= 0 ? row_action(m, row) : MENU_ACT_NONE;
    int t = title_at(x, y);
    if (t == m) { /* clic en su propio título: cierra */
      menubar_close();
      return 1;
    }
    if (a == MENU_ACT_NONE && row >= 0)
      return 1; /* fila deshabilitada: el menú sigue abierto */
    if (act)
      *act = a;
    menubar_close();
    return 1; /* un clic con el menú abierto nunca llega a las ventanas */
  }

  if (y >= 0 && y < MENUBAR_H) {
    int t = title_at(x, y);
    if (t >= 0)
      open_menu(t);
    return 1;
  }
  return 0;
}

/* ---------------- Render ---------------- */
static void join_datetime(char *out, size_t cap) {
  /* Como macOS: "vie 24 oct  17:06". Si el RTC aún no dio fecha, solo la
   * hora. */
  const char *d = taskbar_clock_menubar_date_str();
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

/* Texto con sombra suave: lo que mantiene legible la barra transparente. */
static void shadowed_text(uint32_t *dst, int stride, rect_t clip, int x, int y,
                          const char *s, uint32_t color, font_id_t f) {
  gfx_draw_string(dst, stride, clip, x, y + 1, s, AURORA_MENUBAR_TEXT_SHADOW,
                  f);
  gfx_draw_string(dst, stride, clip, x, y, s, color, f);
}

void menubar_render(uint32_t *dst, int stride, rect_t clip) {
  rect_t bar = bar_rect();
  if (!rect_intersects(clip, menubar_get_damage_rect()))
    return;

  /* Velo muy tenue arriba (la barra es transparente). */
  gfx_gradient_rect_v(dst, stride, clip, bar, AURORA_MENUBAR_SCRIM, 0x00000000);

  int tx[NMENUS], tw[NMENUS];
  layout(tx, tw);

  /* Píldora del título abierto. */
  if (M.open >= 0) {
    int pw = tw[M.open] + PILL_PAD * 2;
    gfx_fill_rounded_rect(
        dst, stride, clip,
        (rect_t){tx[M.open] - PILL_PAD, 3, pw, MENUBAR_H - 6}, 7,
        AURORA_MENUBAR_PILL);
  }

  /* Orbe de Aurora. */
  int oy = (MENUBAR_H - ORB_D) / 2;
  rect_t orb = {ORB_X, oy, ORB_D, ORB_D};
  gfx_fill_rounded_rect(dst, stride, clip, orb, ORB_D / 2, AURORA_ACCENT);
  gfx_fill_rounded_rect(dst, stride, clip,
                        (rect_t){orb.x + 2, orb.y + 1, orb.w - 4, orb.h / 2},
                        (orb.h - 4) / 2, 0x70FFFFFF & 0x70FFFFFF);
  gfx_draw_rounded_border(dst, stride, clip, orb, ORB_D / 2, 0x40FFFFFF);

  /* Títulos. */
  for (int m = 1; m < NMENUS; m++) {
    shadowed_text(dst, stride, clip, tx[m],
                  gfx_text_center_y(menu_font(m), 0, MENUBAR_H), menu_title(m),
                  AURORA_TEXT_PRIMARY, menu_font(m));
  }

  /* Fecha y hora a la derecha. */
  char buf[48];
  join_datetime(buf, sizeof(buf));
  if (buf[0]) {
    int w = gfx_text_width(buf, FONT_ID_MAIN_REGULAR);
    shadowed_text(dst, stride, clip, M.sw - w - 16,
                  gfx_text_center_y(FONT_ID_MAIN_REGULAR, 0, MENUBAR_H), buf,
                  AURORA_TEXT_PRIMARY, FONT_ID_MAIN_REGULAR);
  }

  /* ---- Menú desplegable (cristal) ---- */
  if (M.open < 0)
    return;
  int m = M.open;
  rect_t mr = menu_rect(m);
  gfx_glass_panel(dst, stride, clip, mr, 14, AURORA_MENU_TINT,
                  AURORA_GLASS_BLUR,
                  AURORA_GLASS_SAT,
                  ((size_t)mr.w * mr.h <= M.scratch_px) ? M.scratch : NULL,
                  M.scratch_px);

  int cy = mr.y + MENU_PAD, row = 0;
  for (int i = 0; i < k_menus[m].n; i++, row++) {
    const item_t *it = &k_menus[m].items[i];
    if (!it->label) {
      gfx_blend_rect(dst, stride, clip,
                     (rect_t){mr.x + 14, cy + SEP_H / 2, mr.w - 28, 1},
                     0x26FFFFFF);
      cy += SEP_H;
      continue;
    }
    int enabled = !(it->needs_focus && !has_focus());
    rect_t ir = {mr.x + 6, cy, mr.w - 12, ITEM_H};
    if (row == M.hover && enabled)
      gfx_fill_rounded_rect(dst, stride, clip, ir, 8, AURORA_MENU_HILITE);
    gfx_draw_string(dst, stride, clip, mr.x + 18,
                    gfx_text_center_y(FONT_ID_MAIN_REGULAR, cy, ITEM_H),
                    it->label,
                    enabled ? AURORA_TEXT_PRIMARY : AURORA_TEXT_DISABLED,
                    FONT_ID_MAIN_REGULAR);
    cy += ITEM_H;
  }

  if (k_menus[m].dynamic && M.dyn_n > 0) {
    gfx_blend_rect(dst, stride, clip,
                   (rect_t){mr.x + 14, cy + SEP_H / 2, mr.w - 28, 1},
                   0x26FFFFFF);
    cy += SEP_H;
    row++;
    for (int i = 0; i < M.dyn_n; i++, row++) {
      rect_t ir = {mr.x + 6, cy, mr.w - 12, ITEM_H};
      if (row == M.hover)
        gfx_fill_rounded_rect(dst, stride, clip, ir, 8, AURORA_MENU_HILITE);
      if (M.dyn_focus[i]) { /* marca de verificación de la ventana activa */
        int kx = mr.x + 15, ky = cy + ITEM_H / 2;
        gfx_draw_line(dst, stride, clip, kx, ky, kx + 3, ky + 3, 0xFFFFFFFF);
        gfx_draw_line(dst, stride, clip, kx + 3, ky + 3, kx + 9, ky - 4,
                      0xFFFFFFFF);
      }
      gfx_draw_string(dst, stride, clip, mr.x + 32,
                      gfx_text_center_y(FONT_ID_MAIN_REGULAR, cy, ITEM_H),
                      M.dyn_title[i], AURORA_TEXT_PRIMARY,
                      FONT_ID_MAIN_REGULAR);
      cy += ITEM_H;
    }
  }
}
