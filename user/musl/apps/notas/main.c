// user/musl/apps/notas/main.c
//
// Notas de Aurora OS: un bloc de texto que se guarda solo.
//   - Una nota por fichero: el argumento, o Notas.txt en $HOME (si no se
//     puede escribir allí, en /data y como último recurso en /tmp).
//   - Escritura UTF-8 (tildes y ñ compuestas con user/lib/text_aa.h), ajuste
//     de línea por palabras, cursor con flechas, Inicio/Fin, Re/Av Pág,
//     Supr, Retroceso, Tab, clic para colocar el cursor y rueda para
//     desplazarse.
//   - Guardado automático 1,5 s después del último cambio y al cerrar;
//     Ctrl+S guarda en el momento. Escritura atómica (fichero.tmp + rename).
// Colores sólidos; el amarillo de su icono como color propio; sigue el modo
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
#include "../../../lib/text_aa.h"
#include "../../../syscall.h"
#include <stdlib.h>
#include <time.h>

#undef puts
#undef printf
#include <errno.h>
#include <stdio.h>

#include "../../../lib/font_system.c"

extern void _exit(int status) __attribute__((noreturn));

#define WIN_W 680
#define WIN_H 480
#define TITLEBAR_H 32
#define BAR_H 34
#define MARGIN_X 22
#define MARGIN_Y 14
#define TEXT_MAX (256 * 1024)
#define MAX_VLINES 16384
#define SAVE_DELAY_MS 1500

typedef struct {
  uint32_t bg, bar, line, text, dim, accent, caret;
} palette_t;

static const palette_t PAL_DARK = {0xFF1B1A17, 0xFF24221E, 0xFF34312B,
                                   0xFFEDE9E0, 0xFF9A9486, 0xFFFFC82E,
                                   0xFFFFC82E};
static const palette_t PAL_LIGHT = {0xFFFFFDF7, 0xFFF6F1E4, 0xFFE4DCC8,
                                    0xFF23211C, 0xFF7A7466, 0xFFC99400,
                                    0xFFB58500};
static const palette_t *P = &PAL_DARK;

static uint32_t g_px[WIN_W * WIN_H];
static const font_aa_t *g_font;
static int g_win = -1;
static int g_dirty = 1;
static uint32_t g_theme_seen;
static int g_adv, g_line_h;

// ---- Documento ----
static char g_text[TEXT_MAX + 1];
static int g_len;
static int g_cur;          // cursor (offset en bytes, siempre al inicio de un carácter)
static int g_goal_col = -1; // columna deseada al subir/bajar
static int g_scroll;        // primera línea visual mostrada
static char g_path[256];
static int g_modified;
static uint64_t g_changed_ms;
static char g_status[96];
static uint64_t g_status_ms;
static uint64_t g_blink_ms;
static int g_caret_on = 1;

// Líneas visuales (ajuste de línea): [start, end) en bytes; `hard` = acaba en
// '\n' o en el final del texto (el cursor puede quedarse en `end`).
static int g_vl_start[MAX_VLINES], g_vl_end[MAX_VLINES];
static uint8_t g_vl_hard[MAX_VLINES];
static int g_nvl;

static uint64_t now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

static int text_cols(void) { return (WIN_W - 2 * MARGIN_X) / g_adv; }
static int text_rows(void) { return (WIN_H - BAR_H - 2 * MARGIN_Y) / g_line_h; }

static int cp_count(int a, int b) {
  int n = 0;
  for (int i = a; i < b; i += ui_utf8_len((unsigned char)g_text[i]))
    n++;
  return n;
}

// Offset del carácter número `col` a partir de `a` (sin pasar de `b`).
static int col_to_off(int a, int b, int col) {
  int i = a;
  while (col-- > 0 && i < b)
    i += ui_utf8_len((unsigned char)g_text[i]);
  return i > b ? b : i;
}

