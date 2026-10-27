// SPDX-License-Identifier: GPL-2.0-only
/*
 * I2C por bit-banging (K6, L4), escrito a partir de la especificacion I2C
 * (UM10204): el driver mueve y lee SDA/SCL (lineas de drenador abierto: 1 =
 * soltar) y aqui se generan START, START repetido, STOP, los bits (SDA solo
 * cambia con SCL baja), el ACK de 9.o reloj y la espera del estirado de reloj
 * del esclavo (si hay getscl). Direcciones de 7 y 10 bits; I2C_M_NOSTART,
 * I2C_M_IGNORE_NAK, I2C_M_NO_RD_ACK, I2C_M_REV_DIR_ADDR e I2C_M_RECV_LEN.
 */
#include <linux/kernel.h>
#include <linux/export.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/i2c-algo-bit.h>
#include <linux/jiffies.h>
#include <linux/sched.h>

struct lkpi_bitbus {
	struct i2c_algo_bit_data *a;
	struct i2c_adapter *adap;
};

static inline void bb_delay(struct i2c_algo_bit_data *a)
{
	udelay((a->udelay + 1) / 2);	/* medio periodo */
}

static inline void bb_sda(struct i2c_algo_bit_data *a, int v)
{
	a->setsda(a->data, v);
}

static inline void bb_scllo(struct i2c_algo_bit_data *a)
{
	a->setscl(a->data, 0);
	bb_delay(a);
}

/* Suelta SCL y espera a que suba (el esclavo puede retenerla). */
static int bb_sclhi(struct i2c_algo_bit_data *a)
{
	unsigned long start;

	a->setscl(a->data, 1);
	if (!a->getscl)
		goto done;
	start = jiffies;
	while (!a->getscl(a->data)) {
		if (time_after(jiffies, start + (a->timeout ? a->timeout : HZ / 10))) {
			if (a->getscl(a->data))
				break;
			return -ETIMEDOUT;
		}
		cpu_relax();
	}
done:
	bb_delay(a);
	return 0;
}

/* START: SDA baja con SCL alta; luego SCL baja. */
static void bb_start(struct i2c_algo_bit_data *a)
{
	bb_sda(a, 0);
	bb_delay(a);
	bb_scllo(a);
}

static int bb_repstart(struct i2c_algo_bit_data *a)
{
	int rc;

	bb_sda(a, 1);
	rc = bb_sclhi(a);
	if (rc)
		return rc;
	bb_start(a);
	return 0;
}

/* STOP: SDA sube con SCL alta. */
static void bb_stop(struct i2c_algo_bit_data *a)
{
	bb_sda(a, 0);
	bb_sclhi(a);
	bb_sda(a, 1);
	bb_delay(a);
}

/* Envia un byte: 1 = ACK, 0 = NAK, <0 error. */
static int bb_outb(struct i2c_algo_bit_data *a, u8 c)
{
	int i, ack;

	for (i = 7; i >= 0; i--) {
		bb_sda(a, (c >> i) & 1);
		bb_delay(a);
		if (bb_sclhi(a) < 0)
			return -ETIMEDOUT;
		bb_scllo(a);
	}
	bb_sda(a, 1);			/* el esclavo pone el ACK */
	if (bb_sclhi(a) < 0)
		return -ETIMEDOUT;
	ack = !a->getsda(a->data);
	bb_scllo(a);
	return ack;
}

/* Lee un byte (el maestro pone el ACK despues, ver bb_ack). */
static int bb_inb(struct i2c_algo_bit_data *a)
{
	int i;
	u8 c = 0;

	bb_sda(a, 1);
	for (i = 0; i < 8; i++) {
		if (bb_sclhi(a) < 0)
			return -ETIMEDOUT;
		c = (c << 1) | (a->getsda(a->data) ? 1 : 0);
		a->setscl(a->data, 0);
		bb_delay(a);
	}
	return c;
}

static int bb_ack(struct i2c_algo_bit_data *a, bool ack)
{
	if (ack)
		bb_sda(a, 0);
	bb_delay(a);
	if (bb_sclhi(a) < 0)
		return -ETIMEDOUT;
	bb_scllo(a);
	bb_sda(a, 1);
	return 0;
}

/* Byte de direccion con reintentos (un NAK puede ser un esclavo ocupado). */
static int bb_try_address(struct i2c_adapter *adap, u8 addr, int retries)
{
	struct i2c_algo_bit_data *a = adap->algo_data;
	int i, ret = 0;

	for (i = 0; i <= retries; i++) {
		ret = bb_outb(a, addr);
		if (ret == 1 || i == retries)
			break;
		bb_stop(a);
		udelay(a->udelay);
		yield();
		bb_start(a);
	}
	return ret;
}

