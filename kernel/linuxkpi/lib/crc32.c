// SPDX-License-Identifier: GPL-2.0-only
/* CRC-32 (IEEE 802.3, reflejado y normal) y CRC-32C con tablas de 256 entradas. */
#include <linux/kernel.h>
#include <linux/export.h>
#include <linux/crc32.h>

static u32 tab_le[256], tab_be[256], tab_c[256];
static bool tabs_ready;

static void crc_init(void)
{
	for (u32 i = 0; i < 256; i++) {
		u32 le = i, c = i, be = i << 24;
		for (int k = 0; k < 8; k++) {
			le = (le >> 1) ^ ((le & 1) ? 0xEDB88320u : 0);
			c = (c >> 1) ^ ((c & 1) ? 0x82F63B78u : 0);
			be = (be << 1) ^ ((be & 0x80000000u) ? 0x04C11DB7u : 0);
		}
		tab_le[i] = le;
		tab_c[i] = c;
		tab_be[i] = be;
	}
	smp_wmb();
	WRITE_ONCE(tabs_ready, true);
}

/* Sin el ~ inicial/final: como crc32_le() de Linux. */
u32 lkpi_crc32_le(u32 crc, const void *p, size_t len)
{
	const u8 *b = p;
	if (!READ_ONCE(tabs_ready))
		crc_init();
	while (len--)
		crc = (crc >> 8) ^ tab_le[(crc ^ *b++) & 0xff];
	return crc;
}
EXPORT_SYMBOL(lkpi_crc32_le);

u32 lkpi_crc32_be(u32 crc, const void *p, size_t len)
{
	const u8 *b = p;
	if (!READ_ONCE(tabs_ready))
		crc_init();
	while (len--)
		crc = (crc << 8) ^ tab_be[((crc >> 24) ^ *b++) & 0xff];
	return crc;
}
EXPORT_SYMBOL(lkpi_crc32_be);

u32 lkpi_crc32c(u32 crc, const void *p, size_t len)
{
	const u8 *b = p;
	if (!READ_ONCE(tabs_ready))
		crc_init();
	while (len--)
		crc = (crc >> 8) ^ tab_c[(crc ^ *b++) & 0xff];
	return crc;
}
EXPORT_SYMBOL(lkpi_crc32c);
