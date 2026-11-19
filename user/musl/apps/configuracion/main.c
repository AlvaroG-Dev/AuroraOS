// user/musl/apps/configuracion/main.c
//
// Configuración de Aurora OS. Secciones en la barra lateral:
//   - Apariencia: modo claro u oscuro del escritorio (ASYS_THEME_SET; el
//     compositor lo guarda en /data/aurora-theme).
//   - Fondo: fondos de /system/wallpapers en miniatura o "Automático" (el del
//     modo); ASYS_WALLPAPER_SET, que el sistema recuerda en /data.
//   - Pantalla: resolución actual del framebuffer.
//   - Red: interfaces (/proc/net/dev) con su IPv4 (SIOCGIFADDR), estado y
//     tráfico.
//   - Almacenamiento: sistemas de ficheros montados (/proc/mounts + statfs)
//     con barra de uso.
//   - Acerca de: nombre del equipo (editable: sethostname y
//     /data/aurora-hostname, que el sistema lee al arrancar), versión,
//     procesador (/proc/cpuinfo), núcleos, memoria y tiempo encendido.
// Teclado: flechas arriba/abajo cambian de sección. Argumentos: el nombre o
// número de una sección para abrirla; --claro / --oscuro cambian el modo y
// --fondo [RUTA.bmp] el fondo, sin abrir ventana. Colores sólidos; el gris
// pizarra de su icono como color propio; sigue el modo claro/oscuro.

// Recortar nombres largos a la columna es intencionado.
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

#define WIN_W 760
#define WIN_H 500
#define TITLEBAR_H 38 // = AURORA_TITLEBAR_HEIGHT (kernel/gfx/theme.h)
#define SIDE_W 190
#define CX (SIDE_W + 28) // margen izquierdo del contenido
#define SYS_SOCKET_N 41
#define SYS_SETHOSTNAME_N 170
#define SYS_STATFS_N 137

typedef struct {
  uint32_t bg, side, line, card, text, dim, accent, sel, sel_text, ok, warn;
} palette_t;

static const palette_t PAL_DARK = {0xFF16181C, 0xFF1D2026, 0xFF2B3039, 0xFF1F232A,
                                   0xFFE7EAEE, 0xFF8D95A1, 0xFF8A94A6, 0xFF323846,
                                   0xFFFFFFFF, 0xFF3FCF8E, 0xFFFFB020};
static const palette_t PAL_LIGHT = {0xFFFFFFFF, 0xFFF2F3F5, 0xFFE0E3E8, 0xFFF6F7F9,
                                    0xFF1B1F26, 0xFF667085, 0xFF59606B, 0xFFDDE1E7,
                                    0xFF1B1F26, 0xFF1F9D63, 0xFFC77700};
static const palette_t *P = &PAL_DARK;

static uint32_t g_px[WIN_W * WIN_H];
static const font_aa_t *g_font;
static int g_win = -1;
static int g_dirty = 1;
static uint32_t g_theme_seen;
static char g_msg[128];

enum { S_APARIENCIA, S_FONDO, S_PANTALLA, S_RED, S_ALMAC, S_ACERCA, S_COUNT };
static const char *k_sections[S_COUNT] = {"Apariencia", "Fondo", "Pantalla", "Red",
                                          "Almacenamiento", "Acerca de"};
#define HEAD_Y 22 // título de la sección
#define BODY_Y 54 // primera tarjeta
static int g_sec = S_APARIENCIA;

static uint64_t now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

// ---- Lectura de ficheros pequeños (/proc) ----
static int read_small(const char *path, char *buf, int cap) {
  int fd = (int)syscall(SYS_OPENAT, (uint64_t)(int64_t)AT_FDCWD, (uint64_t)path,
                        (uint64_t)O_RDONLY, 0, 0);
  if (fd < 0) {
    buf[0] = 0;
    return -1;
  }
  int n = 0;
  while (n < cap - 1) {
    long r = syscall(SYS_READ, (uint64_t)fd, (uint64_t)(buf + n), (uint64_t)(cap - 1 - n), 0, 0);
    if (r <= 0)
      break;
    n += (int)r;
  }
  buf[n] = 0;
  syscall(SYS_CLOSE, (uint64_t)fd, 0, 0, 0, 0);
  return n;
}

static void fmt_bytes(char *out, size_t cap, uint64_t b) {
  const char *u[] = {"B", "KB", "MB", "GB", "TB"};
  int i = 0;
  double v = (double)b;
  while (v >= 1024 && i < 4) {
    v /= 1024;
    i++;
  }
  if (i == 0)
    snprintf(out, cap, "%llu B", (unsigned long long)b);
  else
    snprintf(out, cap, v < 10 ? "%.1f %s" : "%.0f %s", v, u[i]);
}

// ---- Datos del sistema ----
static char g_cpu_model[96] = "Desconocido";
static int g_ncpu = 1;
static char g_version[128];
static char g_hostname[64];
static aurora_sysinfo_t g_si;

