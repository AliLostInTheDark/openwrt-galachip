// SPDX-License-Identifier: GPL-2.0-only
/* GC1601 Clause 22 MDIO, derived from the Faraday GPL and GC1601 Linux port. */
#include <dm.h>
#include <miiphy.h>
#include <phy.h>
#include <asm/io.h>
#include <dm/ofnode.h>
#include <linux/delay.h>
#include <linux/iopoll.h>

static int gc_mdio_xfer(struct udevice *dev, int addr, int devad,
			int reg, u16 data, bool write)
{
	void *base = dev_read_addr_ptr(dev);
	u32 val, done = write ? BIT(8) : BIT(0);
	u32 trigger = write ? 0x10 : 0x14;
	int ret;

	if (devad != MDIO_DEVAD_NONE || addr < 0 || addr > 31 || reg < 0 || reg > 31)
		return -EINVAL;
	ret = readl_poll_timeout(base + 0x0c, val, !(val & BIT(15)), 256000);
	if (ret)
		return ret;
	clrbits_le32(base + 0x0c, done);
	writel(2 | (addr << 8) | (reg << 3), base);
	if (write)
		writel(data, base + 4);
	udelay(1000);
	ret = readl_poll_timeout(base + 0x0c, val, !(val & BIT(15)), 256000);
	if (ret)
		return ret;
	writel(1, base + trigger);
	ret = readl_poll_timeout(base + 0x0c, val, val & done, 256000);
	writel(0, base + trigger);
	if (ret)
		return ret;
	return write ? 0 : readl(base + 8) & 0xffff;
}

static int gc_mdio_read(struct udevice *d, int a, int devad, int r)
{
	return gc_mdio_xfer(d, a, devad, r, 0, false);
}

static int gc_mdio_write(struct udevice *d, int a, int devad, int r, u16 v)
{
	return gc_mdio_xfer(d, a, devad, r, v, true);
}

#define GC_SCU_GE_CTRL		0x810
#define GC_SCU_GE_PHY_EN	BIT(13)

static void *gc_mdio_syscon(struct udevice *dev, const char *prop)
{
	ofnode node = ofnode_parse_phandle(dev_ofnode(dev), prop, 0);
	fdt_addr_t addr = ofnode_valid(node) ? ofnode_get_addr(node) : FDT_ADDR_T_NONE;

	return addr == FDT_ADDR_T_NONE ? NULL : (void *)addr;
}

/* Power the GE PHY block, then step its SerDes through the start sequence. */
static int gc_mdio_probe(struct udevice *dev)
{
	static const u32 sequence[] = { 0x4000, 0x20000, 0x20002, 0x20003, 3 };
	void *scu = gc_mdio_syscon(dev, "galachip,scu");
	void *serdes = gc_mdio_syscon(dev, "galachip,ge-serdes");
	unsigned int i;

	if (!dev_read_addr_ptr(dev) || !scu || !serdes)
		return -EINVAL;
	setbits_le32(scu + GC_SCU_GE_CTRL, GC_SCU_GE_PHY_EN);
	mdelay(50);
	for (i = 0; i < ARRAY_SIZE(sequence); i++) {
		writel(sequence[i], serdes);
		mdelay(i == ARRAY_SIZE(sequence) - 1 ? 100 : 10);
	}
	return 0;
}

static const struct mdio_ops gc_mdio_ops = {
	.read = gc_mdio_read, .write = gc_mdio_write,
};
static const struct udevice_id gc_mdio_ids[] = {
	{ .compatible = "galachip,gc1601-mdio" }, {}
};
U_BOOT_DRIVER(gc1601_mdio) = {
	.name = "gc1601_mdio", .id = UCLASS_MDIO, .of_match = gc_mdio_ids,
	.ops = &gc_mdio_ops, .probe = gc_mdio_probe,
};
