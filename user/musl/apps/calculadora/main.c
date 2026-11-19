// user/musl/apps/calculadora/main.c
//
// Calculadora de Aurora OS: las cuatro operaciones, %, cambio de signo y
// borrado, con el ratón o el teclado (cifras, + - * / del teclado numérico,
// '=' o Intro, Retroceso, Esc = AC). Encadena operaciones como una calculadora
// de bolsillo (2 + 3 × 4 = 20) y repite la última con '=' seguidos.
// Colores sólidos; el naranja de su icono como color propio; sigue el modo
// claro/oscuro (sys_theme_get).

#define syscall aurora_syscall
#define puts aurora_puts_userlib
#define printf aurora_printf_userlib

#include "../../../lib/env.h"
#include "../../../lib/file.h"
#include "../../../lib/font_system.h"
#include "../../../lib/malloc.h"
#include "../../../lib/process.h"
#include "../../../lib/string.h"
#include "../../../syscall.h"
#include <math.h>
#include <stdlib.h>

#undef puts
#undef printf
#include <stdio.h>

#include "../../../lib/font_system.c"

extern void _exit(int status) __attribute__((noreturn));

#define WIN_W 312
#define WIN_H 430
#define TITLEBAR_H 38 // = AURORA_TITLEBAR_HEIGHT (kernel/gfx/theme.h)
#define PAD 14
#define GAP 8
#define DISP_H 104
#define COLS 4
#define ROWS 5
#define BTN_W ((WIN_W - 2 * PAD - (COLS - 1) * GAP) / COLS)
#define BTN_H ((WIN_H - DISP_H - 2 * PAD - (ROWS - 1) * GAP) / ROWS)
#define ENTRY_MAX 16

typedef struct {
  uint32_t bg, disp_text, dim, num, num_text, fn, fn_text, op, op_text, pressed;
} palette_t;

static const palette_t PAL_DARK = {0xFF16181B, 0xFFF3F4F6, 0xFF8A9099,
                                   0xFF2C3036, 0xFFF3F4F6, 0xFF474C54,
                                   0xFFF3F4F6, 0xFFFF8A3D, 0xFF2A1404,
                                   0xFF5E646D};
static const palette_t PAL_LIGHT = {0xFFF2F3F5, 0xFF16181B, 0xFF6B7280,
                                    0xFFFFFFFF, 0xFF16181B, 0xFFDDE0E5,
                                    0xFF16181B, 0xFFFF8A3D, 0xFF2A1404,
                                    0xFFC3C8D0};
static const palette_t *P = &PAL_DARK;

static uint32_t g_px[WIN_W * WIN_H];
static const font_aa_t *g_font;
static int g_win = -1;
static int g_dirty = 1;
static uint32_t g_theme_seen;
static int g_pressed = -1; // botón pulsado con el ratón (índice) o -1

// ---- Botones ----
enum { K_DIGIT, K_DOT, K_OP, K_EQ, K_AC, K_SIGN, K_PCT };
typedef struct {
  const char *label;
  int kind;
  char arg; // cifra u operador
  int span; // columnas que ocupa
} calc_key_t;

static const calc_key_t k_keys[] = {
    {"AC", K_AC, 0, 1},    {"+/-", K_SIGN, 0, 1}, {"%", K_PCT, 0, 1},
    {"/", K_OP, '/', 1},   {"7", K_DIGIT, '7', 1}, {"8", K_DIGIT, '8', 1},
    {"9", K_DIGIT, '9', 1}, {"x", K_OP, '*', 1},  {"4", K_DIGIT, '4', 1},
    {"5", K_DIGIT, '5', 1}, {"6", K_DIGIT, '6', 1}, {"-", K_OP, '-', 1},
    {"1", K_DIGIT, '1', 1}, {"2", K_DIGIT, '2', 1}, {"3", K_DIGIT, '3', 1},
    {"+", K_OP, '+', 1},   {"0", K_DIGIT, '0', 2}, {",", K_DOT, 0, 1},
    {"=", K_EQ, 0, 1},
};
#define NKEYS ((int)(sizeof(k_keys) / sizeof(k_keys[0])))

