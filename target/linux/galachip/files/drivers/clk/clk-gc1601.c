// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2015 Faraday Technology
 * Copyright (C) 2015 Bing-Yao Luo <bjluo@faraday-tech.com>
 * Copyright (C) 2020 Faraday Technology
 * Copyright (C) 2020 Bo-Cun Chen <bcchen@faraday-tech.com>
 */
#include <linux/clk-provider.h>
#include <linux/io.h>
#include <linux/of_address.h>
#include <linux/slab.h>

#include <dt-bindings/clock/galachip,gc1601-clock.h>

static void __init gc1601_clk_init(struct device_node *np)
{
	static const char *const names[] = {
		"gc1601-pll0-out1", "gc1601-pll0-out2", "gc1601-pll0-out3",
		"gc1601-cpu", "gc1601-ahb", "gc1601-apb", "gc1601-spi",
	};
	struct clk_hw_onecell_data *data;
	unsigned int divs[GC1601_CLK_COUNT] = { 3, 4, 5 };
	void __iomem *base;
	const char *osc, *parent;
	u32 ctrl, pll, spi;
	unsigned int i, mult, cpu_sel, bus_sel;
	int ret;

	osc = of_clk_get_parent_name(np, 0);
	if (!osc)
		return;
	base = of_iomap(np, 0);
	if (!base)
		return;
	ctrl = readl(base + 0x20);
	pll = readl(base + 0x30);
	spi = readl(base + 0x810);
	iounmap(base);
	mult = ((pll >> 24) & 0xff) * 4;
	if (!mult) {
		pr_err("GC1601 PLL0 multiplier is zero\n");
		return;
	}
	cpu_sel = (ctrl >> 16) & 3;
	bus_sel = (ctrl >> 20) & 3;
	divs[GC1601_CLK_CPU] = cpu_sel == 2 ? 1 : cpu_sel == 1 ? 2 : 4;
	divs[GC1601_CLK_AHB] = bus_sel == 0 ? 8 : 4;
	divs[GC1601_CLK_APB] = bus_sel == 1 ? 4 : 8;
	divs[GC1601_CLK_SPI] = spi & BIT(11) ? 2 : 8;

	data = kzalloc(struct_size(data, hws, GC1601_CLK_COUNT), GFP_KERNEL);
	if (!data)
		return;
	data->num = GC1601_CLK_COUNT;
	for (i = 0; i < GC1601_CLK_COUNT; i++) {
		parent = i <= GC1601_CLK_PLL0_OUT3 ? osc : names[GC1601_CLK_PLL0_OUT1];
		if (i == GC1601_CLK_SPI)
			parent = names[GC1601_CLK_PLL0_OUT2];
		data->hws[i] = clk_hw_register_fixed_factor(NULL, names[i], parent,
							  0, i <= GC1601_CLK_PLL0_OUT3 ? mult : 1,
							  divs[i]);
		if (IS_ERR(data->hws[i])) {
			ret = PTR_ERR(data->hws[i]);
			goto unregister;
		}
	}
	ret = of_clk_add_hw_provider(np, of_clk_hw_onecell_get, data);
	if (!ret) {
		pr_info("GC1601 SCU: pll=%08x ctrl=%08x multiplier=%u CPU/ AHB/ APB divisors=%u/%u/%u\n",
			pll, ctrl, mult, divs[GC1601_CLK_CPU],
			divs[GC1601_CLK_AHB], divs[GC1601_CLK_APB]);
		return;
	}

unregister:
	while (i--)
		clk_hw_unregister_fixed_factor(data->hws[i]);
	kfree(data);
	pr_err("GC1601 clock initialization failed: %d\n", ret);
}
CLK_OF_DECLARE(gc1601, "galachip,gc1601-scu", gc1601_clk_init);
