// user/musl/apps/taskmgr/main.c
//
// Monitor de rendimiento / administrador de tareas de Aurora OS.
//   - Pestaña "Procesos": lista (ASYS_PROCLIST) con CPU, estado y memoria
//     virtual; selecciona uno y pulsa "Finalizar" (SIGKILL).
//   - Pestaña "Rendimiento": uso de CPU (total y por núcleo) y memoria, con
//     historial (ASYS_SYSINFO).
// Colores sólidos, verde aurora como acento; sigue el modo claro/oscuro
// (sys_theme_get).

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
#include <stdlib.h>
#include <time.h>

#undef puts
#undef printf

#include "../../../lib/font_system.c"

extern void _exit(int status) __attribute__((noreturn));

#define WIN_W 760
#define WIN_H 520
#define TITLEBAR_H 32
#define HIST_N 60
#define MAX_PROCS 256
#define ROW_H 24
#define LIST_Y 92
#define LIST_BOTTOM (WIN_H - 52)

typedef struct {
  uint32_t bg, card, line, text, dim, accent, accent_text, graph, danger;
} palette_t;

static const palette_t PAL_DARK = {0xFF0E1412, 0xFF16201C, 0xFF263530,
                                   0xFFE8F2EE, 0xFF8FA39B, 0xFF3EE89B,
                                   0xFF04261A, 0xFF3EE89B, 0xFFFF6A3D};
static const palette_t PAL_LIGHT = {0xFFF4F8F6, 0xFFFFFFFF, 0xFFD2E0D9,
                                    0xFF14201B, 0xFF5B6E66, 0xFF3EE89B,
                                    0xFF04261A, 0xFF1FA56F, 0xFFD9481F};
static const palette_t *P = &PAL_DARK;

static uint32_t g_px[WIN_W * WIN_H];
static const font_aa_t *g_font;
static int g_win = -1;
static int g_tab = 0; // 0 procesos, 1 rendimiento
static int g_dirty = 1;
static uint32_t g_theme_seen;

// ---- Muestras ----
static aurora_sysinfo_t g_si, g_si_prev;
static int g_have_prev;
static uint8_t g_cpu_hist[HIST_N];
static uint8_t g_mem_hist[HIST_N];
static int g_cpu_pct_core[AURORA_SYSINFO_MAX_CPUS];
static int g_cpu_pct_total;

typedef struct {
  aurora_procinfo_t p;
  int cpu_pct; // % de un núcleo
} proc_row_t;
static proc_row_t g_rows[MAX_PROCS];
static int g_nrows;
static aurora_procinfo_t g_prev_procs[MAX_PROCS];
static int g_nprev;
static uint64_t g_prev_proc_ms;
static int g_selected_pid = -1;
static int g_scroll;

static uint64_t now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
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
  for (int j = 0; j < r; j++) {
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
}

static int text_w(const char *s) {
  int w = 0, adv = g_font ? g_font->glyphs['M'].advance : 8;
  for (; *s; s++)
    w += adv;
  return w;
}

static void text(int x, int y, const char *s, uint32_t col) {
  if (!g_font)
    return;
  int adv = g_font->glyphs['M'].advance;
  int baseline = y + g_font->height - 3;
  uint32_t sr = (col >> 16) & 0xFF, sg = (col >> 8) & 0xFF, sb = col & 0xFF;
  for (; *s; s++, x += adv) {
    unsigned char ch = (unsigned char)*s;
    if (ch >= 128)
      continue;
    const glyph_aa_t *g = &g_font->glyphs[ch];
    if (!g->bitmap || !g->width || !g->height)
      continue;
    int gx = x + (adv - g->width) / 2 + g->bearing_x;
    int gy = baseline - g->bearing_y;
    for (int j = 0; j < g->height; j++) {
      int yy = gy + j;
      if (yy < 0 || yy >= WIN_H)
        continue;
      for (int i = 0; i < g->width; i++) {
        int xx = gx + i;
        if (xx < 0 || xx >= WIN_W)
          continue;
        uint32_t a = g->bitmap[j * g->width + i];
        if (!a)
          continue;
        uint32_t d = g_px[yy * WIN_W + xx];
        uint32_t r = (sr * a + ((d >> 16) & 0xFF) * (255 - a)) / 255;
        uint32_t gg = (sg * a + ((d >> 8) & 0xFF) * (255 - a)) / 255;
        uint32_t b = (sb * a + (d & 0xFF) * (255 - a)) / 255;
        g_px[yy * WIN_W + xx] = 0xFF000000u | (r << 16) | (gg << 8) | b;
      }
    }
  }
}

