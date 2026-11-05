// SPDX-License-Identifier: GPL-2.0-only
/*
 * Mini phylib + MDIO de la capa LinuxKPI-NET (Plan_Red.md). Lo justo para
 * que un MAC con PHY C22 integrado (r8169, e1000e/igc usan el suyo propio)
 * funcione con el phylib de Linux sin compilar phy.c/phy_device.c/mdio_bus.c:
 *
 *  - mdiobus: acceso C22/C45 con mdio_lock, sondeo de direcciones y un
 *    phy_device generico por PHY encontrado.
 *  - PHY generico: soft reset, anuncio/auto-negociacion, estado del enlace
 *    por sondeo (1 s, o al instante con phy_mac_interrupt()) y paginas
 *    Realtek (registro 0x1f) para el codigo de r8169_phy_config.c.
 *  - Sin interrupciones de PHY, fixups, SFP, LEDs ni PHY drivers externos.
 */
#include <linux/kernel.h>
#include <linux/export.h>
#include <linux/netdevice.h>
#include <linux/phy.h>
#include <linux/mdio.h>
#include <linux/mii.h>
#include <linux/ethtool.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <linux/delay.h>

#include "../lkpi_internal.h"

#define RTL_PAGE_SELECT 0x1f

/* ------------------------------------------------------------- mdiobus */

struct mii_bus *mdiobus_alloc_size(size_t size)
{
	struct mii_bus *bus = kzalloc(sizeof(*bus) + size, GFP_KERNEL);

	if (!bus)
		return NULL;
	if (size)
		bus->priv = (void *)(bus + 1);
	mutex_init(&bus->mdio_lock);
	bus->state = MDIOBUS_ALLOCATED;
	device_initialize(&bus->dev);
	return bus;
}
EXPORT_SYMBOL(mdiobus_alloc_size);

static void lkpi_devm_free_mdiobus(struct device *d, void *res)
{
	struct mii_bus **p = res;
	kfree(*p);
}

struct mii_bus *devm_mdiobus_alloc_size(struct device *dev, int sizeof_priv)
{
	struct mii_bus *bus = mdiobus_alloc_size(sizeof_priv);
	struct mii_bus **p;

	if (!bus)
		return NULL;
	p = devres_alloc(lkpi_devm_free_mdiobus, sizeof(*p), GFP_KERNEL);
	if (!p) {
		kfree(bus);
		return NULL;
	}
	*p = bus;
	devres_add(dev, p);
	return bus;
}
EXPORT_SYMBOL(devm_mdiobus_alloc_size);

int __mdiobus_read(struct mii_bus *bus, int addr, u32 regnum)
{
	if (!bus->read)
		return -EOPNOTSUPP;
	return bus->read(bus, addr, regnum);
}
EXPORT_SYMBOL(__mdiobus_read);

int __mdiobus_write(struct mii_bus *bus, int addr, u32 regnum, u16 val)
{
	if (!bus->write)
		return -EOPNOTSUPP;
	return bus->write(bus, addr, regnum, val);
}
EXPORT_SYMBOL(__mdiobus_write);

int __mdiobus_c45_read(struct mii_bus *bus, int addr, int devad, u32 regnum)
{
	if (!bus->read_c45)
		return -EOPNOTSUPP;
	return bus->read_c45(bus, addr, devad, regnum);
}
int __mdiobus_c45_write(struct mii_bus *bus, int addr, int devad, u32 regnum, u16 val)
{
	if (!bus->write_c45)
		return -EOPNOTSUPP;
	return bus->write_c45(bus, addr, devad, regnum, val);
}
EXPORT_SYMBOL(__mdiobus_c45_read);
EXPORT_SYMBOL(__mdiobus_c45_write);

int mdiobus_read(struct mii_bus *bus, int addr, u32 regnum)
{
	int r;
	mutex_lock(&bus->mdio_lock);
	r = __mdiobus_read(bus, addr, regnum);
	mutex_unlock(&bus->mdio_lock);
	return r;
}
EXPORT_SYMBOL(mdiobus_read);

