// vgawin.h — ventana nativa de Aurora para la pantalla de la mini-VM (F3d).
// Llamadas de sistema propias (no se incluye user/syscall.h: choca con musl).
#ifndef AVM_VGAWIN_H
#define AVM_VGAWIN_H

#define AW_BASE 0x1000
#define AW_WIN_CREATE (AW_BASE + 0x10)
#define AW_WIN_DESTROY (AW_BASE + 0x11)
#define AW_WIN_BLIT (AW_BASE + 0x12)
#define AW_WIN_POLL (AW_BASE + 0x13)
#define AW_WIN_RAW (AW_BASE + 0x16)
#define AW_WIN_TITLE (AW_BASE + 0x18)
#define AW_TITLEBAR_H 38 // = AURORA_TITLEBAR_HEIGHT (kernel/gfx/theme.h)

struct aw_event {
  uint32_t type;
  int32_t x, y;
  uint32_t data;
};
struct aw_blit {
  int32_t win_id, x, y, w, h, src_x, src_y, src_stride;
  uint32_t *pixels;
};
#define AWEV_CLOSE 1
#define AWEV_FOCUS 2
#define AWEV_BLUR 3
#define AWEV_KEY 5
#define AWEV_RESIZE 9
#define AWEV_PTR 10       // x, y dentro del area de cliente; data = botones (bit0 izq, bit1 der, bit2 medio)
#define AWEV_PTR_LEAVE 11

static long aw_sys(uint64_t n, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e) {
  long ret;
  register uint64_t r10 __asm__("r10") = d;
  register uint64_t r8 __asm__("r8") = e;
  register uint64_t r9 __asm__("r9") = 0;
  __asm__ volatile("syscall"
                   : "=a"(ret), "+D"(a), "+S"(b), "+d"(c), "+r"(r10), "+r"(r8), "+r"(r9)
                   : "a"(n)
                   : "rcx", "r11", "memory");
  return ret;
}

static int g_vw = -1;            // id de la ventana (-1 = no hay)
static int g_vw_w, g_vw_h;       // tamano del cliente creado
static int g_vw_cw, g_vw_ch;     // area de cliente actual
static int g_vw_closed;

static void vw_open(int w, int h) {
  if (g_vw >= 0)
    aw_sys(AW_WIN_DESTROY, (uint64_t)g_vw, 0, 0, 0, 0);
  g_vw = (int)aw_sys(AW_WIN_CREATE, 120, 80, (uint64_t)w, (uint64_t)(h + AW_TITLEBAR_H),
                     (uint64_t)"aurora-vm");
  if (g_vw >= 0) {
    aw_sys(AW_WIN_RAW, (uint64_t)g_vw, 1, 0, 0, 0); // puntero crudo (AWEV_PTR / AWEV_PTR_LEAVE)
    g_vw_w = g_vw_cw = w;
    g_vw_h = g_vw_ch = h;
  }
}

// Copia a la ventana solo las filas que cambiaron desde el ultimo fotograma (el
// cursor de Windows, un texto que se escribe...): antes iban los 3 MB de un
// 1024x768 en cada fotograma y el VMM no hacia correr al huesped mientras tanto.
static uint32_t *g_vw_prev;
static size_t g_vw_prev_n;
static int g_vw_force = 1;
static unsigned g_vw_frames;
static void vw_present(const uint32_t *px, int w, int h) {
  // Cada ~2 s (120 fotogramas) se repinta entero: si el compositor perdio
  // alguna franja (quedaban bandas negras), se recupera sola.
  if (++g_vw_frames % 120 == 0)
    g_vw_force = 1;
  if (g_vw < 0 || w != g_vw_w || h != g_vw_h) {
    vw_open(w, h);
    g_vw_force = 1;
  }
  if (g_vw < 0)
    return;
  size_t n = (size_t)w * (size_t)h;
  if (n > g_vw_prev_n) {
    uint32_t *p = realloc(g_vw_prev, n * 4);
    if (!p)
      return;
    g_vw_prev = p;
    g_vw_prev_n = n;
    g_vw_force = 1;
  }
  int y0 = 0, y1 = h; // [y0, y1)
  if (!g_vw_force) {
    while (y0 < h && !memcmp(px + (size_t)y0 * w, g_vw_prev + (size_t)y0 * w, (size_t)w * 4))
      y0++;
    if (y0 == h)
      return; // nada nuevo
    while (y1 > y0 && !memcmp(px + (size_t)(y1 - 1) * w, g_vw_prev + (size_t)(y1 - 1) * w, (size_t)w * 4))
      y1--;
  }
  memcpy(g_vw_prev + (size_t)y0 * w, px + (size_t)y0 * w, (size_t)(y1 - y0) * w * 4);
  g_vw_force = 0;
  int ch = h < g_vw_ch ? h : g_vw_ch;
  if (y1 > ch)
    y1 = ch;
  if (y1 <= y0)
    return;
  struct aw_blit b = {.win_id = g_vw, .x = 0, .y = y0,
                      .w = w < g_vw_cw ? w : g_vw_cw, .h = y1 - y0,
                      .src_x = 0, .src_y = y0, .src_stride = w, .pixels = (uint32_t *)px};
  if (aw_sys(AW_WIN_BLIT, (uint64_t)&b, 0, 0, 0, 0) < 0)
    g_vw_force = 1; // no llego: la proxima vez, todo
}

#endif
