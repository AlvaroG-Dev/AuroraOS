// kernel/gfx/dock.c
#include "dock.h"
#include "../bmp.h"
#include "../heap.h"
#include "font_manager.h"
#include "gfx_filter.h"
#include "theme.h"
#include <stddef.h>

typedef struct {
  taskbar_item_t *item;
  rect_t r; /* rectángulo del icono (con la magnificación aplicada) */
} dock_slot_t;

static struct {
  int sw, sh;
  int cur_x, cur_y;
  int mag;        /* 0..256: cuánto está activa la magnificación */
  int mag_target; /* 0 o 256 */
  int n;
  int base;  /* tamaño de reposo actual (se reduce si hay muchas apps) */
  int hover; /* índice del slot bajo el cursor, o -1 */
  rect_t panel;
  rect_t damage, prev_damage;
  uint64_t sig, prev_sig;
  int force;
  dock_slot_t slots[DOCK_MAX_ITEMS];
  uint32_t *scratch;
  size_t scratch_px;
} D;

// ---------------------------------------------------------------------------
static inline int imin(int a, int b) { return a < b ? a : b; }
static inline int imax(int a, int b) { return a > b ? a : b; }
static inline int iabs(int a) { return a < 0 ? -a : a; }

void dock_init(int screen_w, int screen_h) {
  D.sw = screen_w;
  D.sh = screen_h;
  D.cur_x = D.cur_y = -1;
  D.hover = -1;
  D.force = 1;
  D.scratch_px = (size_t)screen_w * DOCK_HEIGHT;
  D.scratch = (uint32_t *)kmalloc(D.scratch_px * sizeof(uint32_t));
  if (!D.scratch)
    D.scratch_px = 0; /* sin desenfoque: solo tinte */
}

void dock_set_cursor(int x, int y) {
  D.cur_x = x;
  D.cur_y = y;
}

void dock_invalidate_layout(void) { D.force = 1; }

int dock_reserved_height(void) { return DOCK_HEIGHT + DOCK_MARGIN_BOTTOM * 2; }

rect_t dock_get_panel_rect(void) { return D.panel; }
rect_t dock_get_damage_rect(void) { return D.damage; }

// ---------------------------------------------------------------------------
// Magnificación: parábola^2 sobre la distancia al centro "de reposo" del
// icono (no al centro ampliado), así no hay realimentación ni temblor.
// ---------------------------------------------------------------------------
static int bump256(int dist, int radius) {
  if (dist >= radius)
    return 0;
  int u = (dist * 256) / radius;    /* 0..256 */
  int t = (256 * 256 - u * u) >> 8; /* 256 -> 0 */
  return (t * t) >> 8;              /* afila la campana */
}

static uint64_t fnv(uint64_t h, uint32_t v) {
  h ^= v;
  return h * 1099511628211ULL;
}

