/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LKPI_LINUX_STDDEF_H
#define _LKPI_LINUX_STDDEF_H

#include <linux/compiler_types.h>

#undef NULL
#define NULL ((void *)0)

enum { false = 0, true = 1 };

#undef offsetof
#define offsetof(TYPE, MEMBER) __builtin_offsetof(TYPE, MEMBER)
#define sizeof_field(TYPE, MEMBER) sizeof((((TYPE *)0)->MEMBER))
#define offsetofend(TYPE, MEMBER) \
	(offsetof(TYPE, MEMBER) + sizeof_field(TYPE, MEMBER))

/* Agrupa miembros que tambien se pueden direccionar como una struct. */
#define __struct_group(TAG, NAME, ATTRS, MEMBERS...) \
	union { \
		struct { MEMBERS } ATTRS; \
		struct TAG { MEMBERS } ATTRS NAME; \
	} ATTRS
#define struct_group(NAME, MEMBERS...) __struct_group(/* sin tag */, NAME, /* sin attrs */, MEMBERS)
#define struct_group_attr(NAME, ATTRS, MEMBERS...) __struct_group(, NAME, ATTRS, MEMBERS)
#define struct_group_tagged(TAG, NAME, MEMBERS...) __struct_group(TAG, NAME, /* sin attrs */, MEMBERS)

#define DECLARE_FLEX_ARRAY(TYPE, NAME) \
	struct { struct { } __empty_##NAME; TYPE NAME[]; }

#endif
