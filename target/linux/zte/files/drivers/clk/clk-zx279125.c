// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Clock driver for ZTE Sanechips ZX279125 SoC
 *
 * Copyright (C) 2026 Ifran Ali <khanu.i.i.k.2000@gmail.com>
 */

#include <linux/clk-provider.h>
#include <linux/io.h>
#include <linux/of_address.h>
#include <linux/slab.h>

#include <dt-bindings/clock/zte,zx279125-clock.h>

static void __init zx279125_clk_init(struct device_node *np)
{
	static const char *const names[] = {
		"zx279125-osc",
		"zx279125-cpu",
		"zx279125-ddr",
		"zx279125-axi",
		"zx279125-apb",
		"zx279125-timer",
		"zx279125-uart0",
		"zx279125-uart1",
		"zx279125-spifc",
		"zx279125-gmac",
	};
	struct clk_hw_onecell_data *data;
	void __iomem *base;
	const char *osc;
	u32 mult = 24; /* 25 MHz * 24 = 600 MHz CPU base */
	unsigned int divs[ZX279125_CLK_COUNT] = {
		1,   /* OSC: 25 MHz */
		1,   /* CPU: 600 MHz */
		2,   /* DDR: 266.4 MHz */
		4,   /* AXI: 166.5 MHz */
		6,   /* APB: 100 MHz */
		240, /* TIMER: 2.5 MHz */
		6,   /* UART0: 100 MHz */
		6,   /* UART1: 100 MHz */
		6,   /* SPIFC: 100 MHz */
		5,   /* GMAC: 125 MHz */
	};
	unsigned int i;
	int ret;

	osc = of_clk_get_parent_name(np, 0);
	if (!osc)
		osc = "osc";

	base = of_iomap(np, 0);
	if (base)
		iounmap(base);

	data = kzalloc(struct_size(data, hws, ZX279125_CLK_COUNT), GFP_KERNEL);
	if (!data)
		return;

	data->num = ZX279125_CLK_COUNT;
	data->hws[ZX279125_CLK_OSC] = clk_hw_register_fixed_rate(NULL, names[0], NULL, 0, 25000000);

	for (i = 1; i < ZX279125_CLK_COUNT; i++) {
		data->hws[i] = clk_hw_register_fixed_factor(NULL, names[i], names[0],
							    0, mult, divs[i]);
		if (IS_ERR(data->hws[i])) {
			ret = PTR_ERR(data->hws[i]);
			goto unregister;
		}
	}

	ret = of_clk_add_hw_provider(np, of_clk_hw_onecell_get, data);
	if (!ret) {
		pr_info("ZX279125 CRM clocks registered (CPU=600MHz, APB=100MHz, TIMER=2.5MHz)\n");
		return;
	}

unregister:
	while (i--)
		clk_hw_unregister(data->hws[i]);
	kfree(data);
	pr_err("ZX279125 clock initialization failed: %d\n", ret);
}
CLK_OF_DECLARE(zx279125_crm, "zte,zx279125-crm", zx279125_clk_init);
