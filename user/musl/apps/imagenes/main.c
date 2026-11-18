// user/musl/apps/imagenes/main.c
//
// Imágenes de Aurora OS: visor de PNG, JPEG (baseline), BMP y PPM/PGM.
//   - `imagenes FICHERO` abre una imagen (Archivos la abre con doble clic);
//     sin argumento, la primera de /system/wallpapers. `imagenes --fondo
//     FICHERO` la pone de fondo sin abrir ventana.
//   - Se ajusta a la ventana; rueda o + / - para acercar y alejar (hacia el
//     puntero), 1 = tamaño real, 0 = ajustar; arrastrar para moverse.
//   - Flechas izquierda/derecha: imagen anterior/siguiente de la carpeta.
//   - "Usar como fondo": la escala a la pantalla, la guarda como BMP en /data
//     (o en $HOME) y la pone de fondo de escritorio (ASYS_WALLPAPER_SET).
// La ventana se puede redimensionar y maximizar. Colores sólidos; el coral de
// su icono como color propio; sigue el modo claro/oscuro.

#pragma GCC diagnostic ignored "-Wformat-truncation"

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

#define TITLEBAR_H 32
#define BAR_H 46
#define MAX_FILES 1024
#define PATH_MAX_I 512

typedef struct {
  uint32_t bg, bar, line, text, dim, btn, btn_text, accent, accent_text, check_a, check_b;
} palette_t;

static const palette_t PAL_DARK = {0xFF111214, 0xFF1C1E22, 0xFF2B2E34, 0xFFE8EAED,
                                   0xFF8E949C, 0xFF2A2D33, 0xFFE8EAED, 0xFFF25A5A,
                                   0xFFFFFFFF, 0xFF2A2C30, 0xFF34373C};
static const palette_t PAL_LIGHT = {0xFFE9EBEE, 0xFFFFFFFF, 0xFFDDE0E5, 0xFF1A1D22,
                                    0xFF667085, 0xFFEEF0F3, 0xFF1A1D22, 0xFFE24848,
                                    0xFFFFFFFF, 0xFFFFFFFF, 0xFFE3E6EA};
static const palette_t *P = &PAL_DARK;

static uint32_t *g_px;
static int g_cw = 900, g_ch = 620; // área de cliente
static const font_aa_t *g_font;
static int g_win = -1;
static int g_dirty = 1;
static uint32_t g_theme_seen;
static char g_msg[160];
static uint64_t g_msg_until;

// Imagen actual.
static uint32_t *g_img;
static int g_iw, g_ih;
static char g_path[PATH_MAX_I];
static char g_err[160];
static double g_zoom = 1;
static int g_fit = 1;
static double g_cx, g_cy; // punto de la imagen en el centro de la vista

// Imágenes de la carpeta.
static char (*g_files)[256];
static int g_nfiles, g_idx = -1;
static char g_dir[PATH_MAX_I];

static uint64_t now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

static void flash(const char *m) {
  snprintf(g_msg, sizeof(g_msg), "%s", m);
  g_msg_until = now_ms() + 4000;
  g_dirty = 1;
}

static int view_h(void) { return g_ch - BAR_H; }

// ---- Ficheros ----
static uint8_t *read_all(const char *path, size_t *out_n) {
  int fd = (int)syscall(SYS_OPENAT, (uint64_t)(int64_t)AT_FDCWD, (uint64_t)path,
                        (uint64_t)O_RDONLY, 0, 0);
  if (fd < 0)
    return NULL;
  size_t cap = 1 << 20, n = 0;
  uint8_t *b = (uint8_t *)malloc(cap);
  while (b) {
    if (n == cap) {
      if (cap >= (256u << 20)) { // 256 MiB: no es una imagen razonable
        free(b);
        b = NULL;
        break;
      }
      uint8_t *nb = (uint8_t *)realloc(b, cap * 2);
      if (!nb) {
        free(b);
        b = NULL;
        break;
      }
      b = nb;
      cap *= 2;
    }
    long r = syscall(SYS_READ, (uint64_t)fd, (uint64_t)(b + n), (uint64_t)(cap - n), 0, 0);
    if (r <= 0)
      break;
    n += (size_t)r;
  }
  syscall(SYS_CLOSE, (uint64_t)fd, 0, 0, 0, 0);
  *out_n = n;
  return b;
}

