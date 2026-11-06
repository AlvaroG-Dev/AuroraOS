// user/musl/apps/aurora-installer/main.c
//
// Instalador grafico de AuroraOS (estilo asistente de macOS): pantalla completa,
// sin Dock ni barra de menus (el kernel lo hace con aur.kiosk=1), barra lateral con
// los pasos y un panel de contenido. Es la cara de /usr/sbin/aurora-install:
// recoge disco, cuenta y paquetes opcionales, lanza el motor y muestra su progreso
// (/run/aurora-install.progress). Colores solidos, verde aurora solo como acento.
//
// Pasos: Bienvenida -> Disco -> Cuenta -> Paquetes -> Resumen -> Instalando -> Listo.
// Teclado: Tab cambia de campo, Intro continua, Retroceso borra.

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

// libc (glibc/musl estatica): se declaran a mano para no chocar con syscall.h.
extern int fork(void);
extern int execv(const char *path, char *const argv[]);
extern int c_waitpid(int pid, int *status, int options) __asm__("waitpid");
extern int usleep(unsigned usec);
extern int openat(int dirfd, const char *path, int flags, ...);
extern char *crypt(const char *key, const char *salt);
extern void _exit(int status) __attribute__((noreturn));

#define O_RDONLY_ 0
#define O_WRONLY_ 1
#define O_CREAT_ 0100
#define O_TRUNC_ 01000
#define WNOHANG_ 1

typedef struct {
  uint32_t bg, card, line, text, dim, accent, accent_text, danger, field;
} palette_t;
static const palette_t PAL_DARK = {0xFF0E1412, 0xFF16201C, 0xFF263530, 0xFFE8F2EE, 0xFF8FA39B,
                                   0xFF3EE89B, 0xFF04261A, 0xFFFF6A3D, 0xFF111A17};
static const palette_t PAL_LIGHT = {0xFFF4F8F6, 0xFFFFFFFF, 0xFFD2E0D9, 0xFF14201B, 0xFF5B6E66,
                                    0xFF3EE89B, 0xFF04261A, 0xFFD9481F, 0xFFFFFFFF};
static const palette_t *P = &PAL_DARK;

static int W, H;
static uint32_t *g_px;
static const font_aa_t *g_font;
static int g_win = -1, g_dirty = 1;
static uint32_t g_theme_seen;

// ---- Pasos ----
enum { S_WELCOME, S_DISK, S_ACCOUNT, S_PACKAGES, S_SUMMARY, S_INSTALL, S_DONE, S_N };
static const char *k_step_name[S_N] = {"Bienvenida", "Disco",     "Cuenta", "Paquetes",
                                       "Resumen",    "Instalando", "Listo"};
static int g_step = S_WELCOME;

// ---- Discos ----
#define MAX_DISKS 8
typedef struct {
  char name[24];
  unsigned long long mb;
  int is_boot, too_small;
} disk_t;
static disk_t g_disks[MAX_DISKS];
static int g_ndisks, g_sel_disk = -1;

// ---- Cuenta ----
static char g_user[33], g_pass[65], g_pass2[65], g_full[49];
static int g_field; // 0 nombre completo, 1 usuario, 2 clave, 3 repetir
static char g_err[96];

// ---- Paquetes ----
static const char *k_pkg_id[] = {"devtools", "python", "perl", "gtk", "qt"};
static const char *k_pkg_name[] = {"Herramientas de desarrollo (GCC, make, nasm...)",
                                   "Python 3", "Perl", "Librerias GTK (apps GTK)",
                                   "Librerias Qt5 (apps Qt)"};
static const char *k_pkg_size[] = {"~250 MB", "~25 MB", "~35 MB", "~60 MB", "~70 MB"};
#define N_PKG 5
static int g_pkg_on[N_PKG];

// ---- Instalacion ----
static int g_pid = -1, g_pct, g_failed;
static char g_msg[200];

static void text_clip_setup(void) {}

