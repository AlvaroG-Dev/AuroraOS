// kernel/gfx/cursor.c
#include "cursor.h"
#include "gfx.h"
#include <emmintrin.h>

#define SS 4            /* supermuestreo por eje */
#define OUTLINE_W 1.05f /* grosor del borde blanco, px */
#define SHADOW_A 0.34f  /* opacidad máxima de la sombra */
#define SHADOW_DY 1     /* la sombra cae ligeramente hacia abajo */

typedef struct {
  float x, y;
} pt_t;

static cursor_img_t g_cursors[CURSOR_COUNT];
static int g_ready = 0;

static inline float c_sqrtf(float x) {
  return _mm_cvtss_f32(_mm_sqrt_ss(_mm_set_ss(x)));
}

static int poly_inside(const pt_t *p, int n, float x, float y) {
  int in = 0;
  for (int i = 0, j = n - 1; i < n; j = i++) {
    if ((p[i].y > y) != (p[j].y > y) &&
        x < (p[j].x - p[i].x) * (y - p[i].y) / (p[j].y - p[i].y) + p[i].x)
      in = !in;
  }
  return in;
}

static float poly_dist(const pt_t *p, int n, float x, float y) {
  float best = 1e9f;
  for (int i = 0, j = n - 1; i < n; j = i++) {
    float vx = p[i].x - p[j].x, vy = p[i].y - p[j].y;
    float wx = x - p[j].x, wy = y - p[j].y;
    float l2 = vx * vx + vy * vy;
    float t = l2 > 0.0f ? (wx * vx + wy * vy) / l2 : 0.0f;
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    float dx = wx - vx * t, dy = wy - vy * t;
    float d = dx * dx + dy * dy;
    if (d < best)
      best = d;
  }
  return c_sqrtf(best);
}

/* Box blur 3x3 (dos pasadas ~ gaussiana de radio 2) de una máscara S x S. */
static void blur_mask(float *m, float *tmp) {
  const int S = CURSOR_IMG;
  for (int pass = 0; pass < 2; pass++) {
    for (int y = 0; y < S; y++)
      for (int x = 0; x < S; x++) {
        float s = 0.0f;
        for (int k = -1; k <= 1; k++) {
          int xx = x + k;
          if (xx >= 0 && xx < S)
            s += m[y * S + xx];
        }
        tmp[y * S + x] = s / 3.0f;
      }
    for (int y = 0; y < S; y++)
      for (int x = 0; x < S; x++) {
        float s = 0.0f;
        for (int k = -1; k <= 1; k++) {
          int yy = y + k;
          if (yy >= 0 && yy < S)
            s += tmp[yy * S + x];
        }
        m[y * S + x] = s / 3.0f;
      }
  }
}

/* Rasteriza el polígono: relleno negro, borde blanco exterior y sombra. */
static void build(cursor_img_t *c, const pt_t *poly, int n, int hot_x,
                  int hot_y) {
  const int S = CURSOR_IMG;
  static float fill[CURSOR_IMG * CURSOR_IMG], outer[CURSOR_IMG * CURSOR_IMG];
  static float shadow[CURSOR_IMG * CURSOR_IMG], tmp[CURSOR_IMG * CURSOR_IMG];

  for (int y = 0; y < S; y++) {
    for (int x = 0; x < S; x++) {
      int nf = 0, no = 0;
      for (int sy = 0; sy < SS; sy++) {
        for (int sx = 0; sx < SS; sx++) {
          float fx = (float)x + ((float)sx + 0.5f) / SS;
          float fy = (float)y + ((float)sy + 0.5f) / SS;
          int in = poly_inside(poly, n, fx, fy);
          nf += in;
          no += in || poly_dist(poly, n, fx, fy) <= OUTLINE_W;
        }
      }
      fill[y * S + x] = (float)nf / (SS * SS);
      outer[y * S + x] = (float)no / (SS * SS);
    }
  }

  for (int i = 0; i < S * S; i++)
    shadow[i] = 0.0f;
  for (int y = 0; y + SHADOW_DY < S; y++)
    for (int x = 0; x < S; x++)
      shadow[(y + SHADOW_DY) * S + x] = outer[y * S + x];
  blur_mask(shadow, tmp);

  for (int i = 0; i < S * S; i++) {
    uint32_t p = 0;
    uint32_t sa = (uint32_t)(shadow[i] * SHADOW_A * 255.0f + 0.5f);
    if (sa)
      p = blend_pixel_fast(sa << 24, p);
    uint32_t wa = (uint32_t)(outer[i] * 255.0f + 0.5f);
    if (wa)
      p = blend_pixel_fast((wa << 24) | 0x00FFFFFF, p);
    uint32_t ba = (uint32_t)(fill[i] * 255.0f + 0.5f);
    if (ba)
      p = blend_pixel_fast((ba << 24) | 0x00101010, p);
    c->px[i] = p;
  }
  c->w = S;
  c->h = S;
  c->hot_x = hot_x;
  c->hot_y = hot_y;
}

/* Flecha doble a lo largo del eje (cs, sn), centrada en la imagen. */
static void build_double_arrow(cursor_img_t *c, float cs, float sn,
                               float scale) {
  static const pt_t base[] = {{-9.0f, 0.0f}, {-4.2f, -4.8f}, {-4.2f, -1.3f},
                              {4.2f, -1.3f}, {4.2f, -4.8f},  {9.0f, 0.0f},
                              {4.2f, 4.8f},  {4.2f, 1.3f},   {-4.2f, 1.3f},
                              {-4.2f, 4.8f}};
  const int n = (int)(sizeof(base) / sizeof(base[0]));
  const float cx = CURSOR_IMG / 2.0f, cy = CURSOR_IMG / 2.0f;
  pt_t p[sizeof(base) / sizeof(base[0])];
  for (int i = 0; i < n; i++) {
    float x = base[i].x * scale, y = base[i].y * scale;
    p[i].x = cx + x * cs - y * sn;
    p[i].y = cy + x * sn + y * cs;
  }
  build(c, p, n, CURSOR_IMG / 2, CURSOR_IMG / 2);
}

