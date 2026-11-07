/* net/genetlink.h del arbol de Linux; sin alias de modulo por familia. */
#ifndef _LKPI_NET_GENETLINK_WRAP_H
#define _LKPI_NET_GENETLINK_WRAP_H
#include <kreal/genetlink.h>
#undef MODULE_ALIAS_GENL_FAMILY
#define MODULE_ALIAS_GENL_FAMILY(family)
#endif
