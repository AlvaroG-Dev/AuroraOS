#ifndef _LKPI_LINUX_DIM_H
#define _LKPI_LINUX_DIM_H
#include <linux/types.h>
#include <linux/workqueue.h>
#include <linux/ktime.h>
/* Sin moderacion dinamica de interrupciones (DIM): estructuras y llamadas inertes. */
#define DIM_NEVENTS 5
#define DIM_PROFILE_NUM 5
struct dim_cq_moder { u16 usec; u16 pkts; u16 comps; u8 cq_period_mode; };
struct dim_sample { ktime_t time; u32 pkt_ctr; u32 byte_ctr; u16 event_ctr; u32 comp_ctr; };
struct dim_stats { int ppms; int bpms; int epms; int cpms; int cpe_ratio; };
struct dim {
	u8 state; struct dim_stats prev_stats; struct dim_sample start_sample; struct dim_sample measuring_sample;
	struct work_struct work; void *priv; u8 profile_ix; u8 mode; u8 tune_state; u8 steps_right; u8 steps_left; u8 tired;
};
enum { DIM_START_MEASURE, DIM_MEASURE_IN_PROGRESS, DIM_APPLY_NEW_PROFILE };
enum { DIM_CQ_PERIOD_MODE_START_FROM_EQE, DIM_CQ_PERIOD_MODE_START_FROM_CQE, DIM_CQ_PERIOD_NUM_MODES };
static inline void dim_update_sample(u16 event_ctr, u64 packets, u64 bytes, struct dim_sample *s) { }
static inline void net_dim(struct dim *dim, struct dim_sample *end_sample) { }
static inline struct dim_cq_moder net_dim_get_rx_moderation(u8 mode, int ix) { struct dim_cq_moder m = { 0 }; return m; }
static inline struct dim_cq_moder net_dim_get_tx_moderation(u8 mode, int ix) { struct dim_cq_moder m = { 0 }; return m; }
#endif