static void text_right(int xr, int y, const char *s, uint32_t col) {
  text(xr - text_w(s), y, s, col);
}

// Gráfico de historial: barras finas (0..100) sobre una cuadrícula.
static void graph(int x, int y, int w, int h, const uint8_t *hist, int head) {
  fill_round(x, y, w, h, 6, P->bg);
  for (int k = 1; k < 4; k++)
    fill(x, y + h * k / 4, w, 1, P->line);
  int bw = (w - 8) / HIST_N;
  if (bw < 1)
    bw = 1;
  for (int i = 0; i < HIST_N; i++) {
    int v = hist[(head + i) % HIST_N];
    int bh = (h - 8) * v / 100;
    if (bh < 1 && v > 0)
      bh = 1;
    fill(x + 4 + i * bw, y + h - 4 - bh, bw > 1 ? bw - 1 : 1, bh, P->graph);
  }
}

static void bar(int x, int y, int w, int h, int pct) {
  fill_round(x, y, w, h, h / 2, P->line);
  int fw = w * pct / 100;
  if (fw > h)
    fill_round(x, y, fw, h, h / 2, P->accent);
  else if (pct > 0)
    fill_round(x, y, h, h, h / 2, P->accent);
}

// ---- Muestreo ----
static int g_hist_head;

static void sample_sysinfo(void) {
  if (sys_sysinfo(&g_si) < 0)
    return;
  uint64_t tb = 0, ti = 0;
  if (g_have_prev) {
    for (uint32_t c = 0; c < g_si.ncpu; c++) {
      uint64_t db = g_si.cpu_busy[c] - g_si_prev.cpu_busy[c];
      uint64_t di = g_si.cpu_idle[c] - g_si_prev.cpu_idle[c];
      g_cpu_pct_core[c] = (db + di) ? (int)(db * 100 / (db + di)) : 0;
      tb += db;
      ti += di;
    }
    g_cpu_pct_total = (tb + ti) ? (int)(tb * 100 / (tb + ti)) : 0;
  }
  g_si_prev = g_si;
  g_have_prev = 1;
  g_cpu_hist[g_hist_head] = (uint8_t)g_cpu_pct_total;
  int mp = g_si.mem_total_kb
               ? (int)((g_si.mem_total_kb - g_si.mem_free_kb) * 100 /
                       g_si.mem_total_kb)
               : 0;
  g_mem_hist[g_hist_head] = (uint8_t)mp;
  g_hist_head = (g_hist_head + 1) % HIST_N;
}

static int cmp_rows(const void *a, const void *b) {
  const proc_row_t *x = a, *y = b;
  if (x->cpu_pct != y->cpu_pct)
    return y->cpu_pct - x->cpu_pct;
  return (int)x->p.pid - (int)y->p.pid;
}

static void sample_procs(void) {
  static aurora_procinfo_t cur[MAX_PROCS];
  int n = sys_proclist(cur, MAX_PROCS);
  if (n < 0)
    return;
  uint64_t t = now_ms();
  uint64_t dt = g_prev_proc_ms ? t - g_prev_proc_ms : 0;
  g_nrows = n;
  for (int i = 0; i < n; i++) {
    g_rows[i].p = cur[i];
    g_rows[i].cpu_pct = 0;
    if (dt > 0) {
      for (int j = 0; j < g_nprev; j++) {
        if (g_prev_procs[j].pid == cur[i].pid) {
          uint64_t d = cur[i].cpu_ticks - g_prev_procs[j].cpu_ticks;
          g_rows[i].cpu_pct = (int)(d * 100 / dt);
          break;
        }
      }
    }
  }
  for (int i = 0; i < n; i++)
    g_prev_procs[i] = cur[i];
  g_nprev = n;
  g_prev_proc_ms = t;
  qsort(g_rows, (size_t)n, sizeof(proc_row_t), cmp_rows);
}

// ---- Interfaz ----
static void fmt_kb(char *out, size_t cap, uint64_t kb) {
  if (kb >= 1024 * 1024)
    snprintf(out, cap, "%u.%u GB", (unsigned)(kb / (1024 * 1024)),
             (unsigned)((kb % (1024 * 1024)) * 10 / (1024 * 1024)));
  else if (kb >= 1024)
    snprintf(out, cap, "%u MB", (unsigned)(kb / 1024));
  else
    snprintf(out, cap, "%u KB", (unsigned)kb);
}

#define TAB_Y 12
#define TAB_H 30
static const int k_tab_x[2] = {16, 142};
static const int k_tab_w[2] = {116, 140};
#define BTN_W 170
#define BTN_H 30
#define BTN_X (WIN_W - 16 - BTN_W)
#define BTN_Y (WIN_H - 44)

