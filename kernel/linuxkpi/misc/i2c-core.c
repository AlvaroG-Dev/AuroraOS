// SPDX-License-Identifier: GPL-2.0-only
/*
 * Nucleo I2C (K6, L4): bus "i2c" con adaptadores (i2c-N) y clientes (N-00aa)
 * en el modelo de dispositivos de la capa, i2c_transfer() con reintentos y
 * SMBus emulado con mensajes I2C cuando el adaptador no tiene smbus_xfer.
 * Ver linux/i2c.h.
 */
#include <linux/kernel.h>
#include <linux/export.h>
#include <linux/device.h>
#include <linux/i2c.h>
#include <linux/idr.h>
#include <linux/jiffies.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "../lkpi_internal.h"

static DEFINE_IDR(lkpi_i2c_adapter_idr);
static DEFINE_MUTEX(lkpi_i2c_core_lock);

static const struct device_type lkpi_i2c_adapter_type;
static const struct device_type lkpi_i2c_client_type;

/* ------------------------------------------------------------- bus */

const struct i2c_device_id *lkpi_i2c_match_id(const struct i2c_device_id *id,
					      const struct i2c_client *client)
{
	if (!id || !client)
		return NULL;
	for (; id->name[0]; id++)
		if (!strcmp(client->name, id->name))
			return id;
	return NULL;
}
EXPORT_SYMBOL(lkpi_i2c_match_id);

struct i2c_client *lkpi_i2c_verify_client(struct device *dev)
{
	return dev->type == &lkpi_i2c_client_type ? to_i2c_client(dev) : NULL;
}
EXPORT_SYMBOL(lkpi_i2c_verify_client);

static int lkpi_i2c_match(struct device *dev, const struct device_driver *drv)
{
	struct i2c_client *client = lkpi_i2c_verify_client(dev);
	const struct i2c_driver *driver;

	if (!client)
		return 0;
	driver = to_i2c_driver(drv);
	if (lkpi_i2c_match_id(driver->id_table, client))
		return 1;
	return !strcmp(client->name, drv->name);
}

static int lkpi_i2c_probe(struct device *dev)
{
	struct i2c_client *client = lkpi_i2c_verify_client(dev);
	struct i2c_driver *driver;

	if (!client)
		return 0;
	driver = to_i2c_driver(dev->driver);
	if (!driver->probe)
		return -ENODEV;
	return driver->probe(client);
}

static void lkpi_i2c_remove(struct device *dev)
{
	struct i2c_client *client = lkpi_i2c_verify_client(dev);
	struct i2c_driver *driver;

	if (!client || !dev->driver)
		return;
	driver = to_i2c_driver(dev->driver);
	if (driver->remove)
		driver->remove(client);
}

static int lkpi_i2c_uevent(const struct device *dev, struct kobj_uevent_env *env)
{
	const struct i2c_client *client = dev->type == &lkpi_i2c_client_type ?
		container_of(dev, struct i2c_client, dev) : NULL;

	if (client)
		return add_uevent_var(env, "MODALIAS=%s%s", I2C_MODULE_PREFIX, client->name);
	return 0;
}

const struct bus_type lkpi_i2c_bus_type = {
	.name = "i2c",
	.match = lkpi_i2c_match,
	.probe = lkpi_i2c_probe,
	.remove = lkpi_i2c_remove,
	.uevent = lkpi_i2c_uevent,
};
EXPORT_SYMBOL(lkpi_i2c_bus_type);

static ssize_t name_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	if (dev->type == &lkpi_i2c_adapter_type)
		return sysfs_emit(buf, "%s\n", to_i2c_adapter(dev)->name);
	return sysfs_emit(buf, "%s\n", to_i2c_client(dev)->name);
}
static DEVICE_ATTR_RO(name);
static struct attribute *lkpi_i2c_attrs[] = { &dev_attr_name.attr, NULL };
ATTRIBUTE_GROUPS(lkpi_i2c);

