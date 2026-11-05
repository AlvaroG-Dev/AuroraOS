#ifndef _LKPI_LINUX_SOCKET_H
#define _LKPI_LINUX_SOCKET_H
#include <linux/types.h>
typedef int __kernel_clockid_t;
struct sockaddr { unsigned short sa_family; char sa_data[14]; };
#endif
