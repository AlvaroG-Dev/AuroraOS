/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Static keys sin parcheo de codigo: una variable booleana. Suficiente para
 * los usos de los drivers (activar una rama una vez, p. ej. en drm_cache).
 */
#ifndef _LKPI_LINUX_JUMP_LABEL_H
#define _LKPI_LINUX_JUMP_LABEL_H

#include <linux/types.h>
#include <linux/atomic.h>

struct static_key {
	atomic_t enabled;
};

struct static_key_true {
	struct static_key key;
};

struct static_key_false {
	struct static_key key;
};

#define STATIC_KEY_INIT_TRUE	{ .enabled = ATOMIC_INIT(1) }
#define STATIC_KEY_INIT_FALSE	{ .enabled = ATOMIC_INIT(0) }
#define DEFINE_STATIC_KEY_TRUE(name)	struct static_key_true name = { .key = STATIC_KEY_INIT_TRUE }
#define DEFINE_STATIC_KEY_FALSE(name)	struct static_key_false name = { .key = STATIC_KEY_INIT_FALSE }

#define static_key_enabled(x)	(atomic_read(&(x)->enabled) > 0)
#define static_branch_likely(x)		(atomic_read(&(x)->key.enabled) > 0)
#define static_branch_unlikely(x)	(atomic_read(&(x)->key.enabled) > 0)
#define static_branch_enable(x)		atomic_set(&(x)->key.enabled, 1)
#define static_branch_disable(x)	atomic_set(&(x)->key.enabled, 0)
#define static_branch_inc(x)		atomic_inc(&(x)->key.enabled)
#define static_branch_dec(x)		atomic_dec(&(x)->key.enabled)

#endif
