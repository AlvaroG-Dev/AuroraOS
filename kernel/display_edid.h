// kernel/display_edid.h
//
// EDID del monitor entregado por el firmware (EFI_EDID_ACTIVE_PROTOCOL) y su
// analisis basico: nombre, tamano fisico y modo preferido. Lo usa DRM para la
// propiedad EDID y el tamano fisico del conector.

#ifndef KERNEL_DISPLAY_EDID_H
#define KERNEL_DISPLAY_EDID_H

#include <stdbool.h>
#include <stdint.h>

struct edid_info {
  bool valid;
  char name[14];          // descriptor 0xFC ("DELL U2412M"), o vacio
  char vendor[4];         // PNP ID de 3 letras ("DEL")
  uint16_t product;
  uint32_t width_mm, height_mm; // tamano fisico de la imagen (0 = desconocido)
  uint32_t pref_w, pref_h;      // modo preferido (primer descriptor), 0 = ninguno
  uint32_t pref_khz;            // reloj de pixel del modo preferido en kHz
  uint32_t pref_mhz_x100;       // frecuencia de refresco x100 (6000 = 60 Hz) o 0
  uint32_t blocks;              // 1 (base) o 2 (con extension)
};

// Valida cabecera y checksums y rellena `out`. false si no es un EDID valido.
bool edid_parse(const uint8_t *edid, uint32_t len, struct edid_info *out);

// Guarda el EDID del arranque (copiado de kernel_boot_info). Se llama una vez.
void display_edid_init(const uint8_t *edid, uint32_t len);

// EDID del arranque; NULL si el firmware no dio ninguno o no es valido.
const uint8_t *display_edid_data(uint32_t *len);
const struct edid_info *display_edid_info(void);

// Un modo de video descrito por el EDID.
struct edid_mode {
  uint16_t hdisplay, hsync_start, hsync_end, htotal;
  uint16_t vdisplay, vsync_start, vsync_end, vtotal;
  uint32_t clock_khz;     // 0 = sin temporizacion exacta (modo estandar/establecido)
  uint32_t refresh_hz;    // redondeado
  uint32_t flags;         // bit0 interlaced, bit1 hsync+, bit2 vsync+
  bool detailed;          // viene de un descriptor de temporizacion detallada
};
#define EDID_MODE_INTERLACED 1u
#define EDID_MODE_PHSYNC 2u
#define EDID_MODE_PVSYNC 4u

// Extrae los modos del EDID (descriptores detallados, temporizaciones
// establecidas y estandar del bloque base). Sin duplicados (w, h, refresh).
// Devuelve cuantos escribio en `out` (max `max`).
int edid_modes(const uint8_t *edid, uint32_t len, struct edid_mode *out, int max);

#endif