static void load_static_info(void) {
  static char buf[16384];
  if (read_small("/proc/cpuinfo", buf, sizeof(buf)) > 0) {
    char *m = strstr(buf, "model name");
    if (m && (m = strchr(m, ':'))) {
      m += 2;
      int n = 0;
      while (m[n] && m[n] != '\n' && n < (int)sizeof(g_cpu_model) - 1)
        n++;
      memcpy(g_cpu_model, m, n);
      g_cpu_model[n] = 0;
      // Espacios dobles de la cadena de marca de Intel.
      for (char *p = g_cpu_model; *p; p++)
        while (p[0] == ' ' && p[1] == ' ')
          memmove(p, p + 1, strlen(p));
    }
  }
  if (read_small("/proc/version", g_version, sizeof(g_version)) > 0) {
    char *nl = strchr(g_version, '\n');
    if (nl)
      *nl = 0;
    char *par = strstr(g_version, " (");
    if (par)
      *par = 0; // "Aurora OS version 0.1.0"
  }
}

static void load_hostname(void) {
  struct {
    char f[6][65];
  } u;
  if (syscall(SYS_UNAME, (uint64_t)&u, 0, 0, 0, 0) == 0)
    snprintf(g_hostname, sizeof(g_hostname), "%s", u.f[1]);
}

// Interfaces de red.
typedef struct {
  char name[16];
  uint64_t rx, tx;
  uint32_t ip;
  int have_ip;
} netif_row_t;
static netif_row_t g_if[8];
static int g_nif;

struct ifreq_s {
  char name[16];
  union {
    struct {
      uint16_t family;
      uint16_t port;
      uint32_t addr;
      uint8_t zero[8];
    } sin;
    uint8_t raw[24];
  } u;
};

static void load_net(void) {
  static char buf[4096];
  g_nif = 0;
  if (read_small("/proc/net/dev", buf, sizeof(buf)) <= 0)
    return;
  int sd = (int)syscall(SYS_SOCKET_N, 2 /* AF_INET */, 2 /* SOCK_DGRAM */, 0, 0, 0);
  char *line = buf;
  int ln = 0;
  while (line && *line && g_nif < 8) {
    char *nl = strchr(line, '\n');
    if (nl)
      *nl = 0;
    if (ln++ >= 2) {
      char *colon = strchr(line, ':');
      if (colon) {
        *colon = 0;
        char *nm = line;
        while (*nm == ' ')
          nm++;
        netif_row_t *r = &g_if[g_nif];
        memset(r, 0, sizeof(*r));
        snprintf(r->name, sizeof(r->name), "%s", nm);
        // bytes packets errs drop fifo frame compressed multicast | bytes ...
        char *p = colon + 1;
        uint64_t v[9];
        for (int i = 0; i < 9; i++)
          v[i] = strtoull(p, &p, 10);
        r->rx = v[0];
        r->tx = v[8];
        if (sd >= 0) {
          struct ifreq_s q;
          memset(&q, 0, sizeof(q));
          snprintf(q.name, sizeof(q.name), "%s", r->name);
          if (syscall(SYS_IOCTL, (uint64_t)sd, 0x8915 /* SIOCGIFADDR */, (uint64_t)&q, 0, 0) == 0) {
            r->ip = q.u.sin.addr;
            r->have_ip = r->ip != 0;
          }
        }
        g_nif++;
      }
    }
    line = nl ? nl + 1 : NULL;
  }
  if (sd >= 0)
    syscall(SYS_CLOSE, (uint64_t)sd, 0, 0, 0, 0);
}

// Montajes.
typedef struct {
  char dev[48], dir[64], type[16];
  uint64_t total, used;
  int have, ro;
} mount_row_t;
static mount_row_t g_mnt[12];
static int g_nmnt;

struct statfs_s {
  int64_t type, bsize;
  uint64_t blocks, bfree, bavail, files, ffree;
  int32_t fsid[2];
  int64_t namelen, frsize, flags, spare[4];
};

static int virtual_fs(const char *t) {
  static const char *v[] = {"proc", "procfs", "sysfs", "devtmpfs", "devpts", "tmpfs",
                            "ramfs", "cgroup", "cgroup2", "debugfs", "devfs",
                            "securityfs", "pstore", "mqueue", "fusectl", "bpf"};
  for (unsigned i = 0; i < sizeof(v) / sizeof(v[0]); i++)
    if (!strcmp(t, v[i]))
      return 1;
  return 0;
}

static void load_mounts(void) {
  static char buf[8192];
  g_nmnt = 0;
  if (read_small("/proc/mounts", buf, sizeof(buf)) <= 0)
    return;
  char *line = buf;
  while (line && *line && g_nmnt < 12) {
    char *nl = strchr(line, '\n');
    if (nl)
      *nl = 0;
    char dev[128], dir[128], type[32], opts[128] = "";
    if (sscanf(line, "%127s %127s %31s %127s", dev, dir, type, opts) >= 3 &&
        (!virtual_fs(type) || !strcmp(dir, "/") || !strcmp(dir, "/tmp"))) {
      int dup = 0;
      for (int i = 0; i < g_nmnt; i++)
        dup |= !strcmp(g_mnt[i].dir, dir);
      if (!dup) {
        mount_row_t *m = &g_mnt[g_nmnt];
        memset(m, 0, sizeof(*m));
        snprintf(m->dev, sizeof(m->dev), "%s", dev);
        snprintf(m->dir, sizeof(m->dir), "%s", dir);
        snprintf(m->type, sizeof(m->type), "%s", type);
        m->ro = !strncmp(opts, "ro", 2) && (opts[2] == 0 || opts[2] == ',');
        if (!strcmp(type, "iso9660") || !strcmp(type, "tarfs"))
          m->ro = 1;
        struct statfs_s sf;
        memset(&sf, 0, sizeof(sf));
        if (syscall(SYS_STATFS_N, (uint64_t)dir, (uint64_t)&sf, 0, 0, 0) == 0 && sf.blocks) {
          uint64_t bs = sf.frsize ? (uint64_t)sf.frsize : (uint64_t)sf.bsize;
          m->total = sf.blocks * bs;
          m->used = (sf.blocks - sf.bfree) * bs;
          m->have = 1;
        }
        g_nmnt++;
      }
    }
    line = nl ? nl + 1 : NULL;
  }
}

