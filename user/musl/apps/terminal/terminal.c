// user/musl/apps/terminal/terminal.c
#include "terminal.h"
#include "../../../lib/malloc.h"
#include "../../../lib/string.h"

// History de scrollback.
#define TERM_HISTORY_LINES_ (TERM_HISTORY_LINES)

// ============================================================================
// Colores por defecto
// ============================================================================
// Mismo esquema Solarized Dark que ya usaba el terminal actual.
static const uint32_t PALETTE[16] = {
    0x073642, 0xDC322F, 0x859900, 0xB58900, 0x268BD2, 0xD33682,
    0x2AA198, 0xEEE8D5, 0x002B36, 0xCB4B16, 0x586E75, 0x657B83,
    0x839496, 0x6C71C4, 0x93A1A1, 0xFDF6E3,
};

#define DEF_FG PALETTE[12] // base0  #839496
#define DEF_BG PALETTE[8]  // base03 #002B36

// [FIX #10a] Toggle del gradiente de fondo.
//
// A 1: degradado vertical sutil de base03 a un tono más oscuro.
// A 0: fondo sólido base03.
//
// Por defecto a 0 porque el gradiente con matemática entera en 8-bit
// produce banding visible (pasos de 1-2 unidades de G cada ~17 filas
// en una ventana de 560 px). Se nota más que el degradado aporta.
//
// Para arreglarlo bien si se quiere reactivar: dithering con matriz
// Bayer 4x4 (añadir ±1 LSB de ruido por píxel) o reducir el rango
// del gradiente a ~10 unidades. Ninguna de las dos merece el coste
// para una diferencia tan sutil.
#define TERM_GRADIENT 0

#if TERM_GRADIENT
#define DEF_BG_TOP_OPAQUE 0xFF002B36u
#define DEF_BG_BOTTOM_OPAQUE 0xFF000A0Eu
#endif

// ============================================================================
// DEC special graphics: mapea 0x60..0x7E a símbolos. Solo los más usados.
// ============================================================================
static uint32_t dec_special(uint32_t c) {
  switch (c) {
  case 0x60:
    return 0x25C6; // ◆
  case 0x61:
    return 0x2592; // ▒
  case 0x62:
    return 0x2409; // HT
  case 0x63:
    return 0x240C; // FF
  case 0x64:
    return 0x240D; // CR
  case 0x65:
    return 0x240A; // LF
  case 0x66:
    return 0x00B0; // °
  case 0x67:
    return 0x00B1; // ±
  case 0x68:
    return 0x2424; // NL
  case 0x69:
    return 0x240B; // VT
  case 0x6A:
    return 0x2518; // ┘
  case 0x6B:
    return 0x2510; // ┐
  case 0x6C:
    return 0x250C; // ┌
  case 0x6D:
    return 0x2514; // └
  case 0x6E:
    return 0x253C; // ┼
  case 0x6F:
    return 0x23BA; // ⎺
  case 0x70:
    return 0x23BB; // ⎻
  case 0x71:
    return 0x2500; // ─
  case 0x72:
    return 0x23BC; // ⎼
  case 0x73:
    return 0x23BD; // ⎽
  case 0x74:
    return 0x251C; // ├
  case 0x75:
    return 0x2524; // ┤
  case 0x76:
    return 0x2534; // ┴
  case 0x77:
    return 0x252C; // ┬
  case 0x78:
    return 0x2502; // │
  case 0x79:
    return 0x2264; // ≤
  case 0x7A:
    return 0x2265; // ≥
  case 0x7B:
    return 0x03C0; // π
  case 0x7C:
    return 0x2260; // ≠
  case 0x7D:
    return 0x00A3; // £
  case 0x7E:
    return 0x00B7; // ·
  default:
    return c;
  }
}

static uint32_t translate_charset(terminal_t *t, uint32_t c) {
  int cs = (t->active_charset == 0) ? t->g0_charset : t->g1_charset;
  if (cs == '0' && c >= 0x60 && c <= 0x7E)
    return dec_special(c);
  return c;
}

// ============================================================================
// Dirty tracking
// ============================================================================
static void dirty_mark(terminal_t *t, int x, int y) {
  if (x < t->dirty_min_x)
    t->dirty_min_x = x;
  if (y < t->dirty_min_y)
    t->dirty_min_y = y;
  if (x > t->dirty_max_x)
    t->dirty_max_x = x;
  if (y > t->dirty_max_y)
    t->dirty_max_y = y;
  t->any_dirty = 1;
}

void terminal_clear_dirty(terminal_t *t) {
  t->dirty_min_x = 0x7FFFFFFF;
  t->dirty_min_y = 0x7FFFFFFF;
  t->dirty_max_x = -1;
  t->dirty_max_y = -1;
  t->any_dirty = 0;
}

// ============================================================================
// Cell access
// ============================================================================
static inline cell_t *cell_at(screen_t *s, int x, int y) {
  return &s->cells[(y + s->y_top) * s->alloc_cols + x];
}

static void cell_clear(screen_t *s, int x, int y, uint32_t bg) {
  cell_t *c = cell_at(s, x, y);
  c->codepoint = ' ';
  c->fg = DEF_FG;
  c->bg = bg;
  c->attrs = 0;
}

static void screen_clear(screen_t *s, uint32_t bg) {
  for (int y = 0; y < s->rows; y++)
    for (int x = 0; x < s->cols; x++)
      cell_clear(s, x, y, bg);
}

// ============================================================================
// Scroll region helpers
// ============================================================================
static screen_t *cur_screen(terminal_t *t) {
  return t->using_alternate ? &t->alternate : &t->primary;
}

// Copia una fila al history ring. Cuando está lleno, sobreescribe la
// línea más vieja y avanza history_head.
static void history_push_line(terminal_t *t, const cell_t *src) {
  if (!t->history || t->history_size <= 0)
    return;
  int slot;
  if (t->history_count < t->history_size) {
    slot = (t->history_head + t->history_count) % t->history_size;
    t->history_count++;
  } else {
    slot = t->history_head;
    t->history_head = (t->history_head + 1) % t->history_size;
  }
  memcpy(&t->history[slot * t->primary.cols], src,
         sizeof(cell_t) * t->primary.cols);
}

// Desplaza la región [top, bottom] hacia arriba n filas.
//
// Cuando la región cubre la pantalla entera y no estamos en alternate
// screen, cada fila que sale por arriba se empuja al history. Scrolling
// dentro de una región (vim, less en modo alterno) NO contamina el
// history — eso es lo que hacen todas las terminales reales.
static void scroll_up(terminal_t *t, int n) {
  screen_t *s = cur_screen(t);
  if (n <= 0)
    return;
  int top = t->scroll_top;
  int bot = t->scroll_bottom;
  int h = bot - top + 1;
  if (n > h)
    n = h;

  int full_screen = (top == 0 && bot == s->rows - 1);
  int to_history = full_screen && !t->using_alternate;

  if (to_history) {
    for (int i = 0; i < n; i++)
      history_push_line(t, &s->cells[(s->y_top + top + i) * s->alloc_cols]);
  }

  // Mover filas (stride = alloc_cols, offset = y_top).
  for (int y = top; y <= bot - n; y++) {
    memcpy(&s->cells[(s->y_top + y) * s->alloc_cols],
           &s->cells[(s->y_top + y + n) * s->alloc_cols],
           sizeof(cell_t) * s->alloc_cols);
  }

  // Limpiar las n filas inferiores (solo columnas visibles).
  for (int y = bot - n + 1; y <= bot; y++) {
    for (int x = 0; x < s->cols; x++)
      cell_clear(s, x, y, t->cur_bg);
    // Las columnas x >= cols quedan con contenido viejo; no las
    // tocamos porque no se ven.
  }

  dirty_mark(t, 0, top);
  dirty_mark(t, s->cols - 1, bot);
}

