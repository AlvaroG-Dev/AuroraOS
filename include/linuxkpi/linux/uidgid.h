/* SPDX-License-Identifier: GPL-2.0-only */
/* kuid_t/kgid_t: sin espacios de nombres de usuario; root es 0. */
#ifndef _LKPI_LINUX_UIDGID_H
#define _LKPI_LINUX_UIDGID_H

#include <linux/types.h>

typedef struct { u32 val; } kuid_t;
typedef struct { u32 val; } kgid_t;

#define KUIDT_INIT(v) ((kuid_t){ v })
#define KGIDT_INIT(v) ((kgid_t){ v })
#define GLOBAL_ROOT_UID KUIDT_INIT(0)
#define GLOBAL_ROOT_GID KGIDT_INIT(0)
static inline u32 __kuid_val(kuid_t uid) { return uid.val; }
static inline u32 __kgid_val(kgid_t gid) { return gid.val; }
static inline bool uid_eq(kuid_t a, kuid_t b) { return a.val == b.val; }
static inline bool gid_eq(kgid_t a, kgid_t b) { return a.val == b.val; }

#endif
