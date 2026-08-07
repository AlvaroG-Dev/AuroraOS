// kernel/bmp.h
#ifndef BMP_H
#define BMP_H

#include <stdint.h>
#include <stddef.h>
#include "tarfs.h"
#include "gfx/gfx.h"

// Encabezado principal del archivo BMP (14 bytes)
typedef struct {
    uint16_t type;             // Firma "BM" (0x4D42)
    uint32_t size;             // Tamaño del archivo BMP
    uint16_t reserved1;
    uint16_t reserved2;
    uint32_t offset;           // Desplazamiento hasta los datos de los píxeles
} __attribute__((packed)) bmp_file_header_t;

// Encabezado de información de la imagen (BITMAPINFOHEADER - 40 bytes)
typedef struct {
    uint32_t size;             // Tamaño de este encabezado (40)
    int32_t  width;            // Ancho en píxeles
    int32_t  height;           // Alto en píxeles (si es positivo, la imagen está de abajo hacia arriba)
    uint16_t planes;           // Siempre 1
    uint16_t bpp;              // Bits por píxel (24 o 32)
    uint32_t compression;      // 0 = BI_RGB (sin compresión)
    uint32_t image_size;       // Tamaño de la imagen
    int32_t  x_ppm;
    int32_t  y_ppm;
    uint32_t colors_used;
    uint32_t colors_important;
} __attribute__((packed)) bmp_info_header_t;

// Renderiza un archivo BMP cargado mediante TarFS en el framebuffer o superficie
int bmp_draw(tar_node_t *file, int dest_x, int dest_y);
// Funciones extendidas para renderizar desde TarFS hacia búferes con Clipping y Alpha Blending
int bmp_draw_to_buffer(tar_node_t *file, uint32_t *dst, int dst_stride, rect_t clip, int dest_x, int dest_y);
int bmp_draw_scaled(tar_node_t *file, uint32_t *dst, int dst_stride, rect_t clip, int dest_x, int dest_y, int dest_w, int dest_h);

#endif