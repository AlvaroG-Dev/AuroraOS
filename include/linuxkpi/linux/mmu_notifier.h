/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * mmu_notifier: sin soporte en la capa (no hay HMM ni SVM). Existe para que
 * compilen los ficheros de Linux que lo incluyen solo por anotaciones de
 * lockdep. L6 anadira los stubs que pidan ttm/amdgpu.
 */
#ifndef _LKPI_LINUX_MMU_NOTIFIER_H
#define _LKPI_LINUX_MMU_NOTIFIER_H

#include <linux/mm_types.h>

#endif