int mdiobus_write(struct mii_bus *bus, int addr, u32 regnum, u16 val)
{
	int r;
	mutex_lock(&bus->mdio_lock);
	r = __mdiobus_write(bus, addr, regnum, val);
	mutex_unlock(&bus->mdio_lock);
	return r;
}
EXPORT_SYMBOL(mdiobus_write);

/* ---------------------------------------------------- registros del PHY */

int __phy_modify(struct phy_device *phydev, u32 regnum, u16 mask, u16 set)
{
	int ret = __phy_read(phydev, regnum);

	if (ret < 0)
		return ret;
	ret = (ret & ~mask) | set;
	return __phy_write(phydev, regnum, ret);
}
EXPORT_SYMBOL(__phy_modify);

int phy_modify(struct phy_device *phydev, u32 regnum, u16 mask, u16 set)
{
	int ret;
	phy_lock_mdio_bus(phydev);
	ret = __phy_modify(phydev, regnum, mask, set);
	phy_unlock_mdio_bus(phydev);
	return ret;
}
EXPORT_SYMBOL(phy_modify);

/* MMD: C45 directo si el bus lo tiene; si no, acceso indirecto C22 (reg 13/14). */
static int lkpi_mmd_c22(struct phy_device *phydev, int devad, u16 regnum, int *val, bool wr)
{
	struct mii_bus *bus = phydev->mdio.bus;
	int a = phydev->mdio.addr, ret;

	ret = __mdiobus_write(bus, a, MII_MMD_CTRL, devad);
	if (ret) return ret;
	ret = __mdiobus_write(bus, a, MII_MMD_DATA, regnum);
	if (ret) return ret;
	ret = __mdiobus_write(bus, a, MII_MMD_CTRL, MII_MMD_CTRL_NOINCR | devad);
	if (ret) return ret;
	if (wr)
		return __mdiobus_write(bus, a, MII_MMD_DATA, *val);
	ret = __mdiobus_read(bus, a, MII_MMD_DATA);
	if (ret < 0) return ret;
	*val = ret;
	return 0;
}

int __phy_read_mmd(struct phy_device *phydev, int devad, u32 regnum)
{
	int v = 0, ret;

	if (phydev->mdio.bus->read_c45)
		return __mdiobus_c45_read(phydev->mdio.bus, phydev->mdio.addr, devad, regnum);
	ret = lkpi_mmd_c22(phydev, devad, regnum, &v, false);
	return ret ? ret : v;
}
EXPORT_SYMBOL(__phy_read_mmd);

int phy_read_mmd(struct phy_device *phydev, int devad, u32 regnum)
{
	int r;
	phy_lock_mdio_bus(phydev);
	r = __phy_read_mmd(phydev, devad, regnum);
	phy_unlock_mdio_bus(phydev);
	return r;
}
EXPORT_SYMBOL(phy_read_mmd);

int __phy_write_mmd(struct phy_device *phydev, int devad, u32 regnum, u16 val)
{
	int v = val;

	if (phydev->mdio.bus->write_c45)
		return __mdiobus_c45_write(phydev->mdio.bus, phydev->mdio.addr, devad, regnum, val);
	return lkpi_mmd_c22(phydev, devad, regnum, &v, true);
}
EXPORT_SYMBOL(__phy_write_mmd);

int phy_write_mmd(struct phy_device *phydev, int devad, u32 regnum, u16 val)
{
	int r;
	phy_lock_mdio_bus(phydev);
	r = __phy_write_mmd(phydev, devad, regnum, val);
	phy_unlock_mdio_bus(phydev);
	return r;
}
EXPORT_SYMBOL(phy_write_mmd);

int __phy_modify_mmd(struct phy_device *phydev, int devad, u32 regnum, u16 mask, u16 set)
{
	int ret = __phy_read_mmd(phydev, devad, regnum);

	if (ret < 0)
		return ret;
	return __phy_write_mmd(phydev, devad, regnum, (ret & ~mask) | set);
}
EXPORT_SYMBOL(__phy_modify_mmd);

