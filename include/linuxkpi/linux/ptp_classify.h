#ifndef _LKPI_LINUX_PTP_CLASSIFY_H
#define _LKPI_LINUX_PTP_CLASSIFY_H
#include <linux/skbuff.h>
#define PTP_CLASS_NONE 0x00
#define PTP_CLASS_V2 0x20
#define PTP_CLASS_IPV4 0x10
#define PTP_CLASS_IPV6 0x20
#define PTP_CLASS_L2 0x30
#define PTP_CLASS_VLAN 0x40
#define PTP_EV_PORT 319
#define PTP_GEN_PORT 320
static inline unsigned int ptp_classify_raw(const struct sk_buff *skb) { return PTP_CLASS_NONE; }
#endif
