// SPDX-License-Identifier: GPL-2.0-only
/*
 * Clocks of the GalaChip GC1601 system control unit (SCU)
 *
 * The boot loader programs PLL0 from the 25 MHz oscillator. Its outputs, the
 * PLL divided by 3, 4 and 5, feed the CPU, the buses and the SPI controller.
 * The multiplier of the PLL and the dividers of the CPU and the buses are read
 * from the SCU and never changed. The SPI clock has a selector the driver
 * does change: PLL output 2 divided by 8, or by 2.
 *
 * The clocks register early, from the device tree node of the SCU, because the
 * timer needs its clock before any platform device is probed. The registers
 * are reached through the syscon regmap of that node, so the rest of the
 * kernel can share them.
 *
 * Copyright (C) 2026 Ifran Ali <khanu.i.i.k.2000@gmail.com>
 *
 * Which registers hold which setting was established on the hardware and with
 * Faraday Technology's driver (Copyright (C) 2015 and 2020 Faraday Technology,
 * Bing-Yao Luo and Bo-Cun Chen), whose structure this driver partly follows.
 */
#include <linux/bitfield.h>
#include <linux/clk-provider.h>
#include <linux/err.h>
#include <linux/mfd/syscon.h>
#include <linux/of.h>
#include <linux/regmap.h>
#include <linux/slab.h>

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

static const struct {
	const char *name;
	const char *parent;	/* NULL: the oscillator */
} gc1601_clks[GC1601_CLK_COUNT] = {
	[GC1601_CLK_PLL0_OUT1] = { "gc1601-pll0-out1" },
	[GC1601_CLK_PLL0_OUT2] = { "gc1601-pll0-out2" },
	[GC1601_CLK_PLL0_OUT3] = { "gc1601-pll0-out3" },
	[GC1601_CLK_CPU] = { "gc1601-cpu", "gc1601-pll0-out1" },
	[GC1601_CLK_AHB] = { "gc1601-ahb", "gc1601-pll0-out1" },
	[GC1601_CLK_APB] = { "gc1601-apb", "gc1601-pll0-out1" },
	[GC1601_CLK_SPI] = { "gc1601-spi", "gc1601-pll0-out2" },
};

struct gc1601_spi_clk {
	struct clk_hw hw;
	struct regmap *scu;
};

#define to_gc1601_spi_clk(_hw)	container_of(_hw, struct gc1601_spi_clk, hw)

static unsigned long gc1601_spi_recalc_rate(struct clk_hw *hw, unsigned long parent_rate)
{
	unsigned int val;

	if (regmap_read(to_gc1601_spi_clk(hw)->scu, GC1601_SCU_SPI, &val))
		return 0;

	if (val & GC1601_SCU_SPI_DIV2)
		return parent_rate / GC1601_SPI_DIV_FAST;

	return parent_rate / GC1601_SPI_DIV_SLOW;
}

/* Only two rates exist: the fast one for a request that reaches it, the slow one otherwise. */
static int gc1601_spi_determine_rate(struct clk_hw *hw, struct clk_rate_request *req)
{
	unsigned long fast = req->best_parent_rate / GC1601_SPI_DIV_FAST;

	req->rate = req->rate >= fast ? fast : req->best_parent_rate / GC1601_SPI_DIV_SLOW;

	return 0;
}

static int gc1601_spi_set_rate(struct clk_hw *hw, unsigned long rate, unsigned long parent_rate)
{
	bool fast = rate > parent_rate / GC1601_SPI_DIV_SLOW;

	return regmap_update_bits(to_gc1601_spi_clk(hw)->scu, GC1601_SCU_SPI, GC1601_SCU_SPI_DIV2,
				  fast ? GC1601_SCU_SPI_DIV2 : 0);
}

static const struct clk_ops gc1601_spi_clk_ops = {
	.recalc_rate = gc1601_spi_recalc_rate,
	.determine_rate = gc1601_spi_determine_rate,
	.set_rate = gc1601_spi_set_rate,
};

static struct clk_hw *__init gc1601_register_spi_clk(struct regmap *scu)
{
	struct clk_init_data init = {
		.name = gc1601_clks[GC1601_CLK_SPI].name,
		.ops = &gc1601_spi_clk_ops,
		.parent_names = &gc1601_clks[GC1601_CLK_SPI].parent,
		.num_parents = 1,
	};
	struct gc1601_spi_clk *spi;
	int ret;

