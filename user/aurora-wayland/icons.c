// user/aurora-wayland/icons.c
#define _GNU_SOURCE
#include "icons.h"
#include <dlfcn.h>
#include <errno.h>
#include <png.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define ICON_DIR "/tmp/aurora-icons"
#define SVG_SIZE 128

static int file_exists(const char *p) {
  struct stat st;
  return stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

// --- Escritura del BMP (32 bpp, de abajo arriba, BGRA con alfa recto) ---------
static int write_bmp(const char *dst, const uint8_t *bgra, int w, int h) {
  size_t rows = (size_t)w * h * 4;
  uint8_t hdr[54];
  memset(hdr, 0, sizeof(hdr));
  hdr[0] = 'B';
  hdr[1] = 'M';
  uint32_t fsz = (uint32_t)(54 + rows), off = 54, isz = 40, wd = (uint32_t)w, ht = (uint32_t)h;
  memcpy(hdr + 2, &fsz, 4);
  memcpy(hdr + 10, &off, 4);
  memcpy(hdr + 14, &isz, 4);
  memcpy(hdr + 18, &wd, 4);
  memcpy(hdr + 22, &ht, 4);
  hdr[26] = 1;
  hdr[28] = 32;
  uint32_t img = (uint32_t)rows;
  memcpy(hdr + 34, &img, 4);
  char tmp[160];
  snprintf(tmp, sizeof(tmp), "%s.tmp", dst);
  FILE *f = fopen(tmp, "wb");
  if (!f)
    return -1;
  int ok = fwrite(hdr, 1, sizeof(hdr), f) == sizeof(hdr);
  for (int y = h - 1; ok && y >= 0; y--)
    ok = fwrite(bgra + (size_t)y * w * 4, 1, (size_t)w * 4, f) == (size_t)w * 4;
  ok = (fclose(f) == 0) && ok;
  if (!ok) {
    unlink(tmp);
    return -1;
  }
  return rename(tmp, dst);
}

// --- PNG (libpng, API simplificada: BGRA con alfa recto) ----------------------
static uint8_t *load_png(const char *path, int *w, int *h) {
  png_image im;
  memset(&im, 0, sizeof(im));
  im.version = PNG_IMAGE_VERSION;
  if (!png_image_begin_read_from_file(&im, path))
    return NULL;
  im.format = PNG_FORMAT_BGRA;
  uint8_t *buf = malloc(PNG_IMAGE_SIZE(im));
  if (!buf || !png_image_finish_read(&im, NULL, buf, 0, NULL)) {
    free(buf);
    png_image_free(&im);
    return NULL;
  }
  *w = (int)im.width;
  *h = (int)im.height;
  return buf;
}

// --- SVG (librsvg + cairo, cargadas bajo demanda: pesan bastante) -------------
typedef struct { int width, height; double em, ex; } rsvg_dim_t;
static struct {
  int tried, ok;
  void *(*surface_create)(int, int, int);
  void *(*cairo_create)(void *);
  void (*cairo_destroy)(void *);
  void (*cairo_scale)(void *, double, double);
  void (*surface_flush)(void *);
  uint8_t *(*surface_data)(void *);
  int (*surface_stride)(void *);
  void (*surface_destroy)(void *);
  void *(*rsvg_new)(const char *, void **);
  void (*rsvg_dims)(void *, rsvg_dim_t *);
  int (*rsvg_render)(void *, void *);
  void (*g_unref)(void *);
} svg;

static void svg_init(void) {
  if (svg.tried)
    return;
  svg.tried = 1;
  void *cairo = dlopen("libcairo.so.2", RTLD_NOW | RTLD_GLOBAL);
  void *gobj = dlopen("libgobject-2.0.so.0", RTLD_NOW | RTLD_GLOBAL);
  void *rsvg = dlopen("librsvg-2.so.2", RTLD_NOW | RTLD_GLOBAL);
  if (!cairo || !gobj || !rsvg)
    return;
#define SYM(h, f, n) *(void **)&svg.f = dlsym(h, n)
  SYM(cairo, surface_create, "cairo_image_surface_create");
  SYM(cairo, cairo_create, "cairo_create");
  SYM(cairo, cairo_destroy, "cairo_destroy");
  SYM(cairo, cairo_scale, "cairo_scale");
  SYM(cairo, surface_flush, "cairo_surface_flush");
  SYM(cairo, surface_data, "cairo_image_surface_get_data");
  SYM(cairo, surface_stride, "cairo_image_surface_get_stride");
  SYM(cairo, surface_destroy, "cairo_surface_destroy");
  SYM(rsvg, rsvg_new, "rsvg_handle_new_from_file");
  SYM(rsvg, rsvg_dims, "rsvg_handle_get_dimensions");
  SYM(rsvg, rsvg_render, "rsvg_handle_render_cairo");
  SYM(gobj, g_unref, "g_object_unref");
#undef SYM
  svg.ok = svg.surface_create && svg.cairo_create && svg.cairo_destroy && svg.cairo_scale &&
           svg.surface_flush && svg.surface_data && svg.surface_stride && svg.surface_destroy &&
           svg.rsvg_new && svg.rsvg_dims && svg.rsvg_render && svg.g_unref;
}

static uint8_t *load_svg(const char *path, int *w, int *h) {
  svg_init();
  if (!svg.ok)
    return NULL;
  void *handle = svg.rsvg_new(path, NULL);
  if (!handle)
    return NULL;
  rsvg_dim_t d = {0};
  svg.rsvg_dims(handle, &d);
  if (d.width <= 0 || d.height <= 0) {
    svg.g_unref(handle);
    return NULL;
  }
  void *surf = svg.surface_create(0 /* CAIRO_FORMAT_ARGB32 */, SVG_SIZE, SVG_SIZE);
  void *cr = svg.cairo_create(surf);
  svg.cairo_scale(cr, (double)SVG_SIZE / d.width, (double)SVG_SIZE / d.height);
  int ok = svg.rsvg_render(handle, cr);
  svg.cairo_destroy(cr);
  svg.g_unref(handle);
  svg.surface_flush(surf);
  uint8_t *out = NULL;
  if (ok) {
    const uint8_t *src = svg.surface_data(surf);
    int stride = svg.surface_stride(surf);
    out = malloc((size_t)SVG_SIZE * SVG_SIZE * 4);
    if (out) {
      for (int y = 0; y < SVG_SIZE; y++)
        for (int x = 0; x < SVG_SIZE; x++) {
          const uint8_t *p = src + (size_t)y * stride + x * 4; // B G R A, premultiplicado
          uint8_t *o = out + ((size_t)y * SVG_SIZE + x) * 4;
          uint32_t a = p[3];
          if (a == 0 || a == 255) {
            memcpy(o, p, 4);
          } else {
            o[0] = (uint8_t)((p[0] * 255 + a / 2) / a);
            o[1] = (uint8_t)((p[1] * 255 + a / 2) / a);
            o[2] = (uint8_t)((p[2] * 255 + a / 2) / a);
            o[3] = (uint8_t)a;
          }
        }
      *w = *h = SVG_SIZE;
    }
  }
  svg.surface_destroy(surf);
  return out;
}

// --- Busqueda en los temas -----------------------------------------------------
static int ends_with(const char *s, const char *suf) {
  size_t a = strlen(s), b = strlen(suf);
  return a >= b && !strcmp(s + a - b, suf);
}

// Rellena `out` con la mejor ruta para el nombre (PNG grande > SVG > PNG pequeno).
static int find_icon(const char *name, char *out, size_t n) {
  if (!name || !name[0])
    return 0;
  if (name[0] == '/') {
    if (file_exists(name)) {
      snprintf(out, n, "%s", name);
      return 1;
    }
    return 0;
  }
  static const char *const themes[] = {"Aurora", "hicolor", "Adwaita"};
  static const char *const big[] = {"256x256", "128x128", "64x64"};
  static const char *const small[] = {"48x48", "32x32", "24x24"};
  char p[320];
  // el nombre puede llevar ya extension (Icon=foo.png)
  const char *ext = (ends_with(name, ".png") || ends_with(name, ".svg")) ? "" : ".png";
  for (unsigned t = 0; t < 3; t++)
    for (unsigned s = 0; s < 3; s++) {
      snprintf(p, sizeof(p), "/usr/share/icons/%s/%s/apps/%s%s", themes[t], big[s], name, ext);
      if (file_exists(p)) { snprintf(out, n, "%s", p); return 1; }
    }
  for (unsigned t = 0; t < 3; t++) {
    snprintf(p, sizeof(p), "/usr/share/icons/%s/scalable/apps/%s%s", themes[t], name,
             ext[0] ? ".svg" : "");
    if (file_exists(p)) { snprintf(out, n, "%s", p); return 1; }
  }
  for (unsigned t = 0; t < 3; t++)
    for (unsigned s = 0; s < 3; s++) {
      snprintf(p, sizeof(p), "/usr/share/icons/%s/%s/apps/%s%s", themes[t], small[s], name, ext);
      if (file_exists(p)) { snprintf(out, n, "%s", p); return 1; }
    }
  snprintf(p, sizeof(p), "/usr/share/pixmaps/%s%s", name, ext);
  if (file_exists(p)) { snprintf(out, n, "%s", p); return 1; }
  return 0;
}

static void cache_path(const char *key, char *out, size_t n) {
  char k[64];
  size_t i = 0;
  for (; key[i] && i < sizeof(k) - 1; i++)
    k[i] = (key[i] >= '0' && key[i] <= '9') || (key[i] >= 'a' && key[i] <= 'z') ||
                   (key[i] >= 'A' && key[i] <= 'Z') || key[i] == '-' || key[i] == '.'
               ? key[i]
               : '_';
  k[i] = 0;
  snprintf(out, n, ICON_DIR "/%s.bmp", k);
}

int aw_icon_for(const char *key, const char *const *names, int nnames, char *out, size_t n) {
  mkdir(ICON_DIR, 0755);
  cache_path(key, out, n);
  if (file_exists(out))
    return 1;
  for (int i = 0; i < nnames; i++) {
    char src[320];
    if (!find_icon(names[i], src, sizeof(src)))
      continue;
    int w = 0, h = 0;
    uint8_t *px = ends_with(src, ".svg") ? load_svg(src, &w, &h) : load_png(src, &w, &h);
    if (!px)
      continue;
    int rc = write_bmp(out, px, w, h);
    free(px);
    if (rc == 0)
      return 1;
  }
  return 0;
}

int aw_icon_from_argb(const char *key, const uint32_t *argb, int w, int h, char *out, size_t n) {
  if (w <= 0 || h <= 0 || w > 512 || h > 512)
    return 0;
  mkdir(ICON_DIR, 0755);
  cache_path(key, out, n);
  uint8_t *bgra = malloc((size_t)w * h * 4);
  if (!bgra)
    return 0;
  for (int i = 0; i < w * h; i++) {
    uint32_t v = argb[i]; // A R G B (no premultiplicado)
    bgra[i * 4 + 0] = (uint8_t)v;
    bgra[i * 4 + 1] = (uint8_t)(v >> 8);
    bgra[i * 4 + 2] = (uint8_t)(v >> 16);
    bgra[i * 4 + 3] = (uint8_t)(v >> 24);
  }
  int rc = write_bmp(out, bgra, w, h);
  free(bgra);
  return rc == 0;
}
