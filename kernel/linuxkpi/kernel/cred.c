// SPDX-License-Identifier: GPL-2.0-only
/* Credenciales y espacio de nombres de usuario: uno solo, root (ver linux/cred.h). */
#include <linux/export.h>
#include <linux/cred.h>
#include <linux/pid_namespace.h>

struct user_namespace {
	int level;
};

const struct cred lkpi_root_cred;
EXPORT_SYMBOL(lkpi_root_cred);

struct user_namespace init_user_ns;
EXPORT_SYMBOL(init_user_ns);
struct pid_namespace init_pid_ns;
EXPORT_SYMBOL(init_pid_ns);
