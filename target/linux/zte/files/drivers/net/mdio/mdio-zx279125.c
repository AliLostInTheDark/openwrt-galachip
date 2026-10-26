// SPDX-License-Identifier: GPL-2.0-only
/* ZTE ZX279125 Clause 22 MDIO controller. */

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of_mdio.h>
#include <linux/platform_device.h>

#define ZX_MDIO_WRITE_DATA	0x04
#define ZX_MDIO_READ_DATA	0x08
#define ZX_MDIO_DIVIDER		0x0c
#define ZX_MDIO_STATUS		0x10
#define ZX_MDIO_DONE		BIT(0)
#define ZX_MDIO_CTRL		0x14
#define ZX_MDIO_REG		GENMASK(4, 0)
#define ZX_MDIO_PHY		GENMASK(9, 5)
#define ZX_MDIO_OP		GENMASK(11, 10)
#define ZX_MDIO_OP_WRITE		1
#define ZX_MDIO_OP_READ		2
#define ZX_MDIO_START		BIT(14)

struct zx279125_mdio {
	void __iomem *base;
};

static int zx279125_mdio_xfer(struct mii_bus *bus, int addr, int regnum,
			      bool write, u16 value)
{
	struct zx279125_mdio *mdio = bus->priv;
	void __iomem *base = mdio->base;
	u32 ctrl, status;
	int ret;

	ctrl = readl(base + ZX_MDIO_CTRL) & ~ZX_MDIO_START;
	writel(ctrl, base + ZX_MDIO_CTRL);
	writel(0, base + ZX_MDIO_STATUS);

	ctrl &= ~(ZX_MDIO_PHY | ZX_MDIO_REG | ZX_MDIO_OP);
	ctrl |= FIELD_PREP(ZX_MDIO_PHY, addr) |
		FIELD_PREP(ZX_MDIO_REG, regnum) |
		FIELD_PREP(ZX_MDIO_OP, write ? ZX_MDIO_OP_WRITE : ZX_MDIO_OP_READ);
	if (write)
		writel(value, base + ZX_MDIO_WRITE_DATA);
	writel(ctrl, base + ZX_MDIO_CTRL);
	writel(ctrl | ZX_MDIO_START, base + ZX_MDIO_CTRL);

	ret = readl_poll_timeout(base + ZX_MDIO_STATUS, status,
				 status & ZX_MDIO_DONE, 10, 10000);
	writel(0, base + ZX_MDIO_STATUS);
	ctrl = readl(base + ZX_MDIO_CTRL) & ~ZX_MDIO_START;
	writel(ctrl, base + ZX_MDIO_CTRL);
	if (ret)
		return ret;

	return write ? 0 : readl(base + ZX_MDIO_READ_DATA) & 0xffff;
}

static int zx279125_mdio_read(struct mii_bus *bus, int addr, int regnum)
{
	return zx279125_mdio_xfer(bus, addr, regnum, false, 0);
}

static int zx279125_mdio_write(struct mii_bus *bus, int addr, int regnum,
			       u16 value)
{
	return zx279125_mdio_xfer(bus, addr, regnum, true, value);
}

static int zx279125_mdio_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct zx279125_mdio *mdio;
	struct mii_bus *bus;
	struct clk *clk;
	u32 ctrl;

	bus = devm_mdiobus_alloc_size(dev, sizeof(*mdio));
	if (!bus)
		return -ENOMEM;

	mdio = bus->priv;
	mdio->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(mdio->base))
		return PTR_ERR(mdio->base);

	clk = devm_clk_get_enabled(dev, NULL);
	if (IS_ERR(clk))
		return dev_err_probe(dev, PTR_ERR(clk), "failed to enable MDIO clock\n");

	/* Preserve the stock framing setup; do not reset or tune any PHY. */
	ctrl = readl(mdio->base + ZX_MDIO_CTRL);
	ctrl &= ~(ZX_MDIO_START | BIT(15) | BIT(13));
	ctrl |= BIT(12);
	writel(ctrl, mdio->base + ZX_MDIO_CTRL);
	writel(0, mdio->base + ZX_MDIO_STATUS);
	writel(0, mdio->base + ZX_MDIO_DIVIDER);

	bus->name = "ZX279125 MDIO";
	bus->parent = dev;
	bus->read = zx279125_mdio_read;
	bus->write = zx279125_mdio_write;
	snprintf(bus->id, MII_BUS_ID_SIZE, "%s", dev_name(dev));

	return devm_of_mdiobus_register(dev, bus, dev->of_node);
}

static const struct of_device_id zx279125_mdio_of_match[] = {
	{ .compatible = "zte,zx279125-mdio" },
	{ }
};
MODULE_DEVICE_TABLE(of, zx279125_mdio_of_match);

static struct platform_driver zx279125_mdio_driver = {
	.probe = zx279125_mdio_probe,
	.driver = {
		.name = "zx279125-mdio",
		.of_match_table = zx279125_mdio_of_match,
	},
};
module_platform_driver(zx279125_mdio_driver);

MODULE_DESCRIPTION("ZTE ZX279125 MDIO bus controller");
MODULE_LICENSE("GPL");
