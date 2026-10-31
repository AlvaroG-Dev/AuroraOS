// kernel/gfx/brand.c
#include "brand.h"
#include "brand_emblem_data.h"
#include "brand_wordmark_data.h"
#include "gfx_filter.h"

int brand_emblem_aspect256(void) {
  return BRAND_EMBLEM_W * 256 / BRAND_EMBLEM_H;
}

void brand_draw_emblem(uint32_t *dst, int stride, rect_t clip, int cx, int cy,
                       int h) {
  if (h <= 0)
    return;
  int w = h * BRAND_EMBLEM_W / BRAND_EMBLEM_H;
  gfx_blit_scaled(dst, stride, clip, cx - w / 2, cy - h / 2, w, h,
                  brand_emblem_argb, BRAND_EMBLEM_W, BRAND_EMBLEM_H,
                  GFX_FILTER_BILINEAR);
}

void brand_draw_wordmark(uint32_t *dst, int stride, rect_t clip, int cx, int top,
                         int w) {
  if (w <= 0)
    return;
  int h = w * BRAND_WORDMARK_H / BRAND_WORDMARK_W;
  gfx_blit_scaled(dst, stride, clip, cx - w / 2, top, w, h, brand_wordmark_argb,
                  BRAND_WORDMARK_W, BRAND_WORDMARK_H, GFX_FILTER_BILINEAR);
}
