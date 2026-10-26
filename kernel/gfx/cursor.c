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

/* Forma = varios contornos cerrados concatenados; `ends[i]` es el índice
 * (exclusivo) donde acaba el contorno i. Relleno por regla del número de
 * vueltas (distinto de cero): un contorno interior de sentido contrario es un
 * agujero. */
typedef struct {
  const pt_t *p;
  const int *ends;
  int nc;
} shape_t;

static int shape_inside(const shape_t *s, float x, float y) {
  int wn = 0, a0 = 0;
  for (int c = 0; c < s->nc; c++) {
    int a1 = s->ends[c];
    for (int i = a0, j = a1 - 1; i < a1; j = i++) {
      const pt_t *p = &s->p[j], *q = &s->p[i];
      float side = (q->x - p->x) * (y - p->y) - (x - p->x) * (q->y - p->y);
      if (p->y <= y) {
        if (q->y > y && side > 0.0f)
          wn++;
      } else if (q->y <= y && side < 0.0f) {
        wn--;
      }
    }
    a0 = a1;
  }
  return wn != 0;
}

static float shape_dist(const shape_t *s, float x, float y) {
  float best = 1e9f;
  int a0 = 0;
  for (int c = 0; c < s->nc; c++) {
    int a1 = s->ends[c];
    for (int i = a0, j = a1 - 1; i < a1; j = i++) {
      float vx = s->p[i].x - s->p[j].x, vy = s->p[i].y - s->p[j].y;
      float wx = x - s->p[j].x, wy = y - s->p[j].y;
      float l2 = vx * vx + vy * vy;
      float t = l2 > 0.0f ? (wx * vx + wy * vy) / l2 : 0.0f;
      t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
      float dx = wx - vx * t, dy = wy - vy * t;
      float d = dx * dx + dy * dy;
      if (d < best)
        best = d;
    }
    a0 = a1;
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
static void build_shape(cursor_img_t *c, const shape_t *sh, int hot_x,
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
          int in = shape_inside(sh, fx, fy);
          nf += in;
          no += in || shape_dist(sh, fx, fy) <= OUTLINE_W;
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

/* Un único contorno. */
static void build(cursor_img_t *c, const pt_t *poly, int n, int hot_x,
                  int hot_y) {
  const int ends[1] = {n};
  const shape_t sh = {poly, ends, 1};
  build_shape(c, &sh, hot_x, hot_y);
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

#define NELEM(a) ((int)(sizeof(a) / sizeof((a)[0])))

/* Mano con el índice levantado (enlaces, botones). Punta del dedo en el punto
 * activo. */
static void build_pointer(cursor_img_t *c) {
  static const pt_t hand[] = {
      {4.0f, 1.2f},  {5.1f, 0.0f},  {6.4f, 0.0f},  {7.5f, 1.2f},  {7.5f, 5.4f},
      {8.7f, 4.8f},  {10.2f, 4.8f}, {11.0f, 5.9f}, {11.0f, 6.3f}, {12.4f, 6.0f},
      {13.8f, 6.1f}, {14.6f, 7.2f}, {14.6f, 7.6f}, {15.9f, 7.5f}, {17.0f, 8.6f},
      {17.0f, 12.6f}, {15.4f, 16.0f}, {14.4f, 17.4f}, {6.6f, 17.4f}, {5.4f, 16.0f},
      {3.6f, 13.2f}, {1.2f, 9.6f},  {1.2f, 8.4f},  {2.4f, 7.7f},  {4.0f, 9.6f}};
  const float k = 0.92f, ox = 3.0f, oy = 3.0f;
  pt_t p[NELEM(hand)];
  for (int i = 0; i < NELEM(hand); i++) {
    p[i].x = ox + hand[i].x * k;
    p[i].y = oy + hand[i].y * k;
  }
  build(c, p, NELEM(hand), (int)(ox + 5.7f * k), (int)(oy + 0.8f * k));
}

/* Mano cerrada (arrastre). Punto activo en el centro. */
static void build_grabbing(cursor_img_t *c) {
  static const pt_t fist[] = {
      {-7.0f, -1.0f}, {-6.2f, -4.0f}, {-4.4f, -4.6f}, {-3.2f, -6.0f},
      {-1.2f, -6.6f}, {0.4f, -5.6f},  {1.8f, -6.6f},  {3.6f, -6.0f},
      {4.6f, -4.6f},  {6.2f, -4.6f},  {7.6f, -3.0f},  {7.6f, 3.0f},
      {6.0f, 6.4f},   {4.6f, 7.6f},   {-3.6f, 7.6f},  {-5.2f, 6.0f},
      {-7.0f, 2.4f}};
  const float cx = CURSOR_IMG / 2.0f, cy = CURSOR_IMG / 2.0f;
  pt_t p[NELEM(fist)];
  for (int i = 0; i < NELEM(fist); i++) {
    p[i].x = cx + fist[i].x;
    p[i].y = cy + fist[i].y;
  }
  build(c, p, NELEM(fist), CURSOR_IMG / 2, CURSOR_IMG / 2);
}

/* Cursor de texto (I-beam), centrado. */
static void build_text(cursor_img_t *c) {
  static const pt_t ib[] = {
      {-3.6f, -7.4f}, {3.6f, -7.4f}, {3.6f, -5.8f}, {0.9f, -5.8f},
      {0.9f, 5.8f},   {3.6f, 5.8f},  {3.6f, 7.4f},  {-3.6f, 7.4f},
      {-3.6f, 5.8f},  {-0.9f, 5.8f}, {-0.9f, -5.8f}, {-3.6f, -5.8f}};
  const float cx = CURSOR_IMG / 2.0f, cy = CURSOR_IMG / 2.0f;
  pt_t p[NELEM(ib)];
  for (int i = 0; i < NELEM(ib); i++) {
    p[i].x = cx + ib[i].x;
    p[i].y = cy + ib[i].y;
  }
  build(c, p, NELEM(ib), CURSOR_IMG / 2, CURSOR_IMG / 2);
}

/* Reloj de arena (espera), centrado. */
static void build_wait(cursor_img_t *c) {
  static const pt_t hg[] = {{-4.8f, -7.4f}, {4.8f, -7.4f}, {0.8f, -0.2f},
                            {4.8f, 7.4f},   {-4.8f, 7.4f}, {-0.8f, -0.2f}};
  const float cx = CURSOR_IMG / 2.0f, cy = CURSOR_IMG / 2.0f;
  pt_t p[NELEM(hg)];
  for (int i = 0; i < NELEM(hg); i++) {
    p[i].x = cx + hg[i].x;
    p[i].y = cy + hg[i].y;
  }
  build(c, p, NELEM(hg), CURSOR_IMG / 2, CURSOR_IMG / 2);
}

/* Círculo tachado (acción no permitida): anillo (contorno exterior y agujero
 * de sentido contrario) más una barra diagonal; se solapan y el número de
 * vueltas los une. */
static void build_not_allowed(cursor_img_t *c) {
  enum { N = 28 };
  const float cx = CURSOR_IMG / 2.0f, cy = CURSOR_IMG / 2.0f;
  const float ro = 8.6f, ri = 6.0f, hw = 1.35f;
  pt_t p[2 * N + 4];
  /* rotación incremental (sin libm): paso de 2*pi/N */
  const float cs = 0.97492791f, sn = 0.22252093f; /* cos/sin(2*pi/28) */
  float ux = 1.0f, uy = 0.0f;
  for (int i = 0; i < N; i++) {
    p[i].x = cx + ro * ux;
    p[i].y = cy + ro * uy;
    p[N + i].x = cx + ri * ux; /* sentido contrario al recorrerlo al revés */
    p[N + i].y = cy + ri * uy;
    float nx = ux * cs - uy * sn, ny = ux * sn + uy * cs;
    ux = nx;
    uy = ny;
  }
  /* invertir el contorno interior para que sea agujero */
  for (int i = 0; i < N / 2; i++) {
    pt_t t = p[N + i];
    p[N + i] = p[N + N - 1 - i];
    p[N + N - 1 - i] = t;
  }
  /* barra diagonal de arriba-izquierda a abajo-derecha */
  const float d = 0.70710678f, r = ro - 0.6f;
  float ax = -d * r, ay = -d * r, bx = d * r, by = d * r;
  float nx = d * hw, ny = -d * hw;
  p[2 * N + 0] = (pt_t){cx + ax + nx, cy + ay + ny};
  p[2 * N + 1] = (pt_t){cx + bx + nx, cy + by + ny};
  p[2 * N + 2] = (pt_t){cx + bx - nx, cy + by - ny};
  p[2 * N + 3] = (pt_t){cx + ax - nx, cy + ay - ny};
  const int ends[3] = {N, 2 * N, 2 * N + 4};
  const shape_t sh = {p, ends, 3};
  build_shape(c, &sh, CURSOR_IMG / 2, CURSOR_IMG / 2);
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
  build_pointer(&g_cursors[CURSOR_POINTER]);
  build_not_allowed(&g_cursors[CURSOR_NOT_ALLOWED]);
  build_text(&g_cursors[CURSOR_TEXT]);
  build_grabbing(&g_cursors[CURSOR_GRABBING]);
  build_wait(&g_cursors[CURSOR_WAIT]);
  g_ready = 1;
}

const cursor_img_t *cursor_get(cursor_shape_t shape) {
  if (!g_ready)
    cursor_init();
  if ((int)shape < 0 || shape >= CURSOR_COUNT)
    shape = CURSOR_ARROW;
  return &g_cursors[shape];
}