static void key_rect(int i, int *x, int *y, int *w, int *h) {
  int col = 0, row = 0;
  for (int k = 0; k < i; k++) {
    col += k_keys[k].span;
    if (col >= COLS) {
      col = 0;
      row++;
    }
  }
  *x = PAD + col * (BTN_W + GAP);
  *y = DISP_H + PAD + row * (BTN_H + GAP);
  *w = k_keys[i].span * BTN_W + (k_keys[i].span - 1) * GAP;
  *h = BTN_H;
}

// ---- Estado de la calculadora ----
static char g_entry[ENTRY_MAX + 2] = "0"; // lo que se teclea
static int g_typing = 0;      // 1: g_entry es un número a medio escribir
static double g_acc = 0;      // resultado acumulado
static char g_op = 0;         // operación pendiente
static double g_last_rhs = 0; // para repetir '='
static char g_last_op = 0;
static int g_error = 0;
static char g_expr[80] = ""; // línea pequeña: "12 + "

static double entry_value(void) { return strtod(g_entry, NULL); }

static void fmt_num(char *out, size_t cap, double v) {
  if (!isfinite(v)) {
    snprintf(out, cap, "Error");
    return;
  }
  if (v == 0)
    v = 0; // sin -0
  snprintf(out, cap, "%.12g", v);
  if (strlen(out) > ENTRY_MAX)
    snprintf(out, cap, "%.8g", v);
}

static void set_entry_value(double v) {
  fmt_num(g_entry, sizeof(g_entry), v);
  if (!isfinite(v))
    g_error = 1;
  g_typing = 0;
}

static double apply(double a, char op, double b) {
  switch (op) {
  case '+':
    return a + b;
  case '-':
    return a - b;
  case '*':
    return a * b;
  case '/':
    return b == 0 ? NAN : a / b;
  }
  return b;
}

static const char *op_glyph(char op) {
  return op == '*' ? "x" : op == '/' ? "/" : op == '+' ? "+" : "-";
}

static void clear_all(void) {
  snprintf(g_entry, sizeof(g_entry), "0");
  g_typing = 0;
  g_acc = 0;
  g_op = 0;
  g_last_op = 0;
  g_error = 0;
  g_expr[0] = 0;
}

static void press(int kind, char arg) {
  if (g_error && kind != K_AC) {
    if (kind != K_DIGIT && kind != K_DOT)
      return;
    clear_all();
  }
  switch (kind) {
  case K_DIGIT: {
    if (!g_typing) {
      g_entry[0] = 0;
      g_typing = 1;
    }
    size_t n = strlen(g_entry);
    size_t digits = 0;
    for (size_t i = 0; i < n; i++)
      digits += g_entry[i] >= '0' && g_entry[i] <= '9';
    if (digits >= 15)
      break;
    if (!strcmp(g_entry, "0"))
      n = 0;
    else if (!strcmp(g_entry, "-0"))
      n = 1;
    g_entry[n] = arg;
    g_entry[n + 1] = 0;
    break;
  }
  case K_DOT:
    if (!g_typing) {
      snprintf(g_entry, sizeof(g_entry), "0");
      g_typing = 1;
    }
    if (!strchr(g_entry, '.') && strlen(g_entry) < ENTRY_MAX)
      strcat(g_entry, ".");
    break;
  case K_OP:
    if (g_op && g_typing) {
      g_acc = apply(g_acc, g_op, entry_value());
      set_entry_value(g_acc);
    } else if (!g_op || g_typing || g_expr[0] == 0) {
      g_acc = entry_value();
    }
    if (g_error)
      break;
    g_op = arg;
    g_typing = 0;
    g_last_op = 0;
    {
      char a[32];
      fmt_num(a, sizeof(a), g_acc);
      snprintf(g_expr, sizeof(g_expr), "%s %s", a, op_glyph(arg));
    }
    break;
  case K_EQ: {
    double rhs;
    char op;
    if (g_op) {
      rhs = entry_value();
      op = g_op;
    } else if (g_last_op) {
      rhs = g_last_rhs; // '=' repetido: misma operación otra vez
      op = g_last_op;
      g_acc = entry_value();
    } else {
      break;
    }
    char a[32], b[32];
    fmt_num(a, sizeof(a), g_acc);
    fmt_num(b, sizeof(b), rhs);
    snprintf(g_expr, sizeof(g_expr), "%s %s %s =", a, op_glyph(op), b);
    g_acc = apply(g_acc, op, rhs);
    set_entry_value(g_acc);
    g_last_rhs = rhs;
    g_last_op = op;
    g_op = 0;
    break;
  }
  case K_AC:
    clear_all();
    break;
  case K_SIGN:
    if (g_op && !g_typing) { // "5 +" y luego +/-: empieza el segundo número
      snprintf(g_entry, sizeof(g_entry), "-0");
      g_typing = 1;
      break;
    }
    if (g_entry[0] == '-')
      memmove(g_entry, g_entry + 1, strlen(g_entry));
    else if (strcmp(g_entry, "0") && strlen(g_entry) < ENTRY_MAX) {
      memmove(g_entry + 1, g_entry, strlen(g_entry) + 1);
      g_entry[0] = '-';
    }
    if (!g_typing) // sobre un resultado: el resultado cambia de signo
      g_acc = entry_value();
    break;
  case K_PCT: {
    double v = entry_value();
    // Como las de bolsillo: "200 + 10 %" es 10 % de 200; solo, v / 100.
    if (g_op == '+' || g_op == '-')
      v = g_acc * v / 100.0;
    else
      v = v / 100.0;
    set_entry_value(v);
    g_typing = 1; // se puede seguir operando con él
    break;
  }
  }
  g_dirty = 1;
}

