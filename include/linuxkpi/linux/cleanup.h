/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Limpieza automatica por ambito (__attribute__((cleanup))): __free(),
 * clases (CLASS/DEFINE_CLASS) y guardas de cerrojos (guard(),
 * scoped_guard(), scoped_cond_guard()). Implementacion propia de la API de
 * include/linux/cleanup.h.
 */
#ifndef _LKPI_LINUX_CLEANUP_H
#define _LKPI_LINUX_CLEANUP_H

#include <linux/compiler.h>

/* ---------------------------------------------------- __free(nombre) */

#define DEFINE_FREE(_name, _type, _free) \
	static __always_inline void __free_##_name(void *p) \
	{ _type _T = *(_type *)p; _free; }
#define __free(_name) __cleanup(__free_##_name)

#define __lkpi_take_ptr(p) \
	({ __auto_type __pp = &(p); __auto_type __pv = *__pp; *__pp = NULL; __pv; })
#define no_free_ptr(p) ((typeof(p))__lkpi_take_ptr(p))
#define return_ptr(p) return no_free_ptr(p)
#define retain_and_null_ptr(p) ((void)__lkpi_take_ptr(p))

/* ------------------------------------------------------------ clases */

#define DEFINE_CLASS(_name, _type, _exit, _init, _init_args...) \
	typedef _type class_##_name##_t; \
	static __always_inline void class_##_name##_destructor(_type *p) \
	{ _type _T = *p; _exit; } \
	static __always_inline _type class_##_name##_constructor(_init_args) \
	{ _type __lkpi_t = _init; return __lkpi_t; }

#define EXTEND_CLASS(_name, ext, _init, _init_args...) \
	typedef class_##_name##_t class_##_name##ext##_t; \
	static __always_inline void class_##_name##ext##_destructor(class_##_name##_t *p) \
	{ class_##_name##_destructor(p); } \
	static __always_inline class_##_name##_t class_##_name##ext##_constructor(_init_args) \
	{ class_##_name##_t __lkpi_t = _init; return __lkpi_t; }

#define CLASS(_name, var) \
	class_##_name##_t var __cleanup(class_##_name##_destructor) = \
		class_##_name##_constructor

/* --------------------------------------------------- guardas simples */

/* El tipo es el puntero al cerrojo; NULL = no tomado. */
#define DEFINE_GUARD(_name, _type, _lock, _unlock) \
	DEFINE_CLASS(_name, _type, if (_T) { _unlock; }, ({ _lock; _T; }), _type _T); \
	static __always_inline void *class_##_name##_lock_ptr(class_##_name##_t *_T) \
	{ return (void *)(unsigned long)*_T; }

/* Variante condicional: _condlock es verdadero si se obtuvo el cerrojo. */
#define DEFINE_GUARD_COND(_name, _ext, _condlock) \
	EXTEND_CLASS(_name, _ext, \
		     ({ void *__lkpi_l = _T; if (_T && !(_condlock)) __lkpi_l = NULL; __lkpi_l; }), \
		     class_##_name##_t _T); \
	static __always_inline void *class_##_name##_ext##_lock_ptr(class_##_name##_t *_T) \
	{ return class_##_name##_lock_ptr(_T); }

#define guard(_name) CLASS(_name, __UNIQUE_ID(guard))

#define scoped_guard(_name, args...) \
	for (CLASS(_name, scope)(args), *__lkpi_done = NULL; \
	     class_##_name##_lock_ptr(&scope) && !__lkpi_done; \
	     __lkpi_done = (void *)1)

#define scoped_cond_guard(_name, _fail, args...) \
	for (CLASS(_name, scope)(args), *__lkpi_done = NULL; !__lkpi_done; \
	     __lkpi_done = (void *)1) \
		if (!class_##_name##_lock_ptr(&scope)) { _fail; } else

/* ------------------------------- guardas con estado (p. ej. flags) */

#define __DEFINE_LOCK_GUARD_TYPE(_name, _type, ...) \
	typedef struct { _type *lock; __VA_ARGS__; } class_##_name##_t; \
	static __always_inline void *class_##_name##_lock_ptr(class_##_name##_t *_T) \
	{ return (void *)_T->lock; }

#define __DEFINE_LOCK_GUARD_DTOR(_name, _unlock) \
	static __always_inline void class_##_name##_destructor(class_##_name##_t *_T) \
	{ if (_T->lock) { _unlock; } }

#define DEFINE_LOCK_GUARD_1(_name, _type, _lock, _unlock, ...) \
	__DEFINE_LOCK_GUARD_TYPE(_name, _type, __VA_ARGS__) \
	__DEFINE_LOCK_GUARD_DTOR(_name, _unlock) \
	static __always_inline class_##_name##_t class_##_name##_constructor(_type *l) \
	{ class_##_name##_t __lkpi_t = { .lock = l }, *_T = &__lkpi_t; _lock; return __lkpi_t; }

#define DEFINE_LOCK_GUARD_0(_name, _lock, _unlock, ...) \
	__DEFINE_LOCK_GUARD_TYPE(_name, void, __VA_ARGS__) \
	__DEFINE_LOCK_GUARD_DTOR(_name, _unlock) \
	static __always_inline class_##_name##_t class_##_name##_constructor(void) \
	{ class_##_name##_t __lkpi_t = { .lock = (void *)1 }, *_T __maybe_unused = &__lkpi_t; \
	  _lock; return __lkpi_t; }

#define DEFINE_LOCK_GUARD_1_COND(_name, _ext, _condlock) \
	typedef class_##_name##_t class_##_name##_ext##_t; \
	static __always_inline void class_##_name##_ext##_destructor(class_##_name##_t *p) \
	{ class_##_name##_destructor(p); } \
	static __always_inline void *class_##_name##_ext##_lock_ptr(class_##_name##_t *_T) \
	{ return class_##_name##_lock_ptr(_T); } \
	static __always_inline class_##_name##_t class_##_name##_ext##_constructor(typeof(*((class_##_name##_t *)0)->lock) *l) \
	{ class_##_name##_t __lkpi_t = { .lock = l }, *_T = &__lkpi_t; \
	  _T->lock = (_condlock) ? _T->lock : NULL; return __lkpi_t; }

#endif
