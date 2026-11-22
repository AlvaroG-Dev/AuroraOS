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

static void vw_present(const uint32_t *px, int w, int h) {
  if (g_vw < 0 || w != g_vw_w || h != g_vw_h)
    vw_open(w, h);
  if (g_vw < 0)
    return;
  struct aw_blit b = {.win_id = g_vw, .x = 0, .y = 0,
                      .w = w < g_vw_cw ? w : g_vw_cw, .h = h < g_vw_ch ? h : g_vw_ch,
                      .src_x = 0, .src_y = 0, .src_stride = w, .pixels = (uint32_t *)px};
  aw_sys(AW_WIN_BLIT, (uint64_t)&b, 0, 0, 0, 0);
}

#endif