// ---- Dibujo ----
static void fill(int x, int y, int w, int h, uint32_t c) {
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > W) w = W - x;
  if (y + h > H) h = H - y;
  for (int j = 0; j < h; j++) {
    uint32_t *r = &g_px[(y + j) * W + x];
    for (int i = 0; i < w; i++)
      r[i] = c;
  }
}
static void px(int x, int y, uint32_t c) {
  if (x >= 0 && y >= 0 && x < W && y < H)
    g_px[y * W + x] = c;
}
static void fill_round(int x, int y, int w, int h, int r, uint32_t c) {
  if (r * 2 > h) r = h / 2;
  fill(x + r, y, w - 2 * r, h, c);
  fill(x, y + r, r, h - 2 * r, c);
  fill(x + w - r, y + r, r, h - 2 * r, c);
  for (int j = 0; j < r; j++)
    for (int i = 0; i < r; i++) {
      int dx = r - i, dy = r - j;
      if (dx * dx + dy * dy <= r * r) {
        px(x + i, y + j, c);
        px(x + w - 1 - i, y + j, c);
        px(x + i, y + h - 1 - j, c);
        px(x + w - 1 - i, y + h - 1 - j, c);
      }
    }
}
static void ring(int x, int y, int w, int h, int r, uint32_t line, uint32_t inner) {
  fill_round(x, y, w, h, r, line);
  fill_round(x + 2, y + 2, w - 4, h - 4, r > 2 ? r - 2 : r, inner);
}
static void circle(int cx, int cy, int r, uint32_t c) {
  for (int j = -r; j <= r; j++)
    for (int i = -r; i <= r; i++)
      if (i * i + j * j <= r * r)
        px(cx + i, cy + j, c);
}
static int adv(void) { return g_font ? g_font->glyphs['M'].advance : 8; }
static int text_w(const char *s, int sc) {
  int n = 0;
  for (; *s; s++) n++;
  return n * adv() * sc;
}
// Texto con escala entera (la fuente solo trae un tamano: los titulos se agrandan).
static void text_sc(int x, int y, const char *s, uint32_t col, int sc) {
  if (!g_font) return;
  int a = adv();
  int baseline = y + (g_font->height - 3) * sc;
  uint32_t sr = (col >> 16) & 0xFF, sg = (col >> 8) & 0xFF, sb = col & 0xFF;
  for (; *s; s++, x += a * sc) {
    unsigned char ch = (unsigned char)*s;
    if (ch >= 128) continue;
    const glyph_aa_t *g = &g_font->glyphs[ch];
    if (!g->bitmap || !g->width || !g->height) continue;
    int gx = x + ((a - g->width) / 2 + g->bearing_x) * sc;
    int gy = baseline - g->bearing_y * sc;
    for (int j = 0; j < g->height; j++)
      for (int i = 0; i < g->width; i++) {
        uint32_t al = g->bitmap[j * g->width + i];
        if (!al) continue;
        for (int dj = 0; dj < sc; dj++)
          for (int di = 0; di < sc; di++) {
            int xx = gx + i * sc + di, yy = gy + j * sc + dj;
            if (xx < 0 || yy < 0 || xx >= W || yy >= H) continue;
            uint32_t d = g_px[yy * W + xx];
            uint32_t r = (sr * al + ((d >> 16) & 0xFF) * (255 - al)) / 255;
            uint32_t gg = (sg * al + ((d >> 8) & 0xFF) * (255 - al)) / 255;
            uint32_t b = (sb * al + (d & 0xFF) * (255 - al)) / 255;
            g_px[yy * W + xx] = 0xFF000000u | (r << 16) | (gg << 8) | b;
          }
      }
  }
}
static void text(int x, int y, const char *s, uint32_t col) { text_sc(x, y, s, col, 1); }

// ---- Utilidades ----
static void scopy(char *d, const char *s, int cap) {
  int i = 0;
  for (; s[i] && i < cap - 1; i++) d[i] = s[i];
  d[i] = 0;
}
static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }
static int sequal(const char *a, const char *b) {
  while (*a && *a == *b) { a++; b++; }
  return *a == *b;
}
static void sappend(char *d, const char *s, int cap) {
  int n = slen(d);
  scopy(d + n, s, cap - n);
}
static void utoa_(unsigned long long v, char *out) {
  char t[24]; int n = 0;
  if (!v) t[n++] = '0';
  while (v) { t[n++] = (char)('0' + v % 10); v /= 10; }
  for (int i = 0; i < n; i++) out[i] = t[n - 1 - i];
  out[n] = 0;
}
static uint64_t now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

