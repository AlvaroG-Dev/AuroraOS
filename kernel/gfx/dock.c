// kernel/gfx/dock.c
#include "dock.h"
#include "brand.h"
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

#define DOCK_MAX_BOUNCE 4
#define BOUNCE_MS 420    /* duración de cada salto */
#define BOUNCE_COUNT 2   /* saltos por lanzamiento */
#define BOUNCE_HEIGHT 18 /* altura del primer salto, px */
#define MAG_RADIUS_ICONS 26 /* alcance de la lupa: 2,6 iconos por lado (x10) */

static struct {
  int sw, sh;
  int cur_x, cur_y;
  int mag;        /* 0..256: cuánto está activa la magnificación */
  int mag_target; /* 0 o 256 */
  int n;
  int base;       /* tamaño de reposo actual (se reduce si hay muchas apps) */
  int hover;      /* índice del slot bajo el cursor, o -1 */
  rect_t panel;
  rect_t tip; /* tooltip del icono bajo el cursor; w = 0 si no hay */
  int sep_x;  /* x del separador fijados | abiertas, o -1 */
  rect_t damage, prev_damage;
  uint64_t sig, prev_sig;
  int force;
  taskbar_item_t *pressed;
  struct {
    taskbar_item_t *item;
    int t_ms; /* tiempo transcurrido del rebote */
  } bounce[DOCK_MAX_BOUNCE];
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
  D.sep_x = -1;
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

/* Cambio de modo claro/oscuro: los iconos con baldosa de tema se rehacen. */
void dock_theme_changed(void) {
  for (taskbar_item_t *it = taskbar_items_head(); it; it = it->next)
    it->dock_icon_state = 0;
  D.force = 1;
}

int dock_reserved_height(void) { return DOCK_HEIGHT + DOCK_MARGIN_BOTTOM * 2; }

rect_t dock_get_panel_rect(void) { return D.panel; }
rect_t dock_get_damage_rect(void) { return D.damage; }

#define TIP_BLUR 6
#define TIP_H 28
#define DOCK_GLASS_SAT 215 /* menos saturado que el cristal de los menus: el Dock
                              se parece mas a un panel esmerilado neutro */

int dock_glass_rects(glass_rect_t *out, int max) {
  int n = 0;
  if (D.n > 0 && n < max)
    out[n++] = (glass_rect_t){D.panel, AURORA_GLASS_BLUR};
  if (D.tip.w > 0 && n < max)
    out[n++] = (glass_rect_t){D.tip, TIP_BLUR};
  return n;
}

void dock_set_pressed(taskbar_item_t *item) {
  if (D.pressed != item) {
    D.pressed = item;
    D.force = 1;
  }
}

void dock_bounce(taskbar_item_t *item) {
  if (!item)
    return;
  int slot = 0;
  for (int i = 0; i < DOCK_MAX_BOUNCE; i++) {
    if (D.bounce[i].item == item || !D.bounce[i].item) {
      slot = i;
      break;
    }
    if (D.bounce[i].t_ms > D.bounce[slot].t_ms)
      slot = i; /* sin hueco: reemplaza el más avanzado */
  }
  D.bounce[slot].item = item;
  D.bounce[slot].t_ms = 0;
  D.force = 1;
}

void dock_forget(taskbar_item_t *item) {
  if (D.pressed == item)
    D.pressed = NULL;
  for (int i = 0; i < DOCK_MAX_BOUNCE; i++)
    if (D.bounce[i].item == item)
      D.bounce[i].item = NULL;
  for (int i = 0; i < D.n; i++)
    if (D.slots[i].item == item)
      D.force = 1;
}

static int is_bouncing(const taskbar_item_t *item) {
  for (int i = 0; i < DOCK_MAX_BOUNCE; i++)
    if (D.bounce[i].item == item)
      return 1;
  return 0;
}

/* Altura del rebote de `item` ahora (0 si no rebota). Cada salto es una
 * parábola; el segundo, más bajo, como en macOS. */
static int bounce_offset(const taskbar_item_t *item) {
  for (int i = 0; i < DOCK_MAX_BOUNCE; i++) {
    if (D.bounce[i].item != item)
      continue;
    int t = D.bounce[i].t_ms;
    int k = t / BOUNCE_MS;
    if (k >= BOUNCE_COUNT)
      return 0;
    int u = ((t % BOUNCE_MS) * 256) / BOUNCE_MS; /* 0..256 */
    int h = BOUNCE_HEIGHT * (k == 0 ? 256 : 150) / 256;
    return h * 4 * u * (256 - u) / (256 * 256);
  }
  return 0;
}

static int advance_bounces(int dt_ms) {
  int active = 0;
  for (int i = 0; i < DOCK_MAX_BOUNCE; i++) {
    if (!D.bounce[i].item)
      continue;
    D.bounce[i].t_ms += dt_ms;
    if (D.bounce[i].t_ms >= BOUNCE_MS * BOUNCE_COUNT)
      D.bounce[i].item = NULL;
    else
      active = 1;
  }
  return active;
}

int dock_item_rect(const taskbar_item_t *item, rect_t *out) {
  for (int i = 0; i < D.n; i++) {
    if (D.slots[i].item == item) {
      *out = D.slots[i].r;
      return 1;
    }
  }
  return 0;
}

// ---------------------------------------------------------------------------
// Magnificación: campana suave (smoothstep) sobre la distancia al centro "de
// reposo" del icono (no al centro ampliado), así no hay realimentación ni
// temblor. Más ancha y de cima más plana que la parábola anterior: los
// vecinos crecen un poco y el icono bajo el cursor no destaca tanto.
// ---------------------------------------------------------------------------
static int smooth256(int t) { /* t 0..256 -> smoothstep 0..256 */
  if (t <= 0)
    return 0;
  if (t >= 256)
    return 256;
  return (t * t * (3 * 256 - 2 * t)) >> 16;
}

static int bump256(int dist, int radius) {
  if (dist >= radius)
    return 0;
  return smooth256(256 - (dist * 256) / radius);
}

static uint64_t fnv(uint64_t h, uint32_t v) {
  h ^= v;
  return h * 1099511628211ULL;
}

int dock_update(int dt_ms) {
  /* Items de aplicación, en orden de creación. */
  taskbar_item_t *items[DOCK_MAX_ITEMS];
  int n = 0, npin = 0;
  for (int pass = 0; pass < 2; pass++) {
    /* Primero los lanzadores fijados, luego las ventanas sueltas. */
    for (taskbar_item_t *it = taskbar_items_head(); it && n < DOCK_MAX_ITEMS;
         it = it->next)
      if (it->type == TASKBAR_ITEM_APP_ICON && !!it->pinned == (pass == 0))
        items[n++] = it;
    if (pass == 0)
      npin = n;
  }
  int sep = (npin > 0 && n > npin) ? DOCK_SEP_GAP : 0;
  int launching = 0;
  for (int i = 0; i < n; i++) {
    taskbar_item_t *it = items[i];
    if (it->launching > 0) {
      it->launching -= dt_ms > 0 ? dt_ms : 1;
      if (it->launching < 0)
        it->launching = 0;
      else {
        launching = 1;
        if (!is_bouncing(it))
          dock_bounce(it); /* sigue saltando hasta que aparezca la ventana */
      }
    }
  }

  int avail = D.sw - 2 * DOCK_PAD_X - 48;
  int base = DOCK_ICON_BASE;
  if (n > 0 && n * base + (n - 1) * DOCK_GAP > avail)
    base = imax(24, (avail - (n - 1) * DOCK_GAP) / n);
  int maxs = base + (DOCK_ICON_MAX - DOCK_ICON_BASE) * base / DOCK_ICON_BASE;

  /* Geometría de reposo (sin magnificación). */
  int rest_w = n > 0 ? n * base + (n - 1) * DOCK_GAP + sep : 0;
  int rest_x0 = (D.sw - rest_w) / 2;
  int rest_panel_y = D.sh - DOCK_MARGIN_BOTTOM - DOCK_HEIGHT;

  /* Zona activa: panel de reposo con margen. Usa el panel de reposo (no el
   * ampliado) para que el borde de la zona no oscile al crecer el Dock. */
  int in_zone = n > 0 && D.cur_x >= rest_x0 - DOCK_PAD_X - 30 &&
                D.cur_x < rest_x0 + rest_w + DOCK_PAD_X + 30 &&
                D.cur_y >= rest_panel_y - 12 && D.cur_y < D.sh;
  D.mag_target = in_zone ? 256 : 0;

  int step = imax(6, dt_ms * 256 / 180); /* ~180 ms de 0 a 256 */
  if (D.mag < D.mag_target)
    D.mag = imin(D.mag + step, D.mag_target);
  else if (D.mag > D.mag_target)
    D.mag = imax(D.mag - step, D.mag_target);
  int mag = smooth256(D.mag); /* entrada y salida suaves de la lupa */
  int bouncing = advance_bounces(dt_ms);

  /* Tamaños. */
  int sizes[DOCK_MAX_ITEMS];
  int total = 0;
  int radius = (base + DOCK_GAP) * MAG_RADIUS_ICONS / 10;
  for (int i = 0; i < n; i++) {
    int cx0 = rest_x0 + i * (base + DOCK_GAP) + base / 2 + (i >= npin ? sep : 0);
    int b = bump256(iabs(D.cur_x - cx0), radius);
    sizes[i] = base + ((maxs - base) * b >> 8) * mag / 256;
    total += sizes[i];
  }
  if (n > 1)
    total += (n - 1) * DOCK_GAP + sep;

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
    D.slots[i].r = (rect_t){x, icon_bottom - sizes[i] - bounce_offset(items[i]),
                            sizes[i], sizes[i]};
    if (D.cur_x >= x - DOCK_GAP / 2 && D.cur_x < x + sizes[i] + DOCK_GAP / 2 &&
        D.cur_y >= D.slots[i].r.y && in_zone)
      D.hover = i;
    x += sizes[i] + DOCK_GAP;
    if (sep && i == npin - 1)
      x += sep;
  }
  D.sep_x = -1;
  if (sep)
    D.sep_x = D.slots[npin - 1].r.x + D.slots[npin - 1].r.w + (DOCK_GAP + sep) / 2;