int phy_modify_mmd(struct phy_device *phydev, int devad, u32 regnum, u16 mask, u16 set)
{
	int r;
	phy_lock_mdio_bus(phydev);
	r = __phy_modify_mmd(phydev, devad, regnum, mask, set);
	phy_unlock_mdio_bus(phydev);
	return r;
}
EXPORT_SYMBOL(phy_modify_mmd);

/* Paginas (semantica de Linux: phy-core.c). El PHY generico usa la pagina
 * Realtek (reg 0x1f), que es lo unico que r8169_phy_config.c necesita. */
static int lkpi_read_page(struct phy_device *p) { return __phy_read(p, RTL_PAGE_SELECT); }
static int lkpi_write_page(struct phy_device *p, int page) { return __phy_write(p, RTL_PAGE_SELECT, page); }

int phy_select_page(struct phy_device *phydev, int page)
{
	int ret, oldpage;

	phy_lock_mdio_bus(phydev);
	oldpage = lkpi_read_page(phydev);
	if (oldpage < 0)
		goto out;
	if (oldpage != page) {
		ret = lkpi_write_page(phydev, page);
		if (ret < 0)
			oldpage = ret;
	}
	return oldpage;
out:
	phy_unlock_mdio_bus(phydev);
	return oldpage;
}
EXPORT_SYMBOL(phy_select_page);

int phy_restore_page(struct phy_device *phydev, int oldpage, int ret)
{
	int r;

	if (oldpage >= 0) {
		r = lkpi_write_page(phydev, oldpage);
		if (ret >= 0 && r < 0)
			ret = r;
	} else {
		ret = oldpage;
	}
	phy_unlock_mdio_bus(phydev);
	return ret;
}
EXPORT_SYMBOL(phy_restore_page);

int phy_read_paged(struct phy_device *phydev, int page, u32 regnum)
{
	int ret = 0, oldpage = phy_select_page(phydev, page);

	if (oldpage >= 0)
		ret = __phy_read(phydev, regnum);
	return phy_restore_page(phydev, oldpage, ret);
}
EXPORT_SYMBOL(phy_read_paged);

int phy_write_paged(struct phy_device *phydev, int page, u32 regnum, u16 val)
{
	int ret = 0, oldpage = phy_select_page(phydev, page);

	if (oldpage >= 0)
		ret = __phy_write(phydev, regnum, val);
	return phy_restore_page(phydev, oldpage, ret);
}
EXPORT_SYMBOL(phy_write_paged);

int phy_modify_paged_changed(struct phy_device *phydev, int page, u32 regnum, u16 mask, u16 set)
{
	int ret = 0, oldpage = phy_select_page(phydev, page);

	if (oldpage >= 0)
		ret = __phy_modify_changed(phydev, regnum, mask, set);
	return phy_restore_page(phydev, oldpage, ret);
}
EXPORT_SYMBOL(phy_modify_paged_changed);

int phy_modify_paged(struct phy_device *phydev, int page, u32 regnum, u16 mask, u16 set)
{
	int ret = phy_modify_paged_changed(phydev, page, regnum, mask, set);
	return ret < 0 ? ret : 0;
}
EXPORT_SYMBOL(phy_modify_paged);


/* ----------------------------------------------------- PHY generico */

static struct phy_driver lkpi_genphy = {
	.name = "Generic PHY (LinuxKPI)",
	.phy_id = 0,
	.phy_id_mask = 0,
};

