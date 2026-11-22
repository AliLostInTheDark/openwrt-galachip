// SPDX-License-Identifier: GPL-2.0-only
/*
 * GalaChip GC1601 PON SerDes
 *
 * The SerDes between the optical module and the GPON/EPON MAC. Its PLL and
 * resets sit in the SCU; its analog settings are two register files, a
 * 16-bit control block and a byte-wide configuration space with one byte
 * per 32-bit word. Setting the PON mode programs the PLL for the line rate
 * (1.244/2.488 Gb/s GPON, 1.25 Gb/s EPON), loads that mode's settings,
 * releases the resets and waits for the SerDes to report ready.
 *
 * The settings are the GC1601's own values for each mode as the stock
 * firmware leaves them; what the individual registers do is not documented.
 *
 * Copyright (C) 2026 Ifran Ali <khanu.i.i.k.2000@gmail.com>
 */
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/phy/phy.h>
#include <linux/phy/phy-pon.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

/* SCU registers */
#define GC1601_SCU_PON_STATUS		0x800
#define GC1601_SCU_PON_STATUS_READY	BIT(2)
#define GC1601_SCU_PON_STATUS_PLL_LOCK	BIT(8)
#define GC1601_SCU_PON_PLL		0x804
#define GC1601_SCU_PON_PLL_BIT6		BIT(6)
#define GC1601_SCU_PON_PLL_CFG0		0x808
#define GC1601_SCU_PON_PLL_CFG1		0x80c
#define GC1601_SCU_PON_RESET		0x814

/* Reset stages: all held, control block released, then the whole SerDes. */
#define GC1601_PON_RESET_HOLD		0x011810
#define GC1601_PON_RESET_CTRL		0x051810
#define GC1601_PON_RESET_RUN		0x1d1810

#define GC1601_PON_TIMEOUT_US		10000

/* Configuration space: byte registers on a 4-byte stride */
#define GC1601_SERDES_LOOPBACK		0x0001c
#define GC1601_SERDES_LOOPBACK_CDR	0x04
#define GC1601_SERDES_LOOP_TIMING	0x20034
#define GC1601_SERDES_LOOP_TIMING_ON	0x13

struct gc1601_serdes_reg {
	u32 addr;
	u32 val;
};

struct gc1601_serdes_mode {
	const char *name;
	u32 pll_cfg0, pll_cfg1, pll, pll_clear;
	const struct gc1601_serdes_reg *ctrl, *config;
	unsigned int ctrl_len, config_len;
};

struct gc1601_serdes {
	struct device *dev;
	void __iomem *config;
	void __iomem *ctrl;
	struct regmap *scu;
};

static const struct gc1601_serdes_reg gpon_ctrl[] = {
	{ 0x00, 1 }, { 0x08, 1 }, { 0x14, 0 }, { 0x16, 1 },
	{ 0x18, 0 }, { 0x1a, 2 }, { 0x3c, 1 }, { 0x3e, 7 },
};

static const struct gc1601_serdes_reg epon_ctrl[] = {
	{ 0x00, 1 }, { 0x08, 1 }, { 0x14, 1 }, { 0x16, 1 },
	{ 0x18, 1 }, { 0x1a, 1 }, { 0x3c, 1 }, { 0x3e, 7 },
};

