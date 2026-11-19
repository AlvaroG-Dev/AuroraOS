// user/musl/apps/aplicaciones/main.c
//
// Aplicaciones de Aurora: todas las apps instaladas en una rejilla, para
// verlas y abrirlas. Es el primer icono del Dock, fijo (no se puede quitar);
// el kernel la lanza y su ventana se agrupa en ese icono (app_id
// "aurora.aplicaciones").
//
// Origen de la lista:
//   - Apps de Aurora: /usr/share/aurora/apps/*.desktop (Exec, Icon BMP,
//     X-Aurora-Color, X-Aurora-Order).
//   - Apps de Linux: /usr/share/aurora/dock.conf y
//     /usr/share/applications/*.desktop, con las mismas reglas que
//     aurora-wayland (Terminal=true / NoDisplay=true no salen). Se lanzan con
//     el entorno de Wayland/X11 y el tema claro/oscuro, como desde el Dock.
//
// Uso: un clic (o Intro) abre la app. Escribir filtra por nombre (sin
// distinguir mayúsculas ni tildes); Esc borra la búsqueda. Flechas para
// moverse, rueda para desplazar. Colores sólidos; el magenta de su icono como
// color propio; sigue el modo claro/oscuro. La ventana se puede redimensionar.

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

#define TITLEBAR_H 38 // = AURORA_TITLEBAR_HEIGHT (kernel/gfx/theme.h)
#define TOP_H 60
#define FOOT_H 30
#define CELL_W 132
#define CELL_H 122
#define ICON 64
#define MAX_APPS 128
#define APP_ID "aurora.aplicaciones"
#define WAYLAND_SOCK "/run/user/0/wayland-0"

typedef struct {
  uint32_t bg, top, line, text, dim, field, sel, sel_text, accent, accent_text;
} palette_t;

static const palette_t PAL_DARK = {0xFF15161A, 0xFF1C1D22, 0xFF2B2D34, 0xFFE9EAEE,
                                   0xFF8D929C, 0xFF24262C, 0xFF2E2531, 0xFFFFFFFF,
                                   0xFFD63FA8, 0xFFFFFFFF};
static const palette_t PAL_LIGHT = {0xFFF6F6F8, 0xFFFFFFFF, 0xFFE2E3E8, 0xFF1A1B20,
                                    0xFF667085, 0xFFEEEFF2, 0xFFF6DDEE, 0xFF3A0F2C,
                                    0xFFC2338F, 0xFFFFFFFF};
static const palette_t *P = &PAL_DARK;

typedef struct {
  char name[48];
  char comment[96];
  char exec[256];
  char icon[256];
  char app_id[64];
  uint32_t color;
  int order;
  int linux_app; // 1: app Linux (entorno Wayland/X11)
  uint32_t *pix; // ICON x ICON ARGB (NULL = baldosa con iniciales)
  int icon_tried;
  int backing;   // icono que no llena su cuadro: 1 = baldosa del tema,
                 // 2 = baldosa clara (glifo oscuro, p. ej. XTerm)
} app_t;

static app_t g_apps[MAX_APPS];
static int g_napps;
static int g_vis[MAX_APPS]; // índices visibles (filtrados), en orden
static int g_nvis;
static int g_nvis_native; // los primeros g_nvis_native de g_vis son de Aurora

static uint32_t *g_px;
static int g_cw = 900, g_ch = 600;
static const font_aa_t *g_font;
static int g_win = -1;
static int g_dirty = 1;
static uint32_t g_theme_seen;
static char g_search[64];
static int g_sel = -1;    // posición en g_vis
static int g_pressed = -1;
static int g_scroll;      // píxeles
static char g_msg[160];
static uint64_t g_msg_until;
static int g_children;

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

// ---- Ficheros ----
static int file_exists(const char *p) {
  stat_t st;
  return syscall(SYS_FSTATAT, (uint64_t)(int64_t)AT_FDCWD, (uint64_t)p, (uint64_t)&st, 0, 0) == 0;
}

