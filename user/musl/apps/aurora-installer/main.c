// user/musl/apps/aurora-installer/main.c
//
// Instalador grafico de AuroraOS, al estilo del Asistente de macOS. Pantalla
// completa negra-grafito (el kernel quita Dock y barra de menus con aur.kiosk=1) y
// una unica ventana de tamano fijo, centrada, con los tres botones de ventana: solo
// el rojo esta activo y apaga el equipo. Es la cara de /usr/sbin/aurora-install:
// recoge disco, cuenta y extras opcionales, lanza el motor y muestra su progreso
// (/run/aurora-install.progress). Colores solidos, verde aurora solo como acento.
//
// Pasos: Bienvenida -> Disco -> Cuenta -> Extras -> (confirmacion) -> Instalando -> Listo.
// Teclado: Tab/Intro cambian de campo, Intro continua y confirma, Esc cancela.

#define syscall aurora_syscall
#define puts aurora_puts_userlib
#define printf aurora_printf_userlib

#include "../../../lib/env.h"
#include "../../../lib/file.h"
#include "../../../lib/font_aa.h"
#include "../../../lib/malloc.h"
#include "../../../lib/process.h"
#include "../../../lib/string.h"
#include "../../../syscall.h"
#include <stdlib.h>
#include <time.h>

#undef puts
#undef printf

#include "uitext.h"
#include "fonts/interregular_13.h"
#include "fonts/interregular_15.h"
#include "fonts/intersemibold_15.h"
#include "fonts/intersemibold_34.h"
#include "fonts/intersemibold_48.h"

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
  uint32_t bg, shadow, win, bar, card, line, text, dim, accent, accent_text, danger, field, off;
} palette_t;
// Solo modo oscuro: fondo negro grafito, ventana gris oscuro.
static const palette_t PAL = {0xFF111113, 0xFF08080A, 0xFF242427, 0xFF2D2D31, 0xFF2D2D31, 0xFF3C3C41,
                              0xFFF2F2F4, 0xFF9C9CA3, 0xFF3EE89B, 0xFF04261A, 0xFFFF6A3D, 0xFF19191B,
                              0xFF55555B};
static const palette_t *P = &PAL;

static int W, H;
static uint32_t *g_px;
static int g_win = -1, g_dirty = 1;

// ---- Pasos ----
enum { S_WELCOME, S_DISK, S_ACCOUNT, S_EXTRAS, S_INSTALL, S_DONE, S_N };
#define N_DOTS 4 // pasos con puntos: Bienvenida, Disco, Cuenta, Extras
static int g_step = S_WELCOME;
static int g_confirm; // hoja "borrar el disco e instalar"
static int g_user_touched;

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
static const char *k_pkg_name[] = {"Herramientas de desarrollo", "Python 3", "Perl",
                                   "Apps GTK", "Apps Qt"};
static const char *k_pkg_desc[] = {"GCC, make, nasm y utilidades", "Intérprete y biblioteca estándar",
                                   "Intérprete y módulos", "Librerías GTK 3 y 4", "Librerías Qt 5"};
static const char *k_pkg_size[] = {"250 MB", "25 MB", "35 MB", "60 MB", "70 MB"};
#define N_PKG 5
static int g_pkg_on[N_PKG];

// ---- Instalacion ----
static int g_pid = -1, g_pct, g_failed;
static char g_msg[200];


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
// Fuentes del sistema (Inter): cuerpo, pequena, botones, titulo y cifra grande.
enum { F_SMALL, F_BODY, F_BTN, F_TITLE, F_BIG };
static const font_aa_t *k_fonts[] = {&font_inter_regular_regular_13, &font_inter_regular_regular_15,
                                     &font_inter_semibold_regular_15, &font_inter_semibold_regular_34,
                                     &font_inter_semibold_regular_48};
static int text_wf(const char *s, int f) { return font_aa_text_width(k_fonts[f], s); }
static void textf(int x, int y, const char *s, uint32_t col, int f) {
  rect_t clip = {0, 0, W, H};
  gfx_draw_string_aa(g_px, W, clip, x, y, s, 0xFF000000u | (col & 0xFFFFFF), k_fonts[f]);
}
static void text(int x, int y, const char *s, uint32_t col) { textf(x, y, s, col, F_BODY); }

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
  if (!valid_user(g_user)) { scopy(g_err, "Usa solo minúsculas, números, _ y - (no 'root')", sizeof(g_err)); return 0; }
  if (slen(g_pass) < 4) { scopy(g_err, "La contraseña necesita al menos 4 caracteres", sizeof(g_err)); return 0; }
  if (!sequal(g_pass, g_pass2)) { scopy(g_err, "Las contraseñas no coinciden", sizeof(g_err)); return 0; }
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
  g_pct = 1; g_failed = 0; scopy(g_msg, "Iniciando la instalación...", sizeof(g_msg));
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


