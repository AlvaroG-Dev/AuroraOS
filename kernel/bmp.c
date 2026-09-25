// kernel/bmp.c
#include "bmp.h"
#include "klog.h"

// Referencia externa al framebuffer de main.c / compositor.c
extern uint32_t *fb_ptr;
extern uint32_t fb_width;
extern uint32_t fb_height;
extern uint32_t fb_pitch;

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

static void fb_putpixel(int x, int y, uint32_t color) {
  if (x < 0 || x >= (int)fb_width || y < 0 || y >= (int)fb_height)
    return;
  fb_ptr[y * fb_pitch + x] = color;
}

// Auxiliar interna para Alpha Blending rápido (ARGB)
static inline uint32_t blend_pixel(uint32_t src, uint32_t dst) {
  uint32_t sa = (src >> 24) & 0xFF;
  if (sa == 0)
    return dst;
  if (sa == 255)
    return src;

  // Source-over ARGB. No fuerces el alpha de salida a 0xFF: los caches
  // de iconos se decodifican sobre un canvas transparente.
  uint32_t da = (dst >> 24) & 0xFF;
  uint32_t inv_sa = 255 - sa;
  uint32_t out_a = sa + (da * inv_sa) / 255;

  uint32_t sr = (src >> 16) & 0xFF, sg = (src >> 8) & 0xFF, sb = src & 0xFF;
  uint32_t dr = (dst >> 16) & 0xFF, dg = (dst >> 8) & 0xFF, db = dst & 0xFF;

  uint32_t r, g, b;
  if (out_a == 0) {
    r = g = b = 0;
  } else {
    r = (sr * sa + dr * da * inv_sa / 255) / out_a;
    g = (sg * sa + dg * da * inv_sa / 255) / out_a;
    b = (sb * sa + db * da * inv_sa / 255) / out_a;
  }

  return (out_a << 24) | (r << 16) | (g << 8) | b;
}

int bmp_draw(tar_node_t *file, int dest_x, int dest_y) {
  if (!file || !file->data ||
      file->size < sizeof(bmp_file_header_t) + sizeof(bmp_info_header_t)) {
    LOG_ERR("[BMP] ERROR: Archivo nulo o inválido");
    return -1;
  }

  bmp_file_header_t *file_hdr = (bmp_file_header_t *)file->data;
  bmp_info_header_t *info_hdr =
      (bmp_info_header_t *)(file->data + sizeof(bmp_file_header_t));

  // Validar firma 'BM' (0x4D42 en Little Endian)
  if (file_hdr->type != 0x4D42) {
    LOG_ERR("[BMP] ERROR: No es un archivo BMP válido");
    return -1;
  }

  // Solo soportamos BMP sin compresión (BI_RGB = 0)
  if (info_hdr->compression != 0) {
    LOG_ERR("[BMP] ERROR: Compresión BMP no soportada");
    return -1;
  }

  int width = info_hdr->width;
  int height = info_hdr->height;
  int bpp = info_hdr->bpp;

  if (bpp != 24 && bpp != 32) {
    LOG_ERR("[BMP] ERROR: Solo se soportan BMPs de 24 o 32 bits");
    return -1;
  }

  // En BMP, la altura positiva indica almacenamiento de abajo hacia arriba
  // (Bottom-Up)
  int bottom_up = 1;
  if (height < 0) {
    height = -height;
    bottom_up = 0;
  }

  uint8_t *pixel_data = file->data + file_hdr->offset;
  int bytes_per_pixel = bpp / 8;

  // Cada línea en un BMP está alineada a múltiplos de 4 bytes (Stride)
  int row_stride = ((width * bpp + 31) / 32) * 4;

  for (int y = 0; y < height; y++) {
    // Calcular la fila real de destino
    int src_y = bottom_up ? (height - 1 - y) : y;
    uint8_t *row = pixel_data + (src_y * row_stride);

    for (int x = 0; x < width; x++) {
      uint8_t b = row[x * bytes_per_pixel + 0];
      uint8_t g = row[x * bytes_per_pixel + 1];
      uint8_t r = row[x * bytes_per_pixel + 2];

      // Convertir BGR a 0x00RRGGBB
      uint32_t color = (r << 16) | (g << 8) | b;

      fb_putpixel(dest_x + x, dest_y + y, color);
    }
  }

  LOG_INFO("[BMP] Renderizado: %s", file->name);

  return 0;
}