static void layout(void) {
  int cols = text_cols();
  g_nvl = 0;
  int i = 0;
  for (;;) {
    int ls = i; // inicio de la línea lógica
    int le = ls;
    while (le < g_len && g_text[le] != '\n')
      le++;
    // Partir [ls, le) en trozos de como mucho `cols` caracteres, cortando
    // tras el último espacio si lo hay.
    int s = ls;
    do {
      int e = s, n = 0, last_space = -1;
      while (e < le && n < cols) {
        if (g_text[e] == ' ')
          last_space = e + 1;
        e += ui_utf8_len((unsigned char)g_text[e]);
        n++;
      }
      int hard = e >= le;
      if (!hard && last_space > s)
        e = last_space;
      if (g_nvl < MAX_VLINES) {
        g_vl_start[g_nvl] = s;
        g_vl_end[g_nvl] = e > le ? le : e;
        g_vl_hard[g_nvl] = (uint8_t)hard;
        g_nvl++;
      }
      s = e;
    } while (s < le);
    if (le >= g_len)
      break;
    i = le + 1; // saltar el '\n'
  }
}

// Línea visual del cursor.
static int cur_vline(void) {
  for (int v = 0; v < g_nvl; v++) {
    if (g_cur < g_vl_start[v])
      continue;
    if (g_cur < g_vl_end[v] || (g_cur == g_vl_end[v] && g_vl_hard[v]))
      return v;
  }
  return g_nvl - 1;
}

static void ensure_visible(void) {
  int v = cur_vline(), rows = text_rows();
  if (v < g_scroll)
    g_scroll = v;
  if (v >= g_scroll + rows)
    g_scroll = v - rows + 1;
  if (g_scroll < 0)
    g_scroll = 0;
}

static void set_status(const char *s) {
  snprintf(g_status, sizeof(g_status), "%s", s);
  g_status_ms = now_ms();
  g_dirty = 1;
}

// ---- Fichero ----
// open() de lib/file.h no acepta modo: crear con openat directamente.
static int xcreate(const char *path, int mode) {
  return (int)syscall(SYS_OPENAT, (uint64_t)(int64_t)AT_FDCWD, (uint64_t)path,
                      (uint64_t)(O_WRONLY | O_CREAT | O_TRUNC), (uint64_t)mode, 0);
}
static int try_dir(const char *dir) {
  if (!dir || !dir[0])
    return 0;
  char probe[256];
  snprintf(probe, sizeof(probe), "%s/.notas-probe", dir);
  int fd = xcreate(probe, 0600);
  if (fd < 0)
    return 0;
  close(fd);
  sys_unlink(probe);
  snprintf(g_path, sizeof(g_path), "%s/Notas.txt", dir);
  return 1;
}

static void choose_path(int argc, char **argv) {
  if (argc > 1 && argv[1][0]) {
    snprintf(g_path, sizeof(g_path), "%s", argv[1]);
    return;
  }
  if (!try_dir(getenv("HOME")) && !try_dir("/data") && !try_dir("/tmp"))
    snprintf(g_path, sizeof(g_path), "/tmp/Notas.txt");
}

static void load(void) {
  int fd = open(g_path, O_RDONLY);
  g_len = 0;
  if (fd >= 0) {
    for (;;) {
      ssize_t n = read(fd, g_text + g_len, TEXT_MAX - g_len);
      if (n <= 0)
        break;
      g_len += (int)n;
    }
    close(fd);
  }
  g_text[g_len] = 0;
  g_cur = g_len;
  g_modified = 0;
}

static int save(void) {
  char tmp[272];
  snprintf(tmp, sizeof(tmp), "%s.tmp", g_path);
  int fd = xcreate(tmp, 0644);
  if (fd < 0) {
    set_status("No se puede guardar");
    return -1;
  }
  int off = 0;
  while (off < g_len) {
    ssize_t n = write(fd, g_text + off, g_len - off);
    if (n <= 0)
      break;
    off += (int)n;
  }
  syscall(74 /* fsync */, (uint64_t)fd, 0, 0, 0, 0);
  close(fd);
  if (off != g_len || sys_rename(tmp, g_path) != 0) {
    sys_unlink(tmp);
    set_status("Error al guardar");
    return -1;
  }
  g_modified = 0;
  set_status("Guardado");
  return 0;
}

