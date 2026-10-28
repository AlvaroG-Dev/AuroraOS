/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LKPI_LINUX_PWM_H
#define _LKPI_LINUX_PWM_H
/* Sin controladores PWM: pwm_get() devuelve -ENODEV y los portatiles Intel usan el backlight nativo. */
#include <linux/err.h>
#include <linux/device.h>
struct pwm_device;
struct pwm_state { u64 period; u64 duty_cycle; bool enabled; int polarity; bool usage_power; };
enum { PWM_POLARITY_NORMAL, PWM_POLARITY_INVERSED };
static inline struct pwm_device *pwm_get(struct device *dev, const char *con_id) { return ERR_PTR(-ENODEV); }
static inline void pwm_put(struct pwm_device *pwm) {}
static inline void pwm_init_state(const struct pwm_device *pwm, struct pwm_state *state) {}
static inline void pwm_get_state(const struct pwm_device *pwm, struct pwm_state *state) {}
static inline int pwm_apply_might_sleep(struct pwm_device *pwm, const struct pwm_state *state) { return -ENODEV; }
static inline unsigned int pwm_get_period(const struct pwm_device *pwm) { return 0; }
static inline int pwm_config(struct pwm_device *pwm, int duty_ns, int period_ns) { return -ENODEV; }
static inline int pwm_enable(struct pwm_device *pwm) { return -ENODEV; }
static inline void pwm_disable(struct pwm_device *pwm) {}

static inline bool pwm_is_enabled(const struct pwm_device *pwm) { return false; }
static inline int pwm_set_relative_duty_cycle(struct pwm_state *state, unsigned int duty_cycle, unsigned int scale)
{
	return -ENODEV;
}
static inline unsigned int pwm_get_relative_duty_cycle(const struct pwm_state *state, unsigned int scale) { return 0; }
#endif
