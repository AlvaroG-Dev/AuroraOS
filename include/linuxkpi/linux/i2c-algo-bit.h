/* SPDX-License-Identifier: GPL-2.0-only */
/* I2C por bit-banging (K6, L4): el driver da las lineas, la capa el protocolo. */
#ifndef _LKPI_LINUX_I2C_ALGO_BIT_H
#define _LKPI_LINUX_I2C_ALGO_BIT_H

#include <linux/i2c.h>

struct i2c_algo_bit_data {
	void *data;
	void (*setsda)(void *data, int state);
	void (*setscl)(void *data, int state);
	int (*getsda)(void *data);
	int (*getscl)(void *data);	/* NULL: sin estirado de reloj */
	int (*pre_xfer)(struct i2c_adapter *);
	void (*post_xfer)(struct i2c_adapter *);
	int udelay;			/* medio periodo, en us */
	int timeout;			/* jiffies para el estirado de reloj */
	bool can_do_atomic;
};

extern const struct i2c_algorithm lkpi_i2c_bit_algo;
#define i2c_bit_algo lkpi_i2c_bit_algo
int lkpi_i2c_bit_add_bus(struct i2c_adapter *adap);
int lkpi_i2c_bit_add_numbered_bus(struct i2c_adapter *adap);
#define i2c_bit_add_bus(a) lkpi_i2c_bit_add_bus(a)
#define i2c_bit_add_numbered_bus(a) lkpi_i2c_bit_add_numbered_bus(a)

#endif