static const struct gc1601_serdes_reg gpon_config[] = {
	{ 0x00194, 0xb7 }, { 0x00198, 0xb7 }, { 0x0019c, 0x06 }, { 0x001a0, 0x0c },
	{ 0x001a4, 0x1a }, { 0x001a8, 0x1a }, { 0x001ac, 0x01 }, { 0x001b0, 0x02 },
	{ 0x001b4, 0x17 }, { 0x001b8, 0x0d }, { 0x20194, 0xaa }, { 0x20198, 0x00 },
	{ 0x2019c, 0x4f }, { 0x201a0, 0xb7 }, { 0x201a4, 0x57 }, { 0x201a8, 0x06 },
	{ 0x201ac, 0x0c }, { 0x201b0, 0x1a }, { 0x201b4, 0x0a }, { 0x201b8, 0x05 },
	{ 0x201bc, 0x0f }, { 0x201c0, 0x10 }, { 0x201c4, 0x00 }, { 0x201c8, 0x10 },
	{ 0x201cc, 0x00 }, { 0x201d0, 0xff }, { 0x201d4, 0xb3 }, { 0x201d8, 0xf7 },
	{ 0x201dc, 0xd0 }, { 0x201e0, 0xef }, { 0x201e4, 0xfb }, { 0x201e8, 0xff },
	{ 0x201ec, 0xff }, { 0x201f0, 0xff }, { 0x201f4, 0xff }, { 0x201f8, 0xff },
	{ 0x201fc, 0xd3 }, { 0x20200, 0xd4 }, { 0x20204, 0xe3 }, { 0x20208, 0xef },
	{ 0x2020c, 0xfb }, { 0x20210, 0xfb }, { 0x20214, 0xff }, { 0x20218, 0xef },
	{ 0x2021c, 0xff }, { 0x20220, 0xff }, { 0x20224, 0xd3 }, { 0x20228, 0xd4 },
	{ 0x2022c, 0xe3 }, { 0x20230, 0xef }, { 0x20234, 0xfb }, { 0x20238, 0xfb },
	{ 0x2023c, 0xff }, { 0x20240, 0xef }, { 0x20244, 0xff }, { 0x20248, 0xff },
	{ 0x2024c, 0xfb }, { 0x20250, 0xff }, { 0x20254, 0x3f }, { 0x20258, 0x00 },
	{ 0x2025c, 0x7d }, { 0x20260, 0x33 }, { 0x20264, 0x02 }, { 0x20268, 0x01 },
	{ 0x2026c, 0x08 }, { 0x20270, 0x04 }, { 0x20274, 0x04 }, { 0x20278, 0x00 },
	{ 0x2027c, 0x00 }, { 0x20280, 0x08 }, { 0x20284, 0x04 }, { 0x20288, 0x00 },
	{ 0x2028c, 0x00 }, { 0x20290, 0x04 },
};

static const struct gc1601_serdes_reg epon_config[] = {
	{ 0x00194, 0xbf }, { 0x00198, 0xbf }, { 0x0019c, 0x0a }, { 0x001a0, 0x0a },
	{ 0x001a4, 0x0e }, { 0x001a8, 0x0e }, { 0x001ac, 0x01 }, { 0x001b0, 0x01 },
	{ 0x001b4, 0x11 }, { 0x001b8, 0x0d }, { 0x20194, 0xaa }, { 0x20198, 0x00 },
	{ 0x2019c, 0x3f }, { 0x201a0, 0xbf }, { 0x201a4, 0xbf }, { 0x201a8, 0x0a },
	{ 0x201ac, 0x0a }, { 0x201b0, 0x0e }, { 0x201b4, 0x0e }, { 0x201b8, 0x03 },
	{ 0x201bc, 0x1f }, { 0x201c0, 0x10 }, { 0x201c4, 0x00 }, { 0x201c8, 0x10 },
	{ 0x201cc, 0x00 }, { 0x201d0, 0xff }, { 0x201d4, 0xc3 }, { 0x201d8, 0xf5 },
	{ 0x201dc, 0xd9 }, { 0x201e0, 0xf2 }, { 0x201e4, 0xfc }, { 0x201e8, 0xfe },
	{ 0x201ec, 0xff }, { 0x201f0, 0xff }, { 0x201f4, 0xff }, { 0x201f8, 0xff },
	{ 0x201fc, 0xdb }, { 0x20200, 0xe9 }, { 0x20204, 0xc2 }, { 0x20208, 0xf2 },
	{ 0x2020c, 0xfc }, { 0x20210, 0xfe }, { 0x20214, 0xf2 }, { 0x20218, 0xf2 },
	{ 0x2021c, 0xff }, { 0x20220, 0xff }, { 0x20224, 0xdb }, { 0x20228, 0xe9 },
	{ 0x2022c, 0xc2 }, { 0x20230, 0xf2 }, { 0x20234, 0xfc }, { 0x20238, 0xfe },
	{ 0x2023c, 0xf2 }, { 0x20240, 0xf2 }, { 0x20244, 0xff }, { 0x20248, 0xff },
	{ 0x2024c, 0xff }, { 0x20250, 0xf2 }, { 0x20254, 0x3f }, { 0x20258, 0x00 },
	{ 0x2025c, 0x64 }, { 0x20260, 0x00 }, { 0x20264, 0x02 }, { 0x20268, 0x01 },
	{ 0x2026c, 0x05 }, { 0x20270, 0x05 }, { 0x20274, 0x04 }, { 0x20278, 0x00 },
	{ 0x2027c, 0x00 }, { 0x20280, 0x08 }, { 0x20284, 0x04 }, { 0x20288, 0x00 },
	{ 0x2028c, 0x00 }, { 0x20290, 0x04 },
};

