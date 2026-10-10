// kernel/net/ping.h
//
// Ping kernel-side: kthread que envía un ICMP echo request a 10.0.2.2
// (gateway de QEMU user-mode) cada 2 s y loguea el RTT. Sirve para
// validar end-to-end que el driver e1000e + IP + ARP + ICMP funcionan.

#ifndef KERNEL_NET_PING_H
#define KERNEL_NET_PING_H

void ping_init(void);

#endif