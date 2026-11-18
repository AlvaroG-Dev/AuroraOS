// user/musl/apps/archivos/main.c
//
// Archivos de Aurora OS: explorador de carpetas.
//   - Barra lateral con lugares (Inicio, Datos, Sistema, Programas, Temporal).
//   - Lista de la carpeta: carpetas primero y por nombre, con tamaño y tipo;
//     los ocultos (".x") no se muestran.
//   - Clic selecciona; doble clic o Intro abre: una carpeta entra en ella y un
//     fichero de texto se abre en Notas y una imagen en Imágenes. Retroceso
//     o el botón ^ suben un nivel; < vuelve a la carpeta anterior. Flechas
//     y rueda para moverse.
// Colores sólidos; el azul de su icono como color propio; sigue el modo
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
#include "../../../lib/image_dec.h"
#include "../../../lib/text_aa.h"
#include "../../../syscall.h"
#include <stdlib.h>
#include <time.h>

#undef puts
#undef printf
#include <stdio.h>

#include "../../../lib/font_system.c"

extern void _exit(int status) __attribute__((noreturn));
char *strstr(const char *haystack, const char *needle);

#define WIN_W 780
#define WIN_H 500
#define TITLEBAR_H 32
#define SIDE_W 168
#define TOP_H 44
#define HEAD_H 26
#define ROW_H 26
#define MAX_ENTRIES 2048
#define PATH_MAX_A 512

typedef struct {
  uint32_t bg, side, line, text, dim, accent, accent_text, sel, sel_text, folder, file;
} palette_t;

static const palette_t PAL_DARK = {0xFF15181D, 0xFF1C2027, 0xFF2A303A, 0xFFE6EAF0,
                                   0xFF8B94A3, 0xFF3D8BFF, 0xFFFFFFFF, 0xFF24426E,
                                   0xFFFFFFFF, 0xFF3D8BFF, 0xFF8B94A3};
static const palette_t PAL_LIGHT = {0xFFFFFFFF, 0xFFF2F4F7, 0xFFE1E5EB, 0xFF1A1F27,
                                    0xFF667085, 0xFF2F78F0, 0xFFFFFFFF, 0xFFD6E6FF,
                                    0xFF0F2A55, 0xFF3D8BFF, 0xFF98A2B3};
static const palette_t *P = &PAL_DARK;

static uint32_t g_px[WIN_W * WIN_H];
static const font_aa_t *g_font;
static int g_win = -1;
static int g_dirty = 1;
static uint32_t g_theme_seen;

typedef struct {
  char name[128];
  int is_dir;
  int64_t size;
} entry_t;

static entry_t g_ent[MAX_ENTRIES];
static int g_n;
static int g_sel = -1;
static int g_scroll;
static char g_cwd[PATH_MAX_A] = "/";
static char g_back[8][PATH_MAX_A];
static int g_nback;
static char g_msg[128];
static uint64_t g_last_click_ms;
static int g_last_click_row = -1;

typedef struct {
  const char *label;
  char path[PATH_MAX_A];
} place_t;
static place_t g_places[] = {
    {"Inicio", ""},          {"Datos", "/data"},   {"Sistema", "/"},
    {"Programas", "/usr/bin"}, {"Temporal", "/tmp"},
};
#define NPLACES ((int)(sizeof(g_places) / sizeof(g_places[0])))

static uint64_t now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

static int list_top(void) { return TOP_H + HEAD_H; }
static int visible_rows(void) { return (WIN_H - list_top() - 28) / ROW_H; }

// ---- Carpetas ----
struct linux_dirent64 {
  uint64_t d_ino;
  int64_t d_off;
  unsigned short d_reclen;
  unsigned char d_type;
  char d_name[];
};

static int name_cmp(const char *a, const char *b) {
  for (;; a++, b++) {
    int ca = (unsigned char)*a, cb = (unsigned char)*b;
    if (ca >= 'A' && ca <= 'Z')
      ca += 32;
    if (cb >= 'A' && cb <= 'Z')
      cb += 32;
    if (ca != cb || !ca)
      return ca - cb;
  }
}

static int ent_cmp(const void *pa, const void *pb) {
  const entry_t *a = pa, *b = pb;
  if (a->is_dir != b->is_dir)
    return b->is_dir - a->is_dir;
  return name_cmp(a->name, b->name);
}

static void join(char *out, size_t cap, const char *dir, const char *name) {
  if (!strcmp(dir, "/"))
    snprintf(out, cap, "/%s", name);
  else
    snprintf(out, cap, "%s/%s", dir, name);
}