static void backspace(void) {
  if (g_error) {
    clear_all();
  } else if (g_typing) {
    size_t n = strlen(g_entry);
    if (n > 1 && !(n == 2 && g_entry[0] == '-'))
      g_entry[n - 1] = 0;
    else
      snprintf(g_entry, sizeof(g_entry), "0");
  }
  g_dirty = 1;
}

// ---- Dibujo ----
static void fill(int x, int y, int w, int h, uint32_t c) {
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > WIN_W) w = WIN_W - x;
  if (y + h > WIN_H) h = WIN_H - y;
  for (int j = 0; j < h; j++) {
    uint32_t *r = &g_px[(y + j) * WIN_W + x];
    for (int i = 0; i < w; i++)
      r[i] = c;
  }
}

static void fill_round(int x, int y, int w, int h, int r, uint32_t c) {
  fill(x + r, y, w - 2 * r, h, c);
  fill(x, y + r, r, h - 2 * r, c);
  fill(x + w - r, y + r, r, h - 2 * r, c);
  for (int j = 0; j < r; j++)
    for (int i = 0; i < r; i++) {
      int dx = r - i, dy = r - j;
      if (dx * dx + dy * dy <= r * r) {
        fill(x + i, y + j, 1, 1, c);
        fill(x + w - 1 - i, y + j, 1, 1, c);
        fill(x + i, y + h - 1 - j, 1, 1, c);
        fill(x + w - 1 - i, y + h - 1 - j, 1, 1, c);
      }
    }
}

static void blend(int xx, int yy, uint32_t col, uint32_t a) {
  if (xx < 0 || yy < 0 || xx >= WIN_W || yy >= WIN_H || !a)
    return;
  uint32_t d = g_px[yy * WIN_W + xx];
  uint32_t r = (((col >> 16) & 0xFF) * a + ((d >> 16) & 0xFF) * (255 - a)) / 255;
  uint32_t g = (((col >> 8) & 0xFF) * a + ((d >> 8) & 0xFF) * (255 - a)) / 255;
  uint32_t b = ((col & 0xFF) * a + (d & 0xFF) * (255 - a)) / 255;
  g_px[yy * WIN_W + xx] = 0xFF000000u | (r << 16) | (g << 8) | b;
}

// Texto escalado (x num/den) con muestreo bilineal del alfa de cada glifo: la
// fuente del sistema es de 14 px y el visor necesita cifras grandes.
static int text_w_scaled(const char *s, int num, int den) {
  int adv = g_font->glyphs['M'].advance;
  return (int)strlen(s) * adv * num / den;
}

static uint32_t glyph_alpha(const glyph_aa_t *g, int i, int j) {
  if (i < 0 || j < 0 || i >= g->width || j >= g->height)
    return 0;
  return g->bitmap[j * g->width + i];
}