// Desplaza la región hacia abajo n filas.
static void scroll_down(terminal_t *t, int n) {
  screen_t *s = cur_screen(t);
  if (n <= 0)
    return;
  int top = t->scroll_top;
  int bot = t->scroll_bottom;
  int h = bot - top + 1;
  if (n > h)
    n = h;

  for (int y = bot; y >= top + n; y--) {
    memcpy(&s->cells[(s->y_top + y) * s->alloc_cols],
           &s->cells[(s->y_top + y - n) * s->alloc_cols],
           sizeof(cell_t) * s->alloc_cols);
    for (int x = 0; x < s->cols; x++)
      dirty_mark(t, x, y);
  }
  for (int y = top; y < top + n; y++) {
    for (int x = 0; x < s->cols; x++) {
      cell_clear(s, x, y, t->cur_bg);
      dirty_mark(t, x, y);
    }
  }
}

// LF: baja el cursor. Si ya está en el fondo de la región, scrollea.
static void do_linefeed(terminal_t *t) {
  if (t->cy == t->scroll_bottom) {
    scroll_up(t, 1);
  } else {
    if (t->cy < cur_screen(t)->rows - 1)
      t->cy++;
  }
}

// RI: sube el cursor. Si ya está en el top de la región, scrollea hacia abajo.
static void do_reverse_index(terminal_t *t) {
  if (t->cy == t->scroll_top) {
    scroll_down(t, 1);
  } else {
    if (t->cy > 0)
      t->cy--;
  }
}

// ============================================================================
// Cursor
// ============================================================================
static void cursor_clamp(terminal_t *t) {
  screen_t *s = cur_screen(t);
  if (t->cx < 0)
    t->cx = 0;
  if (t->cx >= s->cols)
    t->cx = s->cols - 1;
  if (t->cy < 0)
    t->cy = 0;
  if (t->cy >= s->rows)
    t->cy = s->rows - 1;
}

static void cursor_set(terminal_t *t, int x, int y) {
  t->cx = x;
  t->cy = y;
  cursor_clamp(t);
}

// ============================================================================
// Escritura de celdas
// ============================================================================
static void put_cell(terminal_t *t, uint32_t cp) {
  screen_t *s = cur_screen(t);

  if (t->mode_insert) {
    for (int x = s->cols - 1; x > t->cx; x--) {
      s->cells[(s->y_top + t->cy) * s->alloc_cols + x] =
          s->cells[(s->y_top + t->cy) * s->alloc_cols + x - 1];
      dirty_mark(t, x, t->cy);
    }
  }

  cell_t *c = cell_at(s, t->cx, t->cy);
  c->codepoint = cp;
  c->fg = t->cur_fg;
  c->bg = t->cur_bg;
  c->attrs = t->cur_attrs;
  dirty_mark(t, t->cx, t->cy);

  if (t->cx + 1 >= s->cols) {
    if (t->mode_autowrap) {
      t->cx = 0;
      // El salto de línea en autowrap: LF + CR.
      if (t->cy == t->scroll_bottom)
        scroll_up(t, 1);
      else if (t->cy < s->rows - 1)
        t->cy++;
      // Marcar la fila entera como sucia (el scroll cambia todo).
      dirty_mark(t, 0, t->cy);
      dirty_mark(t, s->cols - 1, t->cy);
    } else {
      // Sin autowrap, el cursor se queda en la última columna.
    }
  } else {
    t->cx++;
  }
}

// ============================================================================
// Reset del terminal
// ============================================================================
static void reset_modes(terminal_t *t) {
  t->cur_fg = DEF_FG;
  t->cur_bg = DEF_BG;
  t->cur_attrs = 0;

  t->mode_autowrap = 1;
  t->mode_origin = 0;
  t->mode_insert = 0;
  t->mode_lnm = 0;
  t->mode_appcursor = 0;
  t->mode_appkeypad = 0;
  t->mode_bracketed_paste = 0;
  t->mode_mouse = 0;
  t->mode_mouse_sgr = 0;
  t->mode_focus_events = 0;

  t->scroll_top = 0;
  t->scroll_bottom = cur_screen(t)->rows - 1;

  t->cursor_visible = 1;

  // [FIX #5] Cursor de barra por defecto.
  t->cursor_shape = CUR_BAR;
  t->cursor_blink_on = 1;

  t->g0_charset = 'B';
  t->g1_charset = 'B';
  t->active_charset = 0;
  t->single_shift = 0;

  for (int i = 0; i < 256; i++)
    t->tabs[i] = (i % 8 == 0) ? 1 : 0;
}

static void full_reset(terminal_t *t) {
  t->primary.y_top = 0;
  t->alternate.y_top = 0;
  screen_clear(&t->primary, DEF_BG);
  screen_clear(&t->alternate, DEF_BG);
  t->using_alternate = 0;
  reset_modes(t);
  cursor_set(t, 0, 0);
  terminal_clear_dirty(t);
  dirty_mark(t, 0, 0);
  dirty_mark(t, t->primary.cols - 1, t->primary.rows - 1);
}

int terminal_init(terminal_t *t, int cols, int rows) {
  if (!t || cols <= 0 || rows <= 0)
    return -1;
  memset(t, 0, sizeof(*t));

  t->primary.cols = cols;
  t->primary.rows = rows;
  t->primary.alloc_cols = cols;
  t->primary.alloc_rows = rows;
  t->primary.y_top = 0;
  t->primary.cells = malloc(sizeof(cell_t) * cols * rows);
  if (!t->primary.cells)
    return -1;

  t->alternate.cols = cols;
  t->alternate.rows = rows;
  t->alternate.alloc_cols = cols;
  t->alternate.alloc_rows = rows;
  t->alternate.y_top = 0;
  t->alternate.cells = malloc(sizeof(cell_t) * cols * rows);
  if (!t->alternate.cells) {
    free(t->primary.cells);
    return -1;
  }

  t->history_size = TERM_HISTORY_LINES;
  t->history = malloc(sizeof(cell_t) * t->history_size * cols);
  if (!t->history) {
    free(t->primary.cells);
    free(t->alternate.cells);
    return -1;
  }
  t->history_head = 0;
  t->history_count = 0;
  t->scroll_offset = 0;

  t->parser_state = PS_GROUND;
  t->cursor_alpha = 255;
  full_reset(t);
  return 0;
}

void terminal_shutdown(terminal_t *t) {
  if (!t)
    return;
  free(t->primary.cells);
  free(t->alternate.cells);
  free(t->history);
  t->primary.cells = t->alternate.cells = NULL;
  t->history = NULL;
}

