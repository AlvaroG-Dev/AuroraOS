/* bootloader/efi/splash.c  (reemplaza al actual) */
#include "splash.h"
#include "../common/aurora_splash_core.h"

extern EFI_SYSTEM_TABLE *gST;
extern EFI_BOOT_SERVICES *gBS;

static asp_t S;
static uint32_t g_line[ASP_MAX_W];
static uint8_t *g_fb;
static UINT32 g_pitch, g_bpp;
static UINT64 g_start, g_last, g_tsc_hz = 2000000000ULL;
static int g_active;

static UINT64 rdtsc_now(void) {
  UINT32 lo, hi;
  __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
  return ((UINT64)hi << 32) | lo;
}

static void put_row(void *ctx, int x, int y, int w, const uint32_t *px) {
  (void)ctx;
  if (g_bpp == 32) {
    UINT32 *d = (UINT32 *)(g_fb + (UINT64)y * g_pitch) + x;
    for (int i = 0; i < w; i++)
      d[i] = px[i];
  } else {
    UINT8 *d = g_fb + (UINT64)y * g_pitch + (UINT64)x * 3;
    for (int i = 0; i < w; i++) {
      d[i * 3 + 0] = (UINT8)px[i];
      d[i * 3 + 1] = (UINT8)(px[i] >> 8);
      d[i * 3 + 2] = (UINT8)(px[i] >> 16);
    }
  }
}

static UINT32 now_ms(void) {
  return (UINT32)((rdtsc_now() - g_start) / (g_tsc_hz / 1000ULL));
}

int efi_splash_init(EFI_PHYSICAL_ADDRESS lfb_phys, UINT32 w, UINT32 h,
                    UINT32 pitch, UINT32 bpp) {
  if (!lfb_phys || !w || !h || !pitch || (bpp != 24 && bpp != 32))
    return -1;
  g_fb = (uint8_t *)(UINTN)lfb_phys;
  g_pitch = pitch;
  g_bpp = bpp;

  /* Calibrar TSC contra el timer de UEFI (10 ms). */
  UINT64 c0 = rdtsc_now();
  gBS->Stall(10000);
  UINT64 d = rdtsc_now() - c0;
  if (d >= 1000)
    g_tsc_hz = d * 100ULL;

  asp_layout(&S, w, h);
  VOID *mem = NULL;
  UINTN bytes = (UINTN)S.mask_w * (UINTN)S.mask_h;
  if (EFI_ERROR(gBS->AllocatePool(EfiLoaderData, bytes, &mem)) || !mem)
    return -1;
  S.mask = (uint8_t *)mem;
  asp_render_mask(&S);

  asp_fill_bg(&S, put_row, 0, g_line);
  g_start = rdtsc_now();
  g_last = g_start;
  g_active = 1;
  asp_frame(&S, 0, put_row, 0, g_line);
  return 0;
}

void efi_splash_tick(void) {
  if (!g_active)
    return;
  UINT64 now = rdtsc_now();
  if (now - g_last < g_tsc_hz / 60ULL)
    return;
  g_last = now;
  asp_frame(&S, now_ms(), put_row, 0, g_line);
}

/* Anima hasta que hayan pasado min_ms desde el inicio y cierra. */
void efi_splash_wait(UINT32 min_ms) {
  while (g_active && now_ms() < min_ms) {
    efi_splash_tick();
    gBS->Stall(2000);
  }
  efi_splash_done();
}

int efi_splash_active(void) { return g_active; }

UINT32 efi_splash_elapsed_ms(void) { return g_start ? now_ms() : 0; }

void efi_splash_get_state(UINT32 *phase, UINT32 *elapsed_ms) {
  if (!phase || !elapsed_ms)
    return;
  UINT32 ms = efi_splash_elapsed_ms();
  *elapsed_ms = ms;
  *phase = (ms >= ASP_TITLE_END) ? 2 : 1;
}

/* Error fatal: libera la pantalla para que el texto de debug se vea. */
void efi_splash_abort(void) {
  if (!g_active)
    return;
  g_active = 0;
  UINT32 bytes = (UINT32)S.w * (g_bpp / 8);
  for (UINT32 y = 0; y < (UINT32)S.h; y++) {
    UINT8 *d = g_fb + (UINT64)y * g_pitch;
    for (UINT32 i = 0; i < bytes; i++)
      d[i] = 0;
  }
}

void efi_splash_done(void) { g_active = 0; }