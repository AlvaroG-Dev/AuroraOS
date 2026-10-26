/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LKPI_LINUX_LOCKDEP_TYPES_H
#define _LKPI_LINUX_LOCKDEP_TYPES_H

/* Sin lockdep: tipos vacios para que compilen las anotaciones. */
struct lock_class_key { };
struct lockdep_map { };
#define STATIC_LOCKDEP_MAP_INIT(name, key) { }
#define LOCK_STATE_UNKNOWN -1
#define LD_WAIT_SLEEP 0
#define LD_WAIT_SPIN 0
#define LD_WAIT_CONFIG 0

#endif