// user/musl/apps/terminal/terminal.c
//
// Resize con alloc buffer separado del tamaño lógico.
//
// El buffer físico de cada pantalla (alloc_cols × alloc_rows) SOLO crece.
// Al encoger, solo cambiamos cols/rows (lógico) y las celdas de fuera se
// conservan en el buffer. Al volver a agrandar hasta el tamaño previo,
// esas celdas reaparecen. No hay reflow (el texto no se reorganiza),
// pero tampoco se pierde.
void terminal_resize(terminal_t *t, int new_cols, int new_rows) {
  if (!t || new_cols <= 0 || new_rows <= 0)
    return;
  if (new_cols == t->primary.cols && new_rows == t->primary.rows)
    return;

  int old_cols = t->primary.cols;
  int old_rows = t->primary.rows;

  // ---------------------------------------------------------------------
  // 1. Realloc del buffer físico si hace falta (solo crece).
  // ---------------------------------------------------------------------
  screen_t *screens[2] = {&t->primary, &t->alternate};
  for (int k = 0; k < 2; k++) {
    screen_t *s = screens[k];
    int need_realloc = (new_cols > s->alloc_cols) || (new_rows > s->alloc_rows);
    if (need_realloc) {
      int ac = s->alloc_cols;
      int ar = s->alloc_rows;
      if (ac < new_cols)
        ac = new_cols * 2;
      if (ar < new_rows)
        ar = new_rows * 2;
      if (ac < new_cols)
        ac = new_cols;
      if (ar < new_rows)
        ar = new_rows;

      cell_t *nc = malloc(sizeof(cell_t) * (size_t)ac * ar);
      if (!nc)
        return;

      // Copia el buffer viejo, cambiando stride. Todas las filas
      // físicas se copian (no solo las visibles), para que el
      // contenido "por encima" siga ahí.
      for (int y = 0; y < s->alloc_rows; y++) {
        cell_t *src = &s->cells[y * s->alloc_cols];
        cell_t *dst = &nc[y * ac];
        for (int x = 0; x < s->alloc_cols; x++)
          dst[x] = src[x];
        for (int x = s->alloc_cols; x < ac; x++) {
          dst[x].codepoint = ' ';
          dst[x].fg = DEF_FG;
          dst[x].bg = DEF_BG;
          dst[x].attrs = 0;
        }
      }
      for (int y = s->alloc_rows; y < ar; y++) {
        cell_t *dst = &nc[y * ac];
        for (int x = 0; x < ac; x++) {
          dst[x].codepoint = ' ';
          dst[x].fg = DEF_FG;
          dst[x].bg = DEF_BG;
          dst[x].attrs = 0;
        }
      }

      free(s->cells);
      s->cells = nc;
      s->alloc_cols = ac;
      s->alloc_rows = ar;
    }

    s->cols = new_cols;
    s->rows = new_rows;
  }

  // ---------------------------------------------------------------------
  // 2. Realloc del history al nuevo ancho.
  // ---------------------------------------------------------------------
  if (t->history && t->history_size > 0 && new_cols != old_cols) {
    cell_t *new_hist =
        malloc(sizeof(cell_t) * (size_t)t->history_size * new_cols);
    if (new_hist) {
      int copy_cols = (new_cols < old_cols) ? new_cols : old_cols;
      for (int row = 0; row < t->history_count; row++) {
        int slot_old = (t->history_head + row) % t->history_size;
        cell_t *src = &t->history[slot_old * old_cols];
        cell_t *dst = &new_hist[row * new_cols];
        for (int x = 0; x < copy_cols; x++)
          dst[x] = src[x];
        for (int x = copy_cols; x < new_cols; x++) {
          dst[x].codepoint = ' ';
          dst[x].fg = DEF_FG;
          dst[x].bg = DEF_BG;
          dst[x].attrs = 0;
        }
      }
      free(t->history);
      t->history = new_hist;
      t->history_head = 0;
    } else {
      t->history_count = 0;
      t->history_head = 0;
    }
  }

  // ---------------------------------------------------------------------
  // 3. Ajuste de y_top (solo primary; alternate siempre 0).
  //
  // Al ENCOGER: si el cursor quedaría fuera del grid visible, subimos
  // y_top lo justo para que quede en la última fila visible. El
  // contenido del buffer NO se mueve, solo cambia qué trozo se ve.
  //
  // Al AGRANDAR: bajamos y_top hasta 0 para recuperar el contenido
  // que quedó por encima. Así, encoger+agrandar no pierde nada.
  // ---------------------------------------------------------------------
  screen_t *p = &t->primary;
  int cy = t->cy;

  if (new_rows < old_rows) {
    if (cy >= new_rows) {
      int shift = cy - (new_rows - 1);
      // Invariante: p->y_top + old_rows <= p->alloc_rows
      // → new y_top = p->y_top + shift ≤ p->alloc_rows - new_rows. OK.
      p->y_top += shift;
      cy = new_rows - 1;
    }
  } else if (new_rows > old_rows) {
    int delta = new_rows - old_rows;
    int dec = (p->y_top < delta) ? p->y_top : delta;
    p->y_top -= dec;
    cy += dec;
  }
  t->alternate.y_top = 0;

  t->cy = cy;
  t->scroll_top = 0;
  t->scroll_bottom = new_rows - 1;
  cursor_clamp(t);
  t->scroll_offset = 0;

  t->any_dirty = 1;
  t->dirty_min_x = t->dirty_min_y = 0;
  t->dirty_max_x = new_cols - 1;
  t->dirty_max_y = new_rows - 1;
}

// ============================================================================
// Cursor save/restore
// ============================================================================
static void decsc(terminal_t *t) {
  t->s_cx = t->cx;
  t->s_cy = t->cy;
  t->s_fg = t->cur_fg;
  t->s_bg = t->cur_bg;
  t->s_attrs = t->cur_attrs;
  t->s_origin = t->mode_origin;
  t->s_autowrap = t->mode_autowrap;
}

static void decrc(terminal_t *t) {
  cursor_set(t, t->s_cx, t->s_cy);
  t->cur_fg = t->s_fg;
  t->cur_bg = t->s_bg;
  t->cur_attrs = t->s_attrs;
  t->mode_origin = t->s_origin;
  t->mode_autowrap = t->s_autowrap;
}

// ============================================================================
// CSI dispatch
// ============================================================================
static int param(terminal_t *t, int i, int default_val) {
  if (i >= t->nparams)
    return default_val;
  int v = t->params[i];
  return v == 0 ? default_val : v;
}

