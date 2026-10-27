/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * I2C (K6, L4): adaptadores y clientes en el bus "i2c" del modelo de
 * dispositivos de la capa (/sys/bus/i2c/devices/i2c-N y N-00aa), transferencias
 * i2c_transfer() y SMBus emulado sobre ellas cuando el adaptador no lo hace.
 * Sin /dev/i2c-N (i2c-dev) ni deteccion automatica de clientes.
 */
#ifndef _LKPI_LINUX_I2C_H
#define _LKPI_LINUX_I2C_H

#include <linux/types.h>
#include <linux/device.h>
#include <linux/mod_devicetable.h>
#include <linux/mutex.h>
#include <linux/list.h>

struct i2c_msg {
	__u16 addr;
	__u16 flags;
#define I2C_M_RD 0x0001
#define I2C_M_TEN 0x0010
#define I2C_M_DMA_SAFE 0x0200
#define I2C_M_RECV_LEN 0x0400
#define I2C_M_NO_RD_ACK 0x0800
#define I2C_M_IGNORE_NAK 0x1000
#define I2C_M_REV_DIR_ADDR 0x2000
#define I2C_M_NOSTART 0x4000
#define I2C_M_STOP 0x8000
	__u16 len;
	__u8 *buf;
};

#define I2C_FUNC_I2C 0x00000001
#define I2C_FUNC_10BIT_ADDR 0x00000002
#define I2C_FUNC_PROTOCOL_MANGLING 0x00000004
#define I2C_FUNC_SMBUS_PEC 0x00000008
#define I2C_FUNC_NOSTART 0x00000010
#define I2C_FUNC_SLAVE 0x00000020
#define I2C_FUNC_SMBUS_QUICK 0x00010000
#define I2C_FUNC_SMBUS_READ_BYTE 0x00020000
#define I2C_FUNC_SMBUS_WRITE_BYTE 0x00040000
#define I2C_FUNC_SMBUS_READ_BYTE_DATA 0x00080000
#define I2C_FUNC_SMBUS_WRITE_BYTE_DATA 0x00100000
#define I2C_FUNC_SMBUS_READ_WORD_DATA 0x00200000
#define I2C_FUNC_SMBUS_WRITE_WORD_DATA 0x00400000
#define I2C_FUNC_SMBUS_PROC_CALL 0x00800000
#define I2C_FUNC_SMBUS_READ_BLOCK_DATA 0x01000000
#define I2C_FUNC_SMBUS_WRITE_BLOCK_DATA 0x02000000
#define I2C_FUNC_SMBUS_READ_I2C_BLOCK 0x04000000
#define I2C_FUNC_SMBUS_WRITE_I2C_BLOCK 0x08000000
#define I2C_FUNC_SMBUS_BYTE (I2C_FUNC_SMBUS_READ_BYTE | I2C_FUNC_SMBUS_WRITE_BYTE)
#define I2C_FUNC_SMBUS_BYTE_DATA (I2C_FUNC_SMBUS_READ_BYTE_DATA | I2C_FUNC_SMBUS_WRITE_BYTE_DATA)
#define I2C_FUNC_SMBUS_WORD_DATA (I2C_FUNC_SMBUS_READ_WORD_DATA | I2C_FUNC_SMBUS_WRITE_WORD_DATA)
#define I2C_FUNC_SMBUS_BLOCK_DATA \
	(I2C_FUNC_SMBUS_READ_BLOCK_DATA | I2C_FUNC_SMBUS_WRITE_BLOCK_DATA)
#define I2C_FUNC_SMBUS_I2C_BLOCK \
	(I2C_FUNC_SMBUS_READ_I2C_BLOCK | I2C_FUNC_SMBUS_WRITE_I2C_BLOCK)
#define I2C_FUNC_SMBUS_EMUL \
	(I2C_FUNC_SMBUS_QUICK | I2C_FUNC_SMBUS_BYTE | I2C_FUNC_SMBUS_BYTE_DATA | \
	 I2C_FUNC_SMBUS_WORD_DATA | I2C_FUNC_SMBUS_PROC_CALL | \
	 I2C_FUNC_SMBUS_WRITE_BLOCK_DATA | I2C_FUNC_SMBUS_I2C_BLOCK | I2C_FUNC_SMBUS_PEC)