	spi = kzalloc(sizeof(*spi), GFP_KERNEL);
	if (!spi)
		return ERR_PTR(-ENOMEM);
	spi->scu = scu;
	spi->hw.init = &init;

	ret = clk_hw_register(NULL, &spi->hw);
	if (ret) {
		kfree(spi);
		return ERR_PTR(ret);
	}

	return &spi->hw;
}

static void __init gc1601_unregister_clk(struct clk_hw *hw, unsigned int id)
{
	if (id == GC1601_CLK_SPI) {
		clk_hw_unregister(hw);
		kfree(to_gc1601_spi_clk(hw));
	} else {
		clk_hw_unregister_fixed_factor(hw);
	}
}

static void __init gc1601_clk_init(struct device_node *np)
{
	unsigned int div[GC1601_CLK_COUNT] = {
		[GC1601_CLK_PLL0_OUT1] = 3,
		[GC1601_CLK_PLL0_OUT2] = 4,
		[GC1601_CLK_PLL0_OUT3] = 5,
	};
	struct clk_hw_onecell_data *data;
	unsigned int ctrl, pll, spi, mult, id;
	struct regmap *scu;
	const char *osc;
	int ret;

	osc = of_clk_get_parent_name(np, 0);
	if (!osc)
		return;

	scu = syscon_node_to_regmap(np);
	if (IS_ERR(scu)) {
		pr_err("GC1601 SCU: no register map: %pe\n", scu);
		return;
	}
	if (regmap_read(scu, GC1601_SCU_CTRL, &ctrl) || regmap_read(scu, GC1601_SCU_PLL, &pll) ||
	    regmap_read(scu, GC1601_SCU_SPI, &spi)) {
		pr_err("GC1601 SCU: cannot read the clock registers\n");
		return;
	}

	mult = FIELD_GET(GC1601_SCU_PLL_MULT, pll) * 4;
	if (!mult) {
		pr_err("GC1601 PLL0 multiplier is zero\n");
		return;
	}

	switch (FIELD_GET(GC1601_SCU_CTRL_CPU, ctrl)) {
	case 2:
		div[GC1601_CLK_CPU] = 1;
		break;
	case 1:
		div[GC1601_CLK_CPU] = 2;
		break;
	default:
		div[GC1601_CLK_CPU] = 4;
		break;
	}
	div[GC1601_CLK_AHB] = FIELD_GET(GC1601_SCU_CTRL_BUS, ctrl) == 1 ? 4 : 8;
	div[GC1601_CLK_APB] = div[GC1601_CLK_AHB];
	div[GC1601_CLK_SPI] = spi & GC1601_SCU_SPI_DIV2 ? GC1601_SPI_DIV_FAST : GC1601_SPI_DIV_SLOW;

	data = kzalloc(struct_size(data, hws, GC1601_CLK_COUNT), GFP_KERNEL);
	if (!data)
		return;
	data->num = GC1601_CLK_COUNT;

	for (id = 0; id < GC1601_CLK_COUNT; id++) {
		if (id == GC1601_CLK_SPI)
			data->hws[id] = gc1601_register_spi_clk(scu);
		else
			data->hws[id] = clk_hw_register_fixed_factor(NULL, gc1601_clks[id].name,
								     gc1601_clks[id].parent ?: osc,
								     0, id <= GC1601_CLK_PLL0_OUT3 ?
								     mult : 1, div[id]);
		if (IS_ERR(data->hws[id])) {
			ret = PTR_ERR(data->hws[id]);
			goto err_unregister;
		}
	}

	ret = of_clk_add_hw_provider(np, of_clk_hw_onecell_get, data);
	if (ret)
		goto err_unregister;

	pr_debug("GC1601 SCU: pll=%08x ctrl=%08x multiplier=%u CPU/ AHB/ APB/ SPI divisors=%u/%u/%u/%u\n",
		pll, ctrl, mult, div[GC1601_CLK_CPU], div[GC1601_CLK_AHB], div[GC1601_CLK_APB],
		div[GC1601_CLK_SPI]);

	return;

err_unregister:
	while (id--)
		gc1601_unregister_clk(data->hws[id], id);
	kfree(data);
	pr_err("GC1601 clock initialization failed: %d\n", ret);
}

CLK_OF_DECLARE(gc1601, "galachip,gc1601-scu", gc1601_clk_init);