static void draw_tabs(void) {
  static const char *names[2] = {"Procesos", "Rendimiento"};
  for (int i = 0; i < 2; i++) {
    int on = (i == g_tab);
    fill_round(k_tab_x[i], TAB_Y, k_tab_w[i], TAB_H, 8, on ? P->accent : P->card);
    int tw = text_w(names[i]);
    text(k_tab_x[i] + (k_tab_w[i] - tw) / 2, TAB_Y + (TAB_H - g_font->height) / 2,
         names[i], on ? P->accent_text : P->text);
  }
}

static int visible_rows(void) { return (LIST_BOTTOM - LIST_Y) / ROW_H; }

static void draw_procs(void) {
  fill_round(16, 56, WIN_W - 32, WIN_H - 56 - 60, 10, P->card);
  int hy = 62;
  text(32, hy, "PID", P->dim);
  text(100, hy, "Nombre", P->dim);
  text(330, hy, "Estado", P->dim);
  text_right(540, hy, "CPU", P->dim);
  text_right(WIN_W - 32, hy, "Memoria virt.", P->dim);
  fill(28, LIST_Y - 6, WIN_W - 56, 1, P->line);

  int vis = visible_rows();
  if (g_scroll > g_nrows - vis)
    g_scroll = g_nrows - vis;
  if (g_scroll < 0)
    g_scroll = 0;
  for (int i = 0; i < vis && g_scroll + i < g_nrows; i++) {
    const proc_row_t *r = &g_rows[g_scroll + i];
    int y = LIST_Y + i * ROW_H;
    int sel = ((int)r->p.pid == g_selected_pid);
    uint32_t fg = P->text;
    if (sel) {
      fill_round(24, y - 2, WIN_W - 48, ROW_H - 2, 6, P->accent);
      fg = P->accent_text;
    }
    char b[48];
    snprintf(b, sizeof(b), "%u", (unsigned)r->p.pid);
    text(32, y + 2, b, fg);
    char nm[17];
    memcpy(nm, r->p.name, 16);
    nm[16] = 0;
    text(100, y + 2, nm[0] ? nm : "?", fg);
    const char *st = r->p.state == 'R'   ? "Activo"
                     : r->p.state == 'S' ? "En espera"
                     : r->p.state == 'T' ? "Detenido"
                     : r->p.state == 'Z' ? "Zombi"
                                         : "?";
    text(330, y + 2, st, sel ? fg : P->dim);
    snprintf(b, sizeof(b), "%d%%", r->cpu_pct);
    text_right(540, y + 2, b, fg);
    fmt_kb(b, sizeof(b), r->p.vsize_kb);
    text_right(WIN_W - 32, y + 2, b, fg);
  }

  char b[64];
  snprintf(b, sizeof(b), "%d procesos", g_nrows);
  text(20, BTN_Y + (BTN_H - g_font->height) / 2, b, P->dim);
  int can = g_selected_pid > 1;
  fill_round(BTN_X, BTN_Y, BTN_W, BTN_H, 8, can ? P->danger : P->card);
  const char *lbl = "Finalizar proceso";
  text(BTN_X + (BTN_W - text_w(lbl)) / 2, BTN_Y + (BTN_H - g_font->height) / 2,
       lbl, can ? 0xFF1A0A05 : P->dim);
}

static void draw_perf(void) {
  char b[96];
  // Tarjeta CPU
  fill_round(16, 56, WIN_W - 32, 232, 10, P->card);
  text(32, 68, "CPU", P->text);
  snprintf(b, sizeof(b), "%d%%", g_cpu_pct_total);
  text_right(WIN_W - 32, 68, b, P->accent == P->accent ? P->graph : P->graph);
  graph(32, 96, WIN_W - 64, 110, g_cpu_hist, g_hist_head);
  // Barras por núcleo (hasta 16, en 2 filas de 8)
  int nc = (int)g_si.ncpu;
  if (nc > 16)
    nc = 16;
  int per_row = nc > 8 ? 8 : (nc > 0 ? nc : 1);
  int colw = (WIN_W - 64) / per_row;
  for (int c = 0; c < nc; c++) {
    int cx = 32 + (c % per_row) * colw;
    int cy = 216 + (c / per_row) * 34;
    snprintf(b, sizeof(b), "N%d %d%%", c, g_cpu_pct_core[c]);
    text(cx, cy, b, P->dim);
    bar(cx, cy + g_font->height + 2, colw - 14, 6, g_cpu_pct_core[c]);
  }
  // Tarjeta memoria
  fill_round(16, 300, WIN_W - 32, 150, 10, P->card);
  text(32, 312, "Memoria", P->text);
  uint64_t used = g_si.mem_total_kb - g_si.mem_free_kb;
  char u[24], t[24];
  fmt_kb(u, sizeof(u), used);
  fmt_kb(t, sizeof(t), g_si.mem_total_kb);
  snprintf(b, sizeof(b), "%s / %s", u, t);
  text_right(WIN_W - 32, 312, b, P->graph);
  graph(32, 340, WIN_W - 64, 70, g_mem_hist, g_hist_head);
  int mp = g_si.mem_total_kb ? (int)(used * 100 / g_si.mem_total_kb) : 0;
  bar(32, 422, WIN_W - 64, 8, mp);
  // Pie
  uint64_t s = g_si.uptime_ms / 1000;
  snprintf(b, sizeof(b), "Tiempo activo %u:%02u:%02u   Procesos %d   Nucleos %u",
           (unsigned)(s / 3600), (unsigned)((s / 60) % 60), (unsigned)(s % 60),
           g_nrows, (unsigned)g_si.ncpu);
  text(24, WIN_H - 36, b, P->dim);
}