static void sgr(terminal_t *t) {
  if (t->nparams == 0) {
    t->cur_fg = DEF_FG;
    t->cur_bg = DEF_BG;
    t->cur_attrs = 0;
    return;
  }
  for (int i = 0; i < t->nparams; i++) {
    int p = t->params[i];
    if (p == 0) {
      t->cur_fg = DEF_FG;
      t->cur_bg = DEF_BG;
      t->cur_attrs = 0;
    } else if (p == 1)
      t->cur_attrs |= CELL_BOLD;
    else if (p == 2)
      t->cur_attrs |= CELL_DIM;
    else if (p == 3)
      t->cur_attrs |= CELL_ITALIC;
    else if (p == 4)
      t->cur_attrs |= CELL_UNDERLINE;
    else if (p == 7)
      t->cur_attrs |= CELL_REVERSE;
    else if (p == 8)
      t->cur_attrs |= CELL_INVISIBLE;
    else if (p == 22)
      t->cur_attrs &= ~(CELL_BOLD | CELL_DIM);
    else if (p == 23)
      t->cur_attrs &= ~CELL_ITALIC;
    else if (p == 24)
      t->cur_attrs &= ~CELL_UNDERLINE;
    else if (p == 27)
      t->cur_attrs &= ~CELL_REVERSE;
    else if (p == 28)
      t->cur_attrs &= ~CELL_INVISIBLE;
    else if (p >= 30 && p <= 37)
      t->cur_fg = PALETTE[p - 30];
    else if (p == 38) {
      if (i + 2 < t->nparams && t->params[i + 1] == 5) {
        int idx = t->params[i + 2] & 0xFF;
        if (idx < 16)
          t->cur_fg = PALETTE[idx];
        else
          t->cur_fg = 0x808080;
        i += 2;
      } else if (i + 4 < t->nparams && t->params[i + 1] == 2) {
        uint32_t r = t->params[i + 2] & 0xFF;
        uint32_t g = t->params[i + 3] & 0xFF;
        uint32_t b = t->params[i + 4] & 0xFF;
        t->cur_fg = (r << 16) | (g << 8) | b;
        i += 4;
      }
    } else if (p == 39)
      t->cur_fg = DEF_FG;
    else if (p >= 40 && p <= 47)
      t->cur_bg = PALETTE[p - 40];
    else if (p == 48) {
      if (i + 2 < t->nparams && t->params[i + 1] == 5) {
        int idx = t->params[i + 2] & 0xFF;
        if (idx < 16)
          t->cur_bg = PALETTE[idx];
        else
          t->cur_bg = 0x303030;
        i += 2;
      } else if (i + 4 < t->nparams && t->params[i + 1] == 2) {
        uint32_t r = t->params[i + 2] & 0xFF;
        uint32_t g = t->params[i + 3] & 0xFF;
        uint32_t b = t->params[i + 4] & 0xFF;
        t->cur_bg = (r << 16) | (g << 8) | b;
        i += 4;
      }
    } else if (p == 49)
      t->cur_bg = DEF_BG;
    else if (p >= 90 && p <= 97)
      t->cur_fg = PALETTE[8 + (p - 90)];
    else if (p >= 100 && p <= 107)
      t->cur_bg = PALETTE[8 + (p - 100)];
  }
}