static void run_cmd(const char *path, const char *a1) {
  int pid = fork();
  if (pid == 0) {
    char *argv[] = {(char *)path, (char *)a1, NULL};
    execv(path, argv);
    _exit(127);
  }
}

// ---- Discos: /proc/partitions, sin particiones ni el medio de arranque ----
static int read_file(const char *path, char *buf, int cap) {
  int fd = open(path, O_RDONLY_);
  if (fd < 0) return -1;
  long n = read(fd, buf, (unsigned long)cap - 1);
  close(fd);
  if (n < 0) return -1;
  buf[n] = 0;
  return (int)n;
}
static int is_whole_disk(const char *n) {
  int l = slen(n);
  if (l < 3) return 0;
  if (n[0] == 'n' && n[1] == 'v') { // nvme0n1 (entero) vs nvme0n1p1
    for (int i = 0; i + 1 < l; i++)
      if (n[i] == 'p' && n[i + 1] >= '0' && n[i + 1] <= '9') return 0;
    return 1;
  }
  if (n[0] == 'm' && n[1] == 'm') { // mmcblk0 vs mmcblk0p1
    for (int i = 3; i + 1 < l; i++)
      if (n[i] == 'p' && n[i + 1] >= '0' && n[i + 1] <= '9') return 0;
    return 1;
  }
  return !(n[l - 1] >= '0' && n[l - 1] <= '9'); // sda, vda, hda
}
static int has_iso(const char *name) {
  char p[48] = "/dev/";
  sappend(p, name, sizeof(p));
  int fd = sys_open(p, 0);
  if (fd < 0) return 0;
  char sig[6] = {0};
  int r = 0;
  if (sys_seek(fd, 32769, 0) >= 0 && sys_read(fd, sig, 5) == 5)
    r = sig[0] == 'C' && sig[1] == 'D' && sig[2] == '0' && sig[3] == '0' && sig[4] == '1';
  sys_close(fd);
  return r;
}
static void scan_disks(void) {
  static char buf[8192];
  g_ndisks = 0;
  if (read_file("/proc/partitions", buf, sizeof(buf)) < 0) return;
  char *p = buf;
  while (*p && g_ndisks < MAX_DISKS) {
    char *e = p;
    while (*e && *e != '\n') e++;
    char save = *e; *e = 0;
    // "major minor #blocks name"
    char *t = p; unsigned long long blocks = 0; int field = 0; char *name = NULL;
    while (*t) {
      while (*t == ' ') t++;
      if (!*t) break;
      char *s = t;
      while (*t && *t != ' ') t++;
      char ch = *t; *t = 0;
      if (field == 2) { blocks = 0; for (char *q = s; *q >= '0' && *q <= '9'; q++) blocks = blocks * 10 + (unsigned)(*q - '0'); }
      if (field == 3) name = s;
      field++;
      if (!ch) break;
      t++;
    }
    if (name && is_whole_disk(name) && blocks > 0) {
      disk_t *d = &g_disks[g_ndisks++];
      scopy(d->name, name, sizeof(d->name));
      d->mb = blocks / 1024;
      d->is_boot = has_iso(name);
      d->too_small = d->mb < 2048;
    }
    *e = save;
    p = *e ? e + 1 : e;
  }
  g_sel_disk = -1;
  for (int i = 0; i < g_ndisks; i++)
    if (!g_disks[i].is_boot && !g_disks[i].too_small) { g_sel_disk = i; break; }
}