static int stat_path(const char *path, stat_t *st) {
  return (int)syscall(SYS_FSTATAT, (uint64_t)(int64_t)AT_FDCWD, (uint64_t)path,
                      (uint64_t)st, 0, 0);
}

static int load_dir(const char *path) {
  int fd = (int)syscall(SYS_OPENAT, (uint64_t)(int64_t)AT_FDCWD, (uint64_t)path,
                        (uint64_t)(O_RDONLY | 0x10000 /* O_DIRECTORY */), 0, 0);
  if (fd < 0) {
    snprintf(g_msg, sizeof(g_msg), "No se puede abrir %s", path);
    g_dirty = 1;
    return -1;
  }
  static char buf[16384];
  g_n = 0;
  for (;;) {
    long n = syscall(SYS_GETDENTS64, (uint64_t)fd, (uint64_t)buf, sizeof(buf), 0, 0);
    if (n <= 0)
      break;
    for (long off = 0; off < n;) {
      struct linux_dirent64 *d = (struct linux_dirent64 *)(buf + off);
      off += d->d_reclen;
      if (d->d_name[0] == '.' || g_n >= MAX_ENTRIES)
        continue;
      entry_t *e = &g_ent[g_n];
      snprintf(e->name, sizeof(e->name), "%s", d->d_name);
      char full[PATH_MAX_A];
      join(full, sizeof(full), path, d->d_name);
      stat_t st;
      e->size = -1;
      e->is_dir = d->d_type == 4;
      if (stat_path(full, &st) == 0) {
        e->is_dir = (st.st_mode & 0170000) == 0040000;
        e->size = st.st_size;
      }
      g_n++;
    }
  }
  close(fd);
  qsort(g_ent, g_n, sizeof(entry_t), ent_cmp);
  snprintf(g_cwd, sizeof(g_cwd), "%s", path);
  g_sel = g_n > 0 ? 0 : -1;
  g_scroll = 0;
  g_msg[0] = 0;
  g_dirty = 1;
  return 0;
}

static void go(const char *path, int push_back) {
  char prev[PATH_MAX_A];
  snprintf(prev, sizeof(prev), "%s", g_cwd);
  if (load_dir(path) == 0 && push_back && strcmp(prev, g_cwd)) {
    if (g_nback == 8) {
      memmove(g_back[0], g_back[1], sizeof(g_back[0]) * 7);
      g_nback = 7;
    }
    snprintf(g_back[g_nback++], PATH_MAX_A, "%s", prev);
  }
}

static void go_up(void) {
  if (!strcmp(g_cwd, "/"))
    return;
  char up[PATH_MAX_A];
  snprintf(up, sizeof(up), "%s", g_cwd);
  char *s = strrchr(up, '/');
  if (s == up)
    up[1] = 0;
  else if (s)
    *s = 0;
  go(up, 1);
}

static void go_back(void) {
  if (g_nback > 0) {
    g_nback--;
    go(g_back[g_nback], 0);
  }
}

static const char *ext_of(const char *name) {
  const char *d = strrchr(name, '.');
  return d && d != name ? d + 1 : "";
}

static int is_text_name(const char *name) {
  static const char *const k[] = {"txt", "md", "c", "h", "sh", "conf", "cfg", "log",
                                  "ini", "json", "py", "cpp", "hpp", "mk", "desktop",
                                  "service", "xml", "html", "css", "js", "csv", ""};
  const char *e = ext_of(name);
  for (unsigned i = 0; i < sizeof(k) / sizeof(k[0]); i++)
    if (!name_cmp(e, k[i]))
      return 1;
  return !strcmp(name, "Makefile") || !strcmp(name, "README");
}

static const char *type_of(const entry_t *e) {
  if (e->is_dir)
    return "Carpeta";
  const char *x = ext_of(e->name);
  if (!name_cmp(x, "txt") || !name_cmp(x, "md"))
    return "Texto";
  if (img_is_image_name(e->name))
    return "Imagen";
  if (!name_cmp(x, "c") || !name_cmp(x, "h") || !name_cmp(x, "py") || !name_cmp(x, "sh"))
    return "Código";
  if (!name_cmp(x, "kur") || !name_cmp(x, "aur"))
    return "Driver";
  if (!name_cmp(x, "so") || strstr(e->name, ".so."))
    return "Biblioteca";
  if (!x[0])
    return "Documento";
  return "Fichero";
}

static int g_children; // Notas abiertas desde aquí, por recoger

