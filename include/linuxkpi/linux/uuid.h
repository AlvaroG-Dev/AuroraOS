/* SPDX-License-Identifier: GPL-2.0-only */
/* UUID de 16 bytes (GUID little endian y UUID big endian) y utilidades basicas. */
#ifndef _LKPI_LINUX_UUID_H
#define _LKPI_LINUX_UUID_H

#include <linux/types.h>
#include <linux/string.h>

#define UUID_SIZE 16

typedef struct {
	__u8 b[UUID_SIZE];
} guid_t;

typedef struct {
	__u8 b[UUID_SIZE];
} uuid_t;

#define UUID_INIT(a, b, c, d0, d1, d2, d3, d4, d5, d6, d7)			\
((uuid_t)									\
{{ ((a) >> 24) & 0xff, ((a) >> 16) & 0xff, ((a) >> 8) & 0xff, (a) & 0xff,	\
   ((b) >> 8) & 0xff, (b) & 0xff,						\
   ((c) >> 8) & 0xff, (c) & 0xff,						\
   (d0), (d1), (d2), (d3), (d4), (d5), (d6), (d7) }})

#define GUID_INIT(a, b, c, d0, d1, d2, d3, d4, d5, d6, d7)			\
((guid_t)									\
{{ (a) & 0xff, ((a) >> 8) & 0xff, ((a) >> 16) & 0xff, ((a) >> 24) & 0xff,	\
   (b) & 0xff, ((b) >> 8) & 0xff,						\
   (c) & 0xff, ((c) >> 8) & 0xff,						\
   (d0), (d1), (d2), (d3), (d4), (d5), (d6), (d7) }})

#define NULL_UUID_INIT UUID_INIT(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0)
#define NULL_GUID_INIT GUID_INIT(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0)

static inline bool guid_equal(const guid_t *u1, const guid_t *u2)
{
	return memcmp(u1, u2, sizeof(guid_t)) == 0;
}

static inline void guid_copy(guid_t *dst, const guid_t *src)
{
	memcpy(dst, src, sizeof(guid_t));
}

static inline void export_uuid(u8 *dst, const uuid_t *src) { memcpy(dst, src->b, UUID_SIZE); }
static inline void import_uuid(uuid_t *dst, const u8 *src) { memcpy(dst->b, src, UUID_SIZE); }
static inline void import_guid(guid_t *dst, const u8 *src) { memcpy(dst->b, src, UUID_SIZE); }
static inline void export_guid(u8 *dst, const guid_t *src) { memcpy(dst, src->b, UUID_SIZE); }

static inline bool uuid_equal(const uuid_t *u1, const uuid_t *u2)
{
	return memcmp(u1, u2, sizeof(uuid_t)) == 0;
}

static inline void uuid_copy(uuid_t *dst, const uuid_t *src)
{
	memcpy(dst, src, sizeof(uuid_t));
}

static inline bool uuid_is_null(const uuid_t *uuid)
{
	static const uuid_t null = NULL_UUID_INIT;

	return uuid_equal(uuid, &null);
}

void generate_random_uuid(unsigned char uuid[16]);

static inline void uuid_gen(uuid_t *u)
{
	generate_random_uuid(u->b);
}

#endif
