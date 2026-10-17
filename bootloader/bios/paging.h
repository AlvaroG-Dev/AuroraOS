/* bootloader/bios/paging.h */
#ifndef AURORA_BIOS_PAGING_H
#define AURORA_BIOS_PAGING_H
#include "bios.h"

uint32_t paging_build_identity_pml4(uint64_t max_phys);

#endif