// ---- Edición ----
static void changed(void) {
  g_modified = 1;
  g_changed_ms = now_ms();
  g_goal_col = -1;
  layout();
  ensure_visible();
  g_caret_on = 1;
  g_blink_ms = now_ms();
  g_dirty = 1;
}

static void insert_bytes(const char *s, int n) {
  if (g_len + n > TEXT_MAX) {
    set_status("La nota es demasiado larga");
    return;
  }
  memmove(g_text + g_cur + n, g_text + g_cur, g_len - g_cur + 1);
  memcpy(g_text + g_cur, s, n);
  g_len += n;
  g_cur += n;
  changed();
}

static int prev_char(int off) {
  if (off <= 0)
    return 0;
  off--;
  while (off > 0 && ((unsigned char)g_text[off] & 0xC0) == 0x80)
    off--;
  return off;
}

static void delete_range(int a, int b) {
  if (a >= b)
    return;
  memmove(g_text + a, g_text + b, g_len - b + 1);
  g_len -= b - a;
  g_cur = a;
  changed();
}

static void move_to(int off, int keep_goal) {
  g_cur = off < 0 ? 0 : off > g_len ? g_len : off;
  if (!keep_goal)
    g_goal_col = -1;
  ensure_visible();
  g_caret_on = 1;
  g_blink_ms = now_ms();
  g_dirty = 1;
}

static void move_vert(int dv) {
  int v = cur_vline();
  if (g_goal_col < 0)
    g_goal_col = cp_count(g_vl_start[v], g_cur);
  int nv = v + dv;
  if (nv < 0) {
    move_to(0, 1);
    return;
  }
  if (nv >= g_nvl) {
    move_to(g_len, 1);
    return;
  }
  int off = col_to_off(g_vl_start[nv], g_vl_end[nv], g_goal_col);
  // En una línea partida, el final pertenece a la siguiente: quedarse antes.
  if (off == g_vl_end[nv] && !g_vl_hard[nv] && off > g_vl_start[nv])
    off = prev_char(off);
  move_to(off, 1);
}

// Entrada del teclado ya traducida (bytes; flechas como ESC [ x).
static char g_esc[8];
static int g_esc_n;
static char g_u8[4];
static int g_u8_n, g_u8_want;

static void on_escape_seq(const char *seq) {
  int v = cur_vline();
  if (!strcmp(seq, "[A"))
    move_vert(-1);
  else if (!strcmp(seq, "[B"))
    move_vert(1);
  else if (!strcmp(seq, "[C"))
    move_to(g_cur < g_len ? g_cur + ui_utf8_len((unsigned char)g_text[g_cur]) : g_len, 0);
  else if (!strcmp(seq, "[D"))
    move_to(prev_char(g_cur), 0);
  else if (!strcmp(seq, "[H") || !strcmp(seq, "[1~") || !strcmp(seq, "OH"))
    move_to(g_vl_start[v], 0);
  else if (!strcmp(seq, "[F") || !strcmp(seq, "[4~") || !strcmp(seq, "OF")) {
    int e = g_vl_end[v];
    if (!g_vl_hard[v] && e > g_vl_start[v])
      e = prev_char(e);
    move_to(e, 0);
  } else if (!strcmp(seq, "[5~"))
    move_vert(-text_rows());
  else if (!strcmp(seq, "[6~"))
    move_vert(text_rows());
  else if (!strcmp(seq, "[3~") && g_cur < g_len)
    delete_range(g_cur, g_cur + ui_utf8_len((unsigned char)g_text[g_cur]));
}