static void text_scaled(int x, int y, const char *s, uint32_t col, int num, int den) {
  int adv = g_font->glyphs['M'].advance;
  int base = g_font->height - 3;
  for (int k = 0; s[k]; k++) {
    unsigned char ch = (unsigned char)s[k];
    int ox = x + k * adv * num / den;
    if (ch >= 128)
      continue;
    const glyph_aa_t *g = &g_font->glyphs[ch];
    if (!g->bitmap || !g->width || !g->height)
      continue;
    // Origen del glifo en coordenadas de la fuente sin escalar.
    int gx0 = (adv - g->width) / 2 + g->bearing_x;
    int gy0 = base - g->bearing_y;
    int x0 = ox + gx0 * num / den, y0 = y + gy0 * num / den;
    int w = g->width * num / den + 2, h = g->height * num / den + 2;
    for (int j = 0; j < h; j++)
      for (int i = 0; i < w; i++) {
        // Posición en el glifo (sin escalar) del centro del píxel.
        float fx = ((float)i + 0.5f) * (float)den / (float)num - 0.5f;
        float fy = ((float)j + 0.5f) * (float)den / (float)num - 0.5f;
        int ix = (int)floorf(fx), iy = (int)floorf(fy);
        uint32_t ax = (uint32_t)((fx - (float)ix) * 256.0f);
        uint32_t ay = (uint32_t)((fy - (float)iy) * 256.0f);
        uint32_t a00 = glyph_alpha(g, ix, iy), a10 = glyph_alpha(g, ix + 1, iy);
        uint32_t a01 = glyph_alpha(g, ix, iy + 1), a11 = glyph_alpha(g, ix + 1, iy + 1);
        uint32_t top = (a00 * (256 - ax) + a10 * ax) >> 8;
        uint32_t bot = (a01 * (256 - ax) + a11 * ax) >> 8;
        uint32_t a = (top * (256 - ay) + bot * ay) >> 8;
        blend(x0 + i, y0 + j, col, a > 255 ? 255 : a);
      }
  }
}

static void draw_display(void) {
  // Expresión (pequeña) y número (grande), alineados a la derecha.
  int right = WIN_W - PAD - 6;
  if (g_expr[0])
    text_scaled(right - text_w_scaled(g_expr, 1, 1), PAD + 2, g_expr, P->dim, 1, 1);
  char shown[ENTRY_MAX + 2];
  snprintf(shown, sizeof(shown), "%s", g_entry);
  for (char *c = shown; *c; c++)
    if (*c == '.')
      *c = ','; // coma decimal, como en español
  // Escala que quepa: 3x para números cortos, luego 2,5x, 2x y 1,5x.
  static const int k_num[] = {3, 5, 2, 3}, k_den[] = {1, 2, 1, 2};
  int num = 3, den = 1;
  int avail = WIN_W - 2 * PAD - 12;
  for (int i = 0; i < 4; i++) {
    num = k_num[i];
    den = k_den[i];
    if (text_w_scaled(shown, num, den) <= avail)
      break;
  }
  int th = g_font->height * num / den;
  text_scaled(right - text_w_scaled(shown, num, den), DISP_H - th - 6, shown,
              P->disp_text, num, den);
}

static void draw_keys(void) {
  for (int i = 0; i < NKEYS; i++) {
    int x, y, w, h;
    key_rect(i, &x, &y, &w, &h);
    const calc_key_t *k = &k_keys[i];
    uint32_t bg, fg;
    if (k->kind == K_OP || k->kind == K_EQ) {
      bg = P->op;
      fg = P->op_text;
      if (k->kind == K_OP && g_op == k->arg && !g_typing) {
        bg = P->op_text; // operación pendiente: invertida
        fg = P->op;
      }
    } else if (k->kind == K_AC || k->kind == K_SIGN || k->kind == K_PCT) {
      bg = P->fn;
      fg = P->fn_text;
    } else {
      bg = P->num;
      fg = P->num_text;
    }
    if (i == g_pressed)
      bg = P->pressed;
    fill_round(x, y, w, h, 12, bg);
    const char *lab = k->kind == K_AC && (g_typing && strcmp(g_entry, "0")) ? "C" : k->label;
    int tw = text_w_scaled(lab, 3, 2);
    int th = g_font->height * 3 / 2;
    int lx = k->span > 1 ? x + BTN_W / 2 - tw / 2 : x + (w - tw) / 2;
    text_scaled(lx, y + (h - th) / 2 + 1, lab, fg, 3, 2);
  }
}

