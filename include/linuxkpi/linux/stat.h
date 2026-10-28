/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LKPI_LINUX_STAT_H
#define _LKPI_LINUX_STAT_H

#define S_IRWXU 00700
#define S_IRUSR 00400
#define S_IWUSR 00200
#define S_IXUSR 00100
#define S_IRWXG 00070
#define S_IRGRP 00040
#define S_IWGRP 00020
#define S_IXGRP 00010
#define S_IRWXO 00007
#define S_IROTH 00004
#define S_IWOTH 00002
#define S_IXOTH 00001
#define S_IRUGO (S_IRUSR | S_IRGRP | S_IROTH)
#define S_IWUGO (S_IWUSR | S_IWGRP | S_IWOTH)
#define S_IXUGO (S_IXUSR | S_IXGRP | S_IXOTH)

#endif