/* ------------------------------------------------------ adaptadores */

static void lkpi_i2c_adapter_release(struct device *dev)
{
	/* El adaptador es del driver: nada que liberar. */
}

static const struct device_type lkpi_i2c_adapter_type = {
	.name = "i2c_adapter",
	.groups = lkpi_i2c_groups,
	.release = lkpi_i2c_adapter_release,
};

static void i2c_default_lock_bus(struct i2c_adapter *adap, unsigned int flags)
{
	mutex_lock(&adap->bus_lock);
}

static int i2c_default_trylock_bus(struct i2c_adapter *adap, unsigned int flags)
{
	return mutex_trylock(&adap->bus_lock);
}

static void i2c_default_unlock_bus(struct i2c_adapter *adap, unsigned int flags)
{
	mutex_unlock(&adap->bus_lock);
}

static const struct i2c_lock_operations i2c_default_lock_ops = {
	.lock_bus = i2c_default_lock_bus,
	.trylock_bus = i2c_default_trylock_bus,
	.unlock_bus = i2c_default_unlock_bus,
};

static int lkpi_i2c_register_adapter(struct i2c_adapter *adap)
{
	int rc;

	if (WARN(!adap->name[0], "adaptador I2C sin nombre\n") || !adap->algo)
		return -EINVAL;
	mutex_init(&adap->bus_lock);
	if (!adap->lock_ops)
		adap->lock_ops = &i2c_default_lock_ops;
	INIT_LIST_HEAD(&adap->userspace_clients);
	if (!adap->timeout)
		adap->timeout = HZ;
	adap->dev.bus = &lkpi_i2c_bus_type;
	adap->dev.type = &lkpi_i2c_adapter_type;
	dev_set_name(&adap->dev, "i2c-%d", adap->nr);
	rc = device_register(&adap->dev);
	if (rc) {
		put_device(&adap->dev);
		mutex_lock(&lkpi_i2c_core_lock);
		idr_remove(&lkpi_i2c_adapter_idr, adap->nr);
		mutex_unlock(&lkpi_i2c_core_lock);
		return rc;
	}
	dev_dbg(&adap->dev, "adaptador [%s] registrado\n", adap->name);
	return 0;
}

int lkpi_i2c_add_adapter(struct i2c_adapter *adap)
{
	int id;

	mutex_lock(&lkpi_i2c_core_lock);
	id = idr_alloc(&lkpi_i2c_adapter_idr, adap, 0, 0, GFP_KERNEL);
	mutex_unlock(&lkpi_i2c_core_lock);
	if (id < 0)
		return id;
	adap->nr = id;
	return lkpi_i2c_register_adapter(adap);
}
EXPORT_SYMBOL(lkpi_i2c_add_adapter);

int lkpi_i2c_add_numbered_adapter(struct i2c_adapter *adap)
{
	int id;

	if (adap->nr == -1)
		return lkpi_i2c_add_adapter(adap);
	mutex_lock(&lkpi_i2c_core_lock);
	id = idr_alloc(&lkpi_i2c_adapter_idr, adap, adap->nr, adap->nr + 1, GFP_KERNEL);
	mutex_unlock(&lkpi_i2c_core_lock);
	if (id < 0)
		return id == -ENOSPC ? -EBUSY : id;
	return lkpi_i2c_register_adapter(adap);
}
EXPORT_SYMBOL(lkpi_i2c_add_numbered_adapter);

static int lkpi_i2c_unregister_child(struct device *dev, void *data)
{
	struct i2c_client *client = lkpi_i2c_verify_client(dev);

	if (client)
		lkpi_i2c_unregister_device(client);
	return 0;
}

