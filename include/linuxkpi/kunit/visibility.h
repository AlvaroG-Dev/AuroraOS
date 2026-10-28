/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LKPI_KUNIT_VISIBILITY_H
#define _LKPI_KUNIT_VISIBILITY_H

#define VISIBLE_IF_KUNIT static
#define EXPORT_SYMBOL_IF_KUNIT(symbol)
#define EXPORT_SYMBOL_FOR_KUNIT_TESTING(symbol)

#endif