struct linux_dirent64 {
  uint64_t d_ino;
  int64_t d_off;
  unsigned short d_reclen;
  unsigned char d_type;
  char d_name[];
};

static int name_cmp(const void *a, const void *b) {
  const char *x = (const char *)a, *y = (const char *)b;
  for (;; x++, y++) {
    int cx = (unsigned char)*x, cy = (unsigned char)*y;
    if (cx >= 'A' && cx <= 'Z') cx += 32;
    if (cy >= 'A' && cy <= 'Z') cy += 32;
    if (cx != cy || !cx)
      return cx - cy;
  }
}

static void scan_dir(const char *path) {
  snprintf(g_dir, sizeof(g_dir), "%s", path);
  char *sl = strrchr(g_dir, '/');
  const char *base = path;
  if (sl) {
    base = path + (sl - g_dir) + 1;
    if (sl == g_dir)
      sl[1] = 0;
    else
      *sl = 0;
  } else {
    snprintf(g_dir, sizeof(g_dir), ".");
  }
  if (!g_files)
    g_files = malloc(sizeof(*g_files) * MAX_FILES);
  g_nfiles = 0;
  g_idx = -1;
  if (!g_files)
    return;
  int fd = (int)syscall(SYS_OPENAT, (uint64_t)(int64_t)AT_FDCWD, (uint64_t)g_dir,
                        (uint64_t)(O_RDONLY | 0x10000), 0, 0);
  if (fd < 0)
    return;
  static char buf[16384];
  for (;;) {
    long n = syscall(SYS_GETDENTS64, (uint64_t)fd, (uint64_t)buf, sizeof(buf), 0, 0);
    if (n <= 0)
      break;
    for (long off = 0; off < n;) {
      struct linux_dirent64 *d = (struct linux_dirent64 *)(buf + off);
      off += d->d_reclen;
      if (d->d_name[0] == '.' || g_nfiles >= MAX_FILES || !img_is_image_name(d->d_name) ||
          strlen(d->d_name) >= 256)
        continue;
      snprintf(g_files[g_nfiles++], 256, "%s", d->d_name);
    }
  }
  syscall(SYS_CLOSE, (uint64_t)fd, 0, 0, 0, 0);
  qsort(g_files, (size_t)g_nfiles, sizeof(*g_files), name_cmp);
  for (int i = 0; i < g_nfiles; i++)
    if (!strcmp(g_files[i], base))
      g_idx = i;
}

static void set_title(void) {
  const char *b = strrchr(g_path, '/');
  b = b ? b + 1 : g_path;
  char t[96];
  if (g_path[0])
    snprintf(t, sizeof(t), "%s - Imágenes", b);
  else
    snprintf(t, sizeof(t), "Imágenes");
  if (g_win >= 0)
    sys_win_set_title(g_win, t);
}

static void fit_view(void) {
  g_fit = 1;
  g_cx = g_iw / 2.0;
  g_cy = g_ih / 2.0;
  if (!g_img)
    return;
  double zx = (double)(g_cw - 24) / g_iw, zy = (double)(view_h() - 24) / g_ih;
  g_zoom = zx < zy ? zx : zy;
  if (g_zoom > 1)
    g_zoom = 1; // no ampliar las pequeñas al ajustar
  if (g_zoom <= 0)
    g_zoom = 0.01;
}

static void load_image(const char *path) {
  free(g_img);
  g_img = NULL;
  g_iw = g_ih = 0;
  g_err[0] = 0;
  snprintf(g_path, sizeof(g_path), "%s", path);
  size_t n = 0;
  uint8_t *d = read_all(path, &n);
  if (!d) {
    snprintf(g_err, sizeof(g_err), "No se puede leer el fichero");
  } else {
    g_img = img_decode(d, n, &g_iw, &g_ih);
    free(d);
    if (!g_img)
      snprintf(g_err, sizeof(g_err), "No se puede abrir: %s", img_error);
  }
  fit_view();
  set_title();
  g_dirty = 1;
}

static void step_image(int d) {
  if (g_nfiles <= 0)
    return;
  int i = g_idx < 0 ? 0 : (g_idx + d + g_nfiles) % g_nfiles;
  g_idx = i;
  char p[PATH_MAX_I + 260];
  snprintf(p, sizeof(p), "%s%s%s", g_dir, strcmp(g_dir, "/") ? "/" : "", g_files[i]);
  load_image(p);
}