/* Written after the resets are released, the same in both modes */
static const struct gc1601_serdes_reg gc1601_serdes_post[] = {
	{ 0x0015c, 0x13 }, { 0x00054, 0xdb }, { 0x00058, 0x03 }, { 0x0005c, 0x00 },
	{ 0x20020, 0x56 },
};

static const struct gc1601_serdes_mode gc1601_serdes_modes[] = {
	[PHY_PON_GPON] = {
		.name = "GPON",
		.pll_cfg0 = 0x1001a, .pll_cfg1 = 0x353f7d,
		.pll = 0x3e005f, .pll_clear = 0x7,
		.ctrl = gpon_ctrl, .ctrl_len = ARRAY_SIZE(gpon_ctrl),
		.config = gpon_config, .config_len = ARRAY_SIZE(gpon_config),
	},
	[PHY_PON_EPON] = {
		.name = "EPON",
		.pll_cfg0 = 0x1001c, .pll_cfg1 = 0,
		.pll = 0x3c005f, .pll_clear = 0x5,
		.ctrl = epon_ctrl, .ctrl_len = ARRAY_SIZE(epon_ctrl),
		.config = epon_config, .config_len = ARRAY_SIZE(epon_config),
	},
};

static int gc1601_serdes_wait(struct gc1601_serdes *s, u32 bit, const char *what)
{
	u32 val;
	int ret;

	ret = regmap_read_poll_timeout(s->scu, GC1601_SCU_PON_STATUS, val, val & bit,
				       10, GC1601_PON_TIMEOUT_US);
	if (ret)
		dev_err(s->dev, "%s timed out, status %08x\n", what, val);
	return ret;
}

static void gc1601_serdes_clear(struct gc1601_serdes *s, u32 addr, u32 bits)
{
	writel(readl(s->config + addr) & ~bits, s->config + addr);
}

