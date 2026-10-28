/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LKPI_LINUX_CONSOLE_H
#define _LKPI_LINUX_CONSOLE_H
static inline void console_lock(void) {}
static inline int console_trylock(void) { return 1; }
static inline void console_unlock(void) {}
#endif
