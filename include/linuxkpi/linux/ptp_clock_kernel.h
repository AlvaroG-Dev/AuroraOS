/* PTP: sin soporte en la capa (Plan_Red.md). ptp_clock_register() falla y el
 * driver sigue sin reloj de hardware. */
#ifndef _LKPI_LINUX_PTP_CLOCK_KERNEL_H
#define _LKPI_LINUX_PTP_CLOCK_KERNEL_H
#include <linux/types.h>
#include <linux/err.h>
#include <linux/ktime.h>
#include <linux/time64.h>
#include <linux/device.h>
#define PTP_CLOCK_NAME_LEN 32
struct ptp_clock;
struct ptp_clock_request;
struct ptp_system_timestamp { struct timespec64 pre_ts; struct timespec64 post_ts; int clockid; };
struct ptp_pin_desc;
struct system_device_crosststamp;
struct ptp_clock_info {
	struct module *owner;
	char name[PTP_CLOCK_NAME_LEN];
	s32 max_adj;
	int n_alarm, n_ext_ts, n_per_out, n_pins, pps;
	int (*adjfine)(struct ptp_clock_info *ptp, long scaled_ppm);
	int (*adjtime)(struct ptp_clock_info *ptp, s64 delta);
	int (*gettimex64)(struct ptp_clock_info *ptp, struct timespec64 *ts, struct ptp_system_timestamp *sts);
	int (*settime64)(struct ptp_clock_info *p, const struct timespec64 *ts);
	int (*enable)(struct ptp_clock_info *ptp, struct ptp_clock_request *request, int on);
};
static inline struct ptp_clock *ptp_clock_register(struct ptp_clock_info *info, struct device *parent)
{ return ERR_PTR(-EOPNOTSUPP); }
static inline u64 adjust_by_scaled_ppm(u64 base, long scaled_ppm) { return base; }
static inline int ptp_clock_unregister(struct ptp_clock *ptp) { return 0; }
static inline int ptp_clock_index(struct ptp_clock *ptp) { return -1; }
static inline void ptp_read_system_prets(struct ptp_system_timestamp *sts) { }
static inline void ptp_read_system_postts(struct ptp_system_timestamp *sts) { }
#endif