static void redraw(void) {
  fill(0, 0, WIN_W, WIN_H, P->bg);
  draw_tabs();
  if (g_tab == 0)
    draw_procs();
  else
    draw_perf();
  sys_win_blit(g_win, 0, 0, WIN_W, WIN_H, 0, 0, WIN_W, g_px);
  g_dirty = 0;
}

static void on_click(int x, int y) {
  for (int i = 0; i < 2; i++) {
    if (x >= k_tab_x[i] && x < k_tab_x[i] + k_tab_w[i] && y >= TAB_Y &&
        y < TAB_Y + TAB_H) {
      if (g_tab != i) {
        g_tab = i;
        g_dirty = 1;
      }
      return;
    }
  }
  if (g_tab != 0)
    return;
  if (x >= BTN_X && x < BTN_X + BTN_W && y >= BTN_Y && y < BTN_Y + BTN_H) {
    if (g_selected_pid > 1 && g_selected_pid != (int)getpid()) {
      kill(g_selected_pid, SIGKILL);
      g_selected_pid = -1;
      g_prev_proc_ms = 0;
      sample_procs();
      g_dirty = 1;
    }
    return;
  }
  if (y >= LIST_Y - 2 && y < LIST_BOTTOM && x >= 24 && x < WIN_W - 24) {
    int idx = g_scroll + (y - (LIST_Y - 2)) / ROW_H;
    if (idx < g_nrows) {
      g_selected_pid = (int)g_rows[idx].p.pid;
      g_dirty = 1;
    }
  }
}

int main(void) {
  g_font = font_system_mono();
  if (!g_font)
    return 1;
  g_win = sys_win_create(140, 90, WIN_W, WIN_H + TITLEBAR_H,
                         "Monitor de rendimiento");
  if (g_win < 0)
    return 1;
  sys_win_set_icon(g_win, "/system/icons/taskmgr.bmp");

  g_theme_seen = sys_theme_get();
  P = (g_theme_seen & 1) ? &PAL_LIGHT : &PAL_DARK;

  uint64_t last = 0;
  sample_sysinfo();
  sample_procs();
  for (;;) {
    winsrv_event_t ev;
    while (sys_win_poll_event(g_win, &ev, 0) > 0) {
      if (ev.type == WINSRV_EV_CLOSE) {
        sys_win_destroy(g_win);
        _exit(0);
      } else if (ev.type == WINSRV_EV_MOUSE) {
        int btn = (int)(ev.data & 0xFF);
        int pressed = ((ev.data >> 8) & 1) == 0;
        int motion = (int)((ev.data >> 9) & 1);
        if (btn == 64 || btn == 65) {
          g_scroll += btn == 64 ? -3 : 3;
          g_dirty = 1;
        } else if (btn == 0 && pressed && !motion) {
          on_click(ev.x, ev.y);
        }
      } else if (ev.type == WINSRV_EV_FOCUS) {
        g_dirty = 1;
      }
    }

    uint32_t th = sys_theme_get();
    if (th != g_theme_seen) {
      g_theme_seen = th;
      P = (th & 1) ? &PAL_LIGHT : &PAL_DARK;
      g_dirty = 1;
    }

    uint64_t t = now_ms();
    if (t - last >= 1000) {
      last = t;
      sample_sysinfo();
      sample_procs();
      g_dirty = 1;
    }
    if (g_dirty)
      redraw();

    struct pollfd pfd = {-1, 0, 0};
    poll(&pfd, 1, 30);
  }
  return 0;
}