int bmp_draw_scaled(tar_node_t *file, uint32_t *dst, int dst_stride,
                    rect_t clip, int dest_x, int dest_y, int dest_w,
                    int dest_h) {
  if (!file || !file->data || !dst || dest_w <= 0 || dest_h <= 0)
    return -1;

  bmp_file_header_t *file_hdr = (bmp_file_header_t *)file->data;
  bmp_info_header_t *info_hdr =
      (bmp_info_header_t *)(file->data + sizeof(bmp_file_header_t));

  if (file_hdr->type != 0x4D42 || info_hdr->compression != 0)
    return -1;

  int width = info_hdr->width;
  int height = info_hdr->height;
  int bpp = info_hdr->bpp;
  if (bpp != 24 && bpp != 32)
    return -1;

  int bottom_up = 1;
  if (height < 0) {
    height = -height;
    bottom_up = 0;
  }

  uint8_t *pixel_data = file->data + file_hdr->offset;
  int bytes_per_pixel = bpp / 8;
  int row_stride = ((width * bpp + 31) / 32) * 4;

  int start_y = MAX(dest_y, clip.y);
  int end_y = MIN(dest_y + dest_h, clip.y + clip.h);
  int start_x = MAX(dest_x, clip.x);
  int end_x = MIN(dest_x + dest_w, clip.x + clip.w);

  if (start_x >= end_x || start_y >= end_y)
    return 0;

  // Box filter cuando reducimos >4x en cualquier eje. Nearest neighbor
  // en caso contrario (mantiene la nitidez en escalados moderados).
  int use_box = (width / dest_w > 4) || (height / dest_h > 4);

  if (!use_box) {
    // Camino original: nearest neighbor.
    for (int py = start_y; py < end_y; py++) {
      int sy = ((py - dest_y) * height) / dest_h;
      int src_y = bottom_up ? (height - 1 - sy) : sy;
      uint8_t *row = pixel_data + (src_y * row_stride);
      uint32_t *dst_row = &dst[py * dst_stride];

      for (int px = start_x; px < end_x; px++) {
        int sx = ((px - dest_x) * width) / dest_w;
        uint8_t b = row[sx * bytes_per_pixel + 0];
        uint8_t g = row[sx * bytes_per_pixel + 1];
        uint8_t r = row[sx * bytes_per_pixel + 2];
        uint8_t a = (bpp == 32) ? row[sx * bytes_per_pixel + 3] : 0xFF;
        uint32_t src_color =
            ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
        dst_row[px] = blend_pixel(src_color, dst_row[px]);
      }
    }
    return 0;
  }

  // Box filter: promedio de todos los píxeles fuente que caen dentro
  // del píxel destino. Elimina el aliasing en reducciones fuertes.
  for (int py = start_y; py < end_y; py++) {
    int sy0 = ((py - dest_y) * height) / dest_h;
    int sy1 = ((py - dest_y + 1) * height) / dest_h;
    if (sy1 <= sy0)
      sy1 = sy0 + 1;

    uint32_t *dst_row = &dst[py * dst_stride];

    for (int px = start_x; px < end_x; px++) {
      int sx0 = ((px - dest_x) * width) / dest_w;
      int sx1 = ((px - dest_x + 1) * width) / dest_w;
      if (sx1 <= sx0)
        sx1 = sx0 + 1;

      // Promediar en espacio premultiplicado evita halos oscuros
      // alrededor de iconos transparentes.
      uint64_t r_premul = 0, g_premul = 0, b_premul = 0, a_sum = 0;
      int count = 0;

      for (int sy = sy0; sy < sy1; sy++) {
        int src_y = bottom_up ? (height - 1 - sy) : sy;
        uint8_t *row = pixel_data + (src_y * row_stride);
        for (int sx = sx0; sx < sx1; sx++) {
          uint8_t b = row[sx * bytes_per_pixel + 0];
          uint8_t g = row[sx * bytes_per_pixel + 1];
          uint8_t r = row[sx * bytes_per_pixel + 2];
          uint8_t a = (bpp == 32) ? row[sx * bytes_per_pixel + 3] : 0xFF;
          r_premul += (uint32_t)r * a;
          g_premul += (uint32_t)g * a;
          b_premul += (uint32_t)b * a;
          a_sum += a;
          count++;
        }
      }
      if (count == 0)
        continue;

      uint32_t a = (uint32_t)(a_sum / count);
      uint32_t r = 0, g = 0, b = 0;
      if (a != 0) {
        // Mantener toda la precisión del acumulado premultiplicado.
        // Dividir primero entre count introducía truncamiento fuerte al
        // reducir un BMP grande a un icono pequeño, desplazando los colores
        // de los píxeles semitransparentes.
        r = (uint32_t)((r_premul * 255ULL) / a_sum);
        g = (uint32_t)((g_premul * 255ULL) / a_sum);
        b = (uint32_t)((b_premul * 255ULL) / a_sum);
        if (r > 255) r = 255;
        if (g > 255) g = 255;
        if (b > 255) b = 255;
      }

      uint32_t src_color = (a << 24) | (r << 16) | (g << 8) | b;
      dst_row[px] = blend_pixel(src_color, dst_row[px]);
    }
  }
  return 0;
}
/* Escalado para iconos: copia muestras exactas del BMP al cache transparente.
 * No mezcla contra el destino; la composición se hace después por gfx_bit_blat.
 * Esto evita que el filtro de reducción altere los colores del icono. */
