/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LKPI_LINUX_CRED_H
#define _LKPI_LINUX_CRED_H

#include <linux/uidgid.h>
#include <linux/sched.h>
#include <linux/capability.h>

struct cred {
	kuid_t uid, euid, suid, fsuid;
	kgid_t gid, egid, sgid, fsgid;
};

/* Aurora no guarda las credenciales completas de otros procesos: root para todos. */
extern const struct cred lkpi_root_cred;
#define __task_cred(task) (&lkpi_root_cred)
#define current_cred() (&lkpi_root_cred)

#define current_uid() KUIDT_INIT(current->__lkpi_uid)
#define current_euid() KUIDT_INIT(current->__lkpi_euid)
#define current_fsuid() current_euid()

#endif