int dock_update(int dt_ms) {
  /* Items de aplicación, en orden de creación. */
  taskbar_item_t *items[DOCK_MAX_ITEMS];
  int n = 0;
  for (taskbar_item_t *it = taskbar_items_head(); it && n < DOCK_MAX_ITEMS;
       it = it->next)
    if (it->type == TASKBAR_ITEM_APP_ICON)
      items[n++] = it;

  int avail = D.sw - 2 * DOCK_PAD_X - 48;
  int base = DOCK_ICON_BASE;
  if (n > 0 && n * base + (n - 1) * DOCK_GAP > avail)
    base = imax(24, (avail - (n - 1) * DOCK_GAP) / n);
  int maxs = base + (DOCK_ICON_MAX - DOCK_ICON_BASE) * base / DOCK_ICON_BASE;

  /* Geometría de reposo (sin magnificación). */
  int rest_w = n > 0 ? n * base + (n - 1) * DOCK_GAP : 0;
  int rest_x0 = (D.sw - rest_w) / 2;
  int rest_panel_y = D.sh - DOCK_MARGIN_BOTTOM - DOCK_HEIGHT;

  /* Zona activa: panel de reposo con margen. Usa el panel de reposo (no el
   * ampliado) para que el borde de la zona no oscile al crecer el Dock. */
  int in_zone = n > 0 && D.cur_x >= rest_x0 - DOCK_PAD_X - 30 &&
                D.cur_x < rest_x0 + rest_w + DOCK_PAD_X + 30 &&
                D.cur_y >= rest_panel_y - 12 && D.cur_y < D.sh;
  D.mag_target = in_zone ? 256 : 0;

  int step = imax(8, dt_ms * 2); /* ~140 ms de 0 a 256 */
  if (D.mag < D.mag_target)
    D.mag = imin(D.mag + step, D.mag_target);
  else if (D.mag > D.mag_target)
    D.mag = imax(D.mag - step, D.mag_target);

  /* Tamaños. */
  int sizes[DOCK_MAX_ITEMS];
  int total = 0;
  int radius = (base + DOCK_GAP) * 22 / 10; /* ~2.2 iconos a cada lado */
  for (int i = 0; i < n; i++) {
    int cx0 = rest_x0 + i * (base + DOCK_GAP) + base / 2;
    int b = bump256(iabs(D.cur_x - cx0), radius);
    sizes[i] = base + ((maxs - base) * b >> 8) * D.mag / 256;
    total += sizes[i];
  }
  if (n > 1)
    total += (n - 1) * DOCK_GAP;

  /* Distribución centrada; los iconos crecen hacia arriba. */
  D.n = n;
  D.base = base;
  int x = (D.sw - total) / 2;
  D.panel = (rect_t){x - DOCK_PAD_X, rest_panel_y, total + 2 * DOCK_PAD_X,
                     DOCK_HEIGHT};
  if (n == 0)
    D.panel = (rect_t){0, 0, 0, 0};
  int icon_bottom = rest_panel_y + DOCK_PAD_TOP + DOCK_ICON_BASE;
  D.hover = -1;
  for (int i = 0; i < n; i++) {
    D.slots[i].item = items[i];
    D.slots[i].r = (rect_t){x, icon_bottom - sizes[i], sizes[i], sizes[i]};
    if (D.cur_x >= x - DOCK_GAP / 2 && D.cur_x < x + sizes[i] + DOCK_GAP / 2 &&
        D.cur_y >= D.slots[i].r.y && in_zone)
      D.hover = i;
    x += sizes[i] + DOCK_GAP;
  }

  /* Rectángulo de daño máximo. */
  if (n == 0) {
    D.damage = (rect_t){0, 0, 0, 0};
  } else {
    int pad = 64; /* margen lateral para tooltips largos */
    int dx = imax(0, D.panel.x - pad);
    int dr = imin(D.sw, D.panel.x + D.panel.w + pad);
    int dy = imax(0, D.panel.y - DOCK_HEADROOM);
    D.damage = (rect_t){dx, dy, dr - dx, D.sh - dy};
  }

  /* Firma para detectar cambios. */
  uint64_t h = 1469598103934665603ULL;
  h = fnv(h, (uint32_t)n);
  h = fnv(h, (uint32_t)(D.hover + 1));
  h = fnv(h, (uint32_t)D.panel.x);
  h = fnv(h, (uint32_t)D.panel.w);
  for (int i = 0; i < n; i++) {
    h = fnv(h, (uint32_t)(uintptr_t)D.slots[i].item);
    h = fnv(h, (uint32_t)D.slots[i].r.x);
    h = fnv(h, (uint32_t)D.slots[i].r.y);
    h = fnv(h, (uint32_t)D.slots[i].r.w);
    h = fnv(h, (uint32_t)D.slots[i].item->is_active);
  }
  D.sig = h;

  return D.mag != D.mag_target;
}

int dock_take_damage(rect_t *out) {
  if (!D.force && D.sig == D.prev_sig)
    return 0;
  D.force = 0;
  D.prev_sig = D.sig;
  rect_t r = D.damage;
  if (D.prev_damage.w > 0 && D.prev_damage.h > 0)
    r = (r.w > 0 && r.h > 0) ? rect_bounding_box(r, D.prev_damage)
                             : D.prev_damage;
  D.prev_damage = D.damage;
  if (r.w <= 0 || r.h <= 0)
    return 0;
  *out = r;
  return 1;
}

taskbar_item_t *dock_hit_test(int x, int y) {
  if (D.n == 0)
    return NULL;
  for (int i = 0; i < D.n; i++) {
    rect_t r = D.slots[i].r;
    int bottom = D.panel.y + D.panel.h;
    if (x >= r.x - DOCK_GAP / 2 && x < r.x + r.w + DOCK_GAP / 2 && y >= r.y &&
        y < bottom)
      return D.slots[i].item;
  }
  return NULL;
}

// ---------------------------------------------------------------------------
// Icono de la caché: BMP escalado, o baldosa con degradado + símbolo.
// ---------------------------------------------------------------------------
static uint32_t lighten(uint32_t c, int amt) {
  int r = imin(255, (int)((c >> 16) & 0xFF) + amt);
  int g = imin(255, (int)((c >> 8) & 0xFF) + amt);
  int b = imin(255, (int)(c & 0xFF) + amt);
  return 0xFF000000 | (uint32_t)(r << 16) | (uint32_t)(g << 8) | (uint32_t)b;
}