static void on_byte(unsigned char c) {
  if (g_esc_n > 0) {
    g_esc[g_esc_n++] = (char)c;
    // Fin de secuencia: una letra o '~' tras ESC [ / ESC O.
    if (g_esc_n >= 3 && ((c >= 'A' && c <= 'Z') || c == '~' || c >= 'a')) {
      g_esc[g_esc_n] = 0;
      on_escape_seq(g_esc + 1);
      g_esc_n = 0;
    } else if (g_esc_n == 2 && c != '[' && c != 'O') {
      g_esc_n = 0; // ESC suelto + tecla: se ignora el ESC
      on_byte(c);
    } else if (g_esc_n >= (int)sizeof(g_esc) - 1) {
      g_esc_n = 0;
    }
    return;
  }
  if (g_u8_want) {
    if ((c & 0xC0) != 0x80) {
      g_u8_want = 0; // secuencia rota: descartarla
    } else {
      g_u8[g_u8_n++] = (char)c;
      if (g_u8_n == g_u8_want) {
        insert_bytes(g_u8, g_u8_n);
        g_u8_want = 0;
      }
      return;
    }
  }
  if (c == 0x1B) {
    g_esc[0] = 0x1B;
    g_esc_n = 1;
  } else if (c == '\r' || c == '\n') {
    insert_bytes("\n", 1);
  } else if (c == '\t') {
    insert_bytes("    ", 4);
  } else if (c == 0x7F || c == 0x08) {
    if (g_cur > 0)
      delete_range(prev_char(g_cur), g_cur);
  } else if (c == 0x13) { // Ctrl+S
    save();
  } else if (c >= 0x20 && c < 0x7F) {
    char s = (char)c;
    insert_bytes(&s, 1);
  } else if (c >= 0xC0) {
    g_u8[0] = (char)c;
    g_u8_n = 1;
    g_u8_want = ui_utf8_len(c);
    if (g_u8_want <= 1)
      g_u8_want = 0;
  }
}

