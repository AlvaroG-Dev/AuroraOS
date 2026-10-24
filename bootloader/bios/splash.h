/* bootloader/bios/splash.h */
#ifndef BIOS_SPLASH_H
#define BIOS_SPLASH_H
#include <stdint.h>

int bios_splash_init(uint32_t lfb_phys, uint32_t w, uint32_t h, uint32_t pitch,
                     uint32_t bpp);
void bios_splash_tick(
    void); /* con throttle interno: se puede llamar sin miedo */
void bios_splash_wait(uint32_t min_ms);
uint32_t bios_splash_elapsed_ms(void);
void bios_splash_get_state(uint32_t *phase, uint32_t *elapsed_ms);
void bios_splash_done(void);
#endif