void lkpi_i2c_del_adapter(struct i2c_adapter *adap)
{
	mutex_lock(&lkpi_i2c_core_lock);
	if (idr_find(&lkpi_i2c_adapter_idr, adap->nr) != adap) {
		mutex_unlock(&lkpi_i2c_core_lock);
		pr_err("i2c_del_adapter: adaptador no registrado\n");
		return;
	}
	mutex_unlock(&lkpi_i2c_core_lock);
	device_for_each_child(&adap->dev, NULL, lkpi_i2c_unregister_child);
	device_unregister(&adap->dev);
	mutex_lock(&lkpi_i2c_core_lock);
	idr_remove(&lkpi_i2c_adapter_idr, adap->nr);
	mutex_unlock(&lkpi_i2c_core_lock);
	memset(&adap->dev, 0, sizeof(adap->dev));
}
EXPORT_SYMBOL(lkpi_i2c_del_adapter);

static void lkpi_devm_i2c_del_adapter(void *adapter)
{
	lkpi_i2c_del_adapter(adapter);
}

int lkpi_devm_i2c_add_adapter(struct device *dev, struct i2c_adapter *adap)
{
	int rc = lkpi_i2c_add_adapter(adap);

	if (rc)
		return rc;
	return devm_add_action_or_reset(dev, lkpi_devm_i2c_del_adapter, adap);
}
EXPORT_SYMBOL(lkpi_devm_i2c_add_adapter);

struct i2c_adapter *lkpi_i2c_get_adapter(int nr)
{
	struct i2c_adapter *adap;

	mutex_lock(&lkpi_i2c_core_lock);
	adap = idr_find(&lkpi_i2c_adapter_idr, nr);
	if (adap)
		get_device(&adap->dev);
	mutex_unlock(&lkpi_i2c_core_lock);
	return adap;
}
EXPORT_SYMBOL(lkpi_i2c_get_adapter);

void lkpi_i2c_put_adapter(struct i2c_adapter *adap)
{
	if (adap)
		put_device(&adap->dev);
}
EXPORT_SYMBOL(lkpi_i2c_put_adapter);

/* --------------------------------------------------------- clientes */

static void lkpi_i2c_client_release(struct device *dev)
{
	kfree(to_i2c_client(dev));
}

static const struct device_type lkpi_i2c_client_type = {
	.name = "i2c_client",
	.groups = lkpi_i2c_groups,
	.release = lkpi_i2c_client_release,
};

struct i2c_client *lkpi_i2c_new_client_device(struct i2c_adapter *adap,
					      const struct i2c_board_info *info)
{
	struct i2c_client *client;
	int rc;

	if (!info->addr || info->addr > ((info->flags & I2C_CLIENT_TEN) ? 0x3ff : 0x7f))
		return ERR_PTR(-EINVAL);
	client = kzalloc(sizeof(*client), GFP_KERNEL);
	if (!client)
		return ERR_PTR(-ENOMEM);
	client->adapter = adap;
	client->addr = info->addr;
	client->flags = info->flags;
	client->irq = client->init_irq = info->irq;
	strscpy(client->name, info->type, sizeof(client->name));
	client->dev.platform_data = info->platform_data;
	client->dev.parent = &adap->dev;
	client->dev.bus = &lkpi_i2c_bus_type;
	client->dev.type = &lkpi_i2c_client_type;
	if (info->dev_name)
		dev_set_name(&client->dev, "%s", info->dev_name);
	else
		dev_set_name(&client->dev, "%d-%04x", i2c_adapter_id(adap),
			     client->addr | ((client->flags & I2C_CLIENT_TEN) ? 0xa000 : 0));
	rc = device_register(&client->dev);
	if (rc) {
		put_device(&client->dev);
		return ERR_PTR(rc);
	}
	return client;
}
EXPORT_SYMBOL(lkpi_i2c_new_client_device);

struct i2c_client *lkpi_i2c_new_dummy_device(struct i2c_adapter *adapter, u16 address)
{
	struct i2c_board_info info = { I2C_BOARD_INFO("dummy", address) };

