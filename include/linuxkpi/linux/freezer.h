/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_FREEZER_H
#define _LINUX_FREEZER_H
/* Aurora no suspende tareas del kernel: nada que congelar. */
static inline int try_to_freeze(void) { return 0; }
static inline bool freezing(struct task_struct *p) { return false; }
static inline void set_freezable(void) {}
#endif