// ---- Cuenta ----
static int valid_user(const char *u) {
  int l = slen(u);
  if (l < 1 || l > 32) return 0;
  if (sequal(u, "root") || sequal(u, "nobody")) return 0;
  for (; *u; u++)
    if (!((*u >= 'a' && *u <= 'z') || (*u >= '0' && *u <= '9') || *u == '_' || *u == '-')) return 0;
  return 1;
}
static int validate_account(void) {
  g_err[0] = 0;
  if (!valid_user(g_user)) { scopy(g_err, "Usuario no valido (a-z, 0-9, _ y -; no 'root')", sizeof(g_err)); return 0; }
  if (slen(g_pass) < 4) { scopy(g_err, "La contrasena debe tener al menos 4 caracteres", sizeof(g_err)); return 0; }
  if (!sequal(g_pass, g_pass2)) { scopy(g_err, "Las contrasenas no coinciden", sizeof(g_err)); return 0; }
  return 1;
}

// ---- Instalacion ----
static void write_hash_file(void) {
  unsigned char rnd[12] = {0};
  int fd = open("/dev/urandom", O_RDONLY_);
  if (fd >= 0) { read(fd, rnd, sizeof(rnd)); close(fd); }
  else { uint64_t t = now_ms() * 6364136223846793005ULL; for (int i = 0; i < 12; i++) rnd[i] = (unsigned char)(t >> (i * 5)); }
  static const char al[] = "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
  char salt[32] = "$6$";
  int n = 3;
  for (int i = 0; i < 12; i++) salt[n++] = al[rnd[i] & 63];
  salt[n++] = '$'; salt[n] = 0;
  char *h = crypt(g_pass, salt);
  fd = openat(-100, "/run/aurora-install.hash", O_WRONLY_ | O_CREAT_ | O_TRUNC_, 0600);
  if (fd >= 0) {
    if (h) { write(fd, h, (unsigned long)slen(h)); write(fd, "\n", 1); }
    close(fd);
  }
}
static void start_install(void) {
  write_hash_file();
  static char disk[40], pk[160];
  disk[0] = 0; scopy(disk, "/dev/", sizeof(disk)); sappend(disk, g_disks[g_sel_disk].name, sizeof(disk));
  pk[0] = 0;
  for (int i = 0; i < N_PKG; i++)
    if (g_pkg_on[i]) { if (pk[0]) sappend(pk, " ", sizeof(pk)); sappend(pk, k_pkg_id[i], sizeof(pk)); }
  g_pct = 1; g_failed = 0; scopy(g_msg, "Iniciando la instalacion...", sizeof(g_msg));
  unlink("/run/aurora-install.progress");
  int pid = fork();
  if (pid == 0) {
    int fd = open("/dev/kmsg", O_WRONLY_);
    if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); }
    char *argv[16]; int a = 0;
    argv[a++] = "aurora-install"; argv[a++] = "--disk"; argv[a++] = disk; argv[a++] = "--yes"; argv[a++] = "--no-reboot";
    argv[a++] = "--user"; argv[a++] = g_user; argv[a++] = "--hash-file"; argv[a++] = "/run/aurora-install.hash";
    if (g_full[0]) { argv[a++] = "--full-name"; argv[a++] = g_full; }
    if (pk[0]) { argv[a++] = "--packages"; argv[a++] = pk; }
    argv[a] = NULL;
    execv("/usr/sbin/aurora-install", argv);
    {
      static const char m[] = "-1 No se encuentra /usr/sbin/aurora-install: la ISO esta incompleta o desactualizada (reconstruye con make installer-iso)\n";
      int pf = openat(-100, "/run/aurora-install.progress", O_WRONLY_ | O_CREAT_ | O_TRUNC_, 0644);
      if (pf >= 0) { write(pf, m, sizeof(m) - 1); close(pf); }
    }
    _exit(127);
  }
  g_pid = pid;
  g_step = S_INSTALL;
}
static void poll_install(void) {
  char b[256];
  if (read_file("/run/aurora-install.progress", b, sizeof(b)) > 0) {
    int neg = b[0] == '-', v = 0, i = neg ? 1 : 0;
    while (b[i] >= '0' && b[i] <= '9') v = v * 10 + (b[i++] - '0');
    while (b[i] == ' ') i++;
    int l = slen(b + i); while (l > 0 && (b[i + l - 1] == '\n' || b[i + l - 1] == '\r')) b[i + --l] = 0;
    if (neg) { g_failed = 1; scopy(g_msg, b + i, sizeof(g_msg)); }
    else { g_pct = v; scopy(g_msg, b + i, sizeof(g_msg)); }
  }
  if (g_pid > 0) {
    int st = 0;
    int r = c_waitpid(g_pid, &st, WNOHANG_);
    if (r == g_pid) {
      g_pid = -1;
      if (st != 0 || g_failed) { g_failed = 1; if (g_msg[0] == 0) scopy(g_msg, "La instalacion ha fallado", sizeof(g_msg)); }
      else g_pct = 100;
      g_step = S_DONE;
    }
  }
  g_dirty = 1;
}

