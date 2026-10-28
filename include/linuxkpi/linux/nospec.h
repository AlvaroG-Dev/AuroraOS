/* SPDX-License-Identifier: GPL-2.0-only */
/* array_index_nospec: limita el indice sin ramas (mitigacion de Spectre v1). */
#ifndef _LKPI_LINUX_NOSPEC_H
#define _LKPI_LINUX_NOSPEC_H

#define array_index_mask_nospec(index, size) \
	((unsigned long)(-(long)((unsigned long)(index) < (unsigned long)(size))))

#define array_index_nospec(index, size)					\
({									\
	typeof(index) _i = (index);					\
	typeof(size) _s = (size);					\
	unsigned long _mask = array_index_mask_nospec(_i, _s);		\
	(typeof(_i))(_i & _mask);					\
})

#endif