static void reap(void) {
  int st;
  while (g_children > 0 && waitpid(-1, &st, WNOHANG) > 0)
    g_children--;
}

static void open_entry(int i) {
  if (i < 0 || i >= g_n)
    return;
  char full[PATH_MAX_A];
  join(full, sizeof(full), g_cwd, g_ent[i].name);
  if (g_ent[i].is_dir) {
    go(full, 1);
    return;
  }
  if (img_is_image_name(g_ent[i].name)) {
    char *argv[] = {"/usr/bin/imagenes", full, NULL};
    if (spawn_args("/usr/bin/imagenes", argv, 2) < 0)
      snprintf(g_msg, sizeof(g_msg), "No se pudo abrir Imágenes");
    else if (++g_children)
      snprintf(g_msg, sizeof(g_msg), "Abriendo %s en Imágenes", g_ent[i].name);
  } else if (is_text_name(g_ent[i].name) && g_ent[i].size < 256 * 1024) {
    char *argv[] = {"/usr/bin/notas", full, NULL};
    if (spawn_args("/usr/bin/notas", argv, 2) < 0)
      snprintf(g_msg, sizeof(g_msg), "No se pudo abrir Notas");
    else if (++g_children)
      snprintf(g_msg, sizeof(g_msg), "Abriendo %s en Notas", g_ent[i].name);
  } else {
    snprintf(g_msg, sizeof(g_msg), "No hay ninguna app para abrir %s", g_ent[i].name);
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

static int text(int x, int y, const char *s, uint32_t col) {
  return ui_draw_text(g_px, WIN_W, WIN_W, WIN_H, x, y, s, col & 0xFFFFFF, g_font);
}

// Texto recortado a `maxw` píxeles con "..." al final.
static void text_fit(int x, int y, const char *s, int maxw, uint32_t col) {
  if (ui_text_width(g_font, s) <= maxw) {
    text(x, y, s, col);
    return;
  }
  char buf[256];
  int n = (int)strlen(s);
  if (n > 250)
    n = 250;
  while (n > 0) {
    n--;
    while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80)
      n--;
    snprintf(buf, sizeof(buf), "%.*s...", n, s);
    if (ui_text_width(g_font, buf) <= maxw)
      break;
  }
  text(x, y, buf, col);
}

static void icon_folder(int x, int y, uint32_t c) {
  fill_round(x, y + 1, 8, 4, 1, c);
  fill_round(x, y + 3, 16, 11, 2, c);
}

static void icon_file(int x, int y, uint32_t c, uint32_t bg) {
  fill_round(x + 2, y, 12, 15, 2, c);
  fill(x + 4, y + 5, 8, 1, bg);
  fill(x + 4, y + 8, 8, 1, bg);
  fill(x + 4, y + 11, 6, 1, bg);
}

static void fmt_size(char *out, size_t cap, int64_t s) {
  if (s < 0)
    snprintf(out, cap, "-");
  else if (s < 1024)
    snprintf(out, cap, "%d B", (int)s);
  else if (s < 1024 * 1024)
    snprintf(out, cap, "%.1f KB", s / 1024.0);
  else if (s < 1024LL * 1024 * 1024)
    snprintf(out, cap, "%.1f MB", s / (1024.0 * 1024));
  else
    snprintf(out, cap, "%.2f GB", s / (1024.0 * 1024 * 1024));
}

#define BTN_BACK_X (SIDE_W + 14)
#define BTN_UP_X (SIDE_W + 46)
#define BTN_Y 8
#define BTN_S 28

static void redraw(void) {
  int fh = g_font->height;
  fill(0, 0, WIN_W, WIN_H, P->bg);

  // Barra lateral.
  fill(0, 0, SIDE_W, WIN_H, P->side);
  fill(SIDE_W - 1, 0, 1, WIN_H, P->line);
  text(16, 14, "Lugares", P->dim);
  for (int i = 0; i < NPLACES; i++) {
    int y = 40 + i * 30;
    int active = g_places[i].path[0] && !strcmp(g_places[i].path, g_cwd);
    if (active)
      fill_round(8, y - 4, SIDE_W - 16, 26, 7, P->sel);
    icon_folder(18, y + (fh - 14) / 2, P->accent);
    text(42, y, g_places[i].label, active ? P->sel_text : P->text);
  }

  // Barra superior: atrás, subir y ruta.
  fill(SIDE_W, TOP_H - 1, WIN_W - SIDE_W, 1, P->line);
  fill_round(BTN_BACK_X, BTN_Y, BTN_S, BTN_S, 7, P->side);
  fill_round(BTN_UP_X, BTN_Y, BTN_S, BTN_S, 7, P->side);
  text(BTN_BACK_X + 10, BTN_Y + (BTN_S - fh) / 2, "<", g_nback ? P->text : P->dim);
  text(BTN_UP_X + 10, BTN_Y + (BTN_S - fh) / 2, "^", strcmp(g_cwd, "/") ? P->text : P->dim);
  text_fit(BTN_UP_X + BTN_S + 14, BTN_Y + (BTN_S - fh) / 2, g_cwd,
           WIN_W - (BTN_UP_X + BTN_S + 14) - 16, P->text);

  // Cabecera de la lista.
  int lx = SIDE_W + 16, size_x = WIN_W - 250, type_x = WIN_W - 140;
  int hy = TOP_H + (HEAD_H - fh) / 2;
  text(lx + 26, hy, "Nombre", P->dim);
  text(size_x, hy, "Tamaño", P->dim);
  text(type_x, hy, "Tipo", P->dim);
  fill(SIDE_W, TOP_H + HEAD_H - 1, WIN_W - SIDE_W, 1, P->line);

  // Filas.
  int rows = visible_rows();
  for (int r = 0; r < rows && g_scroll + r < g_n; r++) {
    int i = g_scroll + r;
    int y = list_top() + r * ROW_H;
    int sel = i == g_sel;
    if (sel)
      fill_round(SIDE_W + 6, y + 1, WIN_W - SIDE_W - 12, ROW_H - 2, 6, P->sel);
    uint32_t tc = sel ? P->sel_text : P->text;
    int ty = y + (ROW_H - fh) / 2;
    if (g_ent[i].is_dir)
      icon_folder(lx, y + (ROW_H - 14) / 2, P->folder);
    else
      icon_file(lx, y + (ROW_H - 15) / 2, P->file, sel ? P->sel : P->bg);
    text_fit(lx + 26, ty, g_ent[i].name, size_x - lx - 40, tc);
    char sz[32];
    if (g_ent[i].is_dir)
      snprintf(sz, sizeof(sz), "-");
    else
      fmt_size(sz, sizeof(sz), g_ent[i].size);
    text(size_x, ty, sz, sel ? tc : P->dim);
    text(type_x, ty, type_of(&g_ent[i]), sel ? tc : P->dim);
  }
  if (g_n == 0)
    text(lx + 26, list_top() + 10, "Carpeta vacía", P->dim);

  // Barra de estado.
  fill(SIDE_W, WIN_H - 28, WIN_W - SIDE_W, 1, P->line);
  char st[160];
  if (g_msg[0])
    snprintf(st, sizeof(st), "%s", g_msg);
  else
    snprintf(st, sizeof(st), "%d elemento%s", g_n, g_n == 1 ? "" : "s");
  text_fit(SIDE_W + 16, WIN_H - 28 + (28 - fh) / 2, st, WIN_W - SIDE_W - 32, P->dim);

  // Barra de desplazamiento.
  if (g_n > rows) {
    int track = WIN_H - list_top() - 34;
    int h = track * rows / g_n;
    if (h < 20)
      h = 20;
    int y = list_top() + 2 + (track - h) * g_scroll / (g_n - rows);
    fill_round(WIN_W - 7, y, 4, h, 2, P->line);
  }
  sys_win_blit(g_win, 0, 0, WIN_W, WIN_H, 0, 0, WIN_W, g_px);
  g_dirty = 0;
}

static void ensure_sel_visible(void) {
  int rows = visible_rows();
  if (g_sel < g_scroll)
    g_scroll = g_sel;
  if (g_sel >= g_scroll + rows)
    g_scroll = g_sel - rows + 1;
  if (g_scroll < 0)
    g_scroll = 0;
}

static void scroll_by(int d) {
  int max = g_n - visible_rows();
  g_scroll += d;
  if (g_scroll > max)
    g_scroll = max;
  if (g_scroll < 0)
    g_scroll = 0;
  g_dirty = 1;
}

static void on_click(int x, int y) {
  if (x < SIDE_W) {
    for (int i = 0; i < NPLACES; i++) {
      int py = 40 + i * 30;
      if (y >= py - 4 && y < py + 22 && g_places[i].path[0])
        go(g_places[i].path, 1);
    }
    return;
  }
  if (y >= BTN_Y && y < BTN_Y + BTN_S) {
    if (x >= BTN_BACK_X && x < BTN_BACK_X + BTN_S)
      go_back();
    else if (x >= BTN_UP_X && x < BTN_UP_X + BTN_S)
      go_up();
    return;
  }
  if (y >= list_top() && y < WIN_H - 28) {
    int i = g_scroll + (y - list_top()) / ROW_H;
    if (i >= g_n)
      return;
    uint64_t t = now_ms();
    if (i == g_last_click_row && t - g_last_click_ms < 450) {
      g_last_click_row = -1;
      open_entry(i);
      return;
    }
    g_last_click_row = i;
    g_last_click_ms = t;
    g_sel = i;
    g_msg[0] = 0;
    g_dirty = 1;
  }
}

static char g_esc[8];
static int g_esc_n;

static void on_byte(unsigned char c) {
  if (g_esc_n > 0) {
    g_esc[g_esc_n++] = (char)c;
    if (g_esc_n >= 3 && ((c >= 'A' && c <= 'Z') || c == '~')) {
      g_esc[g_esc_n] = 0;
      int rows = visible_rows();
      if (!strcmp(g_esc + 1, "[A") && g_sel > 0)
        g_sel--;
      else if (!strcmp(g_esc + 1, "[B") && g_sel < g_n - 1)
        g_sel++;
      else if (!strcmp(g_esc + 1, "[5~"))
        g_sel = g_sel - rows < 0 ? 0 : g_sel - rows;
      else if (!strcmp(g_esc + 1, "[6~"))
        g_sel = g_sel + rows >= g_n ? g_n - 1 : g_sel + rows;
      else if (!strcmp(g_esc + 1, "[H"))
        g_sel = 0;
      else if (!strcmp(g_esc + 1, "[F"))
        g_sel = g_n - 1;
      else if (!strcmp(g_esc + 1, "[D"))
        go_back();
      ensure_sel_visible();
      g_esc_n = 0;
      g_dirty = 1;
    } else if (g_esc_n >= (int)sizeof(g_esc) - 1 || (g_esc_n == 2 && c != '[' && c != 'O')) {
      g_esc_n = 0;
    }
    return;
  }
  if (c == 0x1B) {
    g_esc[0] = 0x1B;
    g_esc_n = 1;
  } else if (c == '\r' || c == '\n') {
    open_entry(g_sel);
  } else if (c == 0x7F || c == 0x08) {
    go_up();
  } else if (c >= 0x20 && c < 0x7F) {
    // Saltar al primer elemento que empiece por esa letra (tras el actual).
    for (int k = 1; k <= g_n; k++) {
      int i = (g_sel + k) % g_n;
      int a = (unsigned char)g_ent[i].name[0], b = c;
      if (a >= 'A' && a <= 'Z')
        a += 32;
      if (b >= 'A' && b <= 'Z')
        b += 32;
      if (a == b) {
        g_sel = i;
        ensure_sel_visible();
        g_dirty = 1;
        break;
      }
    }
  }
}

int main(int argc, char **argv) {
  g_font = font_system_mono();
  if (!g_font)
    return 1;
  const char *home = getenv("HOME");
  snprintf(g_places[0].path, PATH_MAX_A, "%s", home && home[0] ? home : "/");
  if (load_dir(argc > 1 ? argv[1] : g_places[0].path) != 0)
    load_dir("/");

  g_win = sys_win_create(150, 80, WIN_W, WIN_H + TITLEBAR_H, "Archivos");
  if (g_win < 0)
    return 1;
  sys_win_set_icon(g_win, "/system/icons/app-archivos.bmp");
  g_theme_seen = sys_theme_get();
  P = (g_theme_seen & 1) ? &PAL_LIGHT : &PAL_DARK;

  for (;;) {
    winsrv_event_t ev;
    while (sys_win_poll_event(g_win, &ev, 0) > 0) {
      if (ev.type == WINSRV_EV_CLOSE) {
        sys_win_destroy(g_win);
        _exit(0);
      } else if (ev.type == WINSRV_EV_TTY_INPUT) {
        on_byte((unsigned char)ev.x);
      } else if (ev.type == WINSRV_EV_MOUSE) {
        int btn = (int)(ev.data & 0xFF);
        int released = (ev.data >> 8) & 1;
        int motion = (int)((ev.data >> 9) & 1);
        if (btn == 64 || btn == 65)
          scroll_by(btn == 64 ? -3 : 3);
        else if (btn == 0 && !released && !motion)
          on_click(ev.x, ev.y);
      } else if (ev.type == WINSRV_EV_FOCUS || ev.type == WINSRV_EV_BLUR) {
        g_dirty = 1;
      }
    }
    reap();
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