// ---- Fondo de escritorio ----
static int write_bmp(const char *path, const uint32_t *px, int w, int h) {
  int fd = (int)syscall(SYS_OPENAT, (uint64_t)(int64_t)AT_FDCWD, (uint64_t)path,
                        (uint64_t)(O_WRONLY | O_CREAT | O_TRUNC), 0644, 0);
  if (fd < 0)
    return -1;
  int stride = (w * 3 + 3) & ~3;
  uint32_t size = 54u + (uint32_t)stride * h;
  uint8_t hd[54];
  memset(hd, 0, sizeof(hd));
  hd[0] = 'B';
  hd[1] = 'M';
  memcpy(hd + 2, &size, 4);
  uint32_t v = 54;
  memcpy(hd + 10, &v, 4);
  v = 40;
  memcpy(hd + 14, &v, 4);
  memcpy(hd + 18, &w, 4);
  memcpy(hd + 22, &h, 4);
  hd[26] = 1;
  hd[28] = 24;
  v = (uint32_t)stride * h;
  memcpy(hd + 34, &v, 4);
  int ok = syscall(SYS_WRITE, (uint64_t)fd, (uint64_t)hd, 54, 0, 0) == 54;
  uint8_t *row = (uint8_t *)malloc((size_t)stride * 64);
  for (int y0 = h - 1; ok && row && y0 >= 0; y0 -= 64) {
    int nrows = 0;
    for (int y = y0; y > y0 - 64 && y >= 0; y--, nrows++) {
      uint8_t *r = row + (size_t)nrows * stride;
      memset(r, 0, stride);
      for (int x = 0; x < w; x++) {
        uint32_t c = px[(size_t)y * w + x];
        r[x * 3] = c & 0xFF;
        r[x * 3 + 1] = (c >> 8) & 0xFF;
        r[x * 3 + 2] = (c >> 16) & 0xFF;
      }
    }
    size_t len = (size_t)nrows * stride;
    ok = syscall(SYS_WRITE, (uint64_t)fd, (uint64_t)row, (uint64_t)len, 0, 0) == (long)len;
  }
  free(row);
  syscall(74 /* fsync */, (uint64_t)fd, 0, 0, 0, 0);
  syscall(SYS_CLOSE, (uint64_t)fd, 0, 0, 0, 0);
  return ok && row ? 0 : -1;
}

static void set_as_wallpaper(void) {
  if (!g_img) {
    flash("No hay ninguna imagen abierta");
    return;
  }
  uint32_t ss = sys_screen_size();
  int sw = (int)(ss >> 16), sh = (int)(ss & 0xFFFF);
  if (sw <= 0 || sh <= 0) {
    sw = 1920;
    sh = 1080;
  }
  uint32_t *o = (uint32_t *)malloc((size_t)sw * sh * 4);
  if (!o) {
    flash("Sin memoria para preparar el fondo");
    return;
  }
  // Llenar la pantalla recortando lo que sobre (centrado), muestreo 2x2.
  double z = (double)sw / g_iw > (double)sh / g_ih ? (double)sw / g_iw : (double)sh / g_ih;
  double ox = (g_iw - sw / z) / 2, oy = (g_ih - sh / z) / 2;
  for (int y = 0; y < sh; y++)
    for (int x = 0; x < sw; x++) {
      uint32_t r = 0, g = 0, b = 0;
      for (int k = 0; k < 4; k++) {
        int sx = (int)(ox + (x + 0.25 + 0.5 * (k & 1)) / z);
        int sy = (int)(oy + (y + 0.25 + 0.5 * (k >> 1)) / z);
        if (sx < 0) sx = 0;
        if (sy < 0) sy = 0;
        if (sx >= g_iw) sx = g_iw - 1;
        if (sy >= g_ih) sy = g_ih - 1;
        uint32_t c = g_img[(size_t)sy * g_iw + sx];
        uint32_t a = c >> 24; // transparencia sobre negro
        r += ((c >> 16) & 0xFF) * a / 255;
        g += ((c >> 8) & 0xFF) * a / 255;
        b += (c & 0xFF) * a / 255;
      }
      o[(size_t)y * sw + x] = 0xFF000000u | ((r / 4) << 16) | ((g / 4) << 8) | (b / 4);
    }
  char dst[PATH_MAX_I];
  snprintf(dst, sizeof(dst), "/data/aurora-fondo.bmp");
  int rc = write_bmp(dst, o, sw, sh);
  if (rc != 0) {
    const char *home = getenv("HOME");
    snprintf(dst, sizeof(dst), "%s/.aurora-fondo.bmp", home && home[0] ? home : "/tmp");
    rc = write_bmp(dst, o, sw, sh);
  }
  if (rc != 0) { // sin /data ni $HOME escribibles: solo esta sesión
    snprintf(dst, sizeof(dst), "/tmp/aurora-fondo.bmp");
    rc = write_bmp(dst, o, sw, sh);
  }
  free(o);
  if (rc != 0) {
    flash("No se pudo guardar el fondo");
    return;
  }
  if (sys_wallpaper_set(dst) != 0)
    flash("El sistema no aceptó el fondo");
  else
    flash("Puesta como fondo de escritorio");
}

