// kernel/simd.h
#ifndef KERNEL_SIMD_H
#define KERNEL_SIMD_H

#include <stddef.h>
#include <stdint.h>

/* Blend src row sobre dst row, píxeles BGRA (uint32_t).
 * Asume dst=backbuffer (normalmente opaco); si dst tiene alpha mixto
 * cae a escalar automáticamente. */
void simd_blit_row(uint32_t *dst, const uint32_t *src, int n);

/* Copia un row a VRAM. Usa non-temporal stores cuando están disponibles
 * (evita contaminar la caché con el framebuffer). */
void simd_copy_to_vram(uint32_t *dst, const uint32_t *src, size_t n);

/* Copia un row a memoria normal (backbuffer → backbuffer, etc.). */
void simd_copy_normal(uint32_t *dst, const uint32_t *src, size_t n);

/* Rellena un row con un color sólido. */
void simd_fill_row(uint32_t *dst, uint32_t color, size_t n);

/* Inicializa el dispatch. Llamar DESPUÉS de cpu_simd_init(). */
void simd_init(void);

#endif