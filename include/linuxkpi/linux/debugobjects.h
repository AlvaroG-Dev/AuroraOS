/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LKPI_LINUX_DEBUGOBJECTS_H
#define _LKPI_LINUX_DEBUGOBJECTS_H
/* CONFIG_DEBUG_OBJECTS=n */
struct debug_obj_descr;
static inline void debug_object_init(void *addr, const struct debug_obj_descr *descr) {}
static inline void debug_object_activate(void *addr, const struct debug_obj_descr *descr) {}
static inline void debug_object_deactivate(void *addr, const struct debug_obj_descr *descr) {}
static inline void debug_object_destroy(void *addr, const struct debug_obj_descr *descr) {}
static inline void debug_object_free(void *addr, const struct debug_obj_descr *descr) {}
static inline void debug_object_assert_init(void *addr, const struct debug_obj_descr *descr) {}
#endif