static void lkpi_phy_setup_modes(struct phy_device *p)
{
	int bmsr = phy_read(p, MII_BMSR), est = 0;

	if (bmsr < 0)
		bmsr = 0;
	if (bmsr & BMSR_ESTATEN)
		est = phy_read(p, MII_ESTATUS);
	linkmode_zero(p->supported);
	linkmode_set_bit(ETHTOOL_LINK_MODE_Autoneg_BIT, p->supported);
	linkmode_set_bit(ETHTOOL_LINK_MODE_TP_BIT, p->supported);
	linkmode_set_bit(ETHTOOL_LINK_MODE_MII_BIT, p->supported);
	if (bmsr & BMSR_10HALF) linkmode_set_bit(ETHTOOL_LINK_MODE_10baseT_Half_BIT, p->supported);
	if (bmsr & BMSR_10FULL) linkmode_set_bit(ETHTOOL_LINK_MODE_10baseT_Full_BIT, p->supported);
	if (bmsr & BMSR_100HALF) linkmode_set_bit(ETHTOOL_LINK_MODE_100baseT_Half_BIT, p->supported);
	if (bmsr & BMSR_100FULL) linkmode_set_bit(ETHTOOL_LINK_MODE_100baseT_Full_BIT, p->supported);
	if (est > 0) {
		if (est & ESTATUS_1000_THALF) linkmode_set_bit(ETHTOOL_LINK_MODE_1000baseT_Half_BIT, p->supported);
		if (est & ESTATUS_1000_TFULL) linkmode_set_bit(ETHTOOL_LINK_MODE_1000baseT_Full_BIT, p->supported);
		p->is_gigabit_capable = 1;
	}
	linkmode_set_bit(ETHTOOL_LINK_MODE_Pause_BIT, p->supported);
	linkmode_set_bit(ETHTOOL_LINK_MODE_Asym_Pause_BIT, p->supported);
	linkmode_copy(p->advertising, p->supported);
	/* Pause se anuncia solo si el MAC lo pide (phy_support_asym_pause) */
	linkmode_clear_bit(ETHTOOL_LINK_MODE_Pause_BIT, p->advertising);
	linkmode_clear_bit(ETHTOOL_LINK_MODE_Asym_Pause_BIT, p->advertising);
	p->autoneg = AUTONEG_ENABLE;
	p->speed = SPEED_UNKNOWN;
	p->duplex = DUPLEX_UNKNOWN;
}

static void lkpi_phy_poll(struct work_struct *w);

int __mdiobus_register(struct mii_bus *bus, struct module *owner)
{
	int addr;

	bus->state = MDIOBUS_REGISTERED;
	for (addr = 0; addr < PHY_MAX_ADDR; addr++) {
		struct phy_device *p;
		int id1, id2;

		if (bus->phy_mask & (1U << addr))
			continue;
		id1 = mdiobus_read(bus, addr, MII_PHYSID1);
		if (id1 < 0 || id1 == 0xffff)
			continue;
		id2 = mdiobus_read(bus, addr, MII_PHYSID2);
		if (id2 < 0 || id2 == 0xffff)
			continue;
		p = kzalloc(sizeof(*p), GFP_KERNEL);
		if (!p)
			return -ENOMEM;
		p->mdio.bus = bus;
		p->mdio.addr = addr;
		p->phy_id = (id1 << 16) | id2;
		p->drv = &lkpi_genphy;
		p->state = PHY_READY;
		p->interface = PHY_INTERFACE_MODE_GMII;
		mutex_init(&p->lock);
		INIT_DELAYED_WORK(&p->state_queue, lkpi_phy_poll);
		lkpi_phy_setup_modes(p);
		bus->mdio_map[addr] = (struct mdio_device *)p;
		pr_info("PHY %d: id %08x\n", addr, p->phy_id);
	}
	return 0;
}
EXPORT_SYMBOL(__mdiobus_register);

static void lkpi_devm_unreg_mdiobus(struct device *d, void *res)
{
	struct mii_bus **p = res;
	struct mii_bus *bus = *p;
	int i;

	for (i = 0; i < PHY_MAX_ADDR; i++) {
		struct phy_device *ph = (struct phy_device *)bus->mdio_map[i];
		if (ph) {
			cancel_delayed_work_sync(&ph->state_queue);
			kfree(ph);
			bus->mdio_map[i] = NULL;
		}
	}
	bus->state = MDIOBUS_UNREGISTERED;
}

int __devm_mdiobus_register(struct device *dev, struct mii_bus *bus, struct module *owner)
{
	struct mii_bus **p;
	int ret = __mdiobus_register(bus, owner);

	if (ret)
		return ret;
	p = devres_alloc(lkpi_devm_unreg_mdiobus, sizeof(*p), GFP_KERNEL);
	if (!p)
		return -ENOMEM;
	*p = bus;
	devres_add(dev, p);
	return 0;
}
EXPORT_SYMBOL(__devm_mdiobus_register);