  /* Tooltip sobre el icono bajo el cursor. */
  D.tip = (rect_t){0, 0, 0, 0};
  if (D.hover >= 0 && D.mag > 128) {
    rect_t ir = D.slots[D.hover].r;
    int w = gfx_text_width(D.slots[D.hover].item->label, FONT_ID_MAIN_REGULAR) +
            22;
    int tx = imax(8, imin(ir.x + ir.w / 2 - w / 2, D.sw - w - 8));
    D.tip = (rect_t){tx, ir.y - 10 - TIP_H, w, TIP_H};
  }

  /* Rectángulo de daño: lo que el Dock ocupa de verdad ahora (panel, iconos
   * ampliados o rebotando y tooltip). Antes era una caja fija enorme alrededor
   * del Dock, y cada movimiento del ratón sobre él repintaba y desenfocaba
   * toda esa zona. */
  if (n == 0) {
    D.damage = (rect_t){0, 0, 0, 0};
  } else {
    rect_t u = D.panel;
    for (int i = 0; i < n; i++)
      u = rect_bounding_box(u, rect_grow(D.slots[i].r, 2));
    if (D.tip.w > 0)
      u = rect_bounding_box(u, rect_grow(D.tip, 2));
    D.damage = rect_clip(u, (rect_t){0, 0, D.sw, D.sh});
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
    h = fnv(h, (uint32_t)D.slots[i].item->win_count);
    h = fnv(h, (uint32_t)(D.slots[i].item->launching > 0));
  }
  h = fnv(h, (uint32_t)(uintptr_t)D.pressed);
  D.sig = h;