static uint8_t *read_all(const char *path, size_t *out_n) {
  int fd = (int)syscall(SYS_OPENAT, (uint64_t)(int64_t)AT_FDCWD, (uint64_t)path,
                        (uint64_t)O_RDONLY, 0, 0);
  if (fd < 0)
    return NULL;
  size_t cap = 64 * 1024, n = 0;
  uint8_t *b = (uint8_t *)malloc(cap + 1);
  while (b) {
    if (n == cap) {
      if (cap >= (32u << 20)) {
        free(b);
        b = NULL;
        break;
      }
      uint8_t *nb = (uint8_t *)realloc(b, cap * 2 + 1);
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
  if (b)
    b[n] = 0;
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

// Llama a fn(ruta) para cada fichero *.desktop de dir.
static void for_each_desktop(const char *dir, void (*fn)(const char *path)) {
  int fd = (int)syscall(SYS_OPENAT, (uint64_t)(int64_t)AT_FDCWD, (uint64_t)dir,
                        (uint64_t)(O_RDONLY | 0x10000), 0, 0);
  if (fd < 0)
    return;
  static char buf[8192];
  for (;;) {
    long n = syscall(SYS_GETDENTS64, (uint64_t)fd, (uint64_t)buf, sizeof(buf), 0, 0);
    if (n <= 0)
      break;
    for (long off = 0; off < n;) {
      struct linux_dirent64 *d = (struct linux_dirent64 *)(buf + off);
      off += d->d_reclen;
      size_t ln = strlen(d->d_name);
      if (d->d_name[0] == '.' || ln < 9 || strcmp(d->d_name + ln - 8, ".desktop"))
        continue;
      char p[512];
      snprintf(p, sizeof(p), "%s/%s", dir, d->d_name);
      fn(p);
    }
  }
  syscall(SYS_CLOSE, (uint64_t)fd, 0, 0, 0, 0);
}

static char *trim(char *s) {
  while (*s == ' ' || *s == '\t')
    s++;
  char *e = s + strlen(s);
  while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\n' || e[-1] == '\r'))
    *--e = 0;
  return s;
}

static void exec_clean(char *dst, size_t n, const char *exec) {
  size_t o = 0;
  for (const char *p = exec; *p && o + 1 < n; p++) {
    if (*p == '%' && p[1]) { // %f %F %u %U %i %c %k
      p++;
      continue;
    }
    if (*p == '"')
      continue;
    dst[o++] = *p;
  }
  while (o && dst[o - 1] == ' ')
    o--;
  dst[o] = 0;
}

static void exec_base(const char *exec, char *out, size_t n) {
  char tmp[256];
  snprintf(tmp, sizeof(tmp), "%s", exec);
  char *sp = strchr(tmp, ' ');
  if (sp)
    *sp = 0;
  const char *b = strrchr(tmp, '/');
  snprintf(out, n, "%s", b ? b + 1 : tmp);
}

static int have_app_id(const char *id) {
  for (int i = 0; i < g_napps; i++)
    if (g_apps[i].linux_app && !strcmp(g_apps[i].app_id, id))
      return 1;
  return 0;
}

static uint32_t name_color(const char *s) { // mismo color que el Dock (FNV-1a)
  uint32_t h = 2166136261u;
  for (const char *c = s; *c; c++)
    h = (h ^ (uint8_t)*c) * 16777619u;
  return 0xFF000000u | (0x30 + (h & 0x5F)) << 16 | (0x30 + ((h >> 8) & 0x5F)) << 8 |
         (0x40 + ((h >> 16) & 0x7F));
}

static int g_parsing_native;

static void add_desktop(const char *path) {
  if (g_napps >= MAX_APPS)
    return;
  size_t n = 0;
  char *txt = (char *)read_all(path, &n);
  if (!txt)
    return;
  char name[48] = "", name_es[48] = "", comment[96] = "", comment_es[96] = "", exec[256] = "",
       icon[256] = "", wmclass[64] = "";
  uint32_t color = 0;
  int order = 1000, in_entry = 0, hidden = 0, is_app = 0;
  for (char *line = txt, *nl; line && *line; line = nl) {
    nl = strchr(line, '\n');
    if (nl)
      *nl++ = 0;
    char *t = trim(line);
    if (t[0] == '[') {
      in_entry = !strcmp(t, "[Desktop Entry]");
      continue;
    }
    if (!in_entry)
      continue;
    if (!strncmp(t, "Name=", 5) && !name[0])
      snprintf(name, sizeof(name), "%s", t + 5);
    else if (!strncmp(t, "Name[es]=", 9))
      snprintf(name_es, sizeof(name_es), "%s", t + 9);
    else if (!strncmp(t, "Comment=", 8) && !comment[0])
      snprintf(comment, sizeof(comment), "%s", t + 8);
    else if (!strncmp(t, "Comment[es]=", 12))
      snprintf(comment_es, sizeof(comment_es), "%s", t + 12);
    else if (!strncmp(t, "Exec=", 5))
      exec_clean(exec, sizeof(exec), t + 5);
    else if (!strncmp(t, "Icon=", 5))
      snprintf(icon, sizeof(icon), "%s", t + 5);
    else if (!strncmp(t, "StartupWMClass=", 15))
      snprintf(wmclass, sizeof(wmclass), "%s", t + 15);
    else if (!strncmp(t, "X-Aurora-Color=", 15))
      color = 0xFF000000u | (uint32_t)strtoul(t + 15, NULL, 16);
    else if (!strncmp(t, "X-Aurora-Order=", 15))
      order = atoi(t + 15);
    else if (!strcmp(t, "NoDisplay=true") || !strcmp(t, "Hidden=true") ||
             (!g_parsing_native && !strcmp(t, "Terminal=true")))
      hidden = 1;
    else if (!strcmp(t, "Type=Application"))
      is_app = 1;
  }
  free(txt);
  if (!is_app || hidden || !name[0] || !exec[0])
    return;
  char app_id[64];
  if (wmclass[0])
    snprintf(app_id, sizeof(app_id), "%s", wmclass);
  else
    exec_base(exec, app_id, sizeof(app_id));
  if (!g_parsing_native && have_app_id(app_id))
    return;
  app_t *a = &g_apps[g_napps++];
  memset(a, 0, sizeof(*a));
  snprintf(a->name, sizeof(a->name), "%s", name_es[0] ? name_es : name);
  snprintf(a->comment, sizeof(a->comment), "%s", comment_es[0] ? comment_es : comment);
  snprintf(a->exec, sizeof(a->exec), "%s", exec);
  snprintf(a->icon, sizeof(a->icon), "%s", icon);
  snprintf(a->app_id, sizeof(a->app_id), "%s", app_id);
  a->color = color ? color : name_color(app_id);
  a->order = order;
  a->linux_app = !g_parsing_native;
}

// dock.conf: Nombre | app_id | símbolo | color | icono BMP | orden
static void add_dock_conf(void) {
  size_t n = 0;
  char *txt = (char *)read_all("/usr/share/aurora/dock.conf", &n);
  if (!txt)
    return;
  for (char *line = txt, *nl; line && *line && g_napps < MAX_APPS; line = nl) {
    nl = strchr(line, '\n');
    if (nl)
      *nl++ = 0;
    char *t = trim(line);
    if (!t[0] || t[0] == '#')
      continue;
    char *fld[6] = {0};
    int nf = 0;
    for (char *p = t; nf < 6;) {
      fld[nf++] = p;
      char *bar = strchr(p, '|');
      if (!bar)
        break;
      *bar = 0;
      p = bar + 1;
    }
    if (nf < 6)
      continue;
    for (int i = 0; i < 6; i++)
      fld[i] = trim(fld[i]);
    if (!fld[1][0] || !fld[5][0] || have_app_id(fld[1]))
      continue;
    app_t *a = &g_apps[g_napps++];
    memset(a, 0, sizeof(*a));
    snprintf(a->name, sizeof(a->name), "%s", fld[0]);
    snprintf(a->app_id, sizeof(a->app_id), "%s", fld[1]);
    a->color = 0xFF000000u | (uint32_t)strtoul(fld[3], NULL, 16);
    snprintf(a->icon, sizeof(a->icon), "%s", fld[4]);
    snprintf(a->exec, sizeof(a->exec), "%s", fld[5]);
    a->order = 1000;
    a->linux_app = 1;
  }
  free(txt);
}

// Comparación sin mayúsculas ni tildes (á -> a, ñ -> n...).
static uint32_t fold(uint32_t c) {
  if (c >= 'A' && c <= 'Z')
    return c + 32;
  static const struct {
    uint16_t cp;
    char b;
  } m[] = {{0xE1, 'a'}, {0xE9, 'e'}, {0xED, 'i'}, {0xF3, 'o'}, {0xFA, 'u'}, {0xFC, 'u'},
           {0xF1, 'n'}, {0xC1, 'a'}, {0xC9, 'e'}, {0xCD, 'i'}, {0xD3, 'o'}, {0xDA, 'u'},
           {0xDC, 'u'}, {0xD1, 'n'}, {0xE0, 'a'}, {0xE8, 'e'}, {0xE7, 'c'}, {0xC7, 'c'}};
  for (unsigned i = 0; i < sizeof(m) / sizeof(m[0]); i++)
    if (m[i].cp == c)
      return (uint32_t)m[i].b;
  return c;
}

static void fold_str(const char *s, uint32_t *out, int cap, int *n) {
  int k = 0;
  while (*s && k < cap)
    out[k++] = fold(ui_utf8_next(&s));
  *n = k;
}

static int matches(const app_t *a) {
  if (!g_search[0])
    return 1;
  uint32_t h[96], nd[64];
  int nh, nn;
  fold_str(a->name, h, 96, &nh);
  fold_str(g_search, nd, 64, &nn);
  for (int i = 0; i + nn <= nh; i++) {
    int ok = 1;
    for (int j = 0; j < nn && ok; j++)
      ok = h[i + j] == nd[j];
    if (ok)
      return 1;
  }
  // También por el ejecutable (p. ej. "foot", "xterm").
  return strstr(a->exec, g_search) != NULL;
}

static int app_cmp(const void *pa, const void *pb) {
  const app_t *a = (const app_t *)pa, *b = (const app_t *)pb;
  if (a->linux_app != b->linux_app)
    return a->linux_app - b->linux_app;
  if (a->order != b->order)
    return a->order - b->order;
  uint32_t x[48], y[48];
  int nx, ny;
  fold_str(a->name, x, 48, &nx);
  fold_str(b->name, y, 48, &ny);
  for (int i = 0; i < nx && i < ny; i++)
    if (x[i] != y[i])
      return (int)x[i] - (int)y[i];
  return nx - ny;
}

static void load_apps(void) {
  g_napps = 0;
  g_parsing_native = 1;
  for_each_desktop("/usr/share/aurora/apps", add_desktop);
  g_parsing_native = 0;
  add_dock_conf();
  for_each_desktop("/usr/share/applications", add_desktop);
  qsort(g_apps, (size_t)g_napps, sizeof(app_t), app_cmp);
}

static void refilter(void) {
  g_nvis = g_nvis_native = 0;
  for (int i = 0; i < g_napps; i++)
    if (matches(&g_apps[i])) {
      g_vis[g_nvis++] = i;
      if (!g_apps[i].linux_app)
        g_nvis_native++;
    }
  if (g_sel >= g_nvis)
    g_sel = g_nvis - 1;
  if (g_search[0] && g_nvis > 0 && g_sel < 0)
    g_sel = 0;
  g_scroll = 0;
  g_dirty = 1;
}

// ---- Iconos ----
// Reduce/amplía a ICON x ICON manteniendo la proporción, con promedio por
// caja sobre colores premultiplicados (sin halos en los bordes).
static uint32_t *icon_scale(const uint32_t *src, int sw, int sh) {
  uint32_t *o = (uint32_t *)calloc(ICON * ICON, 4);
  if (!o)
    return NULL;
  int dw = ICON, dh = ICON;
  if (sw > sh)
    dh = ICON * sh / sw;
  else if (sh > sw)
    dw = ICON * sw / sh;
  if (dw < 1) dw = 1;
  if (dh < 1) dh = 1;
  int ox = (ICON - dw) / 2, oy = (ICON - dh) / 2;
  for (int y = 0; y < dh; y++)
    for (int x = 0; x < dw; x++) {
      int x0 = x * sw / dw, x1 = (x + 1) * sw / dw, y0 = y * sh / dh, y1 = (y + 1) * sh / dh;
      if (x1 <= x0) x1 = x0 + 1;
      if (y1 <= y0) y1 = y0 + 1;
      uint64_t r = 0, g = 0, b = 0, al = 0, cnt = 0;
      for (int yy = y0; yy < y1 && yy < sh; yy++)
        for (int xx = x0; xx < x1 && xx < sw; xx++) {
          uint32_t c = src[(size_t)yy * sw + xx], a = c >> 24;
          r += ((c >> 16) & 0xFF) * a;
          g += ((c >> 8) & 0xFF) * a;
          b += (c & 0xFF) * a;
          al += a;
          cnt++;
        }
      uint32_t A = cnt ? (uint32_t)(al / cnt) : 0;
      uint32_t R = al ? (uint32_t)(r / al) : 0, G = al ? (uint32_t)(g / al) : 0,
               B = al ? (uint32_t)(b / al) : 0;
      o[(oy + y) * ICON + ox + x] = (A << 24) | (R << 16) | (G << 8) | B;
    }
  return o;
}

static uint32_t *load_icon_file(const char *path) {
  size_t n = 0;
  uint8_t *d = read_all(path, &n);
  if (!d)
    return NULL;
  int w = 0, h = 0;
  uint32_t *px = img_decode(d, n, &w, &h);
  free(d);
  if (!px)
    return NULL;
  uint32_t *o = icon_scale(px, w, h);
  free(px);
  return o;
}

// Busca el icono como aurora-wayland: ruta absoluta, su caché de BMP, tema.
static uint32_t *find_icon(const app_t *a) {
  if (a->icon[0] == '/' && file_exists(a->icon))
    return load_icon_file(a->icon);
  char p[512];
  // Caché que crea aurora-wayland (incluye los SVG ya convertidos).
  char key[80];
  snprintf(key, sizeof(key), "app-%s", a->app_id);
  for (char *c = key; *c; c++)
    if (!((*c >= '0' && *c <= '9') || (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') ||
          *c == '-' || *c == '.'))
      *c = '_';
  snprintf(p, sizeof(p), "/tmp/aurora-icons/%s.bmp", key);
  if (file_exists(p))
    return load_icon_file(p);
  const char *names[3];
  int nn = 0;
  char base[64];
  exec_base(a->exec, base, sizeof(base));
  if (a->icon[0] && a->icon[0] != '/')
    names[nn++] = a->icon;
  names[nn++] = a->app_id;
  if (strcmp(base, a->app_id))
    names[nn++] = base;
  static const char *const themes[] = {"Aurora", "hicolor", "Adwaita"};
  static const char *const sizes[] = {"128x128", "256x256", "64x64", "96x96", "48x48", "32x32"};
  for (int k = 0; k < nn; k++) {
    const char *nm = names[k];
    size_t ln = strlen(nm);
    const char *ext = ln > 4 && !strcmp(nm + ln - 4, ".png") ? "" : ".png";
    for (unsigned t = 0; t < 3; t++)
      for (unsigned s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
        snprintf(p, sizeof(p), "/usr/share/icons/%s/%s/apps/%s%s", themes[t], sizes[s], nm, ext);
        if (file_exists(p))
          return load_icon_file(p);
      }
    snprintf(p, sizeof(p), "/usr/share/pixmaps/%s%s", nm, ext);
    if (file_exists(p))
      return load_icon_file(p);
  }
  return NULL;
}

// Prepara un icono pendiente de los visibles; 1 si hizo trabajo.
static int icon_step(void) {
  for (int k = 0; k < g_nvis; k++) {
    app_t *a = &g_apps[g_vis[k]];
    if (a->icon_tried)
      continue;
    a->icon_tried = 1;
    a->pix = find_icon(a);
    if (a->pix) { // como el Dock: baldosa detrás de los iconos "sueltos"
      int opaque = 0;
      uint64_t lum = 0;
      for (int i = 0; i < ICON * ICON; i++) {
        uint32_t c = a->pix[i];
        if ((c >> 24) > 128) {
          opaque++;
          lum += (((c >> 16) & 0xFF) * 3 + ((c >> 8) & 0xFF) * 6 + (c & 0xFF)) / 10;
        }
      }
      if (opaque < ICON * ICON / 2)
        a->backing = opaque && lum / opaque < 90 ? 2 : 1;
    }
    return 1;
  }
  return 0;
}

// ---- Lanzar ----
static int path_lookup(const char *name, char *out, size_t n) {
  if (strchr(name, '/')) {
    snprintf(out, n, "%s", name);
    return file_exists(out);
  }
  static const char *const dirs[] = {"/usr/local/bin", "/usr/bin", "/bin", "/usr/games",
                                     "/usr/sbin", "/sbin"};
  for (unsigned i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
    snprintf(out, n, "%s/%s", dirs[i], name);
    if (file_exists(out))
      return 1;
  }
  return 0;
}

static void launch(const app_t *a) {
  char buf[256];
  snprintf(buf, sizeof(buf), "%s", a->exec);
  char *argv[16];
  int ac = 0;
  for (char *tok = strtok(buf, " "); tok && ac < 15; tok = strtok(NULL, " "))
    argv[ac++] = tok;
  argv[ac] = NULL;
  if (!ac)
    return;
  char path[256];
  if (!path_lookup(argv[0], path, sizeof(path))) {
    char m[160];
    snprintf(m, sizeof(m), "No se encuentra %s", argv[0]);
    flash(m);
    return;
  }
  int pid;
  if (!a->linux_app) {
    pid = spawn_args(path, argv, ac);
  } else {
    if (!file_exists(WAYLAND_SOCK)) {
      flash("Las apps de Linux necesitan el escritorio Wayland (aurora-wayland)");
      return;
    }
    // Entorno como el que pone aurora-wayland al lanzar desde el Dock.
    int light = (int)(sys_theme_get() & 1);
    static char *env[160];
    int ne = 0;
    static const char *const over[] = {"XDG_RUNTIME_DIR=", "WAYLAND_DISPLAY=", "DISPLAY=",
                                       "GTK_THEME=", "AURORA_THEME="};
    for (char **e = environ; e && *e && ne < 150; e++) {
      int skip = 0;
      for (unsigned k = 0; k < sizeof(over) / sizeof(over[0]); k++)
        skip |= !strncmp(*e, over[k], strlen(over[k]));
      if (!skip)
        env[ne++] = *e;
    }
    const char *xdg = getenv("XDG_RUNTIME_DIR");
    static char xdg_env[128];
    snprintf(xdg_env, sizeof(xdg_env), "XDG_RUNTIME_DIR=%s", xdg && xdg[0] ? xdg : "/run/user/0");
    env[ne++] = xdg_env;
    env[ne++] = "WAYLAND_DISPLAY=wayland-0";
    env[ne++] = "DISPLAY=:0";
    env[ne++] = light ? "GTK_THEME=Aurora-light" : "GTK_THEME=Aurora";
    env[ne++] = light ? "AURORA_THEME=light" : "AURORA_THEME=dark";
    env[ne] = NULL;
    // libadwaita lee ~/.config/gtk-4.0/gtk.css: un enlace según el modo.
    syscall(SYS_UNLINK, (uint64_t)"/tmp/.config/gtk-4.0/gtk.css", 0, 0, 0, 0);
    syscall(88 /* symlink */,
            (uint64_t)(light ? "/usr/share/aurora/xdg/gtk-4.0/gtk-light.css"
                             : "/usr/share/aurora/xdg/gtk-4.0/gtk.css"),
            (uint64_t)"/tmp/.config/gtk-4.0/gtk.css", 0, 0, 0);
    int nul = (int)syscall(SYS_OPENAT, (uint64_t)(int64_t)AT_FDCWD, (uint64_t)"/dev/null",
                           (uint64_t)O_RDWR, 0, 0);
    if (nul >= 0) {
      spawn_fds_t fds = {nul, nul, nul};
      pid = spawn_args_fds_env(path, argv, ac, &fds, env);
      syscall(SYS_CLOSE, (uint64_t)nul, 0, 0, 0, 0);
    } else {
      pid = spawn_args_env(path, argv, ac, env);
    }
  }
  char m[160];
  if (pid < 0) {
    snprintf(m, sizeof(m), "No se pudo abrir %s", a->name);
  } else {
    g_children++;
    snprintf(m, sizeof(m), "Abriendo %s...", a->name);
  }
  flash(m);
}

static void reap(void) {
  int st;
  while (g_children > 0 && waitpid(-1, &st, WNOHANG) > 0)
    g_children--;
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
  if (r * 2 > w)
    r = w / 2;
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

// Texto centrado en [x, x+w), recortado con "..." si no cabe.
static void text_center(int x, int y, int w, const char *s, uint32_t col) {
  char buf[128];
  snprintf(buf, sizeof(buf), "%s", s);
  if (ui_text_width(g_font, buf) > w) {
    int n = (int)strlen(s);
    if (n > 120)
      n = 120;
    while (n > 0) {
      n--;
      while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80)
        n--;
      snprintf(buf, sizeof(buf), "%.*s...", n, s);
      if (ui_text_width(g_font, buf) <= w)
        break;
    }
  }
  int tw = ui_text_width(g_font, buf);
  text(x + (w - tw) / 2, y, buf, col);
}

static void blit_icon(const uint32_t *ic, int x, int y, int top, int bottom) {
  for (int j = 0; j < ICON; j++) {
    int Y = y + j;
    if (Y < top || Y >= bottom || Y < 0 || Y >= g_ch)
      continue;
    for (int i = 0; i < ICON; i++) {
      int X = x + i;
      if (X < 0 || X >= g_cw)
        continue;
      uint32_t c = ic[j * ICON + i], a = c >> 24;
      if (!a)
        continue;
      uint32_t *d = &g_px[Y * g_cw + X];
      if (a == 255) {
        *d = c;
        continue;
      }
      uint32_t r = (((c >> 16) & 0xFF) * a + ((*d >> 16) & 0xFF) * (255 - a)) / 255;
      uint32_t g = (((c >> 8) & 0xFF) * a + ((*d >> 8) & 0xFF) * (255 - a)) / 255;
      uint32_t b = ((c & 0xFF) * a + (*d & 0xFF) * (255 - a)) / 255;
      *d = 0xFF000000u | (r << 16) | (g << 8) | b;
    }
  }
}

// Baldosa con iniciales para las apps sin icono: texto ampliado x2.
static void tile_initials(const app_t *a, int x, int y, int top, int bottom) {
  static uint32_t tile[ICON * ICON];
  uint32_t c = a->color;
  for (int i = 0; i < ICON * ICON; i++)
    tile[i] = 0;
  // Baldosa redondeada (radio 14) en el color de la app.
  for (int j = 0; j < ICON; j++)
    for (int i = 0; i < ICON; i++) {
      int R = 14, cx = i < R ? R - i : (i >= ICON - R ? i - (ICON - R - 1) : 0);
      int cy = j < R ? R - j : (j >= ICON - R ? j - (ICON - R - 1) : 0);
      if (cx * cx + cy * cy <= R * R)
        tile[j * ICON + i] = c | 0xFF000000u;
    }
  // Iniciales: primera letra de las dos primeras palabras.
  char ini[8] = {0};
  const char *s = a->name;
  int k = 0;
  ini[k++] = s[0];
  const char *sp = strchr(s, ' ');
  if (sp && sp[1])
    ini[k++] = sp[1];
  for (int i = 0; i < k; i++)
    if (ini[i] >= 'a' && ini[i] <= 'z')
      ini[i] -= 32;
  // Dibujar a tamaño normal en un búfer pequeño y ampliarlo x2.
  static uint32_t small[32 * 24];
  for (int i = 0; i < 32 * 24; i++)
    small[i] = 0xFF000000u;
  ui_draw_text(small, 32, 32, 24, 0, 2, ini, 0xFFFFFF, g_font);
  int tw = ui_text_width(g_font, ini);
  int ox = (ICON - tw * 2) / 2, oy = (ICON - 48) / 2;
  for (int j = 0; j < 48; j++)
    for (int i = 0; i < tw * 2 && i < 64; i++) {
      uint32_t v = small[(j / 2) * 32 + i / 2] & 0xFF; // blanco sobre negro: alfa
      if (!v)
        continue;
      int X = ox + i, Y = oy + j;
      if (X < 0 || Y < 0 || X >= ICON || Y >= ICON || !(tile[Y * ICON + X] >> 24))
        continue;
      uint32_t d = tile[Y * ICON + X];
      uint32_t r = (255 * v + ((d >> 16) & 0xFF) * (255 - v)) / 255;
      uint32_t g = (255 * v + ((d >> 8) & 0xFF) * (255 - v)) / 255;
      uint32_t b = (255 * v + (d & 0xFF) * (255 - v)) / 255;
      tile[Y * ICON + X] = 0xFF000000u | (r << 16) | (g << 8) | b;
    }
  blit_icon(tile, x, y, top, bottom);
}

// Geometría de la rejilla (en coordenadas de contenido, antes del scroll).
static int cols(void) {
  int c = (g_cw - 32) / CELL_W;
  return c < 1 ? 1 : c;
}
static int grid_x0(void) { return (g_cw - cols() * CELL_W) / 2; }
#define SECTION_H 34

// Posición de la celda k (índice en g_vis). Devuelve la y de contenido.
static void cell_pos(int k, int *x, int *y) {
  int c = cols();
  int y0 = 12;
  int local = k;
  if (k >= g_nvis_native) { // sección Linux
    int rows_native = (g_nvis_native + c - 1) / c;
    y0 += (g_nvis_native ? SECTION_H + rows_native * CELL_H + 10 : 0);
    local = k - g_nvis_native;
  }
  *x = grid_x0() + (local % c) * CELL_W;
  *y = y0 + SECTION_H + (local / c) * CELL_H;
}

static int content_h(void) {
  if (!g_nvis)
    return 0;
  int x, y;
  cell_pos(g_nvis - 1, &x, &y);
  return y + CELL_H + 12;
}

static int view_top(void) { return TOP_H; }
static int view_h(void) { return g_ch - TOP_H - FOOT_H; }

static void clamp_scroll(void) {
  int maxs = content_h() - view_h();
  if (maxs < 0)
    maxs = 0;
  if (g_scroll > maxs)
    g_scroll = maxs;
  if (g_scroll < 0)
    g_scroll = 0;
}

static void ensure_visible(int k) {
  if (k < 0)
    return;
  int x, y;
  cell_pos(k, &x, &y);
  if (y - 8 < g_scroll)
    g_scroll = y - 8 - (k < g_nvis_native || k == g_nvis_native ? SECTION_H : 0);
  if (y + CELL_H > g_scroll + view_h())
    g_scroll = y + CELL_H - view_h();
  clamp_scroll();
}

static void redraw(void) {
  int fh = g_font->height;
  fill(0, 0, g_cw, g_ch, P->bg);
  int top = view_top(), bottom = g_ch - FOOT_H;

  // Rejilla.
  for (int sec = 0; sec < 2; sec++) {
    int first = sec == 0 ? 0 : g_nvis_native, last = sec == 0 ? g_nvis_native : g_nvis;
    if (first >= last)
      continue;
    int x, y;
    cell_pos(first, &x, &y);
    int hy = top + y - SECTION_H - g_scroll;
    if (hy + fh > top && hy < bottom) {
      char h[48];
      snprintf(h, sizeof(h), "%s  (%d)", sec == 0 ? "Aurora" : "Apps de Linux", last - first);
      text(grid_x0() + 10, hy + 8, h, P->dim);
      fill(grid_x0() + 10, hy + 8 + fh + 6, cols() * CELL_W - 20, 1, P->line);
    }
    for (int k = first; k < last; k++) {
      cell_pos(k, &x, &y);
      int sy = top + y - g_scroll;
      if (sy + CELL_H <= top || sy >= bottom)
        continue;
      app_t *a = &g_apps[g_vis[k]];
      int sel = k == g_sel, pr = k == g_pressed;
      if (sel || pr) {
        // Recortar a la vista: dibujar solo si cabe entera la parte visible.
        int ry = sy + 4, rh = CELL_H - 8;
        int cy0 = ry < top ? top : ry, cy1 = ry + rh > bottom ? bottom : ry + rh;
        if (cy1 > cy0)
          fill_round(x + 6, cy0, CELL_W - 12, cy1 - cy0, cy0 == ry && cy1 == ry + rh ? 12 : 0,
                     pr ? P->line : P->sel);
      }
      int ix = x + (CELL_W - ICON) / 2, iy = sy + 14;
      if (a->pix && a->backing && iy >= top && iy + ICON <= bottom)
        fill_round(ix, iy, ICON, ICON, 14, a->backing == 2 ? 0xFFE9EAEE : P->field);
      if (a->pix)
        blit_icon(a->pix, ix, iy, top, bottom);
      else
        tile_initials(a, ix, iy, top, bottom);
      int ty = iy + ICON + 10;
      if (ty >= top && ty + fh <= bottom)
        text_center(x + 8, ty, CELL_W - 16, a->name, sel ? P->sel_text : P->text);
    }
  }
  if (!g_nvis) {
    const char *m = g_napps ? "Ninguna app coincide con la búsqueda" : "No hay apps instaladas";
    int tw = ui_text_width(g_font, m);
    text((g_cw - tw) / 2, top + view_h() / 2 - fh / 2, m, P->dim);
  }

  // Barra superior (por encima de la rejilla desplazada).
  fill(0, 0, g_cw, TOP_H, P->top);
  fill(0, TOP_H - 1, g_cw, 1, P->line);
  text(20, (TOP_H - fh) / 2, "Aplicaciones", P->text);
  int sw = g_cw / 3 < 200 ? 200 : g_cw / 3 > 320 ? 320 : g_cw / 3;
  int sx = g_cw - sw - 16, sy = (TOP_H - 32) / 2;
  fill_round(sx, sy, sw, 32, 9, P->field);
  // Lupa: anillo y mango.
  for (int j = -7; j <= 7; j++)
    for (int i = -7; i <= 7; i++) {
      int d2 = i * i + j * j;
      if (d2 <= 36 && d2 >= 20)
        fill(sx + 18 + i, sy + 14 + j, 1, 1, P->dim);
    }
  for (int i = 0; i < 5; i++)
    fill(sx + 22 + i, sy + 18 + i, 2, 2, P->dim);
  int tx = sx + 34, ty = sy + (32 - fh) / 2;
  if (g_search[0]) {
    int ex = text(tx, ty, g_search, P->text);
    if ((now_ms() / 500) & 1)
      fill(ex + 1, ty, 2, fh, P->accent);
  } else {
    text(tx, ty, "Buscar", P->dim);
    if ((now_ms() / 500) & 1)
      fill(tx - 2, ty, 2, fh, P->accent);
  }

  // Pie: mensaje, o la descripción de la seleccionada, o el total.
  fill(0, g_ch - FOOT_H, g_cw, FOOT_H, P->top);
  fill(0, g_ch - FOOT_H, g_cw, 1, P->line);
  char foot[200];
  uint32_t fc = P->dim;
  if (g_msg[0] && now_ms() < g_msg_until) {
    snprintf(foot, sizeof(foot), "%s", g_msg);
    fc = P->accent;
  } else {
    g_msg[0] = 0;
    if (g_sel >= 0 && g_sel < g_nvis) {
      const app_t *a = &g_apps[g_vis[g_sel]];
      snprintf(foot, sizeof(foot), "%s%s%s", a->name, a->comment[0] ? "  ·  " : "", a->comment);
    } else {
      snprintf(foot, sizeof(foot), "%d apps instaladas", g_napps);
    }
  }
  text(16, g_ch - FOOT_H + (FOOT_H - fh) / 2, foot, fc);

  // Barra de desplazamiento.
  int ch = content_h(), vh = view_h();
  if (ch > vh) {
    int h = vh * vh / ch;
    if (h < 24)
      h = 24;
    int y = top + 2 + (vh - 4 - h) * g_scroll / (ch - vh);
    fill_round(g_cw - 8, y, 4, h, 2, P->line);
  }
  sys_win_blit(g_win, 0, 0, g_cw, g_ch, 0, 0, g_cw, g_px);
  g_dirty = 0;
}

static int cell_at(int mx, int my) {
  if (my < view_top() || my >= g_ch - FOOT_H)
    return -1;
  int cy = my - view_top() + g_scroll;
  for (int k = 0; k < g_nvis; k++) {
    int x, y;
    cell_pos(k, &x, &y);
    if (mx >= x + 6 && mx < x + CELL_W - 6 && cy >= y + 4 && cy < y + CELL_H - 4)
      return k;
  }
  return -1;
}

static void move_sel(int d) {
  if (!g_nvis)
    return;
  if (g_sel < 0) {
    g_sel = 0;
  } else {
    int c = cols();
    int n = g_sel + d;
    if (d == c || d == -c) { // arriba/abajo: misma columna, saltando de sección
      int x, y, nx, ny;
      cell_pos(g_sel, &x, &y);
      int best = -1, bd = 1 << 30;
      for (int k = 0; k < g_nvis; k++) {
        cell_pos(k, &nx, &ny);
        if ((d > 0 && ny <= y) || (d < 0 && ny >= y))
          continue;
        int dist = abs(ny - y) * 4 + abs(nx - x);
        if (dist < bd) {
          bd = dist;
          best = k;
        }
      }
      n = best < 0 ? g_sel : best;
    }
    if (n < 0)
      n = 0;
    if (n >= g_nvis)
      n = g_nvis - 1;
    g_sel = n;
  }
  ensure_visible(g_sel);
  g_dirty = 1;
}

static char g_esc[8];
static int g_esc_n;

static void on_byte(unsigned char c);
static void on_byte(unsigned char c) {
  if (g_esc_n > 0) {
    g_esc[g_esc_n++] = (char)c;
    if (g_esc_n >= 3 && ((c >= 'A' && c <= 'Z') || c == '~')) {
      g_esc[g_esc_n] = 0;
      if (!strcmp(g_esc + 1, "[C"))
        move_sel(1);
      else if (!strcmp(g_esc + 1, "[D"))
        move_sel(-1);
      else if (!strcmp(g_esc + 1, "[B"))
        move_sel(cols());
      else if (!strcmp(g_esc + 1, "[A"))
        move_sel(-cols());
      else if (!strcmp(g_esc + 1, "[H")) {
        g_sel = g_nvis ? 0 : -1;
        ensure_visible(g_sel);
      } else if (!strcmp(g_esc + 1, "[F")) {
        g_sel = g_nvis - 1;
        ensure_visible(g_sel);
      } else if (!strcmp(g_esc + 1, "[5~")) {
        g_scroll -= view_h();
        clamp_scroll();
      } else if (!strcmp(g_esc + 1, "[6~")) {
        g_scroll += view_h();
        clamp_scroll();
      }
      g_esc_n = 0;
      g_dirty = 1;
    } else if (g_esc_n >= (int)sizeof(g_esc) - 1) {
      g_esc_n = 0;
    } else if (g_esc_n == 2 && c != '[' && c != 'O') {
      g_esc_n = 0; // Esc suelto seguido de otra tecla: procesarla
      on_byte(c);
    }
    return;
  }
  if (c == 0x1B) {
    // Esc suelto: borrar la búsqueda (si lo que sigue no es una secuencia,
    // el siguiente byte cancela la espera).
    g_esc[0] = 0x1B;
    g_esc_n = 1;
    if (g_search[0]) {
      g_search[0] = 0;
      g_sel = -1;
      refilter();
    }
    return;
  }
  size_t n = strlen(g_search);
  if (c == '\r' || c == '\n') {
    if (g_sel >= 0 && g_sel < g_nvis)
      launch(&g_apps[g_vis[g_sel]]);
  } else if (c == 0x7F || c == 0x08) {
    if (n) {
      n--;
      while (n > 0 && ((unsigned char)g_search[n] & 0xC0) == 0x80)
        n--;
      g_search[n] = 0;
      refilter();
    }
  } else if (c == '\t') {
    move_sel(1);
  } else if (c >= 0x20 && n + 1 < sizeof(g_search)) {
    g_search[n] = (char)c;
    g_search[n + 1] = 0;
    g_sel = -1;
    refilter();
  }
  g_dirty = 1;
}

static int resize_buffer(int w, int h) {
  if (w < 300 || h < TOP_H + FOOT_H + CELL_H)
    return -1;
  uint32_t *n = (uint32_t *)malloc((size_t)w * h * 4);
  if (!n)
    return -1;
  free(g_px);
  g_px = n;
  g_cw = w;
  g_ch = h;
  clamp_scroll();
  g_dirty = 1;
  return 0;
}

int main(int argc, char **argv) {
  g_font = font_system_mono();
  if (!g_font)
    return 1;
  load_apps();
  // aplicaciones --lista: escribe las apps encontradas y sale (pruebas).
  if (argc > 1 && !strcmp(argv[1], "--lista")) {
    for (int i = 0; i < g_napps; i++)
      printf("%s|%s|%s|%s\n", g_apps[i].linux_app ? "linux" : "aurora", g_apps[i].name,
             g_apps[i].app_id, g_apps[i].exec);
    return 0;
  }
  refilter();

  uint32_t ss = sys_screen_size();
  int sw = (int)(ss >> 16), sh = (int)(ss & 0xFFFF);
  if (sw <= 0 || sh <= 0) {
    sw = 1280;
    sh = 800;
  }
  int w = CELL_W * 6 + 48, h = 600;
  if (w > sw - 40)
    w = sw - 40;
  if (h > sh - 160)
    h = sh - 160;
  g_px = (uint32_t *)malloc((size_t)w * h * 4);
  if (!g_px)
    return 1;
  g_cw = w;
  g_ch = h;
  g_win = sys_win_create((sw - w) / 2, (sh - h - TITLEBAR_H) / 2 - 20, w, h + TITLEBAR_H,
                         "Aplicaciones");
  if (g_win < 0)
    return 1;
  sys_win_set_icon(g_win, "/system/icons/app-aplicaciones.bmp");
  syscall(ASYS_WIN_SET_APP_ID, (uint64_t)g_win, (uint64_t)APP_ID, 0, 0, 0);
  g_theme_seen = sys_theme_get();
  P = (g_theme_seen & 1) ? &PAL_LIGHT : &PAL_DARK;

  int blink = -1;
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
      } else if (ev.type == WINSRV_EV_MOUSE) {
        int btn = (int)(ev.data & 0xFF);
        int released = (ev.data >> 8) & 1;
        int motion = (int)((ev.data >> 9) & 1);
        if (btn == 64 || btn == 65) {
          g_scroll += btn == 64 ? -CELL_H / 2 : CELL_H / 2;
          clamp_scroll();
          g_dirty = 1;
        } else if (btn == 0 && !motion && !released) {
          g_pressed = cell_at(ev.x, ev.y);
          if (g_pressed >= 0)
            g_sel = g_pressed;
          g_dirty = 1;
        } else if (btn == 0 && !motion && released) {
          int k = cell_at(ev.x, ev.y);
          if (k >= 0 && k == g_pressed)
            launch(&g_apps[g_vis[k]]);
          g_pressed = -1;
          g_dirty = 1;
        }
      } else if (ev.type == WINSRV_EV_FOCUS || ev.type == WINSRV_EV_BLUR) {
        g_pressed = -1;
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
    int b = (int)((now_ms() / 500) & 1);
    if (b != blink) { // cursor de la búsqueda
      blink = b;
      g_dirty = 1;
    }
    if (g_msg[0] && now_ms() >= g_msg_until)
      g_dirty = 1;
    if (icon_step()) {
      g_dirty = 1;
      continue;
    }
    if (g_dirty)
      redraw();
    struct pollfd pfd = {-1, 0, 0};
    poll(&pfd, 1, 30);
  }
  return 0;
}