static void csi_dispatch(terminal_t *t, int final) {
  screen_t *s = cur_screen(t);

  switch (final) {
  // ---- Cursor movement ----
  case 'A': {
    int n = param(t, 0, 1);
    t->cy -= n;
    cursor_clamp(t);
    break;
  }
  case 'B': {
    int n = param(t, 0, 1);
    t->cy += n;
    cursor_clamp(t);
    break;
  }
  case 'C': {
    int n = param(t, 0, 1);
    t->cx += n;
    cursor_clamp(t);
    break;
  }
  case 'D': {
    int n = param(t, 0, 1);
    t->cx -= n;
    cursor_clamp(t);
    break;
  }
  case 'E': {
    int n = param(t, 0, 1);
    t->cy += n;
    t->cx = 0;
    cursor_clamp(t);
    break;
  }
  case 'F': {
    int n = param(t, 0, 1);
    t->cy -= n;
    t->cx = 0;
    cursor_clamp(t);
    break;
  }
  case 'G': {
    int n = param(t, 0, 1);
    t->cx = n - 1;
    cursor_clamp(t);
    break;
  }
  case 'd': {
    int n = param(t, 0, 1);
    t->cy = n - 1;
    cursor_clamp(t);
    break;
  }
  case 'H':
  case 'f': {
    int row = param(t, 0, 1);
    int col = param(t, 1, 1);
    if (t->mode_origin) {
      t->cy = t->scroll_top + row - 1;
    } else {
      t->cy = row - 1;
    }
    t->cx = col - 1;
    cursor_clamp(t);
    break;
  }
  case '`': {
    int n = param(t, 0, 1);
    t->cx = n - 1;
    cursor_clamp(t);
    break;
  }
  case 'a': {
    int n = param(t, 0, 1);
    t->cx += n;
    cursor_clamp(t);
    break;
  }

  // ---- Erase ----
  case 'J': {
    int n = param(t, 0, 0);

    // [FIX] CSI 3J: borrar el scrollback. `clear` no lo envía (usa 2J),
    // pero `printf '\033[3J'` sí. Es un añadido pequeño y sin efectos
    // secundarios, y permite que un shell pueda limpiar el history
    // explícitamente.
    if (n == 3) {
      t->history_count = 0;
      t->history_head = 0;
      t->scroll_offset = 0;
      t->any_dirty = 1;
      t->dirty_min_x = 0;
      t->dirty_min_y = 0;
      t->dirty_max_x = s->cols - 1;
      t->dirty_max_y = s->rows - 1;
      break;
    }

    if (n == 0) {
      for (int x = t->cx; x < s->cols; x++) {
        cell_clear(s, x, t->cy, t->cur_bg);
        dirty_mark(t, x, t->cy);
      }
      for (int y = t->cy + 1; y < s->rows; y++)
        for (int x = 0; x < s->cols; x++) {
          cell_clear(s, x, y, t->cur_bg);
          dirty_mark(t, x, y);
        }
    } else if (n == 1) {
      for (int y = 0; y < t->cy; y++)
        for (int x = 0; x < s->cols; x++) {
          cell_clear(s, x, y, t->cur_bg);
          dirty_mark(t, x, y);
        }
      for (int x = 0; x <= t->cx; x++) {
        cell_clear(s, x, t->cy, t->cur_bg);
        dirty_mark(t, x, t->cy);
      }
    } else {
      // n == 2 (o cualquier otro): toda la pantalla visible.
      for (int y = 0; y < s->rows; y++)
        for (int x = 0; x < s->cols; x++) {
          cell_clear(s, x, y, t->cur_bg);
          dirty_mark(t, x, y);
        }
    }
    break;
  }
  case 'K': {
    int n = param(t, 0, 0);
    if (n == 0) {
      for (int x = t->cx; x < s->cols; x++) {
        cell_clear(s, x, t->cy, t->cur_bg);
        dirty_mark(t, x, t->cy);
      }
    } else if (n == 1) {
      for (int x = 0; x <= t->cx; x++) {
        cell_clear(s, x, t->cy, t->cur_bg);
        dirty_mark(t, x, t->cy);
      }
    } else {
      for (int x = 0; x < s->cols; x++) {
        cell_clear(s, x, t->cy, t->cur_bg);
        dirty_mark(t, x, t->cy);
      }
    }
    break;
  }
  case 'X': {
    int n = param(t, 0, 1);
    for (int i = 0; i < n && t->cx + i < s->cols; i++) {
      cell_clear(s, t->cx + i, t->cy, t->cur_bg);
      dirty_mark(t, t->cx + i, t->cy);
    }
    break;
  }

  // ---- Insert/delete ----
  case '@': {
    int n = param(t, 0, 1);
    if (n > s->cols - t->cx)
      n = s->cols - t->cx;
    for (int x = s->cols - 1; x >= t->cx + n; x--) {
      s->cells[(s->y_top + t->cy) * s->alloc_cols + x] =
          s->cells[(s->y_top + t->cy) * s->alloc_cols + x - n];
      dirty_mark(t, x, t->cy);
    }
    for (int x = t->cx; x < t->cx + n; x++) {
      cell_clear(s, x, t->cy, t->cur_bg);
      dirty_mark(t, x, t->cy);
    }
    break;
  }
  case 'P': {
    int n = param(t, 0, 1);
    if (n > s->cols - t->cx)
      n = s->cols - t->cx;
    for (int x = t->cx; x < s->cols - n; x++) {
      s->cells[(s->y_top + t->cy) * s->alloc_cols + x] =
          s->cells[(s->y_top + t->cy) * s->alloc_cols + x + n];
      dirty_mark(t, x, t->cy);
    }
    for (int x = s->cols - n; x < s->cols; x++) {
      cell_clear(s, x, t->cy, t->cur_bg);
      dirty_mark(t, x, t->cy);
    }
    break;
  }
  case 'L': {
    int n = param(t, 0, 1);
    if (t->cy < t->scroll_top || t->cy > t->scroll_bottom)
      break;
    int save_top = t->scroll_top;
    int save_bot = t->scroll_bottom;
    t->scroll_top = t->cy;
    scroll_down(t, n);
    t->scroll_top = save_top;
    t->scroll_bottom = save_bot;
    break;
  }
  case 'M': {
    int n = param(t, 0, 1);
    if (t->cy < t->scroll_top || t->cy > t->scroll_bottom)
      break;
    int save_top = t->scroll_top;
    int save_bot = t->scroll_bottom;
    t->scroll_top = t->cy;
    scroll_up(t, n);
    t->scroll_top = save_top;
    t->scroll_bottom = save_bot;
    break;
  }
  case 'S': {
    int n = param(t, 0, 1);
    scroll_up(t, n);
    break;
  }
  case 'T': {
    int n = param(t, 0, 1);
    scroll_down(t, n);
    break;
  }

  // ---- SGR ----
  case 'm':
    sgr(t);
    break;

  // ---- Save/restore cursor ----
  case 's':
    decsc(t);
    break;
  case 'u':
    decrc(t);
    break;

  // ---- Scroll region ----
  case 'r': {
    int top = param(t, 0, 1);
    int bot = param(t, 1, s->rows);
    if (top < 1)
      top = 1;
    if (bot > s->rows)
      bot = s->rows;
    if (top < bot) {
      t->scroll_top = top - 1;
      t->scroll_bottom = bot - 1;
      cursor_set(t, 0, t->mode_origin ? t->scroll_top : 0);
    }
    break;
  }

  // ---- Modes ----
  case 'h':
  case 'l': {
    int set = (final == 'h');
    for (int i = 0; i < t->nparams; i++) {
      int p = t->params[i];
      if (t->private_marker == '?') {
        switch (p) {
        case 1:
          t->mode_appcursor = set;
          break;
        case 6:
          t->mode_origin = set;
          if (set)
            cursor_set(t, 0, t->scroll_top);
          else
            cursor_set(t, 0, 0);
          break;
        case 7:
          t->mode_autowrap = set;
          break;
        case 25:
          t->cursor_visible = set;
          break;
        case 47:
        case 1047:
        case 1049: {
          if (set && !t->using_alternate) {
            decsc(t);
            t->using_alternate = 1;
            screen_clear(&t->alternate, DEF_BG);
            cursor_set(t, 0, 0);
            t->any_dirty = 1;
            t->dirty_min_x = t->dirty_min_y = 0;
            t->dirty_max_x = t->alternate.cols - 1;
            t->dirty_max_y = t->alternate.rows - 1;
          } else if (!set && t->using_alternate) {
            t->using_alternate = 0;
            // [FIX] Al volver a primaria, snap a live view. Si el usuario
            // había dejado scroll_offset > 0 (por el bug de arriba),
            // el prompt aparecería en una posición virtual incorrecta.
            t->scroll_offset = 0;
            decrc(t);
            t->any_dirty = 1;
            t->dirty_min_x = t->dirty_min_y = 0;
            t->dirty_max_x = t->primary.cols - 1;
            t->dirty_max_y = t->primary.rows - 1;
          }
          break;
        }
        case 1000:
          t->mode_mouse = set;
          break;
        case 1004:
          t->mode_focus_events = set;
          break;
        case 1006:
          t->mode_mouse_sgr = set;
          break;
        case 2004:
          t->mode_bracketed_paste = set;
          break;
        default:
          break;
        }
      } else {
        switch (p) {
        case 4:
          t->mode_insert = set;
          break;
        case 20:
          t->mode_lnm = set;
          break;
        default:
          break;
        }
      }
    }
    break;
  }

  // ---- Cursor shape ----
  case 'q': {
    // DECSCUSR: CSI Ps SP q. El SP es un intermediate byte, no private
    // marker. Comprobar t->inter[] (donde el parser acumula 0x20-0x2F).
    if (t->n_inter > 0 && t->inter[0] == ' ') {
      int n = param(t, 0, 0);
      if (n == 0 || n == 1)
        t->cursor_shape = CUR_BLOCK;
      else if (n == 3 || n == 4)
        t->cursor_shape = CUR_UNDERLINE;
      else if (n == 5 || n == 6)
        t->cursor_shape = CUR_BAR;
    }
    break;
  }

  // ---- Tabs ----
  case 'g': {
    int n = param(t, 0, 0);
    if (n == 0)
      t->tabs[t->cx] = 0;
    else if (n == 3)
      for (int i = 0; i < 256; i++)
        t->tabs[i] = 0;
    break;
  }

  // ---- DSR: responde con el cursor position ----
  case 'n': {
    int n = param(t, 0, 0);
    if (n == 6) {
      int row = t->cy + 1;
      int col = t->cx + 1;
      int l = 0;
      char buf[32];
      buf[l++] = 0x1B;
      buf[l++] = '[';
      char tmp[8];
      int tn = 0;
      int v = row;
      if (v == 0)
        tmp[tn++] = '0';
      while (v > 0) {
        tmp[tn++] = '0' + v % 10;
        v /= 10;
      }
      while (tn > 0)
        buf[l++] = tmp[--tn];
      buf[l++] = ';';
      tn = 0;
      v = col;
      if (v == 0)
        tmp[tn++] = '0';
      while (v > 0) {
        tmp[tn++] = '0' + v % 10;
        v /= 10;
      }
      while (tn > 0)
        buf[l++] = tmp[--tn];
      buf[l++] = 'R';
      if (t->response_len + l < (int)sizeof(t->response)) {
        memcpy(&t->response[t->response_len], buf, l);
        t->response_len += l;
      }
    }
    break;
  }

  // ---- Device Attributes ----
  case 'c': {
    // Query vs response: las queries reales (CSI c / CSI > c) NUNCA
    // llevan parámetros. Las responses (CSI ? ... c / CSI > ... c)
    // siempre. Si llegamos aquí con params, es una response que se
    // coló por el eco del slave → ignorar. Sin este check, el terminal
    // responde a su propia respuesta → bucle infinito (nano lo
    // disparaba al inicializar ncurses).
    if (t->nparams > 0) {
      break;
    }
    if (t->private_marker == '>') {
      const char *r = "\033[>0;10;1c";
      int l = (int)strlen(r);
      if (t->response_len + l < (int)sizeof(t->response)) {
        memcpy(&t->response[t->response_len], r, l);
        t->response_len += l;
      }
    } else if (t->private_marker == '?') {
      // Primary DA response sin params (raro pero posible) → ignorar.
      break;
    } else {
      const char *r = "\033[?6c";
      int l = (int)strlen(r);
      if (t->response_len + l < (int)sizeof(t->response)) {
        memcpy(&t->response[t->response_len], r, l);
        t->response_len += l;
      }
    }
    break;
  }

  default:
    break;
  }
}

// ============================================================================
// Parser principal
// ============================================================================
static void parser_reset_params(terminal_t *t) {
  t->nparams = 0;
  t->cur_param = 0;
  t->has_cur_param = 0;
  t->private_marker = 0;
  t->n_inter = 0;
}

static void parser_push_param(terminal_t *t) {
  if (t->nparams < 32) {
    t->params[t->nparams++] = t->has_cur_param ? t->cur_param : 0;
  }
  t->cur_param = 0;
  t->has_cur_param = 0;
}

