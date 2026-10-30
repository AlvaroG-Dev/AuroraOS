/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LKPI_LINUX_CIRC_BUF_H
#define _LKPI_LINUX_CIRC_BUF_H

struct circ_buf {
	char *buf;
	int head;
	int tail;
};

/* size debe ser potencia de dos */
#define CIRC_CNT(head, tail, size) (((head) - (tail)) & ((size) - 1))
#define CIRC_SPACE(head, tail, size) CIRC_CNT((tail), ((head) + 1), (size))
#define CIRC_CNT_TO_END(head, tail, size) \
	({ int end = (size) - (tail); int n = ((head) + end) & ((size) - 1); n < end ? n : end; })
#define CIRC_SPACE_TO_END(head, tail, size) \
	({ int end = (size) - 1 - (head); int n = (end + (tail)) & ((size) - 1); \
	   n <= end ? n : end + 1; })

#endif