static int g_walls_loaded;     // (definidos con los fondos, más abajo)
static char g_wall_cur[192];

static void refresh_live(void) {
  memset(&g_si, 0, sizeof(g_si));
  sys_sysinfo(&g_si);
  if (g_si.ncpu)
    g_ncpu = (int)g_si.ncpu;
  if (g_sec == S_RED)
    load_net();
  if (g_sec == S_ALMAC)
    load_mounts();
  if (g_sec == S_FONDO && g_walls_loaded) { // lo puede cambiar Imágenes
    char cur[sizeof(g_wall_cur)];
    if (sys_wallpaper_get(cur, sizeof(cur)) >= 0)
      memcpy(g_wall_cur, cur, sizeof(cur));
  }
}

// ---- Fondos de escritorio ----
#define WALL_MAX 16
#define TH_COLS 4
#define TH_GAP 14
#define TH_W ((WIN_W - CX - 28 - (TH_COLS - 1) * TH_GAP) / TH_COLS)
#define TH_H (TH_W * 9 / 16)
#define TH_ROW (TH_H + 34)
typedef struct {
  char path[128]; // "" = automático (el del modo)
  char label[40];
  uint32_t *thumb;
  int tried;
} wall_t;
static wall_t g_wall[WALL_MAX];
static int g_nwall;
static char g_wall_cur[192];
static int g_walls_loaded;

struct linux_dirent64 {
  uint64_t d_ino;
  int64_t d_off;
  unsigned short d_reclen;
  unsigned char d_type;
  char d_name[];
};