struct phy_device *mdiobus_get_phy(struct mii_bus *bus, int addr)
{
	if (addr < 0 || addr >= PHY_MAX_ADDR)
		return NULL;
	return (struct phy_device *)bus->mdio_map[addr];
}
EXPORT_SYMBOL(mdiobus_get_phy);

/* ---- genphy ---- */

int genphy_soft_reset(struct phy_device *phydev)
{
	int ret, tries = 600;

	ret = phy_write(phydev, MII_BMCR, BMCR_RESET);
	if (ret < 0)
		return ret;
	do {
		msleep(1);
		ret = phy_read(phydev, MII_BMCR);
		if (ret < 0)
			return ret;
	} while ((ret & BMCR_RESET) && --tries);
	return (ret & BMCR_RESET) ? -ETIMEDOUT : 0;
}
EXPORT_SYMBOL(genphy_soft_reset);

static u16 lkpi_adv_to_mii(const unsigned long *adv)
{
	u16 v = ADVERTISE_CSMA;

	if (linkmode_test_bit(ETHTOOL_LINK_MODE_10baseT_Half_BIT, adv)) v |= ADVERTISE_10HALF;
	if (linkmode_test_bit(ETHTOOL_LINK_MODE_10baseT_Full_BIT, adv)) v |= ADVERTISE_10FULL;
	if (linkmode_test_bit(ETHTOOL_LINK_MODE_100baseT_Half_BIT, adv)) v |= ADVERTISE_100HALF;
	if (linkmode_test_bit(ETHTOOL_LINK_MODE_100baseT_Full_BIT, adv)) v |= ADVERTISE_100FULL;
	if (linkmode_test_bit(ETHTOOL_LINK_MODE_Pause_BIT, adv)) v |= ADVERTISE_PAUSE_CAP;
	if (linkmode_test_bit(ETHTOOL_LINK_MODE_Asym_Pause_BIT, adv)) v |= ADVERTISE_PAUSE_ASYM;
	return v;
}

static int lkpi_config_aneg(struct phy_device *p)
{
	u16 adv = lkpi_adv_to_mii(p->advertising);
	u16 g = 0;
	int ret;

	if (!p->autoneg) {
		u16 bmcr = 0;
		if (p->speed == SPEED_100) bmcr |= BMCR_SPEED100;
		if (p->duplex == DUPLEX_FULL) bmcr |= BMCR_FULLDPLX;
		return phy_write(p, MII_BMCR, bmcr);
	}
	ret = phy_modify(p, MII_ADVERTISE, ADVERTISE_ALL | ADVERTISE_PAUSE_CAP | ADVERTISE_PAUSE_ASYM, adv & ~ADVERTISE_CSMA);
	if (ret < 0)
		return ret;
	if (p->is_gigabit_capable) {
		if (linkmode_test_bit(ETHTOOL_LINK_MODE_1000baseT_Half_BIT, p->advertising)) g |= ADVERTISE_1000HALF;
		if (linkmode_test_bit(ETHTOOL_LINK_MODE_1000baseT_Full_BIT, p->advertising)) g |= ADVERTISE_1000FULL;
		ret = phy_modify(p, MII_CTRL1000, ADVERTISE_1000FULL | ADVERTISE_1000HALF, g);
		if (ret < 0)
			return ret;
	}
	return phy_modify(p, MII_BMCR, BMCR_ISOLATE | BMCR_PDOWN, 0) < 0 ? -EIO :
	       phy_modify(p, MII_BMCR, 0, BMCR_ANENABLE | BMCR_ANRESTART);
}

