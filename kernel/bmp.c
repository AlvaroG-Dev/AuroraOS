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

  // Source-over ARGB
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

  if (file_hdr->type != 0x4D42) {
    LOG_ERR("[BMP] ERROR: No es un archivo BMP válido");
    return -1;
  }

  if (info_hdr->compression != 0 && info_hdr->compression != 3) {
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

  int bottom_up = 1;
  if (height < 0) {
    height = -height;
    bottom_up = 0;
  }

  uint8_t *pixel_data = file->data + file_hdr->offset;
  int bytes_per_pixel = bpp / 8;
  int row_stride = ((width * bpp + 31) / 32) * 4;

  for (int y = 0; y < height; y++) {
    int src_y = bottom_up ? (height - 1 - y) : y;
    uint8_t *row = pixel_data + (src_y * row_stride);

    for (int x = 0; x < width; x++) {
      uint8_t b = row[x * bytes_per_pixel + 0];
      uint8_t g = row[x * bytes_per_pixel + 1];
      uint8_t r = row[x * bytes_per_pixel + 2];
      uint32_t color = (r << 16) | (g << 8) | b;
      fb_putpixel(dest_x + x, dest_y + y, color);
    }
  }

  return 0;
}

int bmp_draw_to_buffer(tar_node_t *file, uint32_t *dst, int dst_stride,
                       rect_t clip, int dest_x, int dest_y) {
  if (!file || !file->data || !dst)
    return -1;
  if (file->size < sizeof(bmp_file_header_t) + sizeof(bmp_info_header_t))
    return -1;

  bmp_file_header_t *file_hdr = (bmp_file_header_t *)file->data;
  bmp_info_header_t *info_hdr =
      (bmp_info_header_t *)(file->data + sizeof(bmp_file_header_t));

  if (file_hdr->type != 0x4D42 ||
      (info_hdr->compression != 0 && info_hdr->compression != 3))
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
  int end_y = MIN(dest_y + height, clip.y + clip.h);
  int start_x = MAX(dest_x, clip.x);
  int end_x = MIN(dest_x + width, clip.x + clip.w);

  for (int y = start_y; y < end_y; y++) {
    int src_y = bottom_up ? (height - 1 - (y - dest_y)) : (y - dest_y);
    uint8_t *row = pixel_data + (src_y * row_stride);
    uint32_t *dst_row = &dst[y * dst_stride];

    for (int x = start_x; x < end_x; x++) {
      int src_x = x - dest_x;
      uint8_t b = row[src_x * bytes_per_pixel + 0];
      uint8_t g = row[src_x * bytes_per_pixel + 1];
      uint8_t r = row[src_x * bytes_per_pixel + 2];
      uint8_t a = (bpp == 32) ? row[src_x * bytes_per_pixel + 3] : 0xFF;
      uint32_t src_color =
          ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
      dst_row[x] = blend_pixel(src_color, dst_row[x]);
    }
  }

  return 0;
}

