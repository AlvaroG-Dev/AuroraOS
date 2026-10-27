/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LKPI_LINUX_CRED_H
#define _LKPI_LINUX_CRED_H

#include <linux/uidgid.h>
#include <linux/sched.h>
#include <linux/capability.h>

#define current_uid() KUIDT_INIT(current->__lkpi_uid)
#define current_euid() KUIDT_INIT(current->__lkpi_euid)
#define current_fsuid() current_euid()

#endif
