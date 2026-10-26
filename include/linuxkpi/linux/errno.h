/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LKPI_LINUX_ERRNO_H
#define _LKPI_LINUX_ERRNO_H

#include <uapi/linux/errno.h>

/* Codigos internos del kernel: nunca llegan al espacio de usuario. */
#define ERESTARTSYS 512
#define ERESTARTNOINTR 513
#define ERESTARTNOHAND 514
#define ENOIOCTLCMD 515
#define ERESTART_RESTARTBLOCK 516
#define EPROBE_DEFER 517
#define EOPENSTALE 518
#define ENOPARAM 519
#define EBADHANDLE 521
#define ENOTSYNC 522
#define EBADCOOKIE 523
#define ENOTSUPP 524
#define ETOOSMALL 525
#define ESERVERFAULT 526
#define EBADTYPE 527
#define EJUKEBOX 528
#define EIOCBQUEUED 529
#define ERECALLCONFLICT 530
#define ENOGRACE 531

#endif
