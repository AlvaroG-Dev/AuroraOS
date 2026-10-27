/* SPDX-License-Identifier: GPL-2.0-only */
/* typecheck(): error de compilacion si una variable no es del tipo dado. */
#ifndef _LKPI_LINUX_TYPECHECK_H
#define _LKPI_LINUX_TYPECHECK_H

#define typecheck(type, x) \
({	type __dummy; \
	typeof(x) __dummy2; \
	(void)(&__dummy == &__dummy2); \
	1; \
})

#define typecheck_fn(type, function) \
({	typeof(type) __tmp = function; \
	(void)__tmp; \
})

#endif