static void parser_execute_control(terminal_t *t, uint8_t c) {
  screen_t *s = cur_screen(t);
  switch (c) {
  case 0x07:
    break;
  case 0x08:
    if (t->cx > 0)
      t->cx--;
    break;
  case 0x09: {
    int x = t->cx + 1;
    while (x < s->cols && !t->tabs[x])
      x++;
    if (x >= s->cols)
      x = s->cols - 1;
    t->cx = x;
  } break;
  case 0x0A:
  case 0x0B:
  case 0x0C:
    do_linefeed(t);
    t->cx = 0;
    break;
  case 0x0D:
    t->cx = 0;
    break;
  case 0x0E:
    t->active_charset = 1;
    break;
  case 0x0F:
    t->active_charset = 0;
    break;
  default:
    break;
  }
}

static void parser_feed_one(terminal_t *t, uint8_t c) {
  switch (t->parser_state) {
  case PS_GROUND:
    if (c == 0x1B) {
      t->parser_state = PS_ESCAPE;
      return;
    }
    if (c < 0x20) {
      parser_execute_control(t, c);
      return;
    }
    if (c == 0x7F)
      return;
    put_cell(t, translate_charset(t, c));
    return;

  case PS_ESCAPE:
    if (c == '[') {
      t->parser_state = PS_CSI_ENTRY;
      parser_reset_params(t);
      return;
    }
    if (c == ']') {
      t->parser_state = PS_OSC_STRING;
      return;
    }
    if (c == 'P') {
      t->parser_state = PS_DCS_STRING;
      return;
    }
    if (c == '(' || c == ')') {
      t->private_marker = c;
      return;
    }
    if (c == '#') {
      t->private_marker = '#';
      return;
    }
    if (t->private_marker == '(' || t->private_marker == ')') {
      if (t->private_marker == '(')
        t->g0_charset = c;
      else
        t->g1_charset = c;
      t->private_marker = 0;
      t->parser_state = PS_GROUND;
      return;
    }
    if (t->private_marker == '#') {
      if (c == '8') {
        screen_t *s = cur_screen(t);
        for (int y = 0; y < s->rows; y++)
          for (int x = 0; x < s->cols; x++) {
            cell_t *cl = cell_at(s, x, y);
            cl->codepoint = 'E';
            cl->fg = DEF_FG;
            cl->bg = DEF_BG;
            cl->attrs = 0;
            dirty_mark(t, x, y);
          }
      }
      t->private_marker = 0;
      t->parser_state = PS_GROUND;
      return;
    }
    switch (c) {
    case '7':
      decsc(t);
      break;
    case '8':
      decrc(t);
      break;
    case 'D':
      do_linefeed(t);
      break;
    case 'E':
      do_linefeed(t);
      t->cx = 0;
      break;
    case 'H':
      t->tabs[t->cx] = 1;
      break;
    case 'M':
      do_reverse_index(t);
      break;
    case 'c':
      full_reset(t);
      break;
    case '=':
      t->mode_appkeypad = 1;
      break;
    case '>':
      t->mode_appkeypad = 0;
      break;
    default:
      break;
    }
    t->parser_state = PS_GROUND;
    return;

  case PS_CSI_ENTRY:
    if (c >= '0' && c <= '9') {
      t->cur_param = c - '0';
      t->has_cur_param = 1;
      t->parser_state = PS_CSI_PARAM;
      return;
    }
    if (c == ';') {
      parser_push_param(t);
      t->parser_state = PS_CSI_PARAM;
      return;
    }
    if (c == '<' || c == '=' || c == '>' || c == '?') {
      t->private_marker = c;
      t->parser_state = PS_CSI_PARAM;
      return;
    }
    if (c >= 0x20 && c <= 0x2F) {
      if (t->n_inter < 4)
        t->inter[t->n_inter++] = c;
      t->parser_state = PS_CSI_INTERMEDIATE;
      return;
    }
    if (c >= 0x40 && c <= 0x7E) {
      csi_dispatch(t, c);
      t->parser_state = PS_GROUND;
      return;
    }
    t->parser_state = PS_CSI_IGNORE;
    return;

  case PS_CSI_PARAM:
    if (c >= '0' && c <= '9') {
      t->cur_param = t->cur_param * 10 + (c - '0');
      t->has_cur_param = 1;
      return;
    }
    if (c == ';') {
      parser_push_param(t);
      return;
    }
    if (c == ':') {
      parser_push_param(t);
      return;
    }
    if (c >= 0x20 && c <= 0x2F) {
      if (t->n_inter < 4)
        t->inter[t->n_inter++] = c;
      t->parser_state = PS_CSI_INTERMEDIATE;
      return;
    }
    if (c >= 0x40 && c <= 0x7E) {
      parser_push_param(t);
      csi_dispatch(t, c);
      t->parser_state = PS_GROUND;
      return;
    }
    t->parser_state = PS_CSI_IGNORE;
    return;

  case PS_CSI_INTERMEDIATE:
    if (c >= 0x20 && c <= 0x2F) {
      if (t->n_inter < 4)
        t->inter[t->n_inter++] = c;
      return;
    }
    if (c >= 0x40 && c <= 0x7E) {
      csi_dispatch(t, c);
      t->parser_state = PS_GROUND;
      return;
    }
    t->parser_state = PS_CSI_IGNORE;
    return;

  case PS_CSI_IGNORE:
    if (c >= 0x40 && c <= 0x7E)
      t->parser_state = PS_GROUND;
    return;

  case PS_OSC_STRING:
  case PS_DCS_STRING:
    if (c == 0x07) {
      t->parser_state = PS_GROUND;
      return;
    }
    if (c == 0x1B) {
      t->parser_state = PS_ESCAPE;
      return;
    }
    return;
  }
}

void terminal_feed(terminal_t *t, const uint8_t *buf, size_t n) {
  int old_cx = t->cx;
  int old_cy = t->cy;

  if (t->scroll_offset != 0) {
    t->scroll_offset = 0;
    t->any_dirty = 1;
    t->dirty_min_x = 0;
    t->dirty_min_y = 0;
    t->dirty_max_x = cur_screen(t)->cols - 1;
    t->dirty_max_y = cur_screen(t)->rows - 1;
  }

  for (size_t i = 0; i < n; i++)
    parser_feed_one(t, buf[i]);

  if (t->cx != old_cx || t->cy != old_cy) {
    dirty_mark(t, old_cx, old_cy);
    dirty_mark(t, t->cx, t->cy);
  }
}

// ============================================================================
// Blink
// ============================================================================
// [FIX #10b] El blink binario ya no se usa: cursor_alpha lo reemplaza.
void terminal_blink_toggle(terminal_t *t) { (void)t; }

// ============================================================================
// Render
// ============================================================================
typedef struct {
  uint8_t width;
  uint8_t height;
  int8_t bearing_x;
  int8_t bearing_y;
  uint8_t advance;
  const uint8_t *bitmap;
} ga_glyph_t;

typedef struct {
  uint8_t height;
  ga_glyph_t glyphs[128];
} ga_font_t;