	return lkpi_i2c_new_client_device(adapter, &info);
}
EXPORT_SYMBOL(lkpi_i2c_new_dummy_device);

void lkpi_i2c_unregister_device(struct i2c_client *client)
{
	if (IS_ERR_OR_NULL(client))
		return;
	device_unregister(&client->dev);
}
EXPORT_SYMBOL(lkpi_i2c_unregister_device);

int lkpi_i2c_register_driver(struct module *owner, struct i2c_driver *driver)
{
	driver->driver.owner = owner;
	driver->driver.bus = &lkpi_i2c_bus_type;
	INIT_LIST_HEAD(&driver->clients);
	return driver_register(&driver->driver);
}
EXPORT_SYMBOL(lkpi_i2c_register_driver);

void lkpi_i2c_del_driver(struct i2c_driver *driver)
{
	driver_unregister(&driver->driver);
}
EXPORT_SYMBOL(lkpi_i2c_del_driver);

/* -------------------------------------------------- transferencias */

int lkpi___i2c_transfer(struct i2c_adapter *adap, struct i2c_msg *msgs, int num)
{
	unsigned long orig_jiffies = jiffies;
	int ret, try;

	if (!adap->algo->master_xfer)
		return -EOPNOTSUPP;
	if (!msgs || num < 1)
		return -EINVAL;
	for (ret = 0, try = 0; try <= adap->retries; try++) {
		ret = adap->algo->master_xfer(adap, msgs, num);
		if (ret != -EAGAIN)
			break;
		if (time_after(jiffies, orig_jiffies + adap->timeout))
			break;
	}
	return ret;
}
EXPORT_SYMBOL(lkpi___i2c_transfer);

int lkpi_i2c_transfer(struct i2c_adapter *adap, struct i2c_msg *msgs, int num)
{
	int ret;

	i2c_lock_bus(adap, I2C_LOCK_SEGMENT);
	ret = lkpi___i2c_transfer(adap, msgs, num);
	i2c_unlock_bus(adap, I2C_LOCK_SEGMENT);
	return ret;
}
EXPORT_SYMBOL(lkpi_i2c_transfer);

int lkpi_i2c_transfer_buffer_flags(const struct i2c_client *client, char *buf, int count,
				   u16 flags)
{
	struct i2c_msg msg = {
		.addr = client->addr,
		.flags = flags | (client->flags & I2C_M_TEN),
		.len = count,
		.buf = (u8 *)buf,
	};
	int ret = lkpi_i2c_transfer(client->adapter, &msg, 1);

	return ret == 1 ? count : ret < 0 ? ret : -EIO;
}
EXPORT_SYMBOL(lkpi_i2c_transfer_buffer_flags);

/* SMBus con mensajes I2C (sin PEC). */
static s32 lkpi_i2c_smbus_emulated(struct i2c_adapter *adap, u16 addr, unsigned short flags,
				   char read_write, u8 command, int size,
				   union i2c_smbus_data *data)
{
	u8 buf0[I2C_SMBUS_BLOCK_MAX + 3], buf1[I2C_SMBUS_BLOCK_MAX + 2];
	int nmsgs = read_write == I2C_SMBUS_READ ? 2 : 1, i, ret;
	struct i2c_msg msg[2] = {
		{ .addr = addr, .flags = flags, .len = 1, .buf = buf0 },
		{ .addr = addr, .flags = flags | I2C_M_RD, .len = 0, .buf = buf1 },
	};

