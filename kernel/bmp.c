// kernel/bmp.c
#include "bmp.h"
#include "serial.h"

// Referencia externa al framebuffer de main.c / compositor.c
extern uint32_t *fb_ptr;
extern uint32_t fb_width;
extern uint32_t fb_height;
extern uint32_t fb_pitch;

#define MIN(a,b) ((a) < (b) ? (a) : (b))
#define MAX(a,b) ((a) > (b) ? (a) : (b))

static void fb_putpixel(int x, int y, uint32_t color) {
    if (x < 0 || x >= (int)fb_width || y < 0 || y >= (int)fb_height)
        return;
    fb_ptr[y * fb_pitch + x] = color;
}

// Auxiliar interna para Alpha Blending rápido (ARGB)
static inline uint32_t blend_pixel(uint32_t src, uint32_t dst) {
    uint32_t alpha = (src >> 24) & 0xFF;
    if (alpha == 255) return src;
    if (alpha == 0)   return dst;

    uint32_t inv_alpha = 255 - alpha;
    uint32_t r = (((src >> 16) & 0xFF) * alpha + ((dst >> 16) & 0xFF) * inv_alpha) / 255;
    uint32_t g = (((src >> 8) & 0xFF) * alpha + ((dst >> 8) & 0xFF) * inv_alpha) / 255;
    uint32_t b = ((src & 0xFF) * alpha + (dst & 0xFF) * inv_alpha) / 255;

    return (0xFF000000) | (r << 16) | (g << 8) | b;
}

int bmp_draw(tar_node_t *file, int dest_x, int dest_y) {
    if (!file || !file->data || file->size < sizeof(bmp_file_header_t) + sizeof(bmp_info_header_t)) {
        serial_puts("[BMP] ERROR: Archivo nulo o inválido\n");
        return -1;
    }

    bmp_file_header_t *file_hdr = (bmp_file_header_t *)file->data;
    bmp_info_header_t *info_hdr = (bmp_info_header_t *)(file->data + sizeof(bmp_file_header_t));

    // Validar firma 'BM' (0x4D42 en Little Endian)
    if (file_hdr->type != 0x4D42) {
        serial_puts("[BMP] ERROR: No es un archivo BMP válido\n");
        return -1;
    }

    // Solo soportamos BMP sin compresión (BI_RGB = 0)
    if (info_hdr->compression != 0) {
        serial_puts("[BMP] ERROR: Compresión BMP no soportada\n");
        return -1;
    }

    int width = info_hdr->width;
    int height = info_hdr->height;
    int bpp = info_hdr->bpp;

    if (bpp != 24 && bpp != 32) {
        serial_puts("[BMP] ERROR: Solo se soportan BMPs de 24 o 32 bits\n");
        return -1;
    }

    // En BMP, la altura positiva indica almacenamiento de abajo hacia arriba (Bottom-Up)
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

    serial_puts("[BMP] Renderizado: ");
    serial_puts(file->name);
    serial_puts("\n");

    return 0;
}

int bmp_draw_scaled(tar_node_t *file, uint32_t *dst, int dst_stride, rect_t clip, int dest_x, int dest_y, int dest_w, int dest_h) {
    if (!file || !file->data || !dst || dest_w <= 0 || dest_h <= 0) return -1;

    bmp_file_header_t *file_hdr = (bmp_file_header_t *)file->data;
    bmp_info_header_t *info_hdr = (bmp_info_header_t *)(file->data + sizeof(bmp_file_header_t));

    if (file_hdr->type != 0x4D42 || info_hdr->compression != 0) return -1;

    int width = info_hdr->width;
    int height = info_hdr->height;
    int bpp = info_hdr->bpp;
    if (bpp != 24 && bpp != 32) return -1;

    int bottom_up = 1;
    if (height < 0) {
        height = -height;
        bottom_up = 0;
    }

    uint8_t *pixel_data = file->data + file_hdr->offset;
    int bytes_per_pixel = bpp / 8;
    int row_stride = ((width * bpp + 31) / 32) * 4;

    int start_y = MAX(dest_y, clip.y);
    int end_y   = MIN(dest_y + dest_h, clip.y + clip.h);
    int start_x = MAX(dest_x, clip.x);
    int end_x   = MIN(dest_x + dest_w, clip.x + clip.w);

    if (start_x >= end_x || start_y >= end_y) return 0;

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

            uint32_t src_color = ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
            dst_row[px] = blend_pixel(src_color, dst_row[px]);
        }
    }
    return 0;
}

