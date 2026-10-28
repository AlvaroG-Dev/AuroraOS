/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LKPI_LINUX_UTSNAME_H
#define _LKPI_LINUX_UTSNAME_H
struct new_utsname { char sysname[65], nodename[65], release[65], version[65], machine[65], domainname[65]; };
extern struct new_utsname lkpi_init_utsname;
static inline struct new_utsname *init_utsname(void) { return &lkpi_init_utsname; }
#endif
