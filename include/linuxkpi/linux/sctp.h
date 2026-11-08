#ifndef _LKPI_SCTP_H
#define _LKPI_SCTP_H
#include <linux/types.h>
struct sctphdr { __be16 source; __be16 dest; __be32 vtag; __le32 checksum; };
#endif