int bmp_draw_scaled(tar_node_t *file, uint32_t *dst, int dst_stride,
                    rect_t clip, int dest_x, int dest_y, int dest_w,
                    int dest_h) {
  if (!file || !file->data || !dst || dest_w <= 0 || dest_h <= 0 ||
      dst_stride <= 0) {
    return -1;
  }

  if (file->size < sizeof(bmp_file_header_t) + sizeof(bmp_info_header_t)) {
    return -1;
  }

  bmp_file_header_t *fh = (bmp_file_header_t *)file->data;

  bmp_info_header_t *ih =
      (bmp_info_header_t *)(file->data + sizeof(bmp_file_header_t));

  if (fh->type != 0x4D42) {
    return -1;
  }

  if (ih->compression != 0 && ih->compression != 3) {
    return -1;
  }

  if (ih->bpp != 24 && ih->bpp != 32) {
    return -1;
  }

  if (ih->width <= 0 || ih->height == 0) {
    return -1;
  }

  if (fh->offset >= file->size) {
    return -1;
  }

  /*
   * Usamos uint64_t para todas las coordenadas del BMP.
   * Así evitamos overflow en:
   *
   *   x * src_width
   *   y * src_height
   *   row_stride * height
   */
  const uint64_t src_width = (uint64_t)(uint32_t)ih->width;

  const int32_t raw_height = ih->height;

  const uint64_t src_height = (raw_height < 0)
                                  ? (uint64_t)(-(int64_t)raw_height)
                                  : (uint64_t)(uint32_t)raw_height;

  const int bottom_up = (raw_height > 0);

  const uint32_t bpp = (uint32_t)ih->bpp;

  const uint32_t bytes_per_pixel = bpp / 8;

  /*
   * Pitch del BMP.
   *
   * BMP siempre alinea cada fila a 4 bytes.
   */
  const uint64_t row_stride =
      ((src_width * (uint64_t)bpp + 31ULL) / 32ULL) * 4ULL;

  /*
   * Verificar que el bitmap cabe realmente en el fichero.
   */
  if (src_height != 0 && row_stride > (UINT64_MAX / src_height)) {
    return -1;
  }

  const uint64_t pixel_bytes = row_stride * src_height;

  if (pixel_bytes > (uint64_t)file->size - (uint64_t)fh->offset) {
    return -1;
  }

  uint8_t *pixels = file->data + fh->offset;

  /*
   * Clip del área destino.
   */
  rect_t dst_rect = {dest_x, dest_y, dest_w, dest_h};

  rect_t c = rect_clip(dst_rect, clip);

  if (c.w <= 0 || c.h <= 0) {
    return 0;
  }

  /*
   * Denominador común del área de cada píxel destino.
   *
   * Un píxel de destino representa:
   *
   *   src_width / dest_width
   *   ×
   *   src_height / dest_height
   *
   * unidades de imagen fuente.
   *
   * Las áreas se mantienen como enteros racionales:
   *
   *   overlap_x / dest_width
   *   overlap_y / dest_height
   *
   * por lo que el peso 2D es:
   *
   *   overlap_x * overlap_y
   *
   * y el denominador final:
   *
   *   dest_width * dest_height
   */
  const uint64_t dst_width = (uint64_t)(uint32_t)dest_w;

  const uint64_t dst_height = (uint64_t)(uint32_t)dest_h;

  /*
   * dest_w/dest_h proceden de int positivos, así que el
   * producto cabe en uint64_t.
   */
  const uint64_t area_den = src_width * src_height;

  /*
   * Procesar cada píxel de salida.
   */
  for (int py = c.y; py < c.y + c.h; py++) {

    /*
     * Intervalo vertical exacto del píxel destino
     * expresado con denominador dst_height.
     *
     * [sy0_num, sy1_num)
     */
    const uint64_t dy = (uint64_t)(py - dest_y);

    const uint64_t sy0_num = dy * src_height;

    const uint64_t sy1_num = (dy + 1ULL) * src_height;

    uint64_t sy_first = sy0_num / dst_height;

    uint64_t sy_last = (sy1_num - 1ULL) / dst_height;

    if (sy_first >= src_height) {
      sy_first = src_height - 1;
    }

    if (sy_last >= src_height) {
      sy_last = src_height - 1;
    }

    for (int px = c.x; px < c.x + c.w; px++) {

      /*
       * Intervalo horizontal exacto.
       */
      const uint64_t dx = (uint64_t)(px - dest_x);

      const uint64_t sx0_num = dx * src_width;

      const uint64_t sx1_num = (dx + 1ULL) * src_width;

      uint64_t sx_first = sx0_num / dst_width;

      uint64_t sx_last = (sx1_num - 1ULL) / dst_width;

      if (sx_first >= src_width) {
        sx_first = src_width - 1;
      }

      if (sx_last >= src_width) {
        sx_last = src_width - 1;
      }

      /*
       * Acumuladores del filtro.
       *
       * Para 24 bpp:
       *
       *     sum(channel * area)
       *
       * Para 32 bpp:
       *
       *     sum(alpha * area)
       *     sum(channel * alpha * area)
       *
       * Esto permite hacer el filtrado alpha-correcto
       * sin halos.
       */
      uint64_t sum_r = 0;
      uint64_t sum_g = 0;
      uint64_t sum_b = 0;
      uint64_t sum_a = 0;

      int overflow = 0;

      /*
       * Recorrer exclusivamente los píxeles fuente
       * que realmente intersectan el píxel destino.
       */
      for (uint64_t sy = sy_first; sy <= sy_last; sy++) {

        /*
         * Intersección vertical exacta:
         *
         * [sy0_num, sy1_num)
         * con
         * [sy * dst_height,
         *  (sy + 1) * dst_height)
         */
        uint64_t src_y0 = sy * dst_height;

        uint64_t src_y1 = src_y0 + dst_height;

        uint64_t y_left = (sy0_num > src_y0) ? sy0_num : src_y0;

        uint64_t y_right = (sy1_num < src_y1) ? sy1_num : src_y1;

        if (y_right <= y_left) {
          continue;
        }

        const uint64_t wy = y_right - y_left;

        uint64_t src_y = bottom_up ? (src_height - 1ULL - sy) : sy;

        uint8_t *row = pixels + (size_t)src_y * (size_t)row_stride;

        for (uint64_t sx = sx_first; sx <= sx_last; sx++) {

          /*
           * Intersección horizontal exacta.
           */
          uint64_t src_x0 = sx * dst_width;

          uint64_t src_x1 = src_x0 + dst_width;

          uint64_t x_left = (sx0_num > src_x0) ? sx0_num : src_x0;

          uint64_t x_right = (sx1_num < src_x1) ? sx1_num : src_x1;

          if (x_right <= x_left) {
            continue;
          }

          const uint64_t wx = x_right - x_left;

          /*
           * Peso de área exacto:
           *
           *     wx * wy
           *
           * El denominador común es:
           *
           *     dst_width * dst_height
           */
          const uint64_t weight = wx * wy;

          const uint8_t *p = row + (size_t)sx * (size_t)bytes_per_pixel;

          const uint32_t b = p[0];
          const uint32_t g = p[1];
          const uint32_t r = p[2];

          if (bpp == 24) {

            /*
             * 24-bit: completamente opaco.
             */
            if (weight != 0) {

              if (r > UINT64_MAX / weight ||
                  sum_r > UINT64_MAX - (uint64_t)r * weight) {
                overflow = 1;
                break;
              }

              if (g > UINT64_MAX / weight ||
                  sum_g > UINT64_MAX - (uint64_t)g * weight) {
                overflow = 1;
                break;
              }

              if (b > UINT64_MAX / weight ||
                  sum_b > UINT64_MAX - (uint64_t)b * weight) {
                overflow = 1;
                break;
              }

              sum_r += (uint64_t)r * weight;

              sum_g += (uint64_t)g * weight;

              sum_b += (uint64_t)b * weight;
            }

          } else {

            /*
             * 32-bit BGRA/ARGB en memoria:
             *
             * [B][G][R][A]
             */
            const uint32_t a = p[3];

            /*
             * Alpha acumulado.
             */
            if (a != 0 && weight != 0) {

              if (a > UINT64_MAX / weight ||
                  sum_a > UINT64_MAX - (uint64_t)a * weight) {
                overflow = 1;
                break;
              }

              sum_a += (uint64_t)a * weight;

              /*
               * Premultiplied RGB:
               *
               * R*a
               * G*a
               * B*a
               */
              const uint64_t ra = (uint64_t)r * a;

              const uint64_t ga = (uint64_t)g * a;

              const uint64_t ba = (uint64_t)b * a;

              if (ra > UINT64_MAX / weight ||
                  sum_r > UINT64_MAX - ra * weight) {
                overflow = 1;
                break;
              }

              if (ga > UINT64_MAX / weight ||
                  sum_g > UINT64_MAX - ga * weight) {
                overflow = 1;
                break;
              }

              if (ba > UINT64_MAX / weight ||
                  sum_b > UINT64_MAX - ba * weight) {
                overflow = 1;
                break;
              }

              sum_r += ra * weight;

              sum_g += ga * weight;

              sum_b += ba * weight;
            }
          }
        }

        if (overflow) {
          break;
        }
      }

      /*
       * Si una imagen absurdamente grande provocase
       * overflow aritmético, no dañamos memoria ni
       * generamos basura: el caller puede activar su
       * wallpaper de fallback.
       */
      if (overflow) {
        return -1;
      }

      uint32_t out_r;
      uint32_t out_g;
      uint32_t out_b;
      uint32_t out_a;

      if (bpp == 24) {

        /*
         * Como todos los pesos cubren exactamente el
         * área del píxel destino:
         *
         *     sum(weight) == area_den
         */
        out_r = (uint32_t)(sum_r / area_den);

        out_g = (uint32_t)(sum_g / area_den);

        out_b = (uint32_t)(sum_b / area_den);

        out_a = 255;

      } else {

        /*
         * Alpha medio del área.
         */
        out_a = (uint32_t)(sum_a / area_den);

        if (out_a == 0) {
          out_r = 0;
          out_g = 0;
          out_b = 0;
        } else {

          /*
           * Despremultiplicar RGB.
           *
           * sum_r contiene:
           *
           *     Σ(R * A * area)
           *
           * sum_a contiene:
           *
           *     Σ(A * area)
           *
           * Por tanto:
           *
           *     R = sum_r / sum_a
           */
          out_r = (uint32_t)(sum_r / sum_a);

          out_g = (uint32_t)(sum_g / sum_a);

          out_b = (uint32_t)(sum_b / sum_a);

          if (out_r > 255)
            out_r = 255;

          if (out_g > 255)
            out_g = 255;

          if (out_b > 255)
            out_b = 255;
        }
      }

      uint32_t src_color = (out_a << 24) | (out_r << 16) | (out_g << 8) | out_b;

      dst[(size_t)py * (size_t)dst_stride + (size_t)px] = blend_pixel(
          src_color, dst[(size_t)py * (size_t)dst_stride + (size_t)px]);
    }
  }

  return 0;
}