static void redraw(void) {
  fill(0, 0, WIN_W, WIN_H, P->bg);
  draw_display();
  draw_keys();
  sys_win_blit(g_win, 0, 0, WIN_W, WIN_H, 0, 0, WIN_W, g_px);
  g_dirty = 0;
}

static int key_at(int x, int y) {
  for (int i = 0; i < NKEYS; i++) {
    int kx, ky, kw, kh;
    key_rect(i, &kx, &ky, &kw, &kh);
    if (x >= kx && x < kx + kw && y >= ky && y < ky + kh)
      return i;
  }
  return -1;
}

// Teclado: scancodes del conjunto 1 (el compositor manda solo pulsaciones).
static void on_scancode(int sc) {
  static const char row_digits[] = {0, 0, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0'};
  if (sc >= 0x02 && sc <= 0x0B) {
    press(K_DIGIT, row_digits[sc]);
    return;
  }
  switch (sc) {
  case 0x47: press(K_DIGIT, '7'); break; // teclado numérico
  case 0x48: press(K_DIGIT, '8'); break;
  case 0x49: press(K_DIGIT, '9'); break;
  case 0x4B: press(K_DIGIT, '4'); break;
  case 0x4C: press(K_DIGIT, '5'); break;
  case 0x4D: press(K_DIGIT, '6'); break;
  case 0x4F: press(K_DIGIT, '1'); break;
  case 0x50: press(K_DIGIT, '2'); break;
  case 0x51: press(K_DIGIT, '3'); break;
  case 0x52: press(K_DIGIT, '0'); break;
  case 0x53: case 0x33: case 0x34: press(K_DOT, 0); break; // , . y supr del num.
  case 0x4E: case 0x1B: press(K_OP, '+'); break;  // + numérico y '+' (ES)
  case 0x4A: case 0x35: press(K_OP, '-'); break;  // - numérico y '-' (ES)
  case 0x37: press(K_OP, '*'); break;             // * numérico
  case 0x1C: case 0x0D: press(K_EQ, 0); break;    // Intro y '='
  case 0x0E: backspace(); break;
  case 0x01: press(K_AC, 0); break;               // Esc
  default: break;
  }
}

int main(void) {
  g_font = font_system_mono();
  if (!g_font)
    return 1;
  g_win = sys_win_create(220, 120, WIN_W, WIN_H + TITLEBAR_H, "Calculadora");
  if (g_win < 0)
    return 1;
  sys_win_set_icon(g_win, "/system/icons/app-calculadora.bmp");
  g_theme_seen = sys_theme_get();
  P = (g_theme_seen & 1) ? &PAL_LIGHT : &PAL_DARK;

  for (;;) {
    winsrv_event_t ev;
    while (sys_win_poll_event(g_win, &ev, 0) > 0) {
      if (ev.type == WINSRV_EV_CLOSE) {
        sys_win_destroy(g_win);
        _exit(0);
      } else if (ev.type == WINSRV_EV_MOUSE) {
        int btn = (int)(ev.data & 0xFF);
        int released = (ev.data >> 8) & 1;
        int motion = (int)((ev.data >> 9) & 1);
        if (btn != 0 || motion)
          continue;
        int k = key_at(ev.x, ev.y);
        if (!released) {
          g_pressed = k;
          g_dirty = 1;
        } else {
          if (k >= 0 && k == g_pressed)
            press(k_keys[k].kind, k_keys[k].arg);
          g_pressed = -1;
          g_dirty = 1;
        }
      } else if (ev.type == WINSRV_EV_KEY) {
        on_scancode(ev.x);
      } else if (ev.type == WINSRV_EV_FOCUS || ev.type == WINSRV_EV_BLUR) {
        g_pressed = -1;
        g_dirty = 1;
      }
    }
    uint32_t th = sys_theme_get();
    if (th != g_theme_seen) {
      g_theme_seen = th;
      P = (th & 1) ? &PAL_LIGHT : &PAL_DARK;
      g_dirty = 1;
    }
    if (g_dirty)
      redraw();
    struct pollfd pfd = {-1, 0, 0};
    poll(&pfd, 1, 30);
  }
  return 0;
}