#define I2C_SMBUS_BLOCK_MAX 32
union i2c_smbus_data {
	__u8 byte;
	__u16 word;
	__u8 block[I2C_SMBUS_BLOCK_MAX + 2];
};
#define I2C_SMBUS_READ 1
#define I2C_SMBUS_WRITE 0
#define I2C_SMBUS_QUICK 0
#define I2C_SMBUS_BYTE 1
#define I2C_SMBUS_BYTE_DATA 2
#define I2C_SMBUS_WORD_DATA 3
#define I2C_SMBUS_PROC_CALL 4
#define I2C_SMBUS_BLOCK_DATA 5
#define I2C_SMBUS_I2C_BLOCK_BROKEN 6
#define I2C_SMBUS_BLOCK_PROC_CALL 7
#define I2C_SMBUS_I2C_BLOCK_DATA 8

#define I2C_CLIENT_PEC 0x04
#define I2C_CLIENT_TEN 0x10
#define I2C_CLASS_HWMON (1 << 0)
#define I2C_CLASS_DDC (1 << 3)
#define I2C_CLASS_SPD (1 << 7)
#define I2C_CLIENT_END 0xfffeU

struct i2c_adapter;
struct i2c_client;

struct i2c_algorithm {
	union {
		int (*xfer)(struct i2c_adapter *adap, struct i2c_msg *msgs, int num);
		int (*master_xfer)(struct i2c_adapter *adap, struct i2c_msg *msgs, int num);
	};
	union {
		int (*xfer_atomic)(struct i2c_adapter *adap, struct i2c_msg *msgs, int num);
		int (*master_xfer_atomic)(struct i2c_adapter *adap, struct i2c_msg *msgs, int num);
	};
	int (*smbus_xfer)(struct i2c_adapter *adap, u16 addr, unsigned short flags, char read_write,
			  u8 command, int size, union i2c_smbus_data *data);
	int (*smbus_xfer_atomic)(struct i2c_adapter *adap, u16 addr, unsigned short flags,
				 char read_write, u8 command, int size, union i2c_smbus_data *data);
	u32 (*functionality)(struct i2c_adapter *adap);
};

struct i2c_adapter_quirks {
	u64 flags;
	int max_num_msgs;
	u16 max_write_len, max_read_len, max_comb_1st_msg_len, max_comb_2nd_msg_len;
};

struct i2c_adapter {
	struct module *owner;
	unsigned int class;
	const struct i2c_algorithm *algo;
	void *algo_data;
	struct mutex bus_lock;
	int timeout;			/* jiffies */
	int retries;
	struct device dev;
	unsigned long locked_flags;
	int nr;
	char name[48];
	struct list_head userspace_clients;
	const struct i2c_adapter_quirks *quirks;
};
#define to_i2c_adapter(d) container_of(d, struct i2c_adapter, dev)

struct i2c_client {
	unsigned short flags;
	unsigned short addr;
	char name[I2C_NAME_SIZE];
	struct i2c_adapter *adapter;
	struct device dev;
	int init_irq;
	int irq;
	struct list_head detected;
};
#define to_i2c_client(d) container_of(d, struct i2c_client, dev)

struct i2c_board_info {
	char type[I2C_NAME_SIZE];
	unsigned short flags;
	unsigned short addr;
	const char *dev_name;
	void *platform_data;
	struct device_node *of_node;
	struct fwnode_handle *fwnode;
	const struct software_node *swnode;
	const struct resource *resources;
	unsigned int num_resources;
	int irq;
};
#define I2C_BOARD_INFO(dev_type, dev_addr) .type = dev_type, .addr = (dev_addr)