// ---- Interfaz ----
#define BTN_H 44
typedef struct { int x, y, w, h; } rect_t;
static rect_t g_btn_next, g_btn_back, g_btn_a, g_btn_b;
static int g_next_on, g_back_on, g_a_on, g_b_on;
static const char *g_next_lbl, *g_a_lbl, *g_b_lbl;
static rect_t g_disk_rect[MAX_DISKS], g_pkg_rect[N_PKG], g_field_rect[4];

static void button(rect_t r, const char *lbl, int primary, int enabled) {
  uint32_t bg = primary ? (enabled ? P->accent : P->line) : P->card;
  uint32_t fg = primary ? (enabled ? P->accent_text : P->dim) : P->text;
  if (!primary) ring(r.x, r.y, r.w, r.h, 10, P->line, P->card);
  else fill_round(r.x, r.y, r.w, r.h, 10, bg);
  text(r.x + (r.w - text_w(lbl, 1)) / 2, r.y + (r.h - 18) / 2, lbl, fg);
}
static int hit(rect_t r, int x, int y) { return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h; }

static void draw_field(int idx, int x, int y, int w, const char *label, const char *val, int secret) {
  text(x, y, label, P->dim);
  rect_t r = {x, y + 22, w, 44};
  g_field_rect[idx] = r;
  ring(r.x, r.y, r.w, r.h, 10, g_field == idx ? P->accent : P->line, P->field);
  char shown[80]; int n = slen(val); if (n > 78) n = 78;
  for (int i = 0; i < n; i++) shown[i] = secret ? '*' : val[i];
  shown[n] = 0;
  text(r.x + 14, r.y + 13, shown, P->text);
  if (g_field == idx && ((now_ms() / 500) & 1)) fill(r.x + 14 + n * adv(), r.y + 10, 2, 24, P->accent);
}