static void draw_glyph(uint32_t *pixels, int stride, int px, int py, int cell_w,
                       int cell_h, uint32_t cp, uint32_t fg, uint32_t bg,
                       uint8_t attrs, const ga_font_t *font, int font_h,
                       int total_h_px) {
  int reverse = attrs & CELL_REVERSE;
  uint32_t bgc = reverse ? fg : bg;
  uint32_t fgc = reverse ? bg : fg;

  uint32_t bg_solid = 0xFF000000u | bgc;

  // [FIX #10a] Fondo de la celda. Bloque completo bajo #if para que el
  // compilador no vea DEF_BG_TOP_OPAQUE / DEF_BG_BOTTOM_OPAQUE cuando
  // TERM_GRADIENT=0 (donde no están definidos).
#if TERM_GRADIENT
  int use_gradient = (bgc == DEF_BG);
  for (int y = 0; y < cell_h; y++) {
    uint32_t bg_row;
    if (use_gradient && total_h_px > 0) {
      int gy = py + y;
      if (gy < 0)
        gy = 0;
      if (gy >= total_h_px)
        gy = total_h_px - 1;
      uint32_t a = DEF_BG_TOP_OPAQUE, b = DEF_BG_BOTTOM_OPAQUE;
      int ra = (a >> 16) & 0xFF, ga = (a >> 8) & 0xFF, ba = a & 0xFF;
      int rb = (b >> 16) & 0xFF, gb = (b >> 8) & 0xFF, bb = b & 0xFF;
      int r = ra + ((rb - ra) * gy) / total_h_px;
      int g = ga + ((gb - ga) * gy) / total_h_px;
      int bl = ba + ((bb - ba) * gy) / total_h_px;
      bg_row = 0xFF000000u | (r << 16) | (g << 8) | bl;
    } else {
      bg_row = bg_solid;
    }
    uint32_t *row = &pixels[(py + y) * stride + px];
    for (int x = 0; x < cell_w; x++)
      row[x] = bg_row;
  }
#else
  (void)total_h_px;
  for (int y = 0; y < cell_h; y++) {
    uint32_t *row = &pixels[(py + y) * stride + px];
    for (int x = 0; x < cell_w; x++)
      row[x] = bg_solid;
  }
#endif

  if (attrs & CELL_INVISIBLE)
    return;
  if (cp >= 128)
    return;

  const ga_glyph_t *g = &font->glyphs[cp];
  if (!g->bitmap || g->width == 0 || g->height == 0)
    return;

  if (attrs & CELL_BOLD) {
    uint32_t r = (fgc >> 16) & 0xFF;
    uint32_t gg = (fgc >> 8) & 0xFF;
    uint32_t b = fgc & 0xFF;
    r = r + (255 - r) / 3;
    gg = gg + (255 - gg) / 3;
    b = b + (255 - b) / 3;
    fgc = (r << 16) | (gg << 8) | b;
  }
  if (attrs & CELL_DIM) {
    uint32_t r = ((fgc >> 16) & 0xFF) / 2;
    uint32_t gg = ((fgc >> 8) & 0xFF) / 2;
    uint32_t b = (fgc & 0xFF) / 2;
    fgc = (r << 16) | (gg << 8) | b;
  }

  int baseline_y = py + (cell_h + font_h) / 2 - 3;
  int gx = px + (cell_w - g->width) / 2 + g->bearing_x;
  int gy = baseline_y - g->bearing_y;

  uint32_t sr = (fgc >> 16) & 0xFF;
  uint32_t sg = (fgc >> 8) & 0xFF;
  uint32_t sb = fgc & 0xFF;

  for (int y = 0; y < g->height; y++) {
    int yy = gy + y;
    if (yy < py || yy >= py + cell_h)
      continue;
    const uint8_t *srow = &g->bitmap[y * g->width];
    uint32_t *drow = &pixels[yy * stride];
    for (int x = 0; x < g->width; x++) {
      int xx = gx + x;
      if (xx < px || xx >= px + cell_w)
        continue;
      uint8_t a = srow[x];
      if (!a)
        continue;
      uint32_t inv = 255 - a;
      uint32_t d = drow[xx];
      uint32_t dr = (d >> 16) & 0xFF;
      uint32_t dg = (d >> 8) & 0xFF;
      uint32_t db = d & 0xFF;
      uint32_t r = (sr * a + dr * inv) / 255;
      uint32_t gg = (sg * a + dg * inv) / 255;
      uint32_t b = (sb * a + db * inv) / 255;
      drow[xx] = 0xFF000000u | (r << 16) | (gg << 8) | b;
    }
  }

  if (attrs & CELL_UNDERLINE) {
    int uy = py + cell_h - 2;
    for (int x = 0; x < cell_w; x++)
      pixels[uy * stride + px + x] = 0xFF000000u | fgc;
  }
}

// --- Scrollback -----------------------------------------------------------

void terminal_scroll(terminal_t *t, int delta) {
  // [FIX] En alternate screen no hay scrollback. top/vim/less usan la
  // pantalla alterna y no esperan que el terminal guarde nada. Si el
  // usuario intenta scrollear, ignorarlo — PgUp/PgDn irán al PTY (ver
  // process_input en main.c).
  if (t->using_alternate)
    return;
  if (!t->history || t->history_size <= 0)
    return;
  int max_off = t->history_count;
  int new_off = t->scroll_offset + delta;
  if (new_off < 0)
    new_off = 0;
  if (new_off > max_off)
    new_off = max_off;
  if (new_off == t->scroll_offset)
    return;
  t->scroll_offset = new_off;

  t->any_dirty = 1;
  t->dirty_min_x = 0;
  t->dirty_min_y = 0;
  t->dirty_max_x = t->primary.cols - 1;
  t->dirty_max_y = t->primary.rows - 1;
}

int terminal_is_scrolled(const terminal_t *t) { return t->scroll_offset > 0; }

void terminal_scroll_to_bottom(terminal_t *t) {
  if (t->scroll_offset != 0)
    terminal_scroll(t, -t->scroll_offset);
}

static const cell_t *virtual_row(const terminal_t *t, int y,
                                 int *out_is_history) {
  const screen_t *s = t->using_alternate ? &t->alternate : &t->primary;
  int H = t->history_count;
  int off = t->scroll_offset;
  int logical = H - off + y;

  if (logical < 0) {
    *out_is_history = 1;
    return NULL;
  }
  if (logical < H) {
    *out_is_history = 1;
    int slot = (t->history_head + logical) % t->history_size;
    return &t->history[slot * s->cols];
  }
  int live_y = logical - H;
  if (live_y < 0 || live_y >= s->rows) {
    *out_is_history = 1;
    return NULL;
  }
  *out_is_history = 0;
  return &s->cells[(live_y + s->y_top) * s->alloc_cols];
}