struct i2c_driver {
	unsigned int class;
	int (*probe)(struct i2c_client *client);
	void (*remove)(struct i2c_client *client);
	void (*shutdown)(struct i2c_client *client);
	struct device_driver driver;
	const struct i2c_device_id *id_table;
	const unsigned short *address_list;
	struct list_head clients;
};
#define to_i2c_driver(d) container_of_const(d, struct i2c_driver, driver)

extern const struct bus_type lkpi_i2c_bus_type;
#define i2c_bus_type lkpi_i2c_bus_type

int lkpi_i2c_add_adapter(struct i2c_adapter *adap);
int lkpi_i2c_add_numbered_adapter(struct i2c_adapter *adap);
void lkpi_i2c_del_adapter(struct i2c_adapter *adap);
int lkpi_devm_i2c_add_adapter(struct device *dev, struct i2c_adapter *adap);
int lkpi_i2c_transfer(struct i2c_adapter *adap, struct i2c_msg *msgs, int num);
int lkpi___i2c_transfer(struct i2c_adapter *adap, struct i2c_msg *msgs, int num);
int lkpi_i2c_transfer_buffer_flags(const struct i2c_client *client, char *buf, int count,
				   u16 flags);
s32 lkpi_i2c_smbus_xfer(struct i2c_adapter *adapter, u16 addr, unsigned short flags,
			char read_write, u8 command, int protocol, union i2c_smbus_data *data);
s32 lkpi_i2c_smbus_read_byte_data(const struct i2c_client *client, u8 command);
s32 lkpi_i2c_smbus_write_byte_data(const struct i2c_client *client, u8 command, u8 value);
s32 lkpi_i2c_smbus_read_word_data(const struct i2c_client *client, u8 command);
s32 lkpi_i2c_smbus_write_word_data(const struct i2c_client *client, u8 command, u16 value);
s32 lkpi_i2c_smbus_read_byte(const struct i2c_client *client);
s32 lkpi_i2c_smbus_write_byte(const struct i2c_client *client, u8 value);
s32 lkpi_i2c_smbus_read_i2c_block_data(const struct i2c_client *client, u8 command, u8 length,
				       u8 *values);
s32 lkpi_i2c_smbus_write_i2c_block_data(const struct i2c_client *client, u8 command,
					u8 length, const u8 *values);
struct i2c_client *lkpi_i2c_new_client_device(struct i2c_adapter *adap,
					      const struct i2c_board_info *info);
struct i2c_client *lkpi_i2c_new_dummy_device(struct i2c_adapter *adapter, u16 address);
void lkpi_i2c_unregister_device(struct i2c_client *client);
int lkpi_i2c_register_driver(struct module *owner, struct i2c_driver *driver);
void lkpi_i2c_del_driver(struct i2c_driver *driver);
struct i2c_adapter *lkpi_i2c_get_adapter(int nr);
void lkpi_i2c_put_adapter(struct i2c_adapter *adap);
struct i2c_client *lkpi_i2c_verify_client(struct device *dev);
const struct i2c_device_id *lkpi_i2c_match_id(const struct i2c_device_id *id,
					      const struct i2c_client *client);

#define i2c_add_adapter(a) lkpi_i2c_add_adapter(a)
#define i2c_add_numbered_adapter(a) lkpi_i2c_add_numbered_adapter(a)
#define i2c_del_adapter(a) lkpi_i2c_del_adapter(a)
#define devm_i2c_add_adapter(d, a) lkpi_devm_i2c_add_adapter(d, a)
#define i2c_transfer(a, m, n) lkpi_i2c_transfer(a, m, n)
#define __i2c_transfer(a, m, n) lkpi___i2c_transfer(a, m, n)
#define i2c_transfer_buffer_flags(c, b, n, f) lkpi_i2c_transfer_buffer_flags(c, b, n, f)
#define i2c_smbus_xfer(a, ad, f, rw, c, p, d) lkpi_i2c_smbus_xfer(a, ad, f, rw, c, p, d)
#define i2c_smbus_read_byte_data(c, cmd) lkpi_i2c_smbus_read_byte_data(c, cmd)
#define i2c_smbus_write_byte_data(c, cmd, v) lkpi_i2c_smbus_write_byte_data(c, cmd, v)
#define i2c_smbus_read_word_data(c, cmd) lkpi_i2c_smbus_read_word_data(c, cmd)
#define i2c_smbus_write_word_data(c, cmd, v) lkpi_i2c_smbus_write_word_data(c, cmd, v)
#define i2c_smbus_read_byte(c) lkpi_i2c_smbus_read_byte(c)
#define i2c_smbus_write_byte(c, v) lkpi_i2c_smbus_write_byte(c, v)
#define i2c_smbus_read_i2c_block_data(c, cmd, l, v) lkpi_i2c_smbus_read_i2c_block_data(c, cmd, l, v)
#define i2c_smbus_write_i2c_block_data(c, cmd, l, v) \
	lkpi_i2c_smbus_write_i2c_block_data(c, cmd, l, v)
