/* bootloader/bios/splash.h */
#ifndef BIOS_SPLASH_H
#define BIOS_SPLASH_H

#include <stdint.h>

int bios_splash_init(uint32_t lfb_phys, uint32_t w, uint32_t h, uint32_t pitch,
                     uint32_t bpp);
void bios_splash_tick(void); /* throttle interno, safe to spam */
void bios_splash_done(void);

#endif