/* SPDX-License-Identifier: GPL-2.0-only */
/* Flags de open/fcntl de Linux x86-64 (los mismos que usa Aurora). */
#ifndef _LKPI_UAPI_ASM_GENERIC_FCNTL_H
#define _LKPI_UAPI_ASM_GENERIC_FCNTL_H

#define O_ACCMODE 00000003
#define O_RDONLY 00000000
#define O_WRONLY 00000001
#define O_RDWR 00000002
#define O_CREAT 00000100
#define O_EXCL 00000200
#define O_NOCTTY 00000400
#define O_TRUNC 00001000
#define O_APPEND 00002000
#define O_NONBLOCK 00004000
#define O_DSYNC 00010000
#define FASYNC 00020000
#define O_DIRECT 00040000
#define O_LARGEFILE 00100000
#define O_DIRECTORY 00200000
#define O_NOFOLLOW 00400000
#define O_NOATIME 01000000
#define O_CLOEXEC 02000000
#define O_NDELAY O_NONBLOCK

#define F_DUPFD 0
#define F_GETFD 1
#define F_SETFD 2
#define F_GETFL 3
#define F_SETFL 4
#define F_SETOWN 8
#define F_GETOWN 9
#define FD_CLOEXEC 1

#endif