static int lkpi_read_status(struct phy_device *p)
{
	int bmsr, lpa, stat1000 = 0, ctrl1000 = 0, adv;
	int old_link = p->link;

	bmsr = phy_read(p, MII_BMSR);
	bmsr = phy_read(p, MII_BMSR);		/* el bit de enlace es "latched" */
	if (bmsr < 0)
		return bmsr;
	p->link = !!(bmsr & BMSR_LSTATUS);
	p->autoneg_complete = !!(bmsr & BMSR_ANEGCOMPLETE);
	if (!p->link) {
		p->speed = SPEED_UNKNOWN;
		p->duplex = DUPLEX_UNKNOWN;
		p->pause = p->asym_pause = 0;
		return old_link != p->link;
	}
	if (p->autoneg) {
		lpa = phy_read(p, MII_LPA);
		adv = phy_read(p, MII_ADVERTISE);
		if (p->is_gigabit_capable) {
			stat1000 = phy_read(p, MII_STAT1000);
			ctrl1000 = phy_read(p, MII_CTRL1000);
		}
		if (lpa < 0 || adv < 0)
			return -EIO;
		p->speed = SPEED_10;
		p->duplex = DUPLEX_HALF;
		lpa &= adv;
		if ((stat1000 & LPA_1000FULL) && (ctrl1000 & ADVERTISE_1000FULL)) { p->speed = SPEED_1000; p->duplex = DUPLEX_FULL; }
		else if ((stat1000 & LPA_1000HALF) && (ctrl1000 & ADVERTISE_1000HALF)) { p->speed = SPEED_1000; }
		else if (lpa & (LPA_100FULL | LPA_100HALF)) { p->speed = SPEED_100; p->duplex = (lpa & LPA_100FULL) ? DUPLEX_FULL : DUPLEX_HALF; }
		else if (lpa & LPA_10FULL) p->duplex = DUPLEX_FULL;
		p->pause = p->asym_pause = 0;
		if (p->duplex == DUPLEX_FULL) {
			p->pause = !!(lpa & LPA_PAUSE_CAP);
			p->asym_pause = !!(lpa & LPA_PAUSE_ASYM);
		}
	} else {
		int bmcr = phy_read(p, MII_BMCR);
		if (bmcr < 0)
			return bmcr;
		p->speed = (bmcr & BMCR_SPEED1000) ? SPEED_1000 : (bmcr & BMCR_SPEED100) ? SPEED_100 : SPEED_10;
		p->duplex = (bmcr & BMCR_FULLDPLX) ? DUPLEX_FULL : DUPLEX_HALF;
	}
	return 0;
}

void phy_print_status(struct phy_device *p)
{
	if (p->link)
		netdev_info(p->attached_dev, "Link is Up - %dMbps/%s\n", p->speed,
			    p->duplex == DUPLEX_FULL ? "Full" : "Half");
	else
		netdev_info(p->attached_dev, "Link is Down\n");
}
EXPORT_SYMBOL(phy_print_status);

static void lkpi_phy_poll(struct work_struct *w)
{
	struct phy_device *p = container_of(to_delayed_work(w), struct phy_device, state_queue);
	int old_link = p->link, old_speed = p->speed, old_dup = p->duplex;

	if (p->state != PHY_UP && p->state != PHY_RUNNING && p->state != PHY_NOLINK)
		return;
	mutex_lock(&p->lock);
	lkpi_read_status(p);
	mutex_unlock(&p->lock);
	p->state = p->link ? PHY_RUNNING : PHY_NOLINK;
	if ((old_link != p->link || old_speed != p->speed || old_dup != p->duplex) && p->adjust_link)
		p->adjust_link(p->attached_dev);
	if (p->state != PHY_HALTED)
		schedule_delayed_work(&p->state_queue, HZ);
}

int phy_connect_direct(struct net_device *dev, struct phy_device *phydev,
		       void (*handler)(struct net_device *), phy_interface_t interface)
{
	phydev->attached_dev = dev;
	dev->phydev = phydev;
	phydev->adjust_link = handler;
	phydev->interface = interface;
	phydev->state = PHY_READY;
	return 0;
}
EXPORT_SYMBOL(phy_connect_direct);

void phy_disconnect(struct phy_device *phydev)
{
	phy_stop(phydev);
	phydev->adjust_link = NULL;
	if (phydev->attached_dev)
		phydev->attached_dev->phydev = NULL;
	phydev->attached_dev = NULL;
}
EXPORT_SYMBOL(phy_disconnect);

void phy_start(struct phy_device *phydev)
{
	phydev->state = PHY_UP;
	phydev->link = 0;
	schedule_delayed_work(&phydev->state_queue, 0);
}
EXPORT_SYMBOL(phy_start);