void cursor_init(void) {
  if (g_ready)
    return;
  /* Flecha clásica de macOS (punta en (0,0)), algo mayor que 1x para que se
   * lea bien a 1080p. */
  static const pt_t arrow[] = {{0.0f, 0.0f},   {0.0f, 15.2f}, {3.6f, 11.8f},
                               {6.0f, 17.4f},  {8.5f, 16.3f}, {6.2f, 11.0f},
                               {10.9f, 11.0f}};
  const int n = (int)(sizeof(arrow) / sizeof(arrow[0]));
  /* 0,80: flecha de ~9 x 14 px (antes 12 x 19), cerca del tamaño real de
   * macOS a 1080p. */
  const float k = 0.80f, ox = 3.0f, oy = 3.0f;
  pt_t p[sizeof(arrow) / sizeof(arrow[0])];
  for (int i = 0; i < n; i++) {
    p[i].x = ox + arrow[i].x * k;
    p[i].y = oy + arrow[i].y * k;
  }
  build(&g_cursors[CURSOR_ARROW], p, n, (int)ox, (int)oy);

  const float d = 0.70710678f;
  build_double_arrow(&g_cursors[CURSOR_RESIZE_EW], 1.0f, 0.0f, 0.78f);
  build_double_arrow(&g_cursors[CURSOR_RESIZE_NS], 0.0f, 1.0f, 0.78f);
  build_double_arrow(&g_cursors[CURSOR_RESIZE_NWSE], d, d, 0.72f);
  build_double_arrow(&g_cursors[CURSOR_RESIZE_NESW], d, -d, 0.72f);

  /* I de texto (punto activo en el centro). */
  static const pt_t ibeam[] = {
      {-3.5f, -8.0f}, {3.5f, -8.0f}, {3.5f, -6.5f}, {0.9f, -6.5f},
      {0.9f, 6.5f},   {3.5f, 6.5f},  {3.5f, 8.0f},  {-3.5f, 8.0f},
      {-3.5f, 6.5f},  {-0.9f, 6.5f}, {-0.9f, -6.5f}, {-3.5f, -6.5f}};
  {
    const int m = (int)(sizeof(ibeam) / sizeof(ibeam[0]));
    pt_t q[sizeof(ibeam) / sizeof(ibeam[0])];
    for (int i = 0; i < m; i++) {
      q[i].x = CURSOR_IMG / 2.0f + ibeam[i].x;
      q[i].y = CURSOR_IMG / 2.0f + ibeam[i].y;
    }
    build(&g_cursors[CURSOR_TEXT], q, m, CURSOR_IMG / 2, CURSOR_IMG / 2);
  }

  /* Cruz de precisión. */
  static const pt_t cross[] = {
      {-1.0f, -8.0f}, {1.0f, -8.0f}, {1.0f, -1.0f}, {8.0f, -1.0f},
      {8.0f, 1.0f},   {1.0f, 1.0f},  {1.0f, 8.0f},  {-1.0f, 8.0f},
      {-1.0f, 1.0f},  {-8.0f, 1.0f}, {-8.0f, -1.0f}, {-1.0f, -1.0f}};
  {
    const int m = (int)(sizeof(cross) / sizeof(cross[0]));
    pt_t q[sizeof(cross) / sizeof(cross[0])];
    for (int i = 0; i < m; i++) {
      q[i].x = CURSOR_IMG / 2.0f + cross[i].x;
      q[i].y = CURSOR_IMG / 2.0f + cross[i].y;
    }
    build(&g_cursors[CURSOR_CROSS], q, m, CURSOR_IMG / 2, CURSOR_IMG / 2);
  }

  /* Mano con el índice levantado (punto activo en la yema del índice). */
  static const pt_t hand[] = {
      {-1.5f, 0.0f}, {1.5f, 0.0f},  {1.5f, 6.0f},  {4.2f, 6.0f},
      {4.2f, 7.5f},  {6.8f, 7.5f},  {6.8f, 9.0f},  {8.8f, 9.0f},
      {8.8f, 14.0f}, {7.0f, 17.0f}, {7.0f, 19.0f}, {-3.0f, 19.0f},
      {-3.0f, 16.0f}, {-6.5f, 11.5f}, {-6.5f, 9.5f}, {-4.5f, 9.5f},
      {-1.5f, 12.0f}};
  {
    const int m = (int)(sizeof(hand) / sizeof(hand[0]));
    pt_t q[sizeof(hand) / sizeof(hand[0])];
    for (int i = 0; i < m; i++) {
      q[i].x = 10.0f + hand[i].x;
      q[i].y = 4.0f + hand[i].y;
    }
    build(&g_cursors[CURSOR_HAND], q, m, 10, 4);
  }

  /* Invisible: una imagen de 1x1 transparente. */
  g_cursors[CURSOR_NONE].w = 1;
  g_cursors[CURSOR_NONE].h = 1;
  g_cursors[CURSOR_NONE].hot_x = 0;
  g_cursors[CURSOR_NONE].hot_y = 0;
  g_cursors[CURSOR_NONE].px[0] = 0;
  g_ready = 1;
}

const cursor_img_t *cursor_get(cursor_shape_t shape) {
  if (!g_ready)
    cursor_init();
  if ((int)shape < 0 || shape >= CURSOR_COUNT)
    shape = CURSOR_ARROW;
  return &g_cursors[shape];
}
