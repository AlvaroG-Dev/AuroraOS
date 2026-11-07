#ifndef _LKPI_LINUX_IN_H
#define _LKPI_LINUX_IN_H
#include <linux/types.h>
#include <linux/socket.h>
#define IPPROTO_IP 0
#define IPPROTO_ICMP 1
#define IPPROTO_TCP 6
#define IPPROTO_UDP 17
#define IPPROTO_IPV6 41
#define IPPROTO_ICMPV6 58
#define IPPROTO_SCTP 132
#define IPPROTO_RAW 255
struct in_addr { __be32 s_addr; };
struct sockaddr_in { unsigned short sin_family; __be16 sin_port; struct in_addr sin_addr; unsigned char pad[8]; };
#endif