	buf0[0] = command;
	switch (size) {
	case I2C_SMBUS_QUICK:
		msg[0].len = 0;
		msg[0].flags = flags | (read_write == I2C_SMBUS_READ ? I2C_M_RD : 0);
		nmsgs = 1;
		break;
	case I2C_SMBUS_BYTE:
		if (read_write == I2C_SMBUS_READ) {
			msg[0].flags = I2C_M_RD | flags;
			nmsgs = 1;
		}
		break;
	case I2C_SMBUS_BYTE_DATA:
		if (read_write == I2C_SMBUS_READ) {
			msg[1].len = 1;
		} else {
			msg[0].len = 2;
			buf0[1] = data->byte;
		}
		break;
	case I2C_SMBUS_WORD_DATA:
		if (read_write == I2C_SMBUS_READ) {
			msg[1].len = 2;
		} else {
			msg[0].len = 3;
			buf0[1] = data->word & 0xff;
			buf0[2] = data->word >> 8;
		}
		break;
	case I2C_SMBUS_PROC_CALL:
		nmsgs = 2;
		read_write = I2C_SMBUS_READ;
		msg[0].len = 3;
		msg[1].len = 2;
		buf0[1] = data->word & 0xff;
		buf0[2] = data->word >> 8;
		break;
	case I2C_SMBUS_BLOCK_DATA:
		if (read_write == I2C_SMBUS_READ)
			return -EOPNOTSUPP;	/* necesita I2C_M_RECV_LEN */
		msg[0].len = data->block[0] + 2;
		if (data->block[0] > I2C_SMBUS_BLOCK_MAX)
			return -EINVAL;
		for (i = 1; i < msg[0].len; i++)
			buf0[i] = data->block[i - 1];
		break;
	case I2C_SMBUS_I2C_BLOCK_DATA:
		if (data->block[0] > I2C_SMBUS_BLOCK_MAX)
			return -EINVAL;
		if (read_write == I2C_SMBUS_READ) {
			msg[1].len = data->block[0];
		} else {
			msg[0].len = data->block[0] + 1;
			for (i = 1; i <= data->block[0]; i++)
				buf0[i] = data->block[i];
		}
		break;
	default:
		return -EOPNOTSUPP;
	}
	ret = lkpi___i2c_transfer(adap, msg, nmsgs);
	if (ret < 0)
		return ret;
	if (ret != nmsgs)
		return -EIO;
	if (read_write == I2C_SMBUS_WRITE)
		return 0;
	switch (size) {
	case I2C_SMBUS_BYTE:
		data->byte = buf0[0];
		break;
	case I2C_SMBUS_BYTE_DATA:
		data->byte = buf1[0];
		break;
	case I2C_SMBUS_WORD_DATA:
	case I2C_SMBUS_PROC_CALL:
		data->word = buf1[0] | (buf1[1] << 8);
		break;
	case I2C_SMBUS_I2C_BLOCK_DATA:
		for (i = 0; i < data->block[0]; i++)
			data->block[i + 1] = buf1[i];
		break;
	}
	return 0;
}

s32 lkpi_i2c_smbus_xfer(struct i2c_adapter *adapter, u16 addr, unsigned short flags,
			char read_write, u8 command, int protocol, union i2c_smbus_data *data)
{
	s32 res;

	flags &= I2C_M_TEN | I2C_CLIENT_PEC;
	i2c_lock_bus(adapter, I2C_LOCK_SEGMENT);
	if (adapter->algo->smbus_xfer)
		res = adapter->algo->smbus_xfer(adapter, addr, flags, read_write, command,
						protocol, data);
	else
		res = lkpi_i2c_smbus_emulated(adapter, addr, flags, read_write, command, protocol,
					      data);
	i2c_unlock_bus(adapter, I2C_LOCK_SEGMENT);
	return res;
}
EXPORT_SYMBOL(lkpi_i2c_smbus_xfer);

s32 lkpi_i2c_smbus_read_byte(const struct i2c_client *client)
{
	union i2c_smbus_data data;
	int status = lkpi_i2c_smbus_xfer(client->adapter, client->addr, client->flags,
					 I2C_SMBUS_READ, 0, I2C_SMBUS_BYTE, &data);
	return status < 0 ? status : data.byte;
}
EXPORT_SYMBOL(lkpi_i2c_smbus_read_byte);

