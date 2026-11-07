/* Planificador de paquetes: solo las estructuras de offload (taprio, CBS,
 * ETF, mqprio) que usa igc. La capa no ofrece TSN: los drivers las reciben
 * por ndo_setup_tc, que nadie llama. */
#ifndef _LKPI_NET_PKT_SCHED_H
#define _LKPI_NET_PKT_SCHED_H
#include <linux/netdevice.h>
#include <linux/ktime.h>
#include <linux/netlink.h>
#define TC_QOPT_MAX_QUEUE 16
#define TC_MQPRIO_HW_OFFLOAD_TCS 2
#define TC_MAX_QUEUE 16
#define TC_BITMASK 15
#define TC_TAPRIO_CMD_SET_GATES 0
struct tc_query_caps_base { enum tc_setup_type type; void *caps; };
struct tc_taprio_caps { bool supports_queue_max_sdu; bool gate_mask_per_txq; bool broken_mqprio; };
struct tc_mqprio_caps { bool validate_queue_counts; };
struct tc_mqprio_qopt { __u8 num_tc; __u8 prio_tc_map[16]; __u8 hw; __u16 count[16]; __u16 offset[16]; };
struct tc_mqprio_qopt_offload { struct tc_mqprio_qopt qopt; u16 mode, shaper; u32 flags; u64 min_rate[TC_QOPT_MAX_QUEUE], max_rate[TC_QOPT_MAX_QUEUE]; unsigned long preemptible_tcs; struct netlink_ext_ack *extack; };
struct tc_taprio_sched_entry { u8 command; u32 gate_mask; u32 interval; };
struct tc_taprio_qopt_stats { u64 window_drops; u64 tx_overruns; };
struct tc_taprio_qopt_queue_stats { int queue; struct tc_taprio_qopt_stats stats; };
enum tc_taprio_qopt_cmd { TAPRIO_CMD_REPLACE, TAPRIO_CMD_DESTROY, TAPRIO_CMD_STATS, TAPRIO_CMD_QUEUE_STATS };
struct tc_taprio_qopt_offload {
	enum tc_taprio_qopt_cmd cmd;
	union {
		struct tc_taprio_qopt_stats stats;
		struct tc_taprio_qopt_queue_stats queue_stats;
		struct {
			struct tc_mqprio_qopt_offload mqprio;
			struct netlink_ext_ack *extack;
			ktime_t base_time;
			u64 cycle_time;
			u64 cycle_time_extension;
			u32 max_sdu[TC_MAX_QUEUE];
			size_t num_entries;
			struct tc_taprio_sched_entry entries[];
		};
	};
};
struct tc_cbs_qopt_offload { u8 enable; s32 queue; s32 hicredit; s32 locredit; s32 idleslope; s32 sendslope; };
struct tc_etf_qopt_offload { u8 enable; s32 queue; };
static inline void skb_txtime_consumed(struct sk_buff *skb) { skb->tstamp = 0; }
#endif