static void draw(void) {
  fill(0, 0, W, H, P->bg);
  int sw = W >= 1000 ? 300 : 220;
  fill(0, 0, sw, H, P->card);
  fill(sw - 1, 0, 1, H, P->line);
  // Marca
  fill_round(40, 44, 52, 52, 14, P->accent);
  text_sc(56, 50, "A", P->accent_text, 3);
  text_sc(108, 52, "AuroraOS", P->text, 2);
  text(110, 84, "Instalador", P->dim);
  // Pasos
  int y = 170;
  for (int i = 0; i < S_N; i++) {
    int cy = y + 12;
    if (i < g_step) { circle(56, cy, 9, P->accent); }
    else if (i == g_step) { circle(56, cy, 9, P->accent); circle(56, cy, 4, P->card); }
    else { circle(56, cy, 9, P->line); circle(56, cy, 7, P->card); }
    text(80, y + 3, k_step_name[i], i == g_step ? P->text : (i < g_step ? P->dim : P->dim));
    y += 44;
  }

  int cx = sw + (W - sw >= 1000 ? 100 : 56);
  int cw = W - cx - (W - sw >= 1000 ? 100 : 56);
  if (cw > 760) cw = 760;
  int by = H - 100;
  g_next_on = g_back_on = g_a_on = g_b_on = 0;

  switch (g_step) {
  case S_WELCOME:
    text_sc(cx, 90, "Te damos la bienvenida", P->text, 2);
    text(cx, 150, "Este asistente instalara AuroraOS en tu equipo.", P->dim);
    text(cx, 182, "Necesitaras un disco de al menos 2 GB. Se creara:", P->dim);
    text(cx + 20, 220, "- una particion de arranque (ESP)", P->text);
    text(cx + 20, 248, "- el sistema (ext2) y una particion de datos con tu cuenta", P->text);
    text(cx, 300, "Se borrara el contenido del disco que elijas.", P->danger);
    g_next_lbl = "Continuar"; g_next_on = 1;
    g_a_lbl = "Abrir terminal"; g_a_on = 1;
    break;
  case S_DISK:
    text_sc(cx, 90, "Elige el disco", P->text, 2);
    text(cx, 148, "Todo su contenido sera borrado.", P->danger);
    if (!g_ndisks) text(cx, 200, "No se ha encontrado ningun disco.", P->dim);
    for (int i = 0; i < g_ndisks; i++) {
      rect_t r = {cx, 190 + i * 76, cw, 64};
      g_disk_rect[i] = r;
      int dis = g_disks[i].is_boot || g_disks[i].too_small;
      ring(r.x, r.y, r.w, r.h, 12, i == g_sel_disk ? P->accent : P->line, P->card);
      circle(r.x + 30, r.y + 32, 10, i == g_sel_disk ? P->accent : P->line);
      if (i != g_sel_disk) circle(r.x + 30, r.y + 32, 7, P->card);
      char l[96] = "/dev/"; sappend(l, g_disks[i].name, sizeof(l));
      text(r.x + 60, r.y + 12, l, dis ? P->dim : P->text);
      char sz[48]; utoa_(g_disks[i].mb, sz); sappend(sz, " MiB", sizeof(sz));
      if (g_disks[i].is_boot) sappend(sz, "  (medio de instalacion)", sizeof(sz));
      else if (g_disks[i].too_small) sappend(sz, "  (demasiado pequeno)", sizeof(sz));
      text(r.x + 60, r.y + 36, sz, P->dim);
    }
    g_next_lbl = "Continuar"; g_next_on = g_sel_disk >= 0; g_back_on = 1;
    break;
  case S_ACCOUNT: {
    text_sc(cx, 90, "Crea tu cuenta", P->text, 2);
    draw_field(0, cx, 150, cw, "Nombre completo (opcional)", g_full, 0);
    draw_field(1, cx, 240, cw, "Nombre de usuario", g_user, 0);
    draw_field(2, cx, 330, cw, "Contrasena", g_pass, 1);
    draw_field(3, cx, 420, cw, "Repetir contrasena", g_pass2, 1);
    if (g_err[0]) text(cx, 500, g_err, P->danger);
    g_next_lbl = "Continuar"; g_next_on = 1; g_back_on = 1;
    break;
  }
  case S_PACKAGES:
    text_sc(cx, 90, "Paquetes opcionales", P->text, 2);
    text(cx, 148, "Se descargan de Ubuntu 24.04 con APT en el primer arranque con red.", P->dim);
    for (int i = 0; i < N_PKG; i++) {
      rect_t r = {cx, 190 + i * 64, cw, 52};
      g_pkg_rect[i] = r;
      ring(r.x, r.y, r.w, r.h, 12, P->line, P->card);
      fill_round(r.x + 16, r.y + 14, 24, 24, 6, g_pkg_on[i] ? P->accent : P->line);
      if (!g_pkg_on[i]) fill_round(r.x + 18, r.y + 16, 20, 20, 5, P->card);
      else text(r.x + 22, r.y + 16, "v", P->accent_text);
      text(r.x + 56, r.y + 16, k_pkg_name[i], P->text);
      text(r.x + r.w - 100, r.y + 16, k_pkg_size[i], P->dim);
    }
    g_next_lbl = "Continuar"; g_next_on = 1; g_back_on = 1;
    break;
  case S_SUMMARY: {
    text_sc(cx, 90, "Todo listo para instalar", P->text, 2);
    char l[160];
    l[0] = 0; scopy(l, "Disco:   /dev/", sizeof(l)); sappend(l, g_disks[g_sel_disk].name, sizeof(l));
    text(cx, 160, l, P->text);
    l[0] = 0; scopy(l, "Usuario: ", sizeof(l)); sappend(l, g_user, sizeof(l));
    text(cx, 192, l, P->text);
    l[0] = 0; scopy(l, "Extras:  ", sizeof(l));
    int any = 0;
    for (int i = 0; i < N_PKG; i++) if (g_pkg_on[i]) { if (any) sappend(l, ", ", sizeof(l)); sappend(l, k_pkg_id[i], sizeof(l)); any = 1; }
    if (!any) sappend(l, "ninguno", sizeof(l));
    text(cx, 224, l, P->text);
    text(cx, 290, "Se borrara todo el contenido del disco seleccionado.", P->danger);
    g_next_lbl = "Instalar"; g_next_on = 1; g_back_on = 1;
    break;
  }
  case S_INSTALL: {
    text_sc(cx, 90, "Instalando AuroraOS", P->text, 2);
    text(cx, 150, g_msg, P->dim);
    rect_t r = {cx, 200, cw, 14};
    fill_round(r.x, r.y, r.w, r.h, 7, P->line);
    int fw = r.w * g_pct / 100;
    if (fw > 14) fill_round(r.x, r.y, fw, r.h, 7, P->accent);
    char pc[16]; utoa_((unsigned long long)g_pct, pc); sappend(pc, " %", sizeof(pc));
    text(cx, 232, pc, P->text);
    text(cx, 290, "No apagues el equipo. Puede tardar varios minutos.", P->dim);
    break;
  }
  case S_DONE:
    if (g_failed) {
      text_sc(cx, 90, "La instalacion ha fallado", P->danger, 2);
      text(cx, 150, g_msg, P->text);
      text(cx, 190, "Puedes volver atras y reintentarlo, o abrir un terminal.", P->dim);
      g_next_lbl = "Volver"; g_next_on = 1;
      g_a_lbl = "Abrir terminal"; g_a_on = 1;
    } else {
      text_sc(cx, 90, "Instalacion completada", P->text, 2);
      text(cx, 150, "Retira el medio de instalacion y reinicia para empezar a usar AuroraOS.", P->dim);
      char l[96] = "Tu usuario: "; sappend(l, g_user, sizeof(l));
      text(cx, 190, l, P->text);
      g_next_lbl = "Reiniciar"; g_next_on = 1;
      g_a_lbl = "Apagar"; g_a_on = 1;
    }
    break;
  }

  g_btn_next = (rect_t){W - 80 - 200, by, 200, BTN_H};
  g_btn_back = (rect_t){W - 80 - 200 - 16 - 140, by, 140, BTN_H};
  g_btn_a = (rect_t){cx, by, 200, BTN_H};
  if (g_next_on) button(g_btn_next, g_next_lbl, 1, 1);
  if (g_back_on) button(g_btn_back, "Atras", 0, 1);
  if (g_a_on) button(g_btn_a, g_a_lbl, 0, 1);
  sys_win_blit(g_win, 0, 0, W, H, 0, 0, W, g_px);
  g_dirty = 0;
}