  return D.mag != D.mag_target || bouncing || launching;
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

/* Baldosa por defecto (apps sin icono propio): cuadrado redondeado de color
 * liso con un degradado vertical muy suave (más claro arriba, más oscuro
 * abajo) y la inicial o el símbolo en blanco. */
static void make_fallback_tile(taskbar_item_t *it) {
  const int S = DOCK_ICON_SRC;
  rect_t clip = {0, 0, S, S};
  uint32_t base = it->icon_color ? (it->icon_color | 0xFF000000) : AURORA_ACCENT;
  if (base == 0xFF0078D4 || base == 0xFF60CDFF)
    base = AURORA_ACCENT; /* colores heredados de Windows -> acento de sistema */
  /* Símbolo reservado "@": icono de Aurora (emblema sobre baldosa oscura). */
  int is_brand = it->icon_symbol[0] == '@' && it->icon_symbol[1] == '\0';
  if (is_brand)
    base = 0xFF141F22;

  const int inset = S / 24, radius = S * 9 / 40;
  rect_t tile = {inset, inset, S - 2 * inset, S - 2 * inset};
  gfx_fill_rounded_rect(it->dock_icon, S, clip, tile, radius, base);
  /* Degradado vertical sobre los píxeles de la baldosa (respeta el borde AA). */
  for (int y = 0; y < S; y++) {
    int t = y * 256 / S; /* 0 arriba .. 256 abajo */
    uint32_t over = t < 128 ? (((uint32_t)(0x2C * (128 - t) / 128) << 24) | 0x00FFFFFF)
                            : ((uint32_t)(0x2C * (t - 128) / 128) << 24);
    for (int x = 0; x < S; x++) {
      uint32_t px = it->dock_icon[y * S + x];
      uint32_t a = px >> 24;
      if (!a)
        continue;
      uint32_t nb = blend_pixel_fast(over, px | 0xFF000000);
      it->dock_icon[y * S + x] = (nb & 0x00FFFFFF) | (a << 24);
    }
  }
  gfx_draw_rounded_border(it->dock_icon, S, clip, tile, radius, 0x24FFFFFF);

  if (is_brand) {
    brand_draw_emblem(it->dock_icon, S, clip, S / 2, S / 2, S * 46 / 100);
    return;
  }

  /* Símbolo: se dibuja a 24x24 y se amplía con bilineal para que quede liso. */
  static uint32_t tmp[24 * 24]; /* solo hilo del compositor */
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
  int g = S * 2 / 3;
  gfx_blit_scaled(it->dock_icon, S, clip, (S - g) / 2, (S - g) / 2, g, g, tmp,
                  24, 24, GFX_FILTER_BILINEAR);
}

/* Convierte el icono (alfa recto) en el que se dibuja: con una sombra suave
 * debajo, ya incorporada, y con el alfa PREMULTIPLICADO. Se hace una sola vez
 * al crear el icono; así cada redibujado del Dock es un único blit sin
 * divisiones por icono. La sombra es la silueta desplazada 2 px hacia abajo y
 * difuminada. */
static void bake_shadow_and_premultiply(uint32_t *icon) {
  const int S = DOCK_ICON_SRC;
  static uint8_t sh[DOCK_ICON_SRC * DOCK_ICON_SRC];
  static uint8_t tmp[DOCK_ICON_SRC * DOCK_ICON_SRC];

  for (int y = 0; y < S; y++)
    for (int x = 0; x < S; x++)
      sh[y * S + x] = y >= 2 ? (uint8_t)((icon[(y - 2) * S + x] >> 24) * 0x46 / 255)
                             : 0;
  for (int pass = 0; pass < 2; pass++) { /* box 3x3 separable, dos veces */
    for (int y = 0; y < S; y++)
      for (int x = 0; x < S; x++) {
        int s = sh[y * S + x] + (x > 0 ? sh[y * S + x - 1] : 0) +
                (x < S - 1 ? sh[y * S + x + 1] : 0);
        tmp[y * S + x] = (uint8_t)(s / 3);
      }
    for (int y = 0; y < S; y++)
      for (int x = 0; x < S; x++) {
        int s = tmp[y * S + x] + (y > 0 ? tmp[(y - 1) * S + x] : 0) +
                (y < S - 1 ? tmp[(y + 1) * S + x] : 0);
        sh[y * S + x] = (uint8_t)(s / 3);
      }
  }

  for (int i = 0; i < S * S; i++) {
    uint32_t c = blend_pixel_fast(icon[i], (uint32_t)sh[i] << 24);
    uint32_t a = c >> 24;
    uint32_t r = (((c >> 16) & 0xFF) * a + 127) / 255;
    uint32_t g = (((c >> 8) & 0xFF) * a + 127) / 255;
    uint32_t b = ((c & 0xFF) * a + 127) / 255;
    icon[i] = (a << 24) | (r << 16) | (g << 8) | b;
  }
}

/* Si el icono no es una baldosa a sangre (p. ej. un glifo de GTK/Qt sobre
 * fondo transparente), lo monta sobre una baldosa del color del tema, para que
 * todos los iconos del Dock tengan fondo y respeten el modo claro/oscuro. */
static void apply_theme_backing(uint32_t *icon) {
  const int S = DOCK_ICON_SRC;
  static uint32_t glyph[DOCK_ICON_SRC * DOCK_ICON_SRC];
  long cover = 0;
  for (int i = 0; i < S * S; i++)
    cover += icon[i] >> 24;
  /* Cobertura media de alfa; una baldosa redondeada a sangre pasa del 80 %. */
  if (cover * 100 / ((long)S * S * 255) >= 70)
    return;
  for (int i = 0; i < S * S; i++) {
    glyph[i] = icon[i];
    icon[i] = 0;
  }
  /* Luminosidad media del glifo (ponderada por alfa): si se parece a la de la
   * baldosa del tema, se invierte la baldosa para que el glifo se lea. */
  long lum = 0, wsum = 0;
  for (int i = 0; i < S * S; i++) {
    long a = glyph[i] >> 24;
    long y = (((glyph[i] >> 16) & 0xFF) * 3 + ((glyph[i] >> 8) & 0xFF) * 6 +
              (glyph[i] & 0xFF)) / 10;
    lum += y * a;
    wsum += a;
  }
  lum = wsum ? lum / wsum : 128;
  uint32_t tc = g_theme->icon_tile;
  long tl = (((tc >> 16) & 0xFF) * 3 + ((tc >> 8) & 0xFF) * 6 + (tc & 0xFF)) / 10;
  int inverted = (lum > tl ? lum - tl : tl - lum) < 90;
  if (inverted)
    tc = theme_is_light() ? 0xFF1B2622 : 0xFFF2F6F4;
  int tile_light = ((((tc >> 16) & 0xFF) * 3 + ((tc >> 8) & 0xFF) * 6 +
                     (tc & 0xFF)) / 10) > 128;
  rect_t clip = {0, 0, S, S};
  const int inset = S / 24, radius = S * 9 / 40;
  rect_t tile = {inset, inset, S - 2 * inset, S - 2 * inset};
  gfx_fill_rounded_rect(icon, S, clip, tile, radius, tc);
  gfx_draw_rounded_border(icon, S, clip, tile, radius,
                          tile_light ? 0x20000000 : 0x24FFFFFF);
  int g = S * 70 / 100;
  gfx_blit_scaled(icon, S, clip, (S - g) / 2, (S - g) / 2, g, g, glyph, S, S,
                  GFX_FILTER_BILINEAR);
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
  if (it->icon_bmp_node) {
    ok = (bmp_draw_icon_scaled(it->icon_bmp_node, it->dock_icon, S, S) == 0);
    if (ok)
      apply_theme_backing(it->dock_icon);
  }
  if (!ok) {
    for (int i = 0; i < S * S; i++)
      it->dock_icon[i] = 0;
    make_fallback_tile(it);
  }
  bake_shadow_and_premultiply(it->dock_icon);
  it->dock_icon_state = 1;
  return 1;
}

/* Negro premultiplicado con el alfa del icono por a/255 (oscurecido al
 * pulsar). Solo se usa para el icono pulsado, uno a la vez. Solo hilo del
 * compositor. */
static const uint32_t *silhouette(const taskbar_item_t *it, uint32_t a) {
  static uint32_t sil[DOCK_ICON_SRC * DOCK_ICON_SRC];
  for (int i = 0; i < DOCK_ICON_SRC * DOCK_ICON_SRC; i++)
    sil[i] = (((it->dock_icon[i] >> 24) * a / 255) << 24);
  return sil;
}

// ---------------------------------------------------------------------------
// Render
// ---------------------------------------------------------------------------
void dock_render(uint32_t *dst, int stride, rect_t clip) {
  if (D.n == 0 || !rect_intersects(clip, D.damage))
    return;

  rect_t p = D.panel;

  /* Cristal líquido: fondo desenfocado y saturado + tinte + borde especular. */
  gfx_glass_panel(dst, stride, clip, p, DOCK_RADIUS, AURORA_DOCK_TINT,
                  AURORA_GLASS_BLUR, DOCK_GLASS_SAT, D.scratch, D.scratch_px);

  /* Separador entre lanzadores fijados y ventanas sueltas. */
  if (D.sep_x >= 0) {
    int y0 = p.y + 12, y1 = p.y + p.h - 14;
    for (int y = y0; y < y1; y++) {
      int t = (y - y0) * 256 / (y1 - y0);
      int e = t < 128 ? t : 256 - t;
      gfx_blend_rect(dst, stride, clip, (rect_t){D.sep_x, y, 1, 1},
                     ((uint32_t)(0x4A * e / 128) << 24) | 0x00FFFFFF);
    }
  }

  const int S = DOCK_ICON_SRC;
  for (int i = 0; i < D.n; i++) {
    taskbar_item_t *it = D.slots[i].item;
    rect_t r = D.slots[i].r;
    if (!ensure_icon(it))
      continue;

    /* Un único blit: el icono ya lleva su sombra y el alfa premultiplicado. */
    gfx_blit_scaled_premul(dst, stride, clip, r.x, r.y, r.w, r.h,
                           it->dock_icon, S, S);

    /* Pulsado: se oscurece mientras el botón está abajo. */
    if (it == D.pressed)
      gfx_blit_scaled_premul(dst, stride, clip, r.x, r.y, r.w, r.h,
                             silhouette(it, 0x60), S, S);

    /* Indicador de app abierta: punto blanco bajo el icono (más brillante si
     * es la activa; tenue mientras un lanzador se está abriendo). Un lanzador
     * sin ventanas no lleva punto. */
    if (!it->pinned || it->win_count > 0 || it->launching > 0) {
      uint32_t col = it->is_active ? 0xF2FFFFFF : 0x80FFFFFF;
      if (it->pinned && it->win_count == 0)
        col = 0x50FFFFFF;
      gfx_fill_circle(dst, stride, clip, (float)r.x + r.w / 2.0f,
                      (float)(p.y + p.h) - 5.0f, 2.0f, col);
    }
  }

  /* Tooltip sobre el icono bajo el cursor (geometría de dock_update). */
  if (D.tip.w > 0 && D.hover >= 0) {
    rect_t tr = D.tip;
    gfx_glass_panel(dst, stride, clip, tr, 9, AURORA_TOOLTIP_TINT,
                    TIP_BLUR, 280, D.scratch, D.scratch_px);
    gfx_draw_string(dst, stride, clip, tr.x + 11,
                    gfx_text_center_y(FONT_ID_MAIN_REGULAR, tr.y, tr.h),
                    D.slots[D.hover].item->label, AURORA_TEXT_PRIMARY,
                    FONT_ID_MAIN_REGULAR);
  }
}
