#ifndef _LKPI_LINUX_SOCKET_H
#define _LKPI_LINUX_SOCKET_H
#include <linux/types.h>
typedef int __kernel_clockid_t;
struct sockaddr { unsigned short sa_family; char sa_data[14]; };
#define SIOCGMIIPHY 0x8947
#define SIOCGMIIREG 0x8948
#define SIOCSMIIREG 0x8949
#define SIOCSHWTSTAMP 0x89b0
#define SIOCGHWTSTAMP 0x89b1
#ifndef MSG_DONTWAIT
#define MSG_DONTWAIT 0x40
#endif
#endif