static void go_next(void) {
  switch (g_step) {
  case S_WELCOME: scan_disks(); g_step = S_DISK; break;
  case S_DISK: if (g_sel_disk >= 0) g_step = S_ACCOUNT; break;
  case S_ACCOUNT: if (validate_account()) g_step = S_PACKAGES; break;
  case S_PACKAGES: g_step = S_SUMMARY; break;
  case S_SUMMARY: start_install(); break;
  case S_DONE:
    if (g_failed) { g_step = S_SUMMARY; g_failed = 0; }
    else run_cmd("/usr/bin/reboot", NULL);
    break;
  }
  g_dirty = 1;
}
static void go_back(void) {
  if (g_step > S_WELCOME && g_step < S_INSTALL) g_step--;
  g_err[0] = 0;
  g_dirty = 1;
}
static char *field_buf(int f, int *cap) {
  switch (f) {
  case 0: *cap = sizeof(g_full); return g_full;
  case 1: *cap = sizeof(g_user); return g_user;
  case 2: *cap = sizeof(g_pass); return g_pass;
  default: *cap = sizeof(g_pass2); return g_pass2;
  }
}
static void on_char(int c) {
  if (g_step == S_ACCOUNT) {
    int cap; char *b;
    if (c == 9) { g_field = (g_field + 1) % 4; g_dirty = 1; return; }
    if (c == 10 || c == 13) { go_next(); return; }
    b = field_buf(g_field, &cap);
    int n = slen(b);
    if (c == 8 || c == 127) { if (n) b[n - 1] = 0; }
    else if (c >= 32 && c < 127 && n < cap - 1) {
      if (g_field == 1 && c >= 'A' && c <= 'Z') c += 32;
      b[n] = (char)c; b[n + 1] = 0;
    }
    g_dirty = 1;
  } else if (c == 10 || c == 13) {
    if (g_step != S_INSTALL) go_next();
  }
}
static void on_click(int x, int y) {
  if (g_a_on && hit(g_btn_a, x, y)) {
    if (g_step == S_DONE && !g_failed) run_cmd("/usr/bin/poweroff", NULL);
    else run_cmd("/usr/bin/terminal", NULL);
    return;
  }
  if (g_next_on && hit(g_btn_next, x, y)) { go_next(); return; }
  if (g_back_on && hit(g_btn_back, x, y)) { go_back(); return; }
  if (g_step == S_DISK)
    for (int i = 0; i < g_ndisks; i++)
      if (hit(g_disk_rect[i], x, y) && !g_disks[i].is_boot && !g_disks[i].too_small) { g_sel_disk = i; g_dirty = 1; }
  if (g_step == S_PACKAGES)
    for (int i = 0; i < N_PKG; i++)
      if (hit(g_pkg_rect[i], x, y)) { g_pkg_on[i] ^= 1; g_dirty = 1; }
  if (g_step == S_ACCOUNT)
    for (int i = 0; i < 4; i++)
      if (hit(g_field_rect[i], x, y)) { g_field = i; g_dirty = 1; }
}

