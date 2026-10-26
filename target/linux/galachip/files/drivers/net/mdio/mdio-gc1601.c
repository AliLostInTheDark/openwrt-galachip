// SPDX-License-Identifier: GPL-2.0-only
#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of_mdio.h>
#include <linux/phy.h>
#include <linux/platform_device.h>

#define GC_MDIO_CFG	0x00
#define GC_MDIO_WR_DATA	0x04
#define GC_MDIO_RD_DATA	0x08
#define GC_MDIO_STATUS	0x0c
#define GC_MDIO_WR_EN	0x10
#define GC_MDIO_RD_EN	0x14
#define GC_MDIO_ENABLE	BIT(1)
#define GC_MDIO_REG	GENMASK(7, 3)
#define GC_MDIO_PHY	GENMASK(12, 8)
#define GC_MDIO_RD_DONE	BIT(0)
#define GC_MDIO_WR_DONE	BIT(8)
#define GC_MDIO_BUSY	BIT(15)

struct gc1601_mdio {
	void __iomem *base;
};

static int gc1601_mdio_transfer(struct mii_bus *bus, int phy, int reg,
				bool write, u16 value)
{
	struct gc1601_mdio *mdio = bus->priv;
	void __iomem *base = mdio->base;
	u32 status, done = write ? GC_MDIO_WR_DONE : GC_MDIO_RD_DONE;
	u32 trigger = write ? GC_MDIO_WR_EN : GC_MDIO_RD_EN;
	int ret;

	if (phy < 0 || phy > 31 || reg < 0 || reg > 31)
		return -EINVAL;
	ret = readl_poll_timeout(base + GC_MDIO_STATUS, status,
				 !(status & GC_MDIO_BUSY), 1000, 256000);
	if (ret)
		return ret;
	writel(status & ~done, base + GC_MDIO_STATUS);
	writel(GC_MDIO_ENABLE | FIELD_PREP(GC_MDIO_PHY, phy) |
	       FIELD_PREP(GC_MDIO_REG, reg), base + GC_MDIO_CFG);
	if (write)
		writel(value, base + GC_MDIO_WR_DATA);
	usleep_range(1000, 1500);
	ret = readl_poll_timeout(base + GC_MDIO_STATUS, status,
				 !(status & GC_MDIO_BUSY), 1000, 256000);
	if (ret)
		return ret;
	writel(1, base + trigger);
	ret = readl_poll_timeout(base + GC_MDIO_STATUS, status,
				 status & done, 1000, 256000);
	if (!ret && !write)
		ret = readl(base + GC_MDIO_RD_DATA) & 0xffff;
	writel(0, base + trigger);
	return ret;
}

static int gc1601_mdio_read(struct mii_bus *bus, int phy, int reg)
{
	return gc1601_mdio_transfer(bus, phy, reg, false, 0);
}

static int gc1601_mdio_write(struct mii_bus *bus, int phy, int reg, u16 value)
{
	return gc1601_mdio_transfer(bus, phy, reg, true, value);
}

static ssize_t phy_status_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct mii_bus *bus = dev_get_drvdata(dev);
	struct phy_device *phy = phy_find_first(bus);
	int addr;
	int id1, id2, control, status, vendor;

	if (!phy)
		return -ENODEV;
	addr = phy->mdio.addr;
	id1 = mdiobus_read(bus, addr, MII_PHYSID1);
	if (id1 < 0)
		return id1;
	id2 = mdiobus_read(bus, addr, MII_PHYSID2);
	if (id2 < 0)
		return id2;
	control = mdiobus_read(bus, addr, MII_BMCR);
	if (control < 0)
		return control;
	/* BMSR link status is latched low; use the second read. */
	status = mdiobus_read(bus, addr, MII_BMSR);
	if (status < 0)
		return status;
	status = mdiobus_read(bus, addr, MII_BMSR);
	if (status < 0)
		return status;
	vendor = mdiobus_read(bus, addr, 0x18);
	if (vendor < 0)
		return vendor;
	return sysfs_emit(buf, "phy=%d id=%04x%04x bmcr=%04x bmsr=%04x link=%u vendor18=%04x\n",
			  addr, id1, id2, control, status, !!(status & BMSR_LSTATUS),
			  vendor);
}
static DEVICE_ATTR_RO(phy_status);

static struct attribute *gc1601_mdio_attrs[] = {
	&dev_attr_phy_status.attr,
	NULL,
};
ATTRIBUTE_GROUPS(gc1601_mdio);

static int gc1601_mdio_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct gc1601_mdio *mdio;
	struct mii_bus *bus;
	int ret;

	bus = devm_mdiobus_alloc_size(dev, sizeof(*mdio));
	if (!bus)
		return -ENOMEM;
	mdio = bus->priv;
	mdio->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(mdio->base))
		return PTR_ERR(mdio->base);
	bus->name = "GC1601 MDIO";
	bus->parent = dev;
	bus->read = gc1601_mdio_read;
	bus->write = gc1601_mdio_write;
	snprintf(bus->id, MII_BUS_ID_SIZE, "%s", dev_name(dev));
	platform_set_drvdata(pdev, bus);
	ret = devm_of_mdiobus_register(dev, bus, dev->of_node);
	if (ret)
		return dev_err_probe(dev, ret, "MDIO registration failed\n");
	dev_info(dev, "MDIO ready; no MAC/DMA initialization\n");
	return 0;
}

static const struct of_device_id gc1601_mdio_match[] = {
	{ .compatible = "galachip,gc1601-mdio" },
	{ }
};
MODULE_DEVICE_TABLE(of, gc1601_mdio_match);

static struct platform_driver gc1601_mdio_driver = {
	.probe = gc1601_mdio_probe,
	.driver = {
		.name = "gc1601-mdio",
		.of_match_table = gc1601_mdio_match,
		.dev_groups = gc1601_mdio_groups,
	},
};
module_platform_driver(gc1601_mdio_driver);

MODULE_DESCRIPTION("GalaChip GC1601 Clause 22 MDIO controller");
MODULE_LICENSE("GPL");
