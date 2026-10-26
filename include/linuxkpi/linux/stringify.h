/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LKPI_LINUX_STRINGIFY_H
#define _LKPI_LINUX_STRINGIFY_H

#define __stringify_1(x...) #x
#define __stringify(x...) __stringify_1(x)
#define FILE_LINE __FILE__ ":" __stringify(__LINE__)

#endif