int main(void) {
  g_font = font_system_mono();
  if (!g_font) return 1;
  uint32_t sz = sys_screen_size();
  W = (int)(sz >> 16); H = (int)(sz & 0xFFFF);
  if (W < 640 || H < 480) { W = 1024; H = 768; }
  if (W > 4096) W = 4096;
  if (H > 4096) H = 4096;
  g_px = (uint32_t *)malloc((size_t)W * H * 4);
  if (!g_px) return 1;
  g_win = sys_win_create(0, 0, W, H, "Instalador de AuroraOS");
  if (g_win < 0) return 1;
  sys_win_set_icon(g_win, "/system/icons/app-generic.bmp");
  g_theme_seen = sys_theme_get();
  P = (g_theme_seen & 1) ? &PAL_LIGHT : &PAL_DARK;
  text_clip_setup();

  uint64_t last = 0;
  for (;;) {
    winsrv_event_t ev;
    while (sys_win_poll_event(g_win, &ev, 0) > 0) {
      if (ev.type == WINSRV_EV_MOUSE) {
        int btn = (int)(ev.data & 0xFF);
        int pressed = ((ev.data >> 8) & 1) == 0;
        int motion = (int)((ev.data >> 9) & 1);
        if (btn == 0 && pressed && !motion) on_click(ev.x, ev.y);
      } else if (ev.type == WINSRV_EV_TTY_INPUT) {
        on_char(ev.x);
      } else if (ev.type == WINSRV_EV_FOCUS) {
        g_dirty = 1;
      }
    }
    uint32_t th = sys_theme_get();
    if (th != g_theme_seen) { g_theme_seen = th; P = (th & 1) ? &PAL_LIGHT : &PAL_DARK; g_dirty = 1; }
    uint64_t t = now_ms();
    if (t - last >= 250) {
      last = t;
      if (g_step == S_INSTALL) poll_install();
      if (g_step == S_ACCOUNT) g_dirty = 1; // cursor parpadeante
    }
    if (g_dirty) draw();
    usleep(30000);
  }
  return 0;
}