static int bb_do_address(struct i2c_adapter *adap, struct i2c_msg *msg)
{
	struct i2c_algo_bit_data *a = adap->algo_data;
	unsigned short flags = msg->flags;
	bool nak_ok = flags & I2C_M_IGNORE_NAK;
	int retries = nak_ok ? 0 : adap->retries, ret;
	u8 addr;

	if (flags & I2C_M_TEN) {
		addr = 0xf0 | ((msg->addr >> 7) & 0x06);
		ret = bb_try_address(adap, addr, retries);
		if (ret != 1 && !nak_ok)
			return -ENXIO;
		ret = bb_outb(a, msg->addr & 0xff);
		if (ret != 1 && !nak_ok)
			return -ENXIO;
		if (flags & I2C_M_RD) {
			if (bb_repstart(a) < 0)
				return -ETIMEDOUT;
			ret = bb_try_address(adap, addr | 1, retries);
			if (ret != 1 && !nak_ok)
				return -EIO;
		}
		return 0;
	}
	addr = i2c_8bit_addr_from_msg(msg);
	if (flags & I2C_M_REV_DIR_ADDR)
		addr ^= 1;
	ret = bb_try_address(adap, addr, retries);
	if (ret < 0)
		return ret;
	if (ret != 1 && !nak_ok)
		return -ENXIO;
	return 0;
}

static int bb_sendbytes(struct i2c_adapter *adap, struct i2c_msg *msg)
{
	struct i2c_algo_bit_data *a = adap->algo_data;
	int i, ret;

	for (i = 0; i < msg->len; i++) {
		ret = bb_outb(a, msg->buf[i]);
		if (ret < 0)
			return ret;
		if (ret == 0 && !(msg->flags & I2C_M_IGNORE_NAK))
			return -EIO;
	}
	return i;
}

static int bb_readbytes(struct i2c_adapter *adap, struct i2c_msg *msg)
{
	struct i2c_algo_bit_data *a = adap->algo_data;
	int i, c, count = msg->len;

	for (i = 0; i < count; i++) {
		c = bb_inb(a);
		if (c < 0)
			return c;
		msg->buf[i] = c;
		if (i == 0 && (msg->flags & I2C_M_RECV_LEN)) {
			if (c <= 0 || c > I2C_SMBUS_BLOCK_MAX) {
				bb_ack(a, false);
				return -EPROTO;
			}
			count += c;	/* el byte de longitud ya esta en buf */
			msg->len += c;
		}
		if (!(msg->flags & I2C_M_NO_RD_ACK) && bb_ack(a, i + 1 < count) < 0)
			return -ETIMEDOUT;
	}
	return i;
}

static int bb_xfer(struct i2c_adapter *adap, struct i2c_msg *msgs, int num)
{
	struct i2c_algo_bit_data *a = adap->algo_data;
	int i, ret;

	if (a->pre_xfer) {
		ret = a->pre_xfer(adap);
		if (ret < 0)
			return ret;
	}
	bb_start(a);
	for (i = 0; i < num; i++) {
		struct i2c_msg *m = &msgs[i];

		if (!(m->flags & I2C_M_NOSTART)) {
			if (i) {
				if (m->flags & I2C_M_STOP) {
					bb_stop(a);
					bb_start(a);
				} else if (bb_repstart(a) < 0) {
					ret = -ETIMEDOUT;
					goto out;
				}
			}
			ret = bb_do_address(adap, m);
			if (ret)
				goto out;
		}
		ret = (m->flags & I2C_M_RD) ? bb_readbytes(adap, m) : bb_sendbytes(adap, m);
		if (ret < 0)
			goto out;
		if (ret < m->len) {
			ret = -EIO;
			goto out;
		}
	}
	ret = i;
out:
	bb_stop(a);
	if (a->post_xfer)
		a->post_xfer(adap);
	return ret;
}

static u32 bb_func(struct i2c_adapter *adap)
{
	return I2C_FUNC_I2C | I2C_FUNC_NOSTART | I2C_FUNC_SMBUS_EMUL | I2C_FUNC_10BIT_ADDR |
	       I2C_FUNC_PROTOCOL_MANGLING;
}

const struct i2c_algorithm lkpi_i2c_bit_algo = {
	.master_xfer = bb_xfer,
	.functionality = bb_func,
};
EXPORT_SYMBOL(lkpi_i2c_bit_algo);

static void bb_prepare(struct i2c_adapter *adap)
{
	struct i2c_algo_bit_data *a = adap->algo_data;

	adap->algo = &lkpi_i2c_bit_algo;
	if (!adap->retries)
		adap->retries = 3;
	if (!a->udelay)
		a->udelay = 10;		/* ~50 kHz */
	/* Bus libre: las dos lineas sueltas. */
	a->setsda(a->data, 1);
	a->setscl(a->data, 1);
}

int lkpi_i2c_bit_add_bus(struct i2c_adapter *adap)
{
	bb_prepare(adap);
	return i2c_add_adapter(adap);
}
EXPORT_SYMBOL(lkpi_i2c_bit_add_bus);

int lkpi_i2c_bit_add_numbered_bus(struct i2c_adapter *adap)
{
	bb_prepare(adap);
	return i2c_add_numbered_adapter(adap);
}
EXPORT_SYMBOL(lkpi_i2c_bit_add_numbered_bus);
