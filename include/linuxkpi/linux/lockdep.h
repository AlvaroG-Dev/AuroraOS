/* SPDX-License-Identifier: GPL-2.0-only */
/* lockdep desactivado: todo es no-op, como en un kernel sin CONFIG_LOCKDEP. */
#ifndef _LKPI_LINUX_LOCKDEP_H
#define _LKPI_LINUX_LOCKDEP_H

#include <linux/lockdep_types.h>

#define lockdep_init_map(map, name, key, sub) do { (void)(map); (void)(key); } while (0)
#define lockdep_init_map_type(map, name, key, sub, ...) do { (void)(map); } while (0)
#define lockdep_set_class(lock, key) do { (void)(lock); (void)(key); } while (0)
#define lockdep_set_class_and_name(lock, key, name) do { (void)(lock); (void)(key); } while (0)
#define lockdep_set_class_and_subclass(lock, key, sub) do { (void)(lock); (void)(key); } while (0)
#define lockdep_set_subclass(lock, sub) do { (void)(lock); } while (0)
#define lockdep_set_novalidate_class(lock) do { (void)(lock); } while (0)
#define lockdep_set_notrack_class(lock) do { (void)(lock); } while (0)
#define lockdep_register_key(key) do { (void)(key); } while (0)
#define lockdep_unregister_key(key) do { (void)(key); } while (0)
#define lockdep_match_class(lock, key) 1

#define lockdep_is_held(lock) ((void)(lock), 1)
#define lockdep_is_held_type(lock, r) ((void)(lock), 1)
#define lock_is_held(map) 1
#define lockdep_assert(cond) do { } while (0)
#define lockdep_assert_once(cond) do { } while (0)
#define lockdep_assert_held(l) do { (void)(l); } while (0)
#define lockdep_assert_held_once(l) do { (void)(l); } while (0)
#define lockdep_assert_held_write(l) do { (void)(l); } while (0)
#define lockdep_assert_held_read(l) do { (void)(l); } while (0)
#define lockdep_assert_not_held(l) do { (void)(l); } while (0)
#define lockdep_assert_none_held_once() do { } while (0)
#define lockdep_assert_in_softirq() do { } while (0)
#define lockdep_assert_preemption_disabled() do { } while (0)
#define lockdep_assert_preemption_enabled() do { } while (0)
#define lockdep_off() do { } while (0)
#define lockdep_on() do { } while (0)
#define debug_check_no_locks_freed(from, len) do { } while (0)
#define debug_check_no_locks_held() do { } while (0)

struct pin_cookie { };
#define NIL_COOKIE ((struct pin_cookie){ })
#define lockdep_pin_lock(l) ({ (void)(l); NIL_COOKIE; })
#define lockdep_repin_lock(l, c) do { (void)(l); (void)(c); } while (0)
#define lockdep_unpin_lock(l, c) do { (void)(l); (void)(c); } while (0)

#define lock_acquire(...) do { } while (0)
#define lock_release(...) do { } while (0)
#define lock_acquire_exclusive(...) do { } while (0)
#define lock_acquire_shared(...) do { } while (0)
#define lock_map_acquire(l) do { (void)(l); } while (0)
#define lock_map_acquire_read(l) do { (void)(l); } while (0)
#define lock_map_acquire_try(l) do { (void)(l); } while (0)
#define lock_map_release(l) do { (void)(l); } while (0)
#define might_lock(l) do { (void)(l); } while (0)
#define might_lock_read(l) do { (void)(l); } while (0)
#define might_lock_nested(l, sub) do { (void)(l); } while (0)
#define rwsem_acquire(...) do { } while (0)
#define rwsem_acquire_read(...) do { } while (0)
#define rwsem_release(...) do { } while (0)
#define mutex_acquire(...) do { } while (0)
#define mutex_release(...) do { } while (0)

#define SINGLE_DEPTH_NESTING 1
#define DEFINE_WAIT_OVERRIDE_MAP(name, type) struct lockdep_map name = { }

#endif
