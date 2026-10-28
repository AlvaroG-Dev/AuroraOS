/* SPDX-License-Identifier: GPL-2.0-only */
/* Inyeccion de fallos desactivada. */
#ifndef _LKPI_LINUX_FAULT_INJECT_H
#define _LKPI_LINUX_FAULT_INJECT_H

struct fault_attr { int dummy; };
#define DECLARE_FAULT_ATTR(name) struct fault_attr name = { 0 }
#define should_fail(attr, size) ({ (void)(attr); (void)(size); 0; })

#endif