// ---- Emblema de AuroraOS (assets/brand/aurora-emblem-192.argb: w,h y BGRA con alfa) ----
static uint32_t *g_emb;
static int g_emb_w, g_emb_h;
static void load_emblem(void) {
  static char *buf;
  buf = (char *)malloc(8 + 192 * 192 * 4);
  if (!buf) return;
  int fd = open("/system/brand/aurora-emblem-192.argb", O_RDONLY_);
  if (fd < 0) { free(buf); return; }
  long tot = 0, n;
  while (tot < 8 + 192 * 192 * 4 && (n = read(fd, buf + tot, (unsigned long)(8 + 192 * 192 * 4 - tot))) > 0) tot += n;
  close(fd);
  g_emb_w = *(int *)buf; g_emb_h = *(int *)(buf + 4);
  if (tot < 8 + g_emb_w * g_emb_h * 4 || g_emb_w != 192 || g_emb_h != 192) { free(buf); return; }
  g_emb = (uint32_t *)(buf + 8);
}
// Dibuja el emblema centrado en cx con ancho `size` (muestreo bilineal).
static void draw_emblem(int cx, int top, int size) {
  if (!g_emb) { // sin el recurso: baldosa verde
    fill_round(cx - size / 2, top, size, size, size / 4, P->accent);
    return;
  }
  int x0 = cx - size / 2;
  for (int j = 0; j < size; j++)
    for (int i = 0; i < size; i++) {
      int fx = i * (g_emb_w - 1) * 256 / (size - 1), fy = j * (g_emb_h - 1) * 256 / (size - 1);
      int ix = fx >> 8, iy = fy >> 8, ax = fx & 255, ay = fy & 255;
      int ix1 = ix + 1 < g_emb_w ? ix + 1 : ix, iy1 = iy + 1 < g_emb_h ? iy + 1 : iy;
      uint32_t p[4] = {g_emb[iy * g_emb_w + ix], g_emb[iy * g_emb_w + ix1], g_emb[iy1 * g_emb_w + ix], g_emb[iy1 * g_emb_w + ix1]};
      uint32_t w[4] = {(256 - ax) * (256 - ay), ax * (256 - ay), (256 - ax) * ay, ax * ay};
      uint32_t ch[4] = {0, 0, 0, 0}; // a, r, g, b ponderados (premultiplicados por alfa)
      for (int k = 0; k < 4; k++) {
        uint32_t a = p[k] >> 24;
        ch[0] += a * w[k];
        ch[1] += ((p[k] >> 16) & 0xFF) * a * w[k] / 255;
        ch[2] += ((p[k] >> 8) & 0xFF) * a * w[k] / 255;
        ch[3] += (p[k] & 0xFF) * a * w[k] / 255;
      }
      uint32_t a = ch[0] >> 16;
      if (!a) continue;
      uint32_t r = ch[1] >> 16, g = ch[2] >> 16, b = ch[3] >> 16;
      int x = x0 + i, y = top + j;
      if (x < 0 || y < 0 || x >= W || y >= H) continue;
      uint32_t d = g_px[y * W + x];
      uint32_t inv = 255 - a;
      uint32_t or_ = r + ((d >> 16) & 0xFF) * inv / 255, og = g + ((d >> 8) & 0xFF) * inv / 255, ob = b + (d & 0xFF) * inv / 255;
      g_px[y * W + x] = 0xFF000000u | (or_ > 255 ? 255 : or_) << 16 | (og > 255 ? 255 : og) << 8 | (ob > 255 ? 255 : ob);
    }
}

// ---- Interfaz ----
#define BTN_H 40
#define WIN_W 900
#define WIN_H 640
#define BAR_H 44
static int WX, WY, WW, WH; // ventana (posicion y tamano fijo)
static rect_t g_btn_next, g_btn_back, g_btn_a, g_btn_ok, g_btn_cancel;
static int g_next_on, g_back_on, g_a_on;
static const char *g_next_lbl, *g_a_lbl;
static rect_t g_disk_rect[MAX_DISKS], g_pkg_rect[N_PKG], g_field_rect[4];
static int g_close_on;

