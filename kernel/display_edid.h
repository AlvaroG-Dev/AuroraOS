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

#endif
