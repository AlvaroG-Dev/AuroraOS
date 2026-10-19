/* bootloader/efi/splash.h */
#ifndef EFI_SPLASH_H
#define EFI_SPLASH_H

#include "efi_min.h"

int efi_splash_init(EFI_PHYSICAL_ADDRESS lfb_phys, UINT32 w, UINT32 h,
                    UINT32 pitch, UINT32 bpp);
void efi_splash_tick(void);
void efi_splash_get_state(UINT32 *phase, UINT32 *elapsed_ms);
void efi_splash_done(void);

#endif