static int hit(rect_t r, int x, int y) { return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h; }
static void ctext(int cx, int y, const char *s, uint32_t col, int f) {
  textf(cx - text_wf(s, f) / 2, y, s, col, f);
}
static void button(rect_t r, const char *lbl, int primary, uint32_t bg_override) {
  if (bg_override) fill_round(r.x, r.y, r.w, r.h, 9, bg_override);
  else if (primary) fill_round(r.x, r.y, r.w, r.h, 9, P->accent);
  else fill_round(r.x, r.y, r.w, r.h, 9, P->card), ring(r.x, r.y, r.w, r.h, 9, P->line, P->card);
  uint32_t fg = (primary || bg_override) ? P->accent_text : P->text;
  textf(r.x + (r.w - text_wf(lbl, F_BTN)) / 2, r.y + (r.h - 19) / 2, lbl, fg, F_BTN);
}
static void draw_field(int idx, int x, int y, int w, const char *label, const char *val, int secret) {
  text(x, y, label, P->dim);
  rect_t r = {x, y + 22, w, 42};
  g_field_rect[idx] = r;
  ring(r.x, r.y, r.w, r.h, 9, g_field == idx ? P->accent : P->line, P->field);
  char shown[80]; int n = slen(val); if (n > 78) n = 78;
  for (int i = 0; i < n; i++) shown[i] = secret ? '*' : val[i];
  shown[n] = 0;
  text(r.x + 14, r.y + 11, shown, P->text);
  if (g_field == idx && ((now_ms() / 500) & 1)) fill(r.x + 14 + text_wf(shown, F_BODY), r.y + 9, 2, 24, P->accent);
}
static void thick_line(int x0, int y0, int x1, int y1, int r, uint32_t c) {
  int dx = x1 - x0, dy = y1 - y0, n = (dx < 0 ? -dx : dx) > (dy < 0 ? -dy : dy) ? (dx < 0 ? -dx : dx) : (dy < 0 ? -dy : dy);
  if (n == 0) n = 1;
  for (int i = 0; i <= n; i++) circle(x0 + dx * i / n, y0 + dy * i / n, r, c);
}
static void switch_ctl(int x, int y, int on) {
  fill_round(x, y, 46, 26, 13, on ? P->accent : P->off);
  circle(on ? x + 33 : x + 13, y + 13, 10, 0xFFFFFFFF);
}