static void click_at(int x, int y) {
  if (y < BAR_H + MARGIN_Y - 4)
    return;
  int v = g_scroll + (y - BAR_H - MARGIN_Y) / g_line_h;
  if (v >= g_nvl)
    v = g_nvl - 1;
  if (v < 0)
    v = 0;
  int col = (x - MARGIN_X + g_adv / 2) / g_adv;
  if (col < 0)
    col = 0;
  int off = col_to_off(g_vl_start[v], g_vl_end[v], col);
  if (off == g_vl_end[v] && !g_vl_hard[v] && off > g_vl_start[v])
    off = prev_char(off);
  move_to(off, 0);
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

static void text(int x, int y, const char *s, uint32_t col) {
  ui_draw_text(g_px, WIN_W, WIN_W, WIN_H, x, y, s, col & 0xFFFFFF, g_font);
}

static void redraw(void) {
  fill(0, 0, WIN_W, WIN_H, P->bg);
  // Barra: nombre del fichero, estado y ayuda.
  fill(0, 0, WIN_W, BAR_H, P->bar);
  fill(0, BAR_H - 1, WIN_W, 1, P->line);
  fill(MARGIN_X - 8, 10, 4, BAR_H - 20, P->accent);
  const char *base = strrchr(g_path, '/');
  base = base ? base + 1 : g_path;
  int ty = (BAR_H - g_font->height) / 2;
  int x = ui_draw_text(g_px, WIN_W, WIN_W, WIN_H, MARGIN_X, ty, base, P->text & 0xFFFFFF, g_font);
  char st[128];
  uint64_t t = now_ms();
  if (g_status[0] && t - g_status_ms < 2500)
    snprintf(st, sizeof(st), "  %s", g_status);
  else
    snprintf(st, sizeof(st), "  %s", g_modified ? "Editado" : "");
  text(x, ty, st, P->dim);
  char info[64];
  snprintf(info, sizeof(info), "%d palabras   Ctrl+S guarda", 0);
  {
    int words = 0, in = 0;
    for (int i = 0; i < g_len; i++) {
      int sp = g_text[i] == ' ' || g_text[i] == '\n' || g_text[i] == '\t';
      if (!sp && !in)
        words++;
      in = !sp;
    }
    snprintf(info, sizeof(info), "%d palabra%s   Ctrl+S guarda", words, words == 1 ? "" : "s");
  }
  text(WIN_W - MARGIN_X - ui_text_width(g_font, info), ty, info, P->dim);

  // Texto.
  int rows = text_rows();
  int cv = cur_vline();
  for (int r = 0; r < rows && g_scroll + r < g_nvl; r++) {
    int v = g_scroll + r;
    int y = BAR_H + MARGIN_Y + r * g_line_h;
    ui_draw_text_n(g_px, WIN_W, WIN_W, WIN_H, MARGIN_X, y, g_text + g_vl_start[v],
                   g_vl_end[v] - g_vl_start[v], P->text & 0xFFFFFF, g_font);
    if (v == cv && g_caret_on) {
      int cx = MARGIN_X + cp_count(g_vl_start[v], g_cur) * g_adv;
      fill(cx - 1, y + 1, 2, g_line_h - 2, P->caret);
    }
  }
  if (g_len == 0)
    text(MARGIN_X + 6, BAR_H + MARGIN_Y, "Escribe tu nota...", P->dim);

  // Barra de desplazamiento fina si no cabe todo.
  if (g_nvl > rows) {
    int track = WIN_H - BAR_H - 8;
    int h = track * rows / g_nvl;
    if (h < 24)
      h = 24;
    int y = BAR_H + 4 + (track - h) * g_scroll / (g_nvl - rows);
    fill(WIN_W - 6, y, 3, h, P->line);
  }
  sys_win_blit(g_win, 0, 0, WIN_W, WIN_H, 0, 0, WIN_W, g_px);
  g_dirty = 0;
}

int main(int argc, char **argv) {
  g_font = font_system_mono();
  if (!g_font)
    return 1;
  g_adv = g_font->glyphs['M'].advance;
  g_line_h = g_font->height + 6;
  choose_path(argc, argv);
  load();
  layout();
  ensure_visible();

  g_win = sys_win_create(180, 100, WIN_W, WIN_H + TITLEBAR_H, "Notas");
  if (g_win < 0)
    return 1;
  sys_win_set_icon(g_win, "/system/icons/app-notas.bmp");
  g_theme_seen = sys_theme_get();
  P = (g_theme_seen & 1) ? &PAL_LIGHT : &PAL_DARK;
  g_blink_ms = now_ms();

  for (;;) {
    winsrv_event_t ev;
    while (sys_win_poll_event(g_win, &ev, 0) > 0) {
      if (ev.type == WINSRV_EV_CLOSE) {
        if (g_modified)
          save();
        sys_win_destroy(g_win);
        _exit(0);
      } else if (ev.type == WINSRV_EV_TTY_INPUT) {
        on_byte((unsigned char)ev.x);
      } else if (ev.type == WINSRV_EV_MOUSE) {
        int btn = (int)(ev.data & 0xFF);
        int released = (ev.data >> 8) & 1;
        int motion = (int)((ev.data >> 9) & 1);
        if (btn == 64 || btn == 65) {
          int max = g_nvl - text_rows();
          g_scroll += btn == 64 ? -3 : 3;
          if (g_scroll > max)
            g_scroll = max;
          if (g_scroll < 0)
            g_scroll = 0;
          g_dirty = 1;
        } else if (btn == 0 && !released && !motion) {
          click_at(ev.x, ev.y);
        }
      } else if (ev.type == WINSRV_EV_FOCUS || ev.type == WINSRV_EV_BLUR) {
        g_dirty = 1;
      }
    }
    uint64_t t = now_ms();
    uint32_t th = sys_theme_get();
    if (th != g_theme_seen) {
      g_theme_seen = th;
      P = (th & 1) ? &PAL_LIGHT : &PAL_DARK;
      g_dirty = 1;
    }
    if (g_modified && t - g_changed_ms >= SAVE_DELAY_MS)
      save();
    if (t - g_blink_ms >= 530) {
      g_blink_ms = t;
      g_caret_on = !g_caret_on;
      g_dirty = 1;
    }
    if (g_status[0] && t - g_status_ms >= 2500 && t - g_status_ms < 2600)
      g_dirty = 1;
    if (g_dirty)
      redraw();
    struct pollfd pfd = {-1, 0, 0};
    poll(&pfd, 1, 30);
  }
  return 0;
}
