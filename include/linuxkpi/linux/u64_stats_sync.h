#ifndef _LKPI_LINUX_U64_STATS_SYNC_H
#define _LKPI_LINUX_U64_STATS_SYNC_H
#include <linux/types.h>
struct u64_stats_sync { int dummy; };
static inline void u64_stats_init(struct u64_stats_sync *s) { }
static inline void u64_stats_update_begin(struct u64_stats_sync *s) { }
static inline void u64_stats_update_end(struct u64_stats_sync *s) { }
static inline unsigned int u64_stats_fetch_begin(const struct u64_stats_sync *s) { return 0; }
static inline bool u64_stats_fetch_retry(const struct u64_stats_sync *s, unsigned int start) { return false; }
typedef struct { u64 v; } u64_stats_t;
static inline u64 u64_stats_read(const u64_stats_t *p) { return p->v; }
static inline void u64_stats_set(u64_stats_t *p, u64 v) { p->v = v; }
static inline void u64_stats_add(u64_stats_t *p, unsigned long n) { p->v += n; }
static inline void u64_stats_inc(u64_stats_t *p) { p->v++; }
#endif
