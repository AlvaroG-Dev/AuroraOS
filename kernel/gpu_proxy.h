// kernel/gpu_proxy.h
#ifndef KERNEL_GPU_PROXY_H
#define KERNEL_GPU_PROXY_H

#include "aur.h"
#include "vfs.h"
#include <stdint.h>

// VFS ops de /dev/gpuproxy. Se registran en vfs.c (devfs).
extern vfs_ops_t gpup_dev_ops;

void gpup_init(void);

// El kernel publica donde esta el framebuffer del firmware (main.c).
void gpup_set_framebuffer(uint64_t phys, uint64_t size, uint32_t w, uint32_t h,
                          uint32_t pitch, uint32_t bpp);

// Dónde está el framebuffer del firmware (0 si no hay): la capa LinuxKPI lo usa
// para no dejar que un driver de Linux pise la pantalla del compositor.
int gpup_get_framebuffer(uint64_t *phys, uint64_t *size);
// Su modo: ancho, alto, bytes por fila y bits por pixel.
int gpup_get_fb_mode(uint32_t *w, uint32_t *h, uint32_t *pitch, uint32_t *bpp);

// El compositor ha volcado el rectangulo (x, y, w, h) al framebuffer. Si hay un
// driver GPU registrado con AUR_GPU_NEEDS_PRESENT se acumula; si no, no hace
// nada. Seguro desde cualquier contexto (no bloquea).
void gpup_notify_dirty(int x, int y, int w, int h);

// Pantalla "tardia": si el firmware no dejo framebuffer, el kernel espera a
// que un driver GPU (.aur) pida uno con GPUP_ALLOC_FB. Devuelve 1 si hay
// pantalla al terminar la espera (timeout en ms).
int gpup_wait_display(uint32_t timeout_ms);

// Hay algun controlador de pantalla PCI (clase 0x03) que un driver .aur podria
// manejar.
int gpup_display_expected(void);

// Adaptador registrado (NULL si no hay).
const struct aur_gpu_reg *gpup_adapter(void);

#endif