static uint8_t *read_all(const char *path, size_t *out_n) {
  int fd = (int)syscall(SYS_OPENAT, (uint64_t)(int64_t)AT_FDCWD, (uint64_t)path,
                        (uint64_t)O_RDONLY, 0, 0);
  if (fd < 0)
    return NULL;
  size_t cap = 1 << 20, n = 0;
  uint8_t *b = (uint8_t *)malloc(cap);
  while (b) {
    if (n == cap) {
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

// Reduce `src` (sw x sh) a dw x dh recortando para llenar (como el escritorio
// estira, la miniatura se ve casi igual), promediando 4 muestras por píxel.
static uint32_t *scale_cover(const uint32_t *src, int sw, int sh, int dw, int dh) {
  uint32_t *o = (uint32_t *)malloc((size_t)dw * dh * 4);
  if (!o)
    return NULL;
  double sx = (double)sw / dw, sy = (double)sh / dh;
  for (int y = 0; y < dh; y++)
    for (int x = 0; x < dw; x++) {
      uint32_t r = 0, g = 0, b = 0;
      for (int k = 0; k < 4; k++) {
        int px = (int)((x + 0.25 + 0.5 * (k & 1)) * sx), py = (int)((y + 0.25 + 0.5 * (k >> 1)) * sy);
        if (px >= sw) px = sw - 1;
        if (py >= sh) py = sh - 1;
        uint32_t c = src[(size_t)py * sw + px];
        r += (c >> 16) & 0xFF;
        g += (c >> 8) & 0xFF;
        b += c & 0xFF;
      }
      o[y * dw + x] = 0xFF000000u | ((r / 4) << 16) | ((g / 4) << 8) | (b / 4);
    }
  return o;
}

static int wall_cmp(const void *a, const void *b) {
  return strcmp(((const wall_t *)a)->label, ((const wall_t *)b)->label);
}

static void load_walls(void) {
  g_walls_loaded = 1;
  g_nwall = 0;
  wall_t *a = &g_wall[g_nwall++];
  memset(a, 0, sizeof(*a));
  snprintf(a->label, sizeof(a->label), "Automático");
  const char *dir = "/system/wallpapers";
  int fd = (int)syscall(SYS_OPENAT, (uint64_t)(int64_t)AT_FDCWD, (uint64_t)dir,
                        (uint64_t)(O_RDONLY | 0x10000), 0, 0);
  if (fd >= 0) {
    static char buf[8192];
    for (;;) {
      long n = syscall(SYS_GETDENTS64, (uint64_t)fd, (uint64_t)buf, sizeof(buf), 0, 0);
      if (n <= 0)
        break;
      for (long off = 0; off < n;) {
        struct linux_dirent64 *d = (struct linux_dirent64 *)(buf + off);
        off += d->d_reclen;
        const char *nm = d->d_name;
        size_t ln = strlen(nm);
        if (nm[0] == '.' || ln < 5 || strcmp(nm + ln - 4, ".bmp") || g_nwall >= WALL_MAX)
          continue;
        wall_t *w = &g_wall[g_nwall++];
        memset(w, 0, sizeof(*w));
        snprintf(w->path, sizeof(w->path), "%s/%s", dir, nm);
        const char *b = nm;
        if (!strncmp(b, "aurora-", 7))
          b += 7;
        else if (!strncmp(b, "wallpaper-", 10))
          b += 10;
        snprintf(w->label, sizeof(w->label), "%.*s", (int)(strlen(b) - 4), b);
        for (char *q = w->label; *q; q++)
          if (*q == '-')
            *q = ' ';
        if (w->label[0] >= 'a' && w->label[0] <= 'z')
          w->label[0] -= 32;
      }
    }
    syscall(SYS_CLOSE, (uint64_t)fd, 0, 0, 0, 0);
  }
  if (g_nwall > 2)
    qsort(g_wall + 1, (size_t)(g_nwall - 1), sizeof(wall_t), wall_cmp);
  sys_wallpaper_get(g_wall_cur, sizeof(g_wall_cur));
}

static int wall_find(const char *suffix) {
  for (int i = 1; i < g_nwall; i++) {
    size_t a = strlen(g_wall[i].path), b = strlen(suffix);
    if (a >= b && !strcmp(g_wall[i].path + a - b, suffix))
      return i;
  }
  return -1;
}

// Prepara una miniatura pendiente. Devuelve 1 si hizo trabajo.
static int wall_thumb_step(void) {
  for (int i = 1; i < g_nwall; i++) {
    wall_t *w = &g_wall[i];
    if (w->tried)
      continue;
    w->tried = 1;
    size_t n = 0;
    uint8_t *d = read_all(w->path, &n);
    if (d) {
      int iw, ih;
      uint32_t *px = img_decode(d, n, &iw, &ih);
      free(d);
      if (px) {
        w->thumb = scale_cover(px, iw, ih, TH_W, TH_H);
        free(px);
      }
    }
    return 1;
  }
  return 0;
}

static void choose_wall(int i) {
  if (i < 0 || i >= g_nwall)
    return;
  int rc = sys_wallpaper_set(g_wall[i].path);
  if (rc != 0)
    snprintf(g_msg, sizeof(g_msg), "No se pudo poner ese fondo (%d)", rc);
  else
    g_msg[0] = 0;
  sys_wallpaper_get(g_wall_cur, sizeof(g_wall_cur));
  g_dirty = 1;
}

// ---- Edición del nombre del equipo ----
static int g_editing;
static char g_edit[64];

static int host_char_ok(unsigned char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
         c == '-';
}

static void save_hostname(void) {
  int n = (int)strlen(g_edit);
  while (n > 0 && g_edit[n - 1] == '-')
    g_edit[--n] = 0;
  if (n == 0 || g_edit[0] == '-') {
    snprintf(g_msg, sizeof(g_msg), "Nombre no válido: letras, cifras y guiones");
    return;
  }
  long rc = syscall(SYS_SETHOSTNAME_N, (uint64_t)g_edit, (uint64_t)n, 0, 0, 0);
  int fd = (int)syscall(SYS_OPENAT, (uint64_t)(int64_t)AT_FDCWD,
                        (uint64_t)"/data/aurora-hostname",
                        (uint64_t)(O_WRONLY | O_CREAT | O_TRUNC), 0644, 0);
  int saved = 0;
  if (fd >= 0) {
    char line[72];
    int ln = snprintf(line, sizeof(line), "%s\n", g_edit);
    saved = syscall(SYS_WRITE, (uint64_t)fd, (uint64_t)line, (uint64_t)ln, 0, 0) == ln;
    syscall(SYS_CLOSE, (uint64_t)fd, 0, 0, 0, 0);
  }
  if (rc == 0 && saved)
    snprintf(g_msg, sizeof(g_msg), "Nombre del equipo cambiado");
  else if (rc == 0)
    snprintf(g_msg, sizeof(g_msg), "Cambiado; no se pudo guardar en /data");
  else if (saved)
    snprintf(g_msg, sizeof(g_msg), "Guardado; se aplicará al reiniciar");
  else
    snprintf(g_msg, sizeof(g_msg), "No se pudo cambiar el nombre (%ld)", rc);
  load_hostname();
  g_editing = 0;
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

static void stroke_round(int x, int y, int w, int h, int r, int t, uint32_t c, uint32_t inner) {
  fill_round(x, y, w, h, r, c);
  fill_round(x + t, y + t, w - 2 * t, h - 2 * t, r > t ? r - t : 0, inner);
}

static int text(int x, int y, const char *s, uint32_t col) {
  return ui_draw_text(g_px, WIN_W, WIN_W, WIN_H, x, y, s, col & 0xFFFFFF, g_font);
}

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

// Fila "etiqueta ........ valor" dentro de una tarjeta.
static int row_kv(int x, int y, int w, const char *k, const char *v) {
  int fh = g_font->height;
  text(x + 16, y + (36 - fh) / 2, k, P->dim);
  int vw = ui_text_width(g_font, v);
  int maxv = w - 32 - 150;
  if (vw > maxv)
    vw = maxv;
  text_fit(x + w - 16 - vw, y + (36 - fh) / 2, v, maxv, P->text);
  return y + 36;
}

static void card(int x, int y, int w, int h) { fill_round(x, y, w, h, 10, P->card); }
static void card_sep(int x, int y, int w) { fill(x + 16, y, w - 32, 1, P->line); }

// Miniatura de escritorio para el selector de modo.
static void mini_desktop(int x, int y, int w, int h, int light) {
  uint32_t desk = light ? 0xFFE9ECF1 : 0xFF101317;
  uint32_t bar = light ? 0xFFFFFFFF : 0xFF1B1E24;
  uint32_t win = light ? 0xFFFFFFFF : 0xFF22262D;
  uint32_t line = light ? 0xFFD5DAE1 : 0xFF343A44;
  uint32_t txt = light ? 0xFFB7BEC9 : 0xFF4A515C;
  fill_round(x, y, w, h, 8, desk);
  fill(x + 6, y + 4, w - 12, 8, bar);
  fill_round(x + 14, y + 22, w - 46, h - 40, 6, line);
  fill_round(x + 15, y + 23, w - 48, h - 42, 5, win);
  fill(x + 24, y + 36, w - 90, 4, txt);
  fill(x + 24, y + 46, w - 110, 4, txt);
  fill(x + 24, y + 56, w - 100, 4, txt);
  fill_round(x + w / 2 - 30, y + h - 14, 60, 9, 4, bar);
}

#define MODE_Y (BODY_Y + 4)
#define MODE_W 200
#define MODE_H 124
#define HOST_Y 80 // fila del nombre del equipo (Acerca de)

static void draw_apariencia(void) {
  int fh = g_font->height;
  text(CX, HEAD_Y, "Modo", P->text);
  int light = (int)(g_theme_seen & 1);
  for (int i = 0; i < 2; i++) {
    int x = CX + i * (MODE_W + 24), y = MODE_Y;
    int on = (i == 0) == !!light;
    if (on)
      stroke_round(x - 4, y - 4, MODE_W + 8, MODE_H + 8, 12, 3, P->accent, P->bg);
    mini_desktop(x, y, MODE_W, MODE_H, i == 0);
    const char *lab = i == 0 ? "Claro" : "Oscuro";
    int tw = ui_text_width(g_font, lab);
    text(x + (MODE_W - tw) / 2, y + MODE_H + 12, lab, on ? P->text : P->dim);
  }
  int y = MODE_Y + MODE_H + 12 + fh + 24;
  text_fit(CX, y, "El modo se aplica a todo el escritorio y a las apps, y se recuerda",
           WIN_W - CX - 24, P->dim);
  text_fit(CX, y + fh + 6, "al reiniciar. También en el menú Aurora.", WIN_W - CX - 24, P->dim);
}

static void blit_thumb(const uint32_t *t, int x, int y, int x0, int x1) {
  for (int j = 0; j < TH_H; j++)
    for (int i = x0; i < x1; i++) {
      int X = x + i, Y = y + j;
      if (X >= 0 && Y >= 0 && X < WIN_W && Y < WIN_H)
        g_px[Y * WIN_W + X] = t[j * TH_W + i];
    }
}

// Esquinas redondeadas sobre la miniatura: pintar el fondo fuera del radio.
static void round_corners(int x, int y, int w, int h, int r, uint32_t bg) {
  for (int j = 0; j < r; j++)
    for (int i = 0; i < r; i++) {
      int dx = r - i, dy = r - j;
      if (dx * dx + dy * dy > r * r) {
        fill(x + i, y + j, 1, 1, bg);
        fill(x + w - 1 - i, y + j, 1, 1, bg);
        fill(x + i, y + h - 1 - j, 1, 1, bg);
        fill(x + w - 1 - i, y + h - 1 - j, 1, 1, bg);
      }
    }
}

static void wall_rect(int i, int *x, int *y) {
  *x = CX + (i % TH_COLS) * (TH_W + TH_GAP);
  *y = BODY_Y + 4 + (i / TH_COLS) * TH_ROW;
}

static void draw_fondo(void) {
  text(CX, HEAD_Y, "Fondo de escritorio", P->text);
  for (int i = 0; i < g_nwall; i++) {
    int x, y;
    wall_rect(i, &x, &y);
    if (y + TH_H > WIN_H - 30)
      break;
    wall_t *w = &g_wall[i];
    int on = !strcmp(w->path, g_wall_cur);
    if (on)
      stroke_round(x - 4, y - 4, TH_W + 8, TH_H + 8, 10, 3, P->accent, P->bg);
    if (i == 0) {
      // Automático: mitad claro (alba) y mitad oscuro (noche).
      int a = wall_find("/aurora-alba.bmp"), b = wall_find("/aurora-noche.bmp");
      if (a > 0 && g_wall[a].thumb)
        blit_thumb(g_wall[a].thumb, x, y, 0, TH_W / 2);
      else
        fill(x, y, TH_W / 2, TH_H, 0xFFE9ECF1);
      if (b > 0 && g_wall[b].thumb)
        blit_thumb(g_wall[b].thumb, x, y, TH_W / 2, TH_W);
      else
        fill(x + TH_W / 2, y, TH_W - TH_W / 2, TH_H, 0xFF101317);
    } else if (w->thumb) {
      blit_thumb(w->thumb, x, y, 0, TH_W);
    } else {
      fill(x, y, TH_W, TH_H, P->card);
    }
    round_corners(x, y, TH_W, TH_H, 6, P->bg);
    int tw = ui_text_width(g_font, w->label);
    if (tw > TH_W)
      tw = TH_W;
    text_fit(x + (TH_W - tw) / 2, y + TH_H + 8, w->label, TH_W, on ? P->text : P->dim);
  }
  if (!g_msg[0])
    text_fit(CX, WIN_H - 26, "Automático usa Alba en modo claro y Noche en modo oscuro.",
             WIN_W - CX - 20, P->dim);
}

static void draw_pantalla(void) {
  int w = WIN_W - CX - 28;
  uint32_t ss = sys_screen_size();
  char res[48], ratio[24];
  int sw = (int)(ss >> 16), sh = (int)(ss & 0xFFFF);
  snprintf(res, sizeof(res), "%d x %d", sw, sh);
  int a = sw, b = sh;
  while (b) {
    int t = a % b;
    a = b;
    b = t;
  }
  if (a > 0)
    snprintf(ratio, sizeof(ratio), "%d:%d", sw / a, sh / a);
  else
    snprintf(ratio, sizeof(ratio), "-");
  if (!strcmp(ratio, "8:5"))
    snprintf(ratio, sizeof(ratio), "16:10");
  text(CX, HEAD_Y, "Pantalla", P->text);
  card(CX, BODY_Y, w, 36 * 3);
  int y = row_kv(CX, BODY_Y, w, "Resolución", res);
  card_sep(CX, y, w);
  y = row_kv(CX, y, w, "Proporción", ratio);
  card_sep(CX, y, w);
  row_kv(CX, y, w, "Escala", "100 %");
  // Dibujo de la pantalla a escala.
  int pw = 220, ph = sw > 0 ? pw * sh / sw : 120;
  if (ph > 150)
    ph = 150;
  int px = CX + (w - pw) / 2, py = BODY_Y + 36 * 3 + 30;
  fill_round(px - 6, py - 6, pw + 12, ph + 12, 8, P->line);
  fill_round(px, py, pw, ph, 4, P->sel);
  fill(px + pw / 2 - 18, py + ph + 6, 36, 10, P->line);
  fill_round(px + pw / 2 - 40, py + ph + 16, 80, 6, 3, P->line);
  int tw = ui_text_width(g_font, res);
  text(px + (pw - tw) / 2, py + (ph - g_font->height) / 2, res, P->sel_text);
}

static void draw_red(void) {
  int fh = g_font->height, w = WIN_W - CX - 28;
  text(CX, HEAD_Y, "Red", P->text);
  if (g_nif == 0) {
    text(CX, BODY_Y + 8, "No hay interfaces de red.", P->dim);
    return;
  }
  int y = BODY_Y;
  for (int i = 0; i < g_nif && y + 72 < WIN_H - 30; i++) {
    netif_row_t *r = &g_if[i];
    card(CX, y, w, 64);
    int lo = !strcmp(r->name, "lo");
    uint32_t dot = r->have_ip ? P->ok : P->warn;
    fill_round(CX + 16, y + 14, 10, 10, 5, dot);
    char title[64];
    snprintf(title, sizeof(title), "%s%s", r->name, lo ? "  (bucle local)" : "");
    text(CX + 34, y + 12 - (fh - 14) / 2, title, P->text);
    char ip[32], st[160], rx[24], tx[24];
    if (r->have_ip)
      snprintf(ip, sizeof(ip), "%u.%u.%u.%u", r->ip & 0xFF, (r->ip >> 8) & 0xFF,
               (r->ip >> 16) & 0xFF, r->ip >> 24);
    else
      snprintf(ip, sizeof(ip), "sin dirección");
    int iw = ui_text_width(g_font, ip);
    text(CX + w - 16 - iw, y + 12 - (fh - 14) / 2, ip, r->have_ip ? P->text : P->dim);
    fmt_bytes(rx, sizeof(rx), r->rx);
    fmt_bytes(tx, sizeof(tx), r->tx);
    snprintf(st, sizeof(st), "Recibido %s  ·  Enviado %s", rx, tx);
    text_fit(CX + 34, y + 38, st, w - 50, P->dim);
    y += 76;
  }
}

static void draw_almac(void) {
  int fh = g_font->height, w = WIN_W - CX - 28;
  text(CX, HEAD_Y, "Almacenamiento", P->text);
  if (g_nmnt == 0) {
    text(CX, BODY_Y + 8, "No hay sistemas de ficheros montados.", P->dim);
    return;
  }
  int y = BODY_Y;
  for (int i = 0; i < g_nmnt && y + 74 < WIN_H - 10; i++) {
    mount_row_t *m = &g_mnt[i];
    card(CX, y, w, 66);
    char title[128];
    const char *nice = !strcmp(m->dir, "/") ? "Sistema" : !strcmp(m->dir, "/data") ? "Datos"
                     : !strcmp(m->dir, "/tmp") ? "Temporal" : !strcmp(m->dir, "/cdrom") ? "Disco de arranque"
                     : NULL;
    if (nice)
      snprintf(title, sizeof(title), "%s  (%s)", nice, m->dir);
    else
      snprintf(title, sizeof(title), "%s", m->dir);
    text_fit(CX + 16, y + 10, title, w / 2, P->text);
    char right[96];
    if (m->have) {
      char u[24], t[24];
      fmt_bytes(u, sizeof(u), m->used);
      fmt_bytes(t, sizeof(t), m->total);
      snprintf(right, sizeof(right), "%s de %s", u, t);
    } else {
      snprintf(right, sizeof(right), "%s", m->type);
    }
    int rw = ui_text_width(g_font, right);
    text(CX + w - 16 - rw, y + 10, right, P->dim);
    int bx = CX + 16, bw = w - 32, by = y + 18 + fh;
    fill_round(bx, by, bw, 8, 4, P->line);
    if (m->have && m->total) {
      int pw = (int)((double)bw * (double)m->used / (double)m->total);
      if (pw > 0 && pw < 8)
        pw = 8;
      double pct = (double)m->used / (double)m->total;
      fill_round(bx, by, pw, 8, 4, pct > 0.9 && !m->ro ? P->warn : P->accent);
    }
    char sub[128];
    int nodev = !strcmp(m->dev, "none") || !strcmp(m->dev, m->type);
    snprintf(sub, sizeof(sub), "%s%s%s%s", m->type, nodev ? "" : "  ·  ", nodev ? "" : m->dev,
             m->ro ? "  ·  solo lectura" : "");
    text_fit(CX + 16, by + 12, sub, w - 32, P->dim);
    y += 78;
  }
  (void)fh;
}

static void draw_acerca(void) {
  int fh = g_font->height, w = WIN_W - CX - 28;
  // Identidad: el verde aurora solo aquí.
  fill_round(CX, 18, 44, 44, 12, 0xFF3EE89B);
  fill_round(CX + 12, 30, 20, 20, 10, 0xFF0F3D2A);
  fill_round(CX + 17, 35, 10, 10, 5, 0xFF3EE89B);
  text(CX + 58, 22, "Aurora OS", P->text);
  text(CX + 58, 22 + fh + 4, g_version[0] ? g_version : "Aurora OS", P->dim);

  int y = HOST_Y;
  card(CX, y, w, 36);
  int fy = y + (36 - fh) / 2;
  text(CX + 16, fy, "Nombre del equipo", P->dim);
  if (g_editing) {
    int bw = 220, bx = CX + w - 16 - bw;
    stroke_round(bx, y + 5, bw, 26, 6, 2, P->accent, P->bg);
    int ex = text(bx + 8, fy, g_edit, P->text);
    if ((now_ms() / 500) & 1)
      fill(ex + 1, fy, 2, fh, P->text);
  } else {
    char hv[96];
    snprintf(hv, sizeof(hv), "%s   Cambiar", g_hostname);
    int hw = ui_text_width(g_font, hv);
    int nx = CX + w - 16 - hw;
    int ax = text(nx, fy, g_hostname, P->text);
    text(ax + ui_text_width(g_font, "   "), fy, "Cambiar", P->accent);
  }
  y += 48;

  char cores[16], mem[64], up[48];
  snprintf(cores, sizeof(cores), "%d", g_ncpu);
  char mt[24], mf[24];
  fmt_bytes(mt, sizeof(mt), g_si.mem_total_kb * 1024);
  fmt_bytes(mf, sizeof(mf), g_si.mem_free_kb * 1024);
  snprintf(mem, sizeof(mem), "%s (%s libres)", mt, mf);
  uint64_t s = g_si.uptime_ms / 1000;
  if (s >= 86400)
    snprintf(up, sizeof(up), "%llu d %llu h %llu min", (unsigned long long)(s / 86400),
             (unsigned long long)(s / 3600 % 24), (unsigned long long)(s / 60 % 60));
  else if (s >= 3600)
    snprintf(up, sizeof(up), "%llu h %llu min", (unsigned long long)(s / 3600),
             (unsigned long long)(s / 60 % 60));
  else
    snprintf(up, sizeof(up), "%llu min %llu s", (unsigned long long)(s / 60),
             (unsigned long long)(s % 60));
  uint32_t ss = sys_screen_size();
  char res[32];
  snprintf(res, sizeof(res), "%u x %u", ss >> 16, ss & 0xFFFF);

  card(CX, y, w, 36 * 6);
  int ry = row_kv(CX, y, w, "Procesador", g_cpu_model);
  card_sep(CX, ry, w);
  ry = row_kv(CX, ry, w, "Núcleos", cores);
  card_sep(CX, ry, w);
  ry = row_kv(CX, ry, w, "Memoria", mem);
  card_sep(CX, ry, w);
  ry = row_kv(CX, ry, w, "Pantalla", res);
  card_sep(CX, ry, w);
  ry = row_kv(CX, ry, w, "Arquitectura", "x86_64");
  card_sep(CX, ry, w);
  row_kv(CX, ry, w, "Tiempo encendido", up);
}

static void redraw(void) {
  int fh = g_font->height;
  fill(0, 0, WIN_W, WIN_H, P->bg);
  fill(0, 0, SIDE_W, WIN_H, P->side);
  fill(SIDE_W - 1, 0, 1, WIN_H, P->line);
  for (int i = 0; i < S_COUNT; i++) {
    int y = 20 + i * 34;
    int on = i == g_sec;
    if (on)
      fill_round(10, y, SIDE_W - 20, 28, 7, P->sel);
    fill_round(22, y + 9, 10, 10, 3, on ? P->accent : P->line);
    text(42, y + (28 - fh) / 2, k_sections[i], on ? P->sel_text : P->text);
  }
  switch (g_sec) {
  case S_APARIENCIA: draw_apariencia(); break;
  case S_FONDO: draw_fondo(); break;
  case S_PANTALLA: draw_pantalla(); break;
  case S_RED: draw_red(); break;
  case S_ALMAC: draw_almac(); break;
  case S_ACERCA: draw_acerca(); break;
  }
  if (g_msg[0])
    text_fit(CX, WIN_H - 26, g_msg, WIN_W - CX - 20, P->dim);
  sys_win_blit(g_win, 0, 0, WIN_W, WIN_H, 0, 0, WIN_W, g_px);
  g_dirty = 0;
}

static void set_section(int s) {
  if (s < 0 || s >= S_COUNT || s == g_sec)
    return;
  g_sec = s;
  g_editing = 0;
  g_msg[0] = 0;
  if (s == S_FONDO && !g_walls_loaded)
    load_walls();
  refresh_live();
  g_dirty = 1;
}

static void on_click(int x, int y) {
  if (x < SIDE_W) {
    for (int i = 0; i < S_COUNT; i++) {
      int ry = 20 + i * 34;
      if (y >= ry && y < ry + 28)
        set_section(i);
    }
    return;
  }
  if (g_sec == S_APARIENCIA && y >= MODE_Y && y < MODE_Y + MODE_H + 30) {
    for (int i = 0; i < 2; i++) {
      int bx = CX + i * (MODE_W + 24);
      if (x >= bx && x < bx + MODE_W) {
        int light = i == 0;
        if (light != (int)(g_theme_seen & 1)) {
          if (sys_theme_set(light) != 0)
            snprintf(g_msg, sizeof(g_msg), "No se pudo cambiar el modo");
        }
      }
    }
  } else if (g_sec == S_FONDO) {
    for (int i = 0; i < g_nwall; i++) {
      int wx, wy;
      wall_rect(i, &wx, &wy);
      if (x >= wx && x < wx + TH_W && y >= wy && y < wy + TH_H + 26)
        choose_wall(i);
    }
  } else if (g_sec == S_ACERCA && y >= HOST_Y && y < HOST_Y + 36) {
    if (!g_editing) {
      g_editing = 1;
      snprintf(g_edit, sizeof(g_edit), "%s", g_hostname);
      g_msg[0] = 0;
    }
  } else if (g_editing) {
    g_editing = 0;
  }
  g_dirty = 1;
}

static int g_esc_n;
static char g_esc[8];

static void on_byte(unsigned char c) {
  if (g_esc_n > 0) {
    g_esc[g_esc_n++] = (char)c;
    if (g_esc_n >= 3 && ((c >= 'A' && c <= 'Z') || c == '~')) {
      g_esc[g_esc_n] = 0;
      if (!g_editing) {
        if (!strcmp(g_esc + 1, "[A"))
          set_section(g_sec - 1);
        else if (!strcmp(g_esc + 1, "[B"))
          set_section(g_sec + 1);
      }
      g_esc_n = 0;
    } else if (g_esc_n >= (int)sizeof(g_esc) - 1 || (g_esc_n == 2 && c != '[' && c != 'O')) {
      g_esc_n = 0;
      if (g_editing) // Esc suelto: cancelar
        g_editing = 0;
      g_dirty = 1;
    }
    return;
  }
  if (c == 0x1B) {
    g_esc[0] = 0x1B;
    g_esc_n = 1;
    return;
  }
  if (!g_editing)
    return;
  int n = (int)strlen(g_edit);
  if (c == '\r' || c == '\n') {
    save_hostname();
  } else if (c == 0x7F || c == 0x08) {
    if (n > 0)
      g_edit[n - 1] = 0;
  } else if (host_char_ok(c) && n < 63) {
    g_edit[n] = (char)(c >= 'A' && c <= 'Z' ? c + 32 : c);
    g_edit[n + 1] = 0;
  }
  g_dirty = 1;
}

int main(int argc, char **argv) {
  g_font = font_system_mono();
  if (!g_font)
    return 1;
  // configuracion --claro | --oscuro: cambia el modo sin abrir ventana.
  if (argc > 1 && (!strcmp(argv[1], "--claro") || !strcmp(argv[1], "--oscuro")))
    return sys_theme_set(!strcmp(argv[1], "--claro")) == 0 ? 0 : 1;
  // configuracion --fondo [RUTA.bmp]: pone ese fondo (sin ruta: el del modo).
  if (argc > 1 && !strcmp(argv[1], "--fondo"))
    return sys_wallpaper_set(argc > 2 ? argv[2] : "") == 0 ? 0 : 1;
  if (argc > 1) {
    for (int i = 0; i < S_COUNT; i++)
      if (!strcmp(argv[1], k_sections[i]) || (argv[1][0] == '0' + i && !argv[1][1]))
        g_sec = i;
  }
  load_static_info();
  load_hostname();
  if (g_sec == S_FONDO)
    load_walls();
  refresh_live();

  g_win = sys_win_create(170, 90, WIN_W, WIN_H + TITLEBAR_H, "Configuración");
  if (g_win < 0)
    return 1;
  sys_win_set_icon(g_win, "/system/icons/app-configuracion.bmp");
  g_theme_seen = sys_theme_get();
  P = (g_theme_seen & 1) ? &PAL_LIGHT : &PAL_DARK;

  uint64_t last_refresh = now_ms();
  int blink = 0;
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
        if (btn == 0 && !released && !motion)
          on_click(ev.x, ev.y);
      } else if (ev.type == WINSRV_EV_FOCUS || ev.type == WINSRV_EV_BLUR) {
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
    if (t - last_refresh >= 2000 && (g_sec >= S_RED || g_sec == S_FONDO)) {
      last_refresh = t;
      refresh_live();
      g_dirty = 1;
    }
    if (g_editing && (int)((t / 500) & 1) != blink) {
      blink = (int)((t / 500) & 1);
      g_dirty = 1;
    }
    if (g_sec == S_FONDO && wall_thumb_step()) {
      redraw(); // una miniatura más; seguir sin esperar
      continue;
    }
    if (g_dirty)
      redraw();
    struct pollfd pfd = {-1, 0, 0};
    poll(&pfd, 1, 30);
  }
  return 0;
}