static int gc1601_serdes_load(struct gc1601_serdes *s, const struct gc1601_serdes_mode *m)
{
	unsigned int i;
	int ret;

	regmap_write(s->scu, GC1601_SCU_PON_PLL_CFG0, m->pll_cfg0);
	regmap_write(s->scu, GC1601_SCU_PON_PLL_CFG1, m->pll_cfg1);
	regmap_write(s->scu, GC1601_SCU_PON_PLL, m->pll);
	regmap_clear_bits(s->scu, GC1601_SCU_PON_PLL, m->pll_clear);

	ret = gc1601_serdes_wait(s, GC1601_SCU_PON_STATUS_PLL_LOCK, "PLL lock");
	if (ret)
		return ret;

	regmap_clear_bits(s->scu, GC1601_SCU_PON_PLL, GC1601_SCU_PON_PLL_BIT6);
	regmap_write(s->scu, GC1601_SCU_PON_RESET, GC1601_PON_RESET_HOLD);
	usleep_range(20, 40);

	for (i = 0; i < m->ctrl_len; i++)
		writew(m->ctrl[i].val, s->ctrl + m->ctrl[i].addr);
	regmap_write(s->scu, GC1601_SCU_PON_RESET, GC1601_PON_RESET_CTRL);
	usleep_range(20, 40);

	for (i = 0; i < m->config_len; i++)
		writel(m->config[i].val, s->config + m->config[i].addr);
	/* 0x194 and 0x19c read back only if the configuration space took the settings. */
	if (readl(s->config + m->config[0].addr) != m->config[0].val ||
	    readl(s->config + m->config[2].addr) != m->config[2].val) {
		dev_err(s->dev, "%s settings did not read back\n", m->name);
		return -EIO;
	}

	regmap_write(s->scu, GC1601_SCU_PON_RESET, GC1601_PON_RESET_RUN);
	usleep_range(20, 40);

	for (i = 0; i < ARRAY_SIZE(gc1601_serdes_post); i++)
		writel(gc1601_serdes_post[i].val, s->config + gc1601_serdes_post[i].addr);

	ret = gc1601_serdes_wait(s, GC1601_SCU_PON_STATUS_READY, "SerDes ready");
	if (ret)
		return ret;

	gc1601_serdes_clear(s, 0x2001c, 0xc0);
	gc1601_serdes_clear(s, 0x20184, 0x08);
	writel(GC1601_SERDES_LOOPBACK_CDR, s->config + GC1601_SERDES_LOOPBACK);
	writel(GC1601_SERDES_LOOP_TIMING_ON, s->config + GC1601_SERDES_LOOP_TIMING);
	return 0;
}

static int gc1601_serdes_set_mode(struct phy *phy, enum phy_mode mode, int submode)
{
	struct gc1601_serdes *s = phy_get_drvdata(phy);

	if (mode != PHY_MODE_PON)
		return -EINVAL;
	if (submode != PHY_PON_GPON && submode != PHY_PON_EPON)
		return -EINVAL;

	return gc1601_serdes_load(s, &gc1601_serdes_modes[submode]);
}

static const struct phy_ops gc1601_serdes_ops = {
	.set_mode = gc1601_serdes_set_mode,
	.owner = THIS_MODULE,
};

static int gc1601_serdes_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct phy_provider *provider;
	struct gc1601_serdes *s;
	struct phy *phy;

	s = devm_kzalloc(dev, sizeof(*s), GFP_KERNEL);
	if (!s)
		return -ENOMEM;
	s->dev = dev;

	s->config = devm_platform_ioremap_resource_byname(pdev, "config");
	if (IS_ERR(s->config))
		return PTR_ERR(s->config);
	s->ctrl = devm_platform_ioremap_resource_byname(pdev, "ctrl");
	if (IS_ERR(s->ctrl))
		return PTR_ERR(s->ctrl);
	s->scu = syscon_regmap_lookup_by_phandle(dev->of_node, "galachip,scu");
	if (IS_ERR(s->scu))
		return dev_err_probe(dev, PTR_ERR(s->scu), "no SCU\n");

	phy = devm_phy_create(dev, NULL, &gc1601_serdes_ops);
	if (IS_ERR(phy))
		return PTR_ERR(phy);
	phy_set_drvdata(phy, s);

	provider = devm_of_phy_provider_register(dev, of_phy_simple_xlate);
	return PTR_ERR_OR_ZERO(provider);
}

static const struct of_device_id gc1601_serdes_of_match[] = {
	{ .compatible = "galachip,gc1601-pon-serdes" },
	{ }
};
MODULE_DEVICE_TABLE(of, gc1601_serdes_of_match);

static struct platform_driver gc1601_serdes_driver = {
	.probe = gc1601_serdes_probe,
	.driver = {
		.name = "gc1601-pon-serdes",
		.of_match_table = gc1601_serdes_of_match,
	},
};
module_platform_driver(gc1601_serdes_driver);

MODULE_DESCRIPTION("GalaChip GC1601 PON SerDes driver");
MODULE_LICENSE("GPL");