int bmp_draw_icon_scaled(tar_node_t *file, uint32_t *dst, int dst_w,
                         int dst_h) {
  if (!file || !file->data || !dst || dst_w <= 0 || dst_h <= 0)
    return -1;
  if (file->size < sizeof(bmp_file_header_t) + sizeof(bmp_info_header_t))
    return -1;

  bmp_file_header_t *fh = (bmp_file_header_t *)file->data;
  bmp_info_header_t *ih =
      (bmp_info_header_t *)(file->data + sizeof(bmp_file_header_t));

  if (fh->type != 0x4D42)
    return -1;
  if (ih->compression != 0 && ih->compression != 3)
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

  if (row_stride == 0 || height > UINT64_MAX / row_stride)
    return -1;
  uint64_t pixel_bytes = row_stride * height;
  if (pixel_bytes > file->size - fh->offset)
    return -1;

  uint8_t *pixels = file->data + fh->offset;
  int bottom_up = ih->height > 0;

  /* [FIX] Antes había aquí un "fast path" que, si el BMP ya venía al
   * tamaño exacto del destino (frecuente: los iconos de ventana/taskbar
   * suelen exportarse ya a 22x22/20x20), copiaba el bitmap tal cual sin
   * pasar por la detección de márgenes ni por el recentrado de abajo.
   * Si el asset traía un margen transparente asimétrico "horneado"
   * (p.ej. más aire por debajo que por encima del glifo), ese margen se
   * veía tal cual en pantalla: icono pegado arriba, hueco abajo. Ahora
   * TODO icono pasa por el mismo cálculo de bounding-box + centrado de
   * más abajo, sea cual sea su tamaño original. Para un icono ya bien
   * centrado y sin márgenes el resultado es idéntico salvo por el
   * inset de 1px que aplica pad_x/pad_y (ver más abajo); si se prefiere
   * ese caso a sangre (edge-to-edge), basta con poner pad_x=pad_y=0
   * cuando min/max cubran ya todo el canvas. */

  /* Detección de márgenes transparentes para centrar y escalar el arte (solo
   * 32bpp) */
  uint64_t min_x = 0, min_y = 0, max_x = width, max_y = height;
  if (bytes_per_pixel == 4) {
    min_x = width;
    min_y = height;
    max_x = max_y = 0;

    for (uint64_t sy = 0; sy < height; sy++) {
      uint64_t src_y = bottom_up ? height - 1 - sy : sy;
      uint8_t *row = pixels + src_y * row_stride;
      for (uint64_t sx = 0; sx < width; sx++) {
        if (row[sx * bytes_per_pixel + 3] > 8) {
          if (sx < min_x)
            min_x = sx;
          if (sy < min_y)
            min_y = sy;
          if (sx + 1 > max_x)
            max_x = sx + 1;
          if (sy + 1 > max_y)
            max_y = sy + 1;
        }
      }
    }

    if (min_x >= max_x || min_y >= max_y) {
      min_x = 0;
      min_y = 0;
      max_x = width;
      max_y = height;
    }
  }

  uint64_t src_w = max_x - min_x;
  uint64_t src_h = max_y - min_y;
  /* Si el arte ya ocupa el lienzo entero (icono diseñado a sangre, sin
   * margen que recortar), no metemos el inset de 1px: así un icono ya
   * perfecto no se encoge por el simple hecho de pasar por esta ruta. */
  int full_bleed =
      (min_x == 0 && min_y == 0 && max_x == width && max_y == height);
  int pad_x = (dst_w > 2 && !full_bleed) ? 1 : 0;
  int pad_y = (dst_h > 2 && !full_bleed) ? 1 : 0;
  int inner_w = dst_w - pad_x * 2;
  int inner_h = dst_h - pad_y * 2;
  uint64_t fit_w = (uint64_t)inner_w;
  uint64_t fit_h = (src_h * fit_w) / src_w;
  if (fit_h > (uint64_t)inner_h) {
    fit_h = (uint64_t)inner_h;
    fit_w = (src_w * fit_h) / src_h;
  }
  if (fit_w == 0 || fit_h == 0)
    return -1;

  /* [FIX] Si el sobrante (inner - fit) es IMPAR, no hay forma de repartirlo
   * en dos mitades de píxel entero iguales: off = sobrante/2 trunca hacia
   * abajo y el píxel que sobra se va siempre al lado de "después" (más aire
   * abajo o a la derecha). Esto es justo lo que se veía: unos iconos con
   * sobrante par salían perfectos, otros con sobrante impar salían
   * descentrados 1px según su propio aspect ratio recortado. En vez de
   * aceptar esa asimetría, agrandamos el arte 1px en el eje afectado para
   * que el sobrante sea siempre par: la distorsión de escalado de 1px es
   * imperceptible a estos tamaños y el box-filter de abajo ya la suaviza. */
  if (((inner_w - (int)fit_w) & 1) && fit_w < (uint64_t)inner_w)
    fit_w++;
  if (((inner_h - (int)fit_h) & 1) && fit_h < (uint64_t)inner_h)
    fit_h++;

  uint64_t off_x = ((uint64_t)inner_w - fit_w) / 2;
  uint64_t off_y = ((uint64_t)inner_h - fit_h) / 2;

  for (int dy = 0; dy < dst_h; dy++) {
    for (int dx = 0; dx < dst_w; dx++) {
      dst[dy * dst_w + dx] = 0;
      int ux = dx - pad_x;
      int uy = dy - pad_y;
      if (ux < 0 || uy < 0 || (uint64_t)ux < off_x || (uint64_t)uy < off_y ||
          (uint64_t)ux >= off_x + fit_w || (uint64_t)uy >= off_y + fit_h)
        continue;

      uint64_t tx = (uint64_t)ux - off_x;
      uint64_t ty = (uint64_t)uy - off_y;

      uint64_t sx0 = min_x + (tx * src_w) / fit_w;
      uint64_t sx1 = min_x + (((tx + 1) * src_w) + fit_w - 1) / fit_w;
      uint64_t sy0 = min_y + (ty * src_h) / fit_h;
      uint64_t sy1 = min_y + (((ty + 1) * src_h) + fit_h - 1) / fit_h;

      if (sx1 > max_x)
        sx1 = max_x;
      if (sy1 > max_y)
        sy1 = max_y;
      if (sx1 <= sx0)
        sx1 = sx0 + 1;
      if (sy1 <= sy0)
        sy1 = sy0 + 1;

      uint64_t a_sum = 0, r_sum = 0, g_sum = 0, b_sum = 0;
      uint64_t count = 0;

      for (uint64_t sy = sy0; sy < sy1; sy++) {
        uint64_t src_y = bottom_up ? height - 1 - sy : sy;
        uint8_t *row = pixels + src_y * row_stride;
        for (uint64_t sx = sx0; sx < sx1; sx++) {
          uint8_t *p = row + sx * bytes_per_pixel;
          uint32_t a = (bytes_per_pixel == 4) ? p[3] : 255;
          a_sum += a;
          r_sum += (uint64_t)p[2] * a;
          g_sum += (uint64_t)p[1] * a;
          b_sum += (uint64_t)p[0] * a;
          count++;
        }
      }

      if (count == 0 || a_sum == 0)
        continue;

      uint32_t a = (uint32_t)(a_sum / count);
      uint32_t r = (uint32_t)(r_sum / a_sum);
      uint32_t g = (uint32_t)(g_sum / a_sum);
      uint32_t b = (uint32_t)(b_sum / a_sum);

      if (a > 255)
        a = 255;
      if (r > 255)
        r = 255;
      if (g > 255)
        g = 255;
      if (b > 255)
        b = 255;

      dst[dy * dst_w + dx] = (a << 24) | (r << 16) | (g << 8) | b;
    }
  }

  return 0;
}