#define i2c_new_client_device(a, i) lkpi_i2c_new_client_device(a, i)
#define i2c_new_dummy_device(a, ad) lkpi_i2c_new_dummy_device(a, ad)
#define i2c_unregister_device(c) lkpi_i2c_unregister_device(c)
#define i2c_register_driver(o, d) lkpi_i2c_register_driver(o, d)
#define i2c_add_driver(d) lkpi_i2c_register_driver(THIS_MODULE, d)
#define i2c_del_driver(d) lkpi_i2c_del_driver(d)
#define i2c_get_adapter(nr) lkpi_i2c_get_adapter(nr)
#define i2c_put_adapter(a) lkpi_i2c_put_adapter(a)
#define i2c_verify_client(d) lkpi_i2c_verify_client(d)
#define i2c_match_id(id, c) lkpi_i2c_match_id(id, c)
#define module_i2c_driver(__i2c_driver) \
	module_driver(__i2c_driver, i2c_add_driver, i2c_del_driver)

static inline int i2c_master_send(const struct i2c_client *client, const char *buf, int count)
{
	return lkpi_i2c_transfer_buffer_flags(client, (char *)buf, count, 0);
}
static inline int i2c_master_recv(const struct i2c_client *client, char *buf, int count)
{
	return lkpi_i2c_transfer_buffer_flags(client, buf, count, I2C_M_RD);
}
static inline void *i2c_get_clientdata(const struct i2c_client *client)
{
	return dev_get_drvdata(&client->dev);
}
static inline void i2c_set_clientdata(struct i2c_client *client, void *data)
{
	dev_set_drvdata(&client->dev, data);
}
static inline void *i2c_get_adapdata(const struct i2c_adapter *adap)
{
	return dev_get_drvdata(&adap->dev);
}
static inline void i2c_set_adapdata(struct i2c_adapter *adap, void *data)
{
	dev_set_drvdata(&adap->dev, data);
}
static inline u32 i2c_get_functionality(struct i2c_adapter *adap)
{
	return adap->algo->functionality(adap);
}
static inline int i2c_check_functionality(struct i2c_adapter *adap, u32 func)
{
	return (func & i2c_get_functionality(adap)) == func;
}
static inline int i2c_adapter_id(struct i2c_adapter *adap) { return adap->nr; }
static inline u8 i2c_8bit_addr_from_msg(const struct i2c_msg *msg)
{
	return (msg->addr << 1) | (msg->flags & I2C_M_RD);
}
static inline void i2c_lock_bus(struct i2c_adapter *adap, unsigned int flags) { mutex_lock(&adap->bus_lock); }
static inline int i2c_trylock_bus(struct i2c_adapter *adap, unsigned int flags)
{
	return mutex_trylock(&adap->bus_lock);
}
static inline void i2c_unlock_bus(struct i2c_adapter *adap, unsigned int flags)
{
	mutex_unlock(&adap->bus_lock);
}
#define I2C_LOCK_ROOT_ADAPTER 0x01
#define I2C_LOCK_SEGMENT 0x02
static inline struct i2c_client *i2c_new_ancillary_device(struct i2c_client *client,
							  const char *name, u16 addr)
{
	return lkpi_i2c_new_dummy_device(client->adapter, addr);
}

#endif
