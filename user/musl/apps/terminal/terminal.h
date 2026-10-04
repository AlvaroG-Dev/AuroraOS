// user/musl/apps/terminal/terminal.h
#pragma once
#include <stddef.h>
#include <stdint.h>

// --- Atributos de celda ---
#define CELL_BOLD 0x01
#define CELL_DIM 0x02
#define CELL_ITALIC 0x04
#define CELL_UNDERLINE 0x08
#define CELL_REVERSE 0x10
#define CELL_INVISIBLE 0x20

typedef struct {
  uint32_t codepoint;
  uint32_t fg;
  uint32_t bg;
  uint8_t attrs;
  uint8_t pad[3];
} cell_t;

typedef struct {
  cell_t *cells;       // buffer [alloc_rows * alloc_cols]
  int cols;            // ancho lógico visible (<= alloc_cols)
  int rows;            // alto lógico visible (<= alloc_rows)
  int alloc_cols;      // ancho físico del buffer (solo crece)
  int alloc_rows;      // alto físico del buffer (solo crece)
  int y_top;           // fila física que mapea a la fila lógica 0
                       // [0, alloc_rows - rows]
} screen_t;

enum {
  PS_GROUND = 0,
  PS_ESCAPE,
  PS_CSI_ENTRY,
  PS_CSI_PARAM,
  PS_CSI_INTERMEDIATE,
  PS_CSI_IGNORE,
  PS_OSC_STRING,
  PS_DCS_STRING,
};

enum { CUR_BLOCK = 0, CUR_UNDERLINE = 1, CUR_BAR = 2 };

// Capacidad del ring buffer de scrollback (líneas).
#define TERM_HISTORY_LINES 500

typedef struct {
  int cx, cy;
  int cursor_visible;
  int cursor_blink_on;
  int cursor_shape;

  // [FIX #10b] Alpha del cursor (0=invisible, 255=opaco). Lo actualiza
  // main.c cada frame con una onda triangular para el fade suave.
  uint8_t cursor_alpha;

  int parser_state;
  int params[32];
  int nparams;
  int cur_param;
  int has_cur_param;
  int private_marker;
  uint8_t inter[4];
  int n_inter;

  uint32_t cur_fg, cur_bg;
  uint8_t cur_attrs;

  int mode_autowrap;
  int mode_origin;
  int mode_insert;
  int mode_lnm;
  int mode_appcursor;
  int mode_appkeypad;
  int mode_bracketed_paste;
  int mode_mouse;
  int mode_mouse_sgr;
  int mode_focus_events;

  int scroll_top;
  int scroll_bottom;

  int s_cx, s_cy;
  uint32_t s_fg, s_bg;
  uint8_t s_attrs;
  int s_origin;
  int s_autowrap;

  int g0_charset;
  int g1_charset;
  int active_charset;
  int single_shift;

  uint8_t tabs[256];

  screen_t primary;
  screen_t alternate;
  int using_alternate;

  int dirty_min_x, dirty_min_y;
  int dirty_max_x, dirty_max_y;
  int any_dirty;

  char response[64];
  int response_len;

  // --- Scrollback ---
  cell_t *history;
  int history_size;
  int history_head;
  int history_count;
  int scroll_offset;
} terminal_t;

// --- API ---
int  terminal_init(terminal_t *t, int cols, int rows);
void terminal_shutdown(terminal_t *t);
void terminal_resize(terminal_t *t, int new_cols, int new_rows);
void terminal_feed(terminal_t *t, const uint8_t *buf, size_t n);

typedef struct { int x, y, w, h; } term_rect_t;

term_rect_t terminal_render(terminal_t *t, uint32_t *pixels, int stride,
                            int pad_x, int pad_y,
                            int cell_w, int cell_h, const void *font);

void terminal_clear_dirty(terminal_t *t);
void terminal_blink_toggle(terminal_t *t);

// --- Scrollback API ---
void terminal_scroll(terminal_t *t, int delta);
int  terminal_is_scrolled(const terminal_t *t);
void terminal_scroll_to_bottom(terminal_t *t);

// --- [FIX #10b] Cursor animado ---
void terminal_set_cursor_alpha(terminal_t *t, uint8_t alpha);

// --- [FIX #6] Mouse tracking ---
void terminal_mouse_event(terminal_t *t, int col, int row,
                          int button, int pressed, int mods);

// [FIX] Empuja todas las filas no vacías de la grid al scrollback.
// Se usa antes de un resize para no perder el contenido visible.
void terminal_push_to_history(terminal_t *t);