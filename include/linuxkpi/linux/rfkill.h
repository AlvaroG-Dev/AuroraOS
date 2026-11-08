/* rfkill: sin interruptor de radio. Las llamadas son no-ops (el estado es siempre "no bloqueado"). */
#ifndef _LKPI_LINUX_RFKILL_H
#define _LKPI_LINUX_RFKILL_H
#include <linux/types.h>
#include <linux/device.h>
#include <linux/err.h>
#include <uapi/linux/rfkill.h>
struct rfkill;
struct rfkill_ops { void (*poll)(struct rfkill *rfkill, void *data); void (*query)(struct rfkill *rfkill, void *data); int (*set_block)(void *data, bool blocked); };
static inline struct rfkill *rfkill_alloc(const char *name, struct device *parent, const enum rfkill_type type, const struct rfkill_ops *ops, void *ops_data) { return NULL; }
static inline int __must_check rfkill_register(struct rfkill *rfkill) { return 0; }
static inline void rfkill_pause_polling(struct rfkill *rfkill) { }
static inline void rfkill_resume_polling(struct rfkill *rfkill) { }
static inline void rfkill_unregister(struct rfkill *rfkill) { }
static inline void rfkill_destroy(struct rfkill *rfkill) { }
static inline bool rfkill_set_hw_state(struct rfkill *rfkill, bool blocked) { return blocked; }
static inline bool rfkill_set_hw_state_reason(struct rfkill *rfkill, bool blocked, unsigned int reason) { return blocked; }
static inline bool rfkill_set_sw_state(struct rfkill *rfkill, bool blocked) { return blocked; }
static inline void rfkill_init_sw_state(struct rfkill *rfkill, bool state) { }
static inline void rfkill_set_states(struct rfkill *rfkill, bool sw, bool hw) { }
static inline bool rfkill_blocked(struct rfkill *rfkill) { return false; }
static inline bool rfkill_soft_blocked(struct rfkill *rfkill) { return false; }
static inline bool rfkill_get_global_sw_state(const enum rfkill_type type) { return false; }
#define RFKILL_HARD_BLOCK_SIGNAL 0x1
#define RFKILL_HARD_BLOCK_NOT_OWNER 0x2
#endif
