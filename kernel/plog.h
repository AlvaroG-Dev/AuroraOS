// kernel/plog.h
//
// Log del kernel en RAM persistente (ver bootloader/include/persist_log.h).
// El bootloader reserva la region y pasa su base en kernel_boot_info.
#pragma once
#include <stddef.h>
#include <stdint.h>

// Engancha la region persistente. Vuelca lo que klog ya tiene en su ring.
// base = 0 -> deshabilitado. Llamar cuando phys_to_virt() ya cubre la region.
void plog_attach(uint64_t base, uint64_t size);

// Espejo de cada byte que sale por serie/klog. No-op si no hay region.
void plog_putc(char c);

// Longitud y copia (cronologica) del log del arranque ANTERIOR.
size_t plog_prev_len(void);
size_t plog_prev_read(char *out, size_t max);