void phy_stop(struct phy_device *phydev)
{
	phydev->state = PHY_HALTED;
	cancel_delayed_work_sync(&phydev->state_queue);
	if (phydev->link) {
		phydev->link = 0;
		if (phydev->adjust_link)
			phydev->adjust_link(phydev->attached_dev);
	}
}
EXPORT_SYMBOL(phy_stop);

void phy_mac_interrupt(struct phy_device *phydev)
{
	if (phydev->state == PHY_UP || phydev->state == PHY_RUNNING || phydev->state == PHY_NOLINK)
		mod_delayed_work(system_wq, &phydev->state_queue, 0);
}
EXPORT_SYMBOL(phy_mac_interrupt);

int phy_start_aneg(struct phy_device *phydev)
{
	int ret;

	mutex_lock(&phydev->lock);
	ret = lkpi_config_aneg(phydev);
	mutex_unlock(&phydev->lock);
	if (phydev->state == PHY_UP || phydev->state == PHY_RUNNING || phydev->state == PHY_NOLINK)
		mod_delayed_work(system_wq, &phydev->state_queue, HZ / 4);
	return ret;
}
EXPORT_SYMBOL(phy_start_aneg);

int phy_init_hw(struct phy_device *phydev)
{
	return 0;
}
EXPORT_SYMBOL(phy_init_hw);

int phy_suspend(struct phy_device *phydev)
{
	int r = phy_set_bits(phydev, MII_BMCR, BMCR_PDOWN);
	phydev->suspended = 1;
	return r < 0 ? r : 0;
}
EXPORT_SYMBOL(phy_suspend);

int phy_resume(struct phy_device *phydev)
{
	int r = phy_clear_bits(phydev, MII_BMCR, BMCR_PDOWN);
	phydev->suspended = 0;
	return r < 0 ? r : 0;
}
EXPORT_SYMBOL(phy_resume);

/* Reducir/restaurar la velocidad para WoL: no se usa en la capa (sin
 * suspension a RAM con la NIC): devuelven exito sin tocar el PHY. */
int phy_speed_down(struct phy_device *phydev, bool sync) { return 0; }
int phy_speed_up(struct phy_device *phydev) { return 0; }
EXPORT_SYMBOL(phy_speed_down);
EXPORT_SYMBOL(phy_speed_up);

void phy_set_max_speed(struct phy_device *phydev, u32 max_speed)
{
	if (max_speed < SPEED_1000) {
		linkmode_clear_bit(ETHTOOL_LINK_MODE_1000baseT_Half_BIT, phydev->supported);
		linkmode_clear_bit(ETHTOOL_LINK_MODE_1000baseT_Full_BIT, phydev->supported);
		linkmode_clear_bit(ETHTOOL_LINK_MODE_1000baseT_Half_BIT, phydev->advertising);
		linkmode_clear_bit(ETHTOOL_LINK_MODE_1000baseT_Full_BIT, phydev->advertising);
	}
	if (max_speed < SPEED_100) {
		linkmode_clear_bit(ETHTOOL_LINK_MODE_100baseT_Half_BIT, phydev->supported);
		linkmode_clear_bit(ETHTOOL_LINK_MODE_100baseT_Full_BIT, phydev->supported);
		linkmode_clear_bit(ETHTOOL_LINK_MODE_100baseT_Half_BIT, phydev->advertising);
		linkmode_clear_bit(ETHTOOL_LINK_MODE_100baseT_Full_BIT, phydev->advertising);
	}
}
EXPORT_SYMBOL(phy_set_max_speed);

void phy_attached_info(struct phy_device *phydev)
{
	netdev_info(phydev->attached_dev, "attached PHY driver [%s] (mii_bus:phy_addr=%d, id=%08x)\n",
		    phydev->drv ? phydev->drv->name : "?", phydev->mdio.addr, phydev->phy_id);
}
EXPORT_SYMBOL(phy_attached_info);

/* Pausa ---------------------------------------------------------------- */

