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

#define overflowuid 65534
#define overflowgid 65534
struct user_namespace;
extern struct user_namespace init_user_ns;
static inline u32 from_kuid(struct user_namespace *ns, kuid_t uid) { return uid.val; }
static inline u32 from_kuid_munged(struct user_namespace *ns, kuid_t uid) { return uid.val; }
static inline u32 from_kgid(struct user_namespace *ns, kgid_t gid) { return gid.val; }
static inline u32 from_kgid_munged(struct user_namespace *ns, kgid_t gid) { return gid.val; }
static inline kuid_t make_kuid(struct user_namespace *ns, u32 uid) { return KUIDT_INIT(uid); }
static inline kgid_t make_kgid(struct user_namespace *ns, u32 gid) { return KGIDT_INIT(gid); }
static inline bool uid_valid(kuid_t uid) { return uid.val != (u32)-1; }
static inline bool gid_valid(kgid_t gid) { return gid.val != (u32)-1; }

#endif