term_rect_t terminal_render(terminal_t *t, uint32_t *pixels, int stride,
                            int pad_x, int pad_y, int cell_w, int cell_h,
                            const void *font_p) {
  term_rect_t r = {0, 0, 0, 0};
  if (!t->any_dirty)
    return r;

  const ga_font_t *font = (const ga_font_t *)font_p;
  int font_h = font->height;
  screen_t *s = cur_screen(t);

  int total_h_px = t->primary.rows * cell_h + 2 * pad_y;

  int scrolled = (t->scroll_offset > 0);
  int x0, y0, x1, y1;

  if (scrolled) {
    x0 = 0;
    y0 = 0;
    x1 = s->cols - 1;
    y1 = s->rows - 1;
  } else {
    x0 = t->dirty_min_x;
    y0 = t->dirty_min_y;
    x1 = t->dirty_max_x;
    y1 = t->dirty_max_y;
    if (x0 < 0)
      x0 = 0;
    if (y0 < 0)
      y0 = 0;
    if (x1 >= s->cols)
      x1 = s->cols - 1;
    if (y1 >= s->rows)
      y1 = s->rows - 1;
  }

  for (int y = y0; y <= y1; y++) {
    int is_history = 0;
    const cell_t *row = virtual_row(t, y, &is_history);
    for (int x = x0; x <= x1; x++) {
      cell_t c;
      if (row) {
        c = row[x];
      } else {
        c.codepoint = ' ';
        c.fg = DEF_FG;
        c.bg = DEF_BG;
        c.attrs = 0;
      }
      draw_glyph(pixels, stride, pad_x + x * cell_w, pad_y + y * cell_h, cell_w,
                 cell_h, c.codepoint, c.fg, c.bg, c.attrs, font, font_h,
                 total_h_px);
    }
  }

  // [FIX #10b] Cursor con alpha en lugar de blink binario.
  if (!scrolled && t->cursor_visible && t->cursor_alpha > 0) {
    int cur_in_bbox =
        (t->cx >= x0 && t->cx <= x1 && t->cy >= y0 && t->cy <= y1);
    if (!cur_in_bbox) {
      cell_t *c = cell_at(s, t->cx, t->cy);
      draw_glyph(pixels, stride, pad_x + t->cx * cell_w, pad_y + t->cy * cell_h,
                 cell_w, cell_h, c->codepoint, c->fg, c->bg, c->attrs, font,
                 font_h, total_h_px);
      if (t->cx < x0)
        x0 = t->cx;
      if (t->cy < y0)
        y0 = t->cy;
      if (t->cx > x1)
        x1 = t->cx;
      if (t->cy > y1)
        y1 = t->cy;
    }

    int px = pad_x + t->cx * cell_w;
    int py = pad_y + t->cy * cell_h;
    const uint32_t CUR_BG = 0xFF93A1A1u;
    const uint32_t CUR_FG = 0xFF002B36u;
    uint32_t alpha = t->cursor_alpha;

    if (t->cursor_shape == CUR_BLOCK) {
      for (int y = 0; y < cell_h; y++) {
        uint32_t *row2 = &pixels[(py + y) * stride + px];
        for (int x = 0; x < cell_w; x++) {
          uint32_t d = row2[x];
          uint32_t dr = (d >> 16) & 0xFF;
          uint32_t dg = (d >> 8) & 0xFF;
          uint32_t db = d & 0xFF;
          uint32_t r = (0x93 * alpha + dr * (255 - alpha)) / 255;
          uint32_t g = (0xA1 * alpha + dg * (255 - alpha)) / 255;
          uint32_t b = (0xA1 * alpha + db * (255 - alpha)) / 255;
          row2[x] = 0xFF000000u | (r << 16) | (g << 8) | b;
        }
      }
      cell_t *c = cell_at(s, t->cx, t->cy);
      if (c->codepoint != ' ' && alpha > 128) {
        draw_glyph(pixels, stride, px, py, cell_w, cell_h, c->codepoint, CUR_FG,
                   CUR_BG, 0, font, font_h, total_h_px);
      }
    } else if (t->cursor_shape == CUR_UNDERLINE) {
      int uy = py + cell_h - 2;
      for (int x = 0; x < cell_w; x++) {
        uint32_t d = pixels[uy * stride + px + x];
        uint32_t dr = (d >> 16) & 0xFF;
        uint32_t dg = (d >> 8) & 0xFF;
        uint32_t db = d & 0xFF;
        uint32_t r = (0x93 * alpha + dr * (255 - alpha)) / 255;
        uint32_t g = (0xA1 * alpha + dg * (255 - alpha)) / 255;
        uint32_t b = (0xA1 * alpha + db * (255 - alpha)) / 255;
        pixels[uy * stride + px + x] = 0xFF000000u | (r << 16) | (g << 8) | b;
      }
    } else if (t->cursor_shape == CUR_BAR) {
      for (int y = 0; y < cell_h; y++) {
        for (int x = 0; x < 2 && x < cell_w; x++) {
          int idx = (py + y) * stride + px + x;
          uint32_t d = pixels[idx];
          uint32_t dr = (d >> 16) & 0xFF;
          uint32_t dg = (d >> 8) & 0xFF;
          uint32_t db = d & 0xFF;
          uint32_t r = (0x93 * alpha + dr * (255 - alpha)) / 255;
          uint32_t g = (0xA1 * alpha + dg * (255 - alpha)) / 255;
          uint32_t b = (0xA1 * alpha + db * (255 - alpha)) / 255;
          pixels[idx] = 0xFF000000u | (r << 16) | (g << 8) | b;
        }
      }
    }
  }

  // Scrollbar lateral.
  if (scrolled && t->history_count > 0) {
    int track_x = pad_x + s->cols * cell_w - 3;
    int track_y = pad_y;
    int track_h = s->rows * cell_h;
    int track_w = 2;
    int total = t->history_count + s->rows;
    int thumb_h = (s->rows * track_h) / total;
    if (thumb_h < 20)
      thumb_h = 20;
    if (thumb_h > track_h)
      thumb_h = track_h;
    int pos_from_top = t->history_count - t->scroll_offset;
    if (pos_from_top < 0)
      pos_from_top = 0;
    int thumb_y = track_y + (pos_from_top * track_h) / total;
    if (thumb_y + thumb_h > track_y + track_h)
      thumb_y = track_y + track_h - thumb_h;
    const uint32_t THUMB = 0xFF93A1A1u;
    for (int y = 0; y < thumb_h; y++) {
      uint32_t *row = &pixels[(thumb_y + y) * stride + track_x];
      for (int x = 0; x < track_w; x++)
        row[x] = THUMB;
    }
  }

  r.x = pad_x + x0 * cell_w;
  r.y = pad_y + y0 * cell_h;
  r.w = (x1 - x0 + 1) * cell_w;
  r.h = (y1 - y0 + 1) * cell_h;
  return r;
}

// [FIX #10b] Actualiza el alpha del cursor. 0 = invisible, 255 = opaco.
void terminal_set_cursor_alpha(terminal_t *t, uint8_t alpha) {
  if (t->cursor_alpha == alpha)
    return;
  t->cursor_alpha = alpha;
  dirty_mark(t, t->cx, t->cy);
}

// [FIX #6] Mouse tracking.
void terminal_mouse_event(terminal_t *t, int col, int row, int button,
                          int pressed, int mods) {
  if (!t || !t->mode_mouse)
    return;
  if (col < 0 || col >= t->primary.cols)
    return;
  if (row < 0 || row >= t->primary.rows)
    return;

  int b = button;
  if (mods & 1)
    b += 4;
  if (mods & 2)
    b += 8;
  if (mods & 4)
    b += 16;

  char buf[32];
  int n;

  if (t->mode_mouse_sgr) {
    n = snprintf(buf, sizeof(buf), "\033[<%d;%d;%d%c", b, col + 1, row + 1,
                 pressed ? 'M' : 'm');
  } else {
    int bb = b;
    if (!pressed && button < 64)
      bb = 3;
    int x = col + 1 + 32;
    int y = row + 1 + 32;
    if (x > 255 || y > 255)
      return;
    buf[0] = 0x1B;
    buf[1] = '[';
    buf[2] = 'M';
    buf[3] = (char)(bb + 32);
    buf[4] = (char)x;
    buf[5] = (char)y;
    n = 6;
  }

  if (t->response_len + n <= (int)sizeof(t->response)) {
    memcpy(&t->response[t->response_len], buf, n);
    t->response_len += n;
  }
}