void phy_support_asym_pause(struct phy_device *phydev)
{
	linkmode_set_bit(ETHTOOL_LINK_MODE_Pause_BIT, phydev->supported);
	linkmode_set_bit(ETHTOOL_LINK_MODE_Asym_Pause_BIT, phydev->supported);
}
EXPORT_SYMBOL(phy_support_asym_pause);

void phy_set_asym_pause(struct phy_device *phydev, bool rx, bool tx)
{
	linkmode_clear_bit(ETHTOOL_LINK_MODE_Pause_BIT, phydev->advertising);
	linkmode_clear_bit(ETHTOOL_LINK_MODE_Asym_Pause_BIT, phydev->advertising);
	if (rx) linkmode_set_bit(ETHTOOL_LINK_MODE_Pause_BIT, phydev->advertising);
	if (rx != tx) linkmode_set_bit(ETHTOOL_LINK_MODE_Asym_Pause_BIT, phydev->advertising);
	if (phydev->autoneg)
		phy_start_aneg(phydev);
}
EXPORT_SYMBOL(phy_set_asym_pause);

void phy_get_pause(struct phy_device *phydev, bool *tx_pause, bool *rx_pause)
{
	*tx_pause = *rx_pause = false;
	if (phydev->duplex != DUPLEX_FULL)
		return;
	*rx_pause = phydev->pause;
	*tx_pause = phydev->pause != phydev->asym_pause;
}
EXPORT_SYMBOL(phy_get_pause);

/* EEE: el PHY generico no lo anuncia; r8169 lo gestiona con sus registros. */
void phy_support_eee(struct phy_device *phydev) { phydev->eee_cfg.eee_enabled = true; }
void phy_disable_eee(struct phy_device *phydev) { phydev->eee_cfg.eee_enabled = false; }
int phy_ethtool_get_eee(struct phy_device *phydev, struct ethtool_keee *data) { return 0; }
int phy_ethtool_set_eee(struct phy_device *phydev, struct ethtool_keee *data) { return 0; }
EXPORT_SYMBOL(phy_support_eee);
EXPORT_SYMBOL(phy_disable_eee);
EXPORT_SYMBOL(phy_ethtool_get_eee);
EXPORT_SYMBOL(phy_ethtool_set_eee);

/* ethtool --------------------------------------------------------------- */

int phy_ethtool_get_link_ksettings(struct net_device *ndev, struct ethtool_link_ksettings *cmd)
{
	struct phy_device *p = ndev->phydev;

	if (!p)
		return -ENODEV;
	linkmode_copy(cmd->link_modes.supported, p->supported);
	linkmode_copy(cmd->link_modes.advertising, p->advertising);
	linkmode_copy(cmd->link_modes.lp_advertising, p->lp_advertising);
	cmd->base.speed = p->link ? p->speed : SPEED_UNKNOWN;
	cmd->base.duplex = p->link ? p->duplex : DUPLEX_UNKNOWN;
	cmd->base.port = PORT_TP;
	cmd->base.phy_address = p->mdio.addr;
	cmd->base.autoneg = p->autoneg;
	return 0;
}
EXPORT_SYMBOL(phy_ethtool_get_link_ksettings);

int phy_ethtool_set_link_ksettings(struct net_device *ndev, const struct ethtool_link_ksettings *cmd)
{
	struct phy_device *p = ndev->phydev;

	if (!p)
		return -ENODEV;
	p->autoneg = cmd->base.autoneg;
	if (p->autoneg)
		linkmode_copy(p->advertising, cmd->link_modes.advertising);
	else {
		p->speed = cmd->base.speed;
		p->duplex = cmd->base.duplex;
	}
	return phy_start_aneg(p);
}
EXPORT_SYMBOL(phy_ethtool_set_link_ksettings);

int phy_ethtool_nway_reset(struct net_device *ndev)
{
	if (!ndev->phydev)
		return -ENODEV;
	return phy_start_aneg(ndev->phydev);
}
EXPORT_SYMBOL(phy_ethtool_nway_reset);

int phy_do_ioctl_running(struct net_device *dev, struct ifreq *ifr, int cmd)
{
	return -EOPNOTSUPP;
}
EXPORT_SYMBOL(phy_do_ioctl_running);