// ---- Dibujo ----
static void fill(int x, int y, int w, int h, uint32_t c) {
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > g_cw) w = g_cw - x;
  if (y + h > g_ch) h = g_ch - y;
  for (int j = 0; j < h; j++) {
    uint32_t *r = &g_px[(y + j) * g_cw + x];
    for (int i = 0; i < w; i++)
      r[i] = c;
  }
}

static void fill_round(int x, int y, int w, int h, int r, uint32_t c) {
  if (r * 2 > h)
    r = h / 2;
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
  return ui_draw_text(g_px, g_cw, g_cw, g_ch, x, y, s, col & 0xFFFFFF, g_font);
}

static void text_fit(int x, int y, const char *s, int maxw, uint32_t col) {
  if (maxw <= 0)
    return;
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

static inline uint32_t over(uint32_t c, uint32_t bg) {
  uint32_t a = c >> 24;
  if (a == 255)
    return c;
  if (a == 0)
    return bg;
  uint32_t r = (((c >> 16) & 0xFF) * a + ((bg >> 16) & 0xFF) * (255 - a)) / 255;
  uint32_t g = (((c >> 8) & 0xFF) * a + ((bg >> 8) & 0xFF) * (255 - a)) / 255;
  uint32_t b = ((c & 0xFF) * a + (bg & 0xFF) * (255 - a)) / 255;
  return 0xFF000000u | (r << 16) | (g << 8) | b;
}

static void draw_image(void) {
  int vh = view_h();
  double z = g_zoom;
  // Rectángulo de la imagen en pantalla.
  double left = g_cw / 2.0 - g_cx * z, top = vh / 2.0 - g_cy * z;
  int x0 = (int)(left > 0 ? left : 0), y0 = (int)(top > 0 ? top : 0);
  int x1 = (int)(left + g_iw * z), y1 = (int)(top + g_ih * z);
  if (x1 > g_cw) x1 = g_cw;
  if (y1 > vh) y1 = vh;
  int ss = z < 0.75 ? 2 : 1; // submuestreo al reducir
  for (int y = y0; y < y1; y++) {
    uint32_t *row = &g_px[y * g_cw];
    for (int x = x0; x < x1; x++) {
      uint32_t chk = (((x >> 3) ^ (y >> 3)) & 1) ? P->check_a : P->check_b;
      if (ss == 1) {
        int sx = (int)((x + 0.5 - left) / z), sy = (int)((y + 0.5 - top) / z);
        if (sx < 0 || sy < 0 || sx >= g_iw || sy >= g_ih)
          continue;
        row[x] = over(g_img[(size_t)sy * g_iw + sx], chk);
      } else {
        uint32_t r = 0, g = 0, b = 0;
        for (int k = 0; k < 4; k++) {
          int sx = (int)((x + 0.25 + 0.5 * (k & 1) - left) / z);
          int sy = (int)((y + 0.25 + 0.5 * (k >> 1) - top) / z);
          if (sx < 0) sx = 0;
          if (sy < 0) sy = 0;
          if (sx >= g_iw) sx = g_iw - 1;
          if (sy >= g_ih) sy = g_ih - 1;
          uint32_t c = over(g_img[(size_t)sy * g_iw + sx], chk);
          r += (c >> 16) & 0xFF;
          g += (c >> 8) & 0xFF;
          b += c & 0xFF;
        }
        row[x] = 0xFF000000u | ((r / 4) << 16) | ((g / 4) << 8) | (b / 4);
      }
    }
  }
}

// Botones de la barra inferior (de derecha a izquierda).
enum { B_WALL, B_FIT, B_ZIN, B_ZOUT, B_NEXT, B_PREV, B_COUNT };
static const char *k_btn_label[B_COUNT] = {"Usar como fondo", "Ajustar", "+", "-", ">", "<"};
static int g_btn_x[B_COUNT], g_btn_w[B_COUNT];
static int g_btn_down = -1;

static void layout_buttons(void) {
  int x = g_cw - 12;
  for (int i = 0; i < B_COUNT; i++) {
    int w = ui_text_width(g_font, k_btn_label[i]) + 22;
    if (w < 32)
      w = 32;
    x -= w;
    g_btn_x[i] = x;
    g_btn_w[i] = w;
    x -= i == B_WALL || i == B_FIT || i == B_NEXT ? 14 : 6;
  }
}

static void redraw(void) {
  int fh = g_font->height, vh = view_h();
  fill(0, 0, g_cw, vh, P->bg);
  if (g_img)
    draw_image();
  else {
    const char *m = g_err[0] ? g_err : "Abre una imagen desde Archivos";
    int tw = ui_text_width(g_font, m);
    text((g_cw - tw) / 2, vh / 2 - fh / 2, m, P->dim);
  }
  // Barra inferior.
  int by = vh;
  fill(0, by, g_cw, BAR_H, P->bar);
  fill(0, by, g_cw, 1, P->line);
  layout_buttons();
  int bty = by + (BAR_H - 28) / 2;
  for (int i = 0; i < B_COUNT; i++) {
    int on = i == g_btn_down;
    uint32_t bg = i == B_WALL ? P->accent : P->btn;
    uint32_t fg = i == B_WALL ? P->accent_text : P->btn_text;
    if (on)
      bg = P->line;
    fill_round(g_btn_x[i], bty, g_btn_w[i], 28, 7, bg);
    int tw = ui_text_width(g_font, k_btn_label[i]);
    text(g_btn_x[i] + (g_btn_w[i] - tw) / 2, bty + (28 - fh) / 2, k_btn_label[i], fg);
  }
  char info[200];
  int left_max = g_btn_x[B_COUNT - 1] - 28;
  if (g_msg[0] && now_ms() < g_msg_until) {
    snprintf(info, sizeof(info), "%s", g_msg);
    text_fit(14, by + (BAR_H - fh) / 2, info, left_max, P->accent);
  } else {
    g_msg[0] = 0;
    const char *b = strrchr(g_path, '/');
    b = b ? b + 1 : g_path;
    if (g_img) {
      char pos[24] = "";
      if (g_nfiles > 1 && g_idx >= 0)
        snprintf(pos, sizeof(pos), "  ·  %d de %d", g_idx + 1, g_nfiles);
      snprintf(info, sizeof(info), "%s  ·  %d x %d  ·  %d %%%s", b, g_iw, g_ih,
               (int)(g_zoom * 100 + 0.5), pos);
    } else {
      snprintf(info, sizeof(info), "%s", b);
    }
    text_fit(14, by + (BAR_H - fh) / 2, info, left_max, P->text);
  }
  sys_win_blit(g_win, 0, 0, g_cw, g_ch, 0, 0, g_cw, g_px);
  g_dirty = 0;
}

// ---- Zoom y desplazamiento ----
static void clamp_center(void) {
  double hw = g_cw / 2.0 / g_zoom, hh = view_h() / 2.0 / g_zoom;
  if (g_iw * g_zoom <= g_cw)
    g_cx = g_iw / 2.0;
  else {
    if (g_cx < hw) g_cx = hw;
    if (g_cx > g_iw - hw) g_cx = g_iw - hw;
  }
  if (g_ih * g_zoom <= view_h())
    g_cy = g_ih / 2.0;
  else {
    if (g_cy < hh) g_cy = hh;
    if (g_cy > g_ih - hh) g_cy = g_ih - hh;
  }
}

// Cambia el zoom manteniendo fijo el punto de la vista (vx, vy).
static void zoom_at(double nz, int vx, int vy) {
  if (!g_img)
    return;
  if (nz < 0.02)
    nz = 0.02;
  if (nz > 32)
    nz = 32;
  double ix = g_cx + (vx - g_cw / 2.0) / g_zoom, iy = g_cy + (vy - view_h() / 2.0) / g_zoom;
  g_zoom = nz;
  g_cx = ix - (vx - g_cw / 2.0) / nz;
  g_cy = iy - (vy - view_h() / 2.0) / nz;
  g_fit = 0;
  clamp_center();
  g_dirty = 1;
}

static void zoom_step(int dir, int vx, int vy) {
  static const double steps[] = {0.05, 0.1, 0.25, 0.33, 0.5, 0.67, 0.75, 1, 1.5,
                                 2,    3,   4,    6,    8,   12,   16,   24, 32};
  int n = (int)(sizeof(steps) / sizeof(steps[0]));
  double nz = g_zoom;
  if (dir > 0) {
    for (int i = 0; i < n; i++)
      if (steps[i] > g_zoom * 1.01) {
        nz = steps[i];
        break;
      }
  } else {
    for (int i = n - 1; i >= 0; i--)
      if (steps[i] < g_zoom * 0.99) {
        nz = steps[i];
        break;
      }
  }
  zoom_at(nz, vx, vy);
}

static void press_button(int b) {
  switch (b) {
  case B_WALL: set_as_wallpaper(); break;
  case B_FIT: fit_view(); g_dirty = 1; break;
  case B_ZIN: zoom_step(1, g_cw / 2, view_h() / 2); break;
  case B_ZOUT: zoom_step(-1, g_cw / 2, view_h() / 2); break;
  case B_NEXT: step_image(1); break;
  case B_PREV: step_image(-1); break;
  }
}

static int button_at(int x, int y) {
  if (y < view_h())
    return -1;
  for (int i = 0; i < B_COUNT; i++)
    if (x >= g_btn_x[i] && x < g_btn_x[i] + g_btn_w[i])
      return i;
  return -1;
}

static char g_esc[8];
static int g_esc_n;

static void on_byte(unsigned char c) {
  if (g_esc_n > 0) {
    g_esc[g_esc_n++] = (char)c;
    if (g_esc_n >= 3 && ((c >= 'A' && c <= 'Z') || c == '~')) {
      g_esc[g_esc_n] = 0;
      if (!strcmp(g_esc + 1, "[D") || !strcmp(g_esc + 1, "[5~"))
        step_image(-1);
      else if (!strcmp(g_esc + 1, "[C") || !strcmp(g_esc + 1, "[6~"))
        step_image(1);
      else if (!strcmp(g_esc + 1, "[H"))
        step_image(-g_idx);
      else if (!strcmp(g_esc + 1, "[F") && g_nfiles > 0)
        step_image(g_nfiles - 1 - g_idx);
      else if (!strcmp(g_esc + 1, "[A")) {
        g_cy -= 60 / g_zoom;
        clamp_center();
      } else if (!strcmp(g_esc + 1, "[B")) {
        g_cy += 60 / g_zoom;
        clamp_center();
      }
      g_esc_n = 0;
      g_dirty = 1;
    } else if (g_esc_n >= (int)sizeof(g_esc) - 1 || (g_esc_n == 2 && c != '[' && c != 'O')) {
      g_esc_n = 0;
    }
    return;
  }
  switch (c) {
  case 0x1B: g_esc[0] = 0x1B; g_esc_n = 1; break;
  case '+': case '=': zoom_step(1, g_cw / 2, view_h() / 2); break;
  case '-': case '_': zoom_step(-1, g_cw / 2, view_h() / 2); break;
  case '1': zoom_at(1, g_cw / 2, view_h() / 2); break;
  case '0': case 'f': case 'F': fit_view(); g_dirty = 1; break;
  case ' ': step_image(1); break;
  case 0x7F: case 0x08: step_image(-1); break;
  default: break;
  }
}

static int resize_buffer(int w, int h) {
  if (w < 200 || h < BAR_H + 60)
    return -1;
  uint32_t *n = (uint32_t *)malloc((size_t)w * h * 4);
  if (!n)
    return -1;
  free(g_px);
  g_px = n;
  g_cw = w;
  g_ch = h;
  if (g_fit)
    fit_view();
  else
    clamp_center();
  g_dirty = 1;
  return 0;
}

int main(int argc, char **argv) {
  g_font = font_system_mono();
  if (!g_font)
    return 1;
  // imagenes --fondo FICHERO: ponerlo de fondo de escritorio sin ventana.
  if (argc > 2 && !strcmp(argv[1], "--fondo")) {
    load_image(argv[2]);
    if (!g_img) {
      fprintf(stderr, "imagenes: %s\n", g_err);
      return 1;
    }
    set_as_wallpaper();
    printf("%s\n", g_msg);
    return strstr(g_msg, "Puesta") ? 0 : 1;
  }
  const char *start = argc > 1 ? argv[1] : NULL;
  if (start) {
    load_image(start);
    scan_dir(start);
  } else {
    scan_dir("/system/wallpapers/x");
    if (g_nfiles > 0)
      step_image(1);
  }
  // Tamaño inicial: el de la imagen, entre 520x380 y el 80 % de la pantalla.
  uint32_t ss = sys_screen_size();
  int sw = (int)(ss >> 16), sh = (int)(ss & 0xFFFF);
  if (sw <= 0 || sh <= 0) {
    sw = 1280;
    sh = 800;
  }
  int w = g_img ? g_iw + 24 : 900, h = g_img ? g_ih + 24 + BAR_H : 620;
  if (w > sw * 8 / 10) w = sw * 8 / 10;
  if (h > sh * 8 / 10 - TITLEBAR_H) h = sh * 8 / 10 - TITLEBAR_H;
  if (w < 620) w = 620;
  if (h < 420) h = 420;
  g_px = (uint32_t *)malloc((size_t)w * h * 4);
  if (!g_px)
    return 1;
  g_cw = w;
  g_ch = h;
  fit_view();

  g_win = sys_win_create((sw - w) / 2, (sh - h - TITLEBAR_H) / 2, w, h + TITLEBAR_H, "Imágenes");
  if (g_win < 0)
    return 1;
  sys_win_set_icon(g_win, "/system/icons/app-imagenes.bmp");
  set_title();
  g_theme_seen = sys_theme_get();
  P = (g_theme_seen & 1) ? &PAL_LIGHT : &PAL_DARK;

  int dragging = 0, drag_x = 0, drag_y = 0;
  for (;;) {
    winsrv_event_t ev;
    while (sys_win_poll_event(g_win, &ev, 0) > 0) {
      if (ev.type == WINSRV_EV_CLOSE) {
        sys_win_destroy(g_win);
        _exit(0);
      } else if (ev.type == WINSRV_EV_RESIZE) {
        resize_buffer(ev.x, ev.y);
      } else if (ev.type == WINSRV_EV_TTY_INPUT) {
        on_byte((unsigned char)ev.x);
      } else if (ev.type == WINSRV_EV_KEY) {
        // + y - del teclado numérico (no llegan como texto).
        if (ev.x == 0x4E)
          zoom_step(1, g_cw / 2, view_h() / 2);
        else if (ev.x == 0x4A)
          zoom_step(-1, g_cw / 2, view_h() / 2);
      } else if (ev.type == WINSRV_EV_MOUSE) {
        int btn = (int)(ev.data & 0xFF);
        int released = (ev.data >> 8) & 1;
        int motion = (int)((ev.data >> 9) & 1);
        if (btn == 64 || btn == 65) {
          if (ev.y < view_h())
            zoom_step(btn == 64 ? 1 : -1, ev.x, ev.y);
        } else if (btn == 0 && motion) {
          if (dragging && g_img) {
            g_cx -= (ev.x - drag_x) / g_zoom;
            g_cy -= (ev.y - drag_y) / g_zoom;
            drag_x = ev.x;
            drag_y = ev.y;
            g_fit = 0;
            clamp_center();
            g_dirty = 1;
          }
        } else if (btn == 0 && !released) {
          int b = button_at(ev.x, ev.y);
          if (b >= 0) {
            g_btn_down = b;
          } else if (ev.y < view_h()) {
            dragging = 1;
            drag_x = ev.x;
            drag_y = ev.y;
          }
          g_dirty = 1;
        } else if (btn == 0 && released) {
          int b = button_at(ev.x, ev.y);
          if (b >= 0 && b == g_btn_down)
            press_button(b);
          g_btn_down = -1;
          dragging = 0;
          g_dirty = 1;
        }
      } else if (ev.type == WINSRV_EV_FOCUS || ev.type == WINSRV_EV_BLUR) {
        g_btn_down = -1;
        dragging = 0;
        g_dirty = 1;
      }
    }
    uint32_t th = sys_theme_get();
    if (th != g_theme_seen) {
      g_theme_seen = th;
      P = (th & 1) ? &PAL_LIGHT : &PAL_DARK;
      g_dirty = 1;
    }
    if (g_msg[0] && now_ms() >= g_msg_until)
      g_dirty = 1;
    if (g_dirty)
      redraw();
    struct pollfd pfd = {-1, 0, 0};
    poll(&pfd, 1, 30);
  }
  return 0;
}