int bmp_draw_icon_scaled(tar_node_t *file, uint32_t *dst, int dst_w,
                         int dst_h) {
  if (!file || !file->data || !dst || dst_w <= 0 || dst_h <= 0)
    return -1;
  if (file->size < sizeof(bmp_file_header_t) + sizeof(bmp_info_header_t))
    return -1;

  bmp_file_header_t *fh = (bmp_file_header_t *)file->data;
  bmp_info_header_t *ih =
      (bmp_info_header_t *)(file->data + sizeof(bmp_file_header_t));

  if (fh->type != 0x4D42 || ih->compression != 0)
    return -1;
  if ((ih->bpp != 24 && ih->bpp != 32) || ih->width <= 0 || ih->height == 0)
    return -1;
  if (fh->offset < sizeof(bmp_file_header_t) || fh->offset >= file->size)
    return -1;

  uint64_t width = (uint64_t)ih->width;
  uint64_t height =
      (uint64_t)(ih->height < 0 ? -(int64_t)ih->height : ih->height);
  uint64_t bytes_per_pixel = (uint64_t)ih->bpp / 8;
  uint64_t row_stride = ((width * (uint64_t)ih->bpp + 31) / 32) * 4;

  if (row_stride == 0 || height > (UINT64_MAX / row_stride))
    return -1;
  uint64_t pixel_bytes = row_stride * height;
  if (pixel_bytes > file->size - fh->offset)
    return -1;

  uint8_t *pixels = file->data + fh->offset;
  int bottom_up = ih->height > 0;

  /*
   * Icons are opaque/transparent ARGB images. Use a single representative
   * source pixel for each destination pixel, but choose the center of the
   * corresponding source cell. This preserves the actual BMP colors and
   * avoids averaging white transparent/background pixels into the icon.
   */
  for (int dy = 0; dy < dst_h; dy++) {
    uint64_t sy = ((uint64_t)(2 * dy + 1) * height) /
                  (uint64_t)(2 * dst_h);
    if (sy >= height)
      sy = height - 1;
    uint64_t src_y = bottom_up ? height - 1 - sy : sy;
    uint8_t *row = pixels + src_y * row_stride;

    for (int dx = 0; dx < dst_w; dx++) {
      uint64_t sx = ((uint64_t)(2 * dx + 1) * width) /
                    (uint64_t)(2 * dst_w);
      if (sx >= width)
        sx = width - 1;

      uint8_t *p = row + sx * bytes_per_pixel;
      uint32_t a = (bytes_per_pixel == 4) ? p[3] : 255;

      dst[dy * dst_w + dx] =
          (a << 24) | ((uint32_t)p[2] << 16) |
          ((uint32_t)p[1] << 8) | p[0];
    }
  }

  return 0;
}