s32 lkpi_i2c_smbus_write_byte(const struct i2c_client *client, u8 value)
{
	return lkpi_i2c_smbus_xfer(client->adapter, client->addr, client->flags,
				   I2C_SMBUS_WRITE, value, I2C_SMBUS_BYTE, NULL);
}
EXPORT_SYMBOL(lkpi_i2c_smbus_write_byte);

s32 lkpi_i2c_smbus_read_byte_data(const struct i2c_client *client, u8 command)
{
	union i2c_smbus_data data;
	int status = lkpi_i2c_smbus_xfer(client->adapter, client->addr, client->flags,
					 I2C_SMBUS_READ, command, I2C_SMBUS_BYTE_DATA, &data);
	return status < 0 ? status : data.byte;
}
EXPORT_SYMBOL(lkpi_i2c_smbus_read_byte_data);

s32 lkpi_i2c_smbus_write_byte_data(const struct i2c_client *client, u8 command, u8 value)
{
	union i2c_smbus_data data = { .byte = value };

	return lkpi_i2c_smbus_xfer(client->adapter, client->addr, client->flags,
				   I2C_SMBUS_WRITE, command, I2C_SMBUS_BYTE_DATA, &data);
}
EXPORT_SYMBOL(lkpi_i2c_smbus_write_byte_data);

s32 lkpi_i2c_smbus_read_word_data(const struct i2c_client *client, u8 command)
{
	union i2c_smbus_data data;
	int status = lkpi_i2c_smbus_xfer(client->adapter, client->addr, client->flags,
					 I2C_SMBUS_READ, command, I2C_SMBUS_WORD_DATA, &data);
	return status < 0 ? status : data.word;
}
EXPORT_SYMBOL(lkpi_i2c_smbus_read_word_data);

s32 lkpi_i2c_smbus_write_word_data(const struct i2c_client *client, u8 command, u16 value)
{
	union i2c_smbus_data data = { .word = value };

	return lkpi_i2c_smbus_xfer(client->adapter, client->addr, client->flags,
				   I2C_SMBUS_WRITE, command, I2C_SMBUS_WORD_DATA, &data);
}
EXPORT_SYMBOL(lkpi_i2c_smbus_write_word_data);

s32 lkpi_i2c_smbus_read_i2c_block_data(const struct i2c_client *client, u8 command, u8 length,
				       u8 *values)
{
	union i2c_smbus_data data;
	int status;

	if (length > I2C_SMBUS_BLOCK_MAX)
		length = I2C_SMBUS_BLOCK_MAX;
	data.block[0] = length;
	status = lkpi_i2c_smbus_xfer(client->adapter, client->addr, client->flags,
				     I2C_SMBUS_READ, command, I2C_SMBUS_I2C_BLOCK_DATA, &data);
	if (status < 0)
		return status;
	memcpy(values, &data.block[1], data.block[0]);
	return data.block[0];
}
EXPORT_SYMBOL(lkpi_i2c_smbus_read_i2c_block_data);

s32 lkpi_i2c_smbus_write_i2c_block_data(const struct i2c_client *client, u8 command,
					u8 length, const u8 *values)
{
	union i2c_smbus_data data;

	if (length > I2C_SMBUS_BLOCK_MAX)
		length = I2C_SMBUS_BLOCK_MAX;
	data.block[0] = length;
	memcpy(data.block + 1, values, length);
	return lkpi_i2c_smbus_xfer(client->adapter, client->addr, client->flags,
				   I2C_SMBUS_WRITE, command, I2C_SMBUS_I2C_BLOCK_DATA, &data);
}
EXPORT_SYMBOL(lkpi_i2c_smbus_write_i2c_block_data);

int lkpi_i2c_init(void)
{
	return bus_register(&lkpi_i2c_bus_type);
}

void lkpi_i2c_exit(void)
{
	bus_unregister(&lkpi_i2c_bus_type);
	idr_destroy(&lkpi_i2c_adapter_idr);
}