static void draw(void) {
  int cxm = WX + WW / 2; // centro horizontal de la ventana
  fill(0, 0, W, H, P->bg);
  // Sombra y ventana
  fill_round(WX - 10, WY - 2, WW + 20, WH + 22, 22, P->shadow);
  fill_round(WX - 1, WY - 1, WW + 2, WH + 2, 15, P->line);
  fill_round(WX, WY, WW, WH, 14, P->win);
  fill_round(WX, WY, WW, BAR_H, 14, P->bar);
  fill(WX, WY + BAR_H / 2, WW, BAR_H / 2, P->bar);
  fill(WX, WY + BAR_H, WW, 1, P->line);
  // Semaforo: solo el rojo funciona (apaga). Minimizar y maximizar, apagados.
  g_close_on = g_step != S_INSTALL;
  circle(WX + 24, WY + BAR_H / 2, 7, g_close_on ? 0xFFFF6A3D : P->off);
  circle(WX + 48, WY + BAR_H / 2, 7, P->off);
  circle(WX + 72, WY + BAR_H / 2, 7, P->off);
  if (g_close_on) { // aspa del boton cerrar
    thick_line(WX + 21, WY + BAR_H / 2 - 3, WX + 27, WY + BAR_H / 2 + 3, 0, 0xFF5A1A08);
    thick_line(WX + 27, WY + BAR_H / 2 - 3, WX + 21, WY + BAR_H / 2 + 3, 0, 0xFF5A1A08);
  }
  ctext(cxm, WY + 14, "Instalar AuroraOS", P->dim, F_BODY);

  int by = WY + WH - 70; // fila de botones
  int pad = 44;
  g_next_on = g_back_on = g_a_on = 0;
  int cw = 600, cx = cxm - cw / 2; // columna de contenido centrada
  int ty = WY + BAR_H + 44;         // y del titulo

  switch (g_step) {
  case S_WELCOME:
    draw_emblem(cxm, ty + 4, 128);
        ctext(cxm, ty + 150, "Te damos la bienvenida", P->text, F_BIG);
    ctext(cxm, ty + 220, "Vamos a instalar AuroraOS en este equipo.", P->dim, F_BODY);
    ctext(cxm, ty + 246, "Solo te harán falta unos minutos.", P->dim, F_BODY);
    g_next_lbl = "Empezar"; g_next_on = 1;
    g_a_lbl = "Abrir terminal"; g_a_on = 1;
    break;
  case S_DISK:
    ctext(cxm, ty, "Elige dónde instalar", P->text, F_TITLE);
    ctext(cxm, ty + 48, "Se borrará todo el contenido del disco elegido.", P->dim, F_BODY);
    if (!g_ndisks) ctext(cxm, ty + 140, "No se ha encontrado ningún disco.", P->dim, F_BODY);
    for (int i = 0; i < g_ndisks; i++) {
      rect_t r = {cx, ty + 96 + i * 74, cw, 62};
      g_disk_rect[i] = r;
      int dis = g_disks[i].is_boot || g_disks[i].too_small;
      ring(r.x, r.y, r.w, r.h, 12, i == g_sel_disk ? P->accent : P->line, P->card);
      circle(r.x + 32, r.y + 31, 10, i == g_sel_disk ? P->accent : P->off);
      if (i != g_sel_disk) circle(r.x + 32, r.y + 31, 7, P->card);
      else circle(r.x + 32, r.y + 31, 4, P->accent_text);
      char l[96] = "Disco ("; sappend(l, g_disks[i].name, sizeof(l)); sappend(l, ")", sizeof(l));
      text(r.x + 62, r.y + 11, l, dis ? P->dim : P->text);
      char sz[48];
      if (g_disks[i].mb >= 1024) { utoa_(g_disks[i].mb / 1024, sz); sappend(sz, " GB", sizeof(sz)); }
      else { utoa_(g_disks[i].mb, sz); sappend(sz, " MB", sizeof(sz)); }
      if (g_disks[i].is_boot) sappend(sz, "  -  medio de instalacion", sizeof(sz));
      else if (g_disks[i].too_small) sappend(sz, "  -  demasiado pequeno", sizeof(sz));
      text(r.x + 62, r.y + 34, sz, P->dim);
    }
    g_next_lbl = "Continuar"; g_next_on = g_sel_disk >= 0; g_back_on = 1;
    break;
  case S_ACCOUNT: {
    ctext(cxm, ty - 8, "Crea tu cuenta", P->text, F_TITLE);
    int fy = ty + 44;
    draw_field(0, cx, fy, cw, "Nombre completo", g_full, 0);
    draw_field(1, cx, fy + 84, cw, "Nombre de usuario", g_user, 0);
    draw_field(2, cx, fy + 168, cw, "Contraseña", g_pass, 1);
    draw_field(3, cx, fy + 252, cw, "Repetir contraseña", g_pass2, 1);
    if (g_err[0]) ctext(cxm, fy + 336, g_err, P->danger, F_BODY);
    g_next_lbl = "Continuar"; g_next_on = 1; g_back_on = 1;
    break;
  }
  case S_EXTRAS:
    ctext(cxm, ty - 8, "Extras opcionales", P->text, F_TITLE);
    ctext(cxm, ty + 38, "Se descargan después, la primera vez que haya red.", P->dim, F_BODY);
    ctext(cxm, ty + 60, "Puedes saltarte esto y añadirlos más tarde.", P->dim, F_BODY);
    for (int i = 0; i < N_PKG; i++) {
      rect_t r = {cx, ty + 98 + i * 64, cw, 54};
      g_pkg_rect[i] = r;
      ring(r.x, r.y, r.w, r.h, 12, P->line, P->card);
      text(r.x + 20, r.y + 8, k_pkg_name[i], P->text);
      char d[64]; d[0] = 0; scopy(d, k_pkg_desc[i], sizeof(d)); sappend(d, "  -  ", sizeof(d)); sappend(d, k_pkg_size[i], sizeof(d));
      text(r.x + 20, r.y + 29, d, P->dim);
      switch_ctl(r.x + r.w - 46 - 18, r.y + 14, g_pkg_on[i]);
    }
    g_next_lbl = "Instalar"; g_next_on = 1; g_back_on = 1;
    break;
  case S_INSTALL: {
    ctext(cxm, ty + 40, "Instalando AuroraOS", P->text, F_TITLE);
    char pc[16]; utoa_((unsigned long long)g_pct, pc); sappend(pc, " %", sizeof(pc));
    ctext(cxm, ty + 100, pc, P->accent, F_BIG);
    rect_t r = {cxm - 240, ty + 170, 480, 10};
    fill_round(r.x, r.y, r.w, r.h, 5, P->line);
    int fw = r.w * g_pct / 100;
    if (fw > 10) fill_round(r.x, r.y, fw, r.h, 5, P->accent);
    ctext(cxm, ty + 204, g_msg, P->dim, F_BODY);
    ctext(cxm, ty + 270, "No apagues el equipo. Puede tardar varios minutos.", P->dim, F_BODY);
    break;
  }
  case S_DONE:
    if (g_failed) {
      circle(cxm, ty + 52, 40, P->danger);
      ctext(cxm, ty + 30, "!", P->accent_text, F_BIG);
      ctext(cxm, ty + 120, "No se ha podido instalar", P->text, F_TITLE);
      ctext(cxm, ty + 172, g_msg, P->dim, F_BODY);
      ctext(cxm, ty + 204, "Puedes volver atrás y reintentarlo, o abrir un terminal.", P->dim, F_BODY);
      g_next_lbl = "Volver"; g_next_on = 1;
      g_a_lbl = "Abrir terminal"; g_a_on = 1;
    } else {
      circle(cxm, ty + 52, 40, P->accent);
      thick_line(cxm - 16, ty + 54, cxm - 5, ty + 66, 3, P->accent_text);
      thick_line(cxm - 5, ty + 66, cxm + 18, ty + 38, 3, P->accent_text);
      ctext(cxm, ty + 120, "AuroraOS está instalado", P->text, F_TITLE);
      char l[96] = "Tu usuario es "; sappend(l, g_user, sizeof(l));
      ctext(cxm, ty + 172, l, P->text, F_BODY);
      ctext(cxm, ty + 204, "Retira el medio de instalación y reinicia para empezar.", P->dim, F_BODY);
      g_next_lbl = "Reiniciar"; g_next_on = 1;
      g_a_lbl = "Apagar"; g_a_on = 1;
    }
    break;
  }

  // Puntos de progreso (solo en los pasos de datos)
  if (g_step < N_DOTS)
    for (int i = 0; i < N_DOTS; i++)
      circle(cxm - (N_DOTS - 1) * 12 + i * 24, by + BTN_H / 2, i == g_step ? 5 : 4, i == g_step ? P->text : P->off);
  g_btn_next = (rect_t){WX + WW - pad - 160, by, 160, BTN_H};
  g_btn_back = (rect_t){WX + WW - pad - 160 - 12 - 120, by, 120, BTN_H};
  g_btn_a = (rect_t){WX + pad, by, 160, BTN_H};
  if (g_step == S_WELCOME || g_step == S_DONE) g_btn_next.x = WX + WW - pad - 160;
  if (g_next_on) button(g_btn_next, g_next_lbl, 1, 0);
  if (g_back_on) button(g_btn_back, "Atrás", 0, 0);
  if (g_a_on) button(g_btn_a, g_a_lbl, 0, 0);

  // Hoja de confirmacion (sobre la ventana)
  if (g_confirm) {
    // Oscurecer la ventana tras la hoja (respetando las esquinas redondeadas).
    for (int j = WY; j < WY + WH && j < H; j++)
      for (int i = WX; i < WX + WW && i < W; i++) {
        int ex = i < WX + 14 ? WX + 14 - i : (i >= WX + WW - 14 ? i - (WX + WW - 15) : 0);
        int ey = j < WY + 14 ? WY + 14 - j : (j >= WY + WH - 14 ? j - (WY + WH - 15) : 0);
        if (ex && ey && ex * ex + ey * ey > 196) continue;
        uint32_t c = g_px[j * W + i];
        g_px[j * W + i] = 0xFF000000u | (((c >> 16 & 0xFF) * 45 / 100) << 16) | (((c >> 8 & 0xFF) * 45 / 100) << 8) | ((c & 0xFF) * 45 / 100);
      }
    rect_t sh = {cxm - 270, WY + WH / 2 - 130, 540, 260};
    fill_round(sh.x - 1, sh.y - 1, sh.w + 2, sh.h + 2, 17, P->line);
    fill_round(sh.x, sh.y, sh.w, sh.h, 16, P->bar);
    ctext(cxm, sh.y + 32, "¿Borrar el disco e instalar?", P->text, F_TITLE);
    char l[120] = "Se borrará todo el contenido de ";
    sappend(l, g_disks[g_sel_disk].name, sizeof(l)); sappend(l, ".", sizeof(l));
    ctext(cxm, sh.y + 96, l, P->dim, F_BODY);
    ctext(cxm, sh.y + 122, "Esta acción no se puede deshacer.", P->dim, F_BODY);
    g_btn_cancel = (rect_t){sh.x + 36, sh.y + sh.h - 76, 200, BTN_H};
    g_btn_ok = (rect_t){sh.x + sh.w - 36 - 240, sh.y + sh.h - 76, 240, BTN_H};
    button(g_btn_cancel, "Cancelar", 0, 0);
    button(g_btn_ok, "Borrar e instalar", 0, P->danger);
  }
  sys_win_blit(g_win, 0, 0, W, H, 0, 0, W, g_px);
  g_dirty = 0;
}

