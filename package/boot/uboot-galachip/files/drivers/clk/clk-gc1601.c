// SPDX-License-Identifier: GPL-2.0-only
/*
 * Clocks of the GalaChip GC1601 system control unit (SCU)
 *
 * The DDR boot stage programs PLL0 from the 25 MHz oscillator. Its outputs,
 * the PLL divided by 3, 4 and 5, feed the CPU, the buses and the SPI
 * controller. U-Boot reads the multiplier and the dividers, and sets only the
 * SPI clock's selector: PLL0 output 2 divided by 2 or by 8.
 *
 * Copyright (C) 2026 Ifran Ali <khanu.i.i.k.2000@gmail.com>
 *
 * Which registers hold which setting was established on the hardware and with
 * Faraday Technology's driver (Copyright (C) 2015 and 2020 Faraday Technology,
 * Bing-Yao Luo and Bo-Cun Chen), whose structure this driver partly follows.
 */

#include <clk-uclass.h>
#include <dm.h>
#include <asm/io.h>
#include <dm/device_compat.h>
#include <linux/bitfield.h>
#include <linux/err.h>

#include <dt-bindings/clock/galachip,gc1601-clock.h>

#define GC1601_SCU_CTRL			0x20
#define GC1601_SCU_CTRL_CPU		GENMASK(17, 16)
#define GC1601_SCU_CTRL_BUS		GENMASK(21, 20)
#define GC1601_SCU_PLL			0x30
/* The PLL multiplier counts in steps of four. */
#define GC1601_SCU_PLL_MULT		GENMASK(31, 24)
#define GC1601_SCU_SPI			0x810
#define GC1601_SCU_SPI_DIV2		BIT(11)

#define GC1601_SPI_DIV_SLOW		8
#define GC1601_SPI_DIV_FAST		2

struct gc1601_clk_priv {
	void __iomem *base;
	ulong pll;
};

static unsigned int gc1601_cpu_div(u32 ctrl)
{
	switch (FIELD_GET(GC1601_SCU_CTRL_CPU, ctrl)) {
	case 2:
		return 1;
	case 1:
		return 2;
	default:
		return 4;
	}
}

static unsigned int gc1601_bus_div(u32 ctrl)
{
	return FIELD_GET(GC1601_SCU_CTRL_BUS, ctrl) == 1 ? 4 : 8;
}

static ulong gc1601_rate(struct gc1601_clk_priv *priv, ulong id)
{
	u32 ctrl = readl(priv->base + GC1601_SCU_CTRL);

	switch (id) {
	case GC1601_CLK_PLL0_OUT1:
		return priv->pll / 3;
	case GC1601_CLK_PLL0_OUT2:
		return priv->pll / 4;
	case GC1601_CLK_PLL0_OUT3:
		return priv->pll / 5;
	case GC1601_CLK_CPU:
		return priv->pll / 3 / gc1601_cpu_div(ctrl);
	case GC1601_CLK_AHB:
	case GC1601_CLK_APB:
		return priv->pll / 3 / gc1601_bus_div(ctrl);
	case GC1601_CLK_SPI:
		if (readl(priv->base + GC1601_SCU_SPI) & GC1601_SCU_SPI_DIV2)
			return priv->pll / 4 / GC1601_SPI_DIV_FAST;
		return priv->pll / 4 / GC1601_SPI_DIV_SLOW;
	default:
		return -EINVAL;
	}
}

static ulong gc1601_clk_get_rate(struct clk *clk)
{
	return gc1601_rate(dev_get_priv(clk->dev), clk->id);
}

/* The SPI clock is the fast one whenever the slow one is below @rate. */
static ulong gc1601_clk_set_rate(struct clk *clk, ulong rate)
{
	struct gc1601_clk_priv *priv = dev_get_priv(clk->dev);
	bool fast = rate > priv->pll / 4 / GC1601_SPI_DIV_SLOW;

	if (clk->id != GC1601_CLK_SPI)
		return -EINVAL;

	clrsetbits_le32(priv->base + GC1601_SCU_SPI, GC1601_SCU_SPI_DIV2,
			fast ? GC1601_SCU_SPI_DIV2 : 0);

	return gc1601_rate(priv, GC1601_CLK_SPI);
}

static int gc1601_clk_probe(struct udevice *dev)
{
	struct gc1601_clk_priv *priv = dev_get_priv(dev);
	struct clk osc;
	ulong osc_rate;
	u64 pll;
	u32 mult;
	int ret;

	priv->base = dev_read_addr_ptr(dev);
	if (!priv->base)
		return -EINVAL;

	ret = clk_get_by_index(dev, 0, &osc);
	if (ret)
		return ret;
	osc_rate = clk_get_rate(&osc);
	if (IS_ERR_VALUE(osc_rate) || !osc_rate)
		return -EINVAL;

	mult = FIELD_GET(GC1601_SCU_PLL_MULT, readl(priv->base + GC1601_SCU_PLL)) * 4;
	pll = (u64)osc_rate * mult;
	if (!pll || pll > ULONG_MAX)
		return -EINVAL;
	priv->pll = pll;

	dev_dbg(dev, "PLL0 %lu Hz, CPU %lu Hz, APB %lu Hz, SPI %lu Hz\n", priv->pll,
		gc1601_rate(priv, GC1601_CLK_CPU), gc1601_rate(priv, GC1601_CLK_APB),
		gc1601_rate(priv, GC1601_CLK_SPI));

	return 0;
}

static const struct clk_ops gc1601_clk_ops = {
	.get_rate = gc1601_clk_get_rate,
	.set_rate = gc1601_clk_set_rate,
};

static const struct udevice_id gc1601_clk_ids[] = {
	{ .compatible = "galachip,gc1601-scu" },
	{ }
};

U_BOOT_DRIVER(gc1601_clk) = {
	.name = "gc1601_clk",
	.id = UCLASS_CLK,
	.of_match = gc1601_clk_ids,
	.probe = gc1601_clk_probe,
	.ops = &gc1601_clk_ops,
	.priv_auto = sizeof(struct gc1601_clk_priv),
	.flags = DM_FLAG_PRE_RELOC,
};
