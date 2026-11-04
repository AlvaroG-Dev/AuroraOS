// Lectura por CPU de dma-bufs que viven en la VRAM (NVIDIA, modificador no lineal):
// el compositor los importa por EGL (EGL_EXT_image_dma_buf_import) y los lee con
// glReadPixels. Todo por dlopen: sin libEGL/libgbm/GLES2 el compositor sigue igual.
#ifndef GPUIMP_H
#define GPUIMP_H
#include <stdint.h>
// Devuelve un buffer BGRA (XRGB/ARGB8888 de DRM) de w*h*4 bytes, valido hasta la
// siguiente llamada, o NULL si no se pudo (motivo en stderr).
// *hp: asa opaca por wl_buffer (NULL inicial) donde se cachea el import; liberar con gpuimp_release.
const uint8_t *gpuimp_read(void **hp, int fd, int w, int h, uint32_t stride, uint32_t offset,
                           uint32_t fourcc, uint64_t modifier);
void gpuimp_release(void *hp);
int gpuimp_query_modifiers(uint32_t fourcc, uint64_t *mods, int max);
#endif