static void go_next(void) {
  switch (g_step) {
  case S_WELCOME: scan_disks(); g_step = S_DISK; break;
  case S_DISK: if (g_sel_disk >= 0) g_step = S_ACCOUNT; break;
  case S_ACCOUNT: if (validate_account()) g_step = S_EXTRAS; break;
  case S_EXTRAS: g_confirm = 1; break;
  case S_DONE:
    if (g_failed) { g_step = S_EXTRAS; g_failed = 0; }
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
// Sugiere el usuario a partir del nombre completo (primera palabra, minusculas).
static void suggest_user(void) {
  int n = 0;
  for (int i = 0; g_full[i] && g_full[i] != ' ' && n < 20; i++) {
    char c = g_full[i];
    if (c >= 'A' && c <= 'Z') c += 32;
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) g_user[n++] = c;
  }
  g_user[n] = 0;
}
static void on_char(int c) {
  if (g_confirm) {
    if (c == 27) { g_confirm = 0; g_dirty = 1; }
    else if (c == 10 || c == 13) { g_confirm = 0; start_install(); g_dirty = 1; }
    return;
  }
  if (g_step == S_ACCOUNT) {
    int cap; char *b;
    if (c == 9) { g_field = (g_field + 1) % 4; g_dirty = 1; return; }
    if (c == 10 || c == 13) {
      if (g_field < 3) { g_field++; g_dirty = 1; } else go_next();
      return;
    }
    b = field_buf(g_field, &cap);
    int n = slen(b);
    if (c == 8 || c == 127) { if (n) b[n - 1] = 0; }
    else if (c >= 32 && c < 127 && n < cap - 1) {
      if (g_field == 1 && c >= 'A' && c <= 'Z') c += 32;
      b[n] = (char)c; b[n + 1] = 0;
    }
    if (g_field == 0 && !g_user_touched) suggest_user();
    if (g_field == 1) g_user_touched = 1;
    g_dirty = 1;
  } else if (c == 10 || c == 13) {
    if (g_step != S_INSTALL) go_next();
  }
}
static void on_click(int x, int y) {
  if (g_confirm) {
    if (hit(g_btn_ok, x, y)) { g_confirm = 0; start_install(); }
    else if (hit(g_btn_cancel, x, y)) g_confirm = 0;
    g_dirty = 1;
    return;
  }
  // Boton rojo: apagar (no durante la instalacion)
  int dx = x - (WX + 24), dy = y - (WY + BAR_H / 2);
  if (g_close_on && dx * dx + dy * dy <= 100) { run_cmd("/usr/bin/poweroff", NULL); return; }
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
  if (g_step == S_EXTRAS)
    for (int i = 0; i < N_PKG; i++)
      if (hit(g_pkg_rect[i], x, y)) { g_pkg_on[i] ^= 1; g_dirty = 1; }
  if (g_step == S_ACCOUNT)
    for (int i = 0; i < 4; i++)
      if (hit(g_field_rect[i], x, y)) { g_field = i; g_dirty = 1; }
}

int main(void) {
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
  load_emblem();
  WW = WIN_W; WH = WIN_H;
  if (WW > W - 40) WW = W - 40;
  if (WH > H - 40) WH = H - 40;
  WX = (W - WW) / 2; WY = (H - WH) / 2;

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