static void make_fallback_tile(taskbar_item_t *it) {
  const int S = DOCK_ICON_SRC;
  rect_t clip = {0, 0, S, S};
  uint32_t base =
      it->icon_color ? (it->icon_color | 0xFF000000) : AURORA_ACCENT;
  if (base == 0xFF0078D4 || base == 0xFF60CDFF)
    base = AURORA_ACCENT; /* colores heredados de Windows -> acento Aurora */

  gfx_fill_rounded_rect_gradient_v(it->dock_icon, S, clip,
                                   (rect_t){2, 2, S - 4, S - 4}, 16,
                                   lighten(base, 40), base);
  gfx_draw_rounded_border(it->dock_icon, S, clip, (rect_t){2, 2, S - 4, S - 4},
                          16, 0x30FFFFFF);

  /* Símbolo: se dibuja a 24x24 y se amplía con bilineal para que quede liso. */
  uint32_t *tmp = (uint32_t *)kmalloc(24 * 24 * sizeof(uint32_t));
  if (!tmp)
    return;
  for (int i = 0; i < 24 * 24; i++)
    tmp[i] = 0;
  char sym[8];
  int k = 0;
  while (it->icon_symbol[k] && k < 7) {
    sym[k] = it->icon_symbol[k];
    k++;
  }
  sym[k] = '\0';
  if (!sym[0] && it->label[0]) {
    sym[0] = it->label[0];
    sym[1] = '\0';
  }
  rect_t tclip = {0, 0, 24, 24};
  int tw = gfx_text_width(sym, FONT_ID_MAIN_BOLD);
  gfx_draw_string(tmp, 24, tclip, (24 - tw) / 2,
                  gfx_text_center_y(FONT_ID_MAIN_BOLD, 0, 24), sym, 0xFFFFFFFF,
                  FONT_ID_MAIN_BOLD);
  gfx_blit_scaled(it->dock_icon, S, clip, (S - 48) / 2, (S - 48) / 2, 48, 48,
                  tmp, 24, 24, GFX_FILTER_BILINEAR);
  kfree(tmp);
}

static int ensure_icon(taskbar_item_t *it) {
  if (it->dock_icon && it->dock_icon_state == 1)
    return 1;
  const int S = DOCK_ICON_SRC;
  if (!it->dock_icon)
    it->dock_icon = (uint32_t *)kmalloc((size_t)S * S * sizeof(uint32_t));
  if (!it->dock_icon)
    return 0;
  for (int i = 0; i < S * S; i++)
    it->dock_icon[i] = 0;

  int ok = 0;
  if (it->icon_bmp_node)
    ok = (bmp_draw_icon_scaled(it->icon_bmp_node, it->dock_icon, S, S) == 0);
  if (!ok) {
    for (int i = 0; i < S * S; i++)
      it->dock_icon[i] = 0;
    make_fallback_tile(it);
  }
  it->dock_icon_state = 1;
  return 1;
}

// ---------------------------------------------------------------------------
// Render
// ---------------------------------------------------------------------------
void dock_render(uint32_t *dst, int stride, rect_t clip) {
  if (D.n == 0 || !rect_intersects(clip, D.damage))
    return;

  rect_t p = D.panel;

  /* Cristal esmerilado: desenfoque del fondo ya compuesto + tinte + borde. */
  if (D.scratch)
    gfx_backdrop_blur_rounded(dst, stride, clip, p, DOCK_RADIUS, 8, D.scratch,
                              D.scratch_px);
  gfx_fill_rounded_rect(dst, stride, clip, p, DOCK_RADIUS, AURORA_DOCK_TINT);
  gfx_draw_rounded_border(dst, stride, clip, p, DOCK_RADIUS,
                          AURORA_DOCK_BORDER);
  gfx_blend_rect(dst, stride, clip,
                 (rect_t){p.x + DOCK_RADIUS, p.y, p.w - 2 * DOCK_RADIUS, 1},
                 AURORA_DOCK_TOPLIGHT);

  for (int i = 0; i < D.n; i++) {
    taskbar_item_t *it = D.slots[i].item;
    rect_t r = D.slots[i].r;
    if (ensure_icon(it))
      gfx_blit_scaled(dst, stride, clip, r.x, r.y, r.w, r.h, it->dock_icon,
                      DOCK_ICON_SRC, DOCK_ICON_SRC, GFX_FILTER_BILINEAR);

    /* Indicador de app abierta (más brillante si es la activa). */
    int dot_y = p.y + p.h - 9;
    gfx_fill_rounded_rect(dst, stride, clip,
                          (rect_t){r.x + r.w / 2 - 2, dot_y, 4, 4}, 2,
                          it->is_active ? 0xFFFFFFFF : 0x99FFFFFF);
  }

  /* Tooltip sobre el icono bajo el cursor. */
  if (D.hover >= 0 && D.mag > 128) {
    taskbar_item_t *it = D.slots[D.hover].item;
    rect_t ir = D.slots[D.hover].r;
    const char *label = it->label;
    int tw = gfx_text_width(label, FONT_ID_MAIN_REGULAR);
    int w = tw + 22, h = 26;
    int x = ir.x + ir.w / 2 - w / 2;
    x = imax(8, imin(x, D.sw - w - 8));
    int y = ir.y - 10 - h;
    rect_t tr = {x, y, w, h};
    gfx_fill_rounded_rect(dst, stride, clip, tr, 8, 0xE6262628);
    gfx_draw_rounded_border(dst, stride, clip, tr, 8, 0x30FFFFFF);
    gfx_draw_string(dst, stride, clip, x + 11,
                    gfx_text_center_y(FONT_ID_MAIN_REGULAR, y, h), label,
                    AURORA_TEXT_PRIMARY, FONT_ID_MAIN_REGULAR);
  }
}
