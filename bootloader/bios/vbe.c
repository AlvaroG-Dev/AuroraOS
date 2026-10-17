/* bootloader/bios/vbe.c
 *
 * stage2.asm ya ha dejado el modo VBE activo y ha escrito en 0x21000
 * los datos basicos. Aqui solo leemos y validamos. Si el bootloader no
 * encuentra un modo valido, se devuelve 0 y el kernel arrancara sin FB.
 */
#include "bios.h"

void vbe_get(vbe_desc_t *out) {
  const vbe_desc_t *v = (const vbe_desc_t *)VBE_INFO_ADDR;
  *out = *v;
  /* El modo VBE 0x118 de SeaBIOS es RGB de 24 bits, no 32 bits. */
  if (!v->ok || v->lfb_phys == 0 || v->width == 0 || v->height == 0 ||
      v->pitch == 0 || (v->bpp != 24 && v->bpp != 32)) {
    out->ok = 0;
  }
}