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
#include <linux/spinlock.h>

#include <dt-bindings/clock/galachip,gc1601-clock.h>

/*
 * The SPI clock is PLL0 output 2 divided by 8, or by 2 when SCU 0x810 bit 11
 * is set. The boot loader leaves the bit clear; the SPI driver can ask for the
 * faster clock.
 */
#define GC1601_SCU_SPI			0x810
#define GC1601_SCU_SPI_FAST		BIT(11)

struct gc1601_spi_clk {
	struct clk_hw hw;
	void __iomem *reg;
	spinlock_t lock;
};

#define to_gc1601_spi_clk(_hw)	container_of(_hw, struct gc1601_spi_clk, hw)

static unsigned long gc1601_spi_recalc_rate(struct clk_hw *hw, unsigned long parent_rate)
{
	u32 val = readl(to_gc1601_spi_clk(hw)->reg);

	return parent_rate / (val & GC1601_SCU_SPI_FAST ? 2 : 8);
}

static int gc1601_spi_determine_rate(struct clk_hw *hw, struct clk_rate_request *req)
{
	unsigned long slow = req->best_parent_rate / 8;
	unsigned long fast = req->best_parent_rate / 2;

	req->rate = req->rate >= fast ? fast : slow;
	return 0;
}

static int gc1601_spi_set_rate(struct clk_hw *hw, unsigned long rate, unsigned long parent_rate)
{
	struct gc1601_spi_clk *spi = to_gc1601_spi_clk(hw);
	unsigned long flags;
	u32 val;

	spin_lock_irqsave(&spi->lock, flags);
	val = readl(spi->reg);
	if (rate > parent_rate / 8)
		val |= GC1601_SCU_SPI_FAST;
	else
		val &= ~GC1601_SCU_SPI_FAST;
	writel(val, spi->reg);
	spin_unlock_irqrestore(&spi->lock, flags);

	return 0;
}

static const struct clk_ops gc1601_spi_clk_ops = {
	.recalc_rate = gc1601_spi_recalc_rate,
	.determine_rate = gc1601_spi_determine_rate,
	.set_rate = gc1601_spi_set_rate,
};

static struct clk_hw *__init gc1601_register_spi_clk(void __iomem *base, const char *name,
						     const char *parent)
{
	struct clk_init_data init = {
		.name = name,
		.ops = &gc1601_spi_clk_ops,
		.parent_names = &parent,
		.num_parents = 1,
	};
	struct gc1601_spi_clk *spi;
	int ret;

	spi = kzalloc(sizeof(*spi), GFP_KERNEL);
	if (!spi)
		return ERR_PTR(-ENOMEM);
	spi->reg = base + GC1601_SCU_SPI;
	spin_lock_init(&spi->lock);
	spi->hw.init = &init;

	ret = clk_hw_register(NULL, &spi->hw);
	if (ret) {
		kfree(spi);
		return ERR_PTR(ret);
	}
	return &spi->hw;
}

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
	mult = ((pll >> 24) & 0xff) * 4;
	if (!mult) {
		pr_err("GC1601 PLL0 multiplier is zero\n");
		iounmap(base);
		return;
	}
	cpu_sel = (ctrl >> 16) & 3;
	bus_sel = (ctrl >> 20) & 3;
	divs[GC1601_CLK_CPU] = cpu_sel == 2 ? 1 : cpu_sel == 1 ? 2 : 4;
	divs[GC1601_CLK_AHB] = bus_sel == 1 ? 4 : 8;
	divs[GC1601_CLK_APB] = bus_sel == 1 ? 4 : 8;
	divs[GC1601_CLK_SPI] = spi & BIT(11) ? 2 : 8;

	data = kzalloc(struct_size(data, hws, GC1601_CLK_COUNT), GFP_KERNEL);
	if (!data) {
		iounmap(base);
		return;
	}
	data->num = GC1601_CLK_COUNT;
	for (i = 0; i < GC1601_CLK_COUNT; i++) {
		parent = i <= GC1601_CLK_PLL0_OUT3 ? osc : names[GC1601_CLK_PLL0_OUT1];
		if (i == GC1601_CLK_SPI) {
			data->hws[i] = gc1601_register_spi_clk(base, names[i],
							       names[GC1601_CLK_PLL0_OUT2]);
		} else {
			data->hws[i] = clk_hw_register_fixed_factor(NULL, names[i], parent,
								  0, i <= GC1601_CLK_PLL0_OUT3 ? mult : 1,
								  divs[i]);
		}
		if (IS_ERR(data->hws[i])) {
			ret = PTR_ERR(data->hws[i]);
			goto unregister;
		}
	}
	ret = of_clk_add_hw_provider(np, of_clk_hw_onecell_get, data);
	if (!ret) {
		pr_info("GC1601 SCU: pll=%08x ctrl=%08x multiplier=%u CPU/ AHB/ APB/ SPI divisors=%u/%u/%u/%u\n",
			pll, ctrl, mult, divs[GC1601_CLK_CPU],
			divs[GC1601_CLK_AHB], divs[GC1601_CLK_APB], divs[GC1601_CLK_SPI]);
		return;
	}

unregister:
	while (i--) {
		if (i == GC1601_CLK_SPI) {
			clk_hw_unregister(data->hws[i]);
			kfree(to_gc1601_spi_clk(data->hws[i]));
		} else {
			clk_hw_unregister_fixed_factor(data->hws[i]);
		}
	}
	kfree(data);
	iounmap(base);
	pr_err("GC1601 clock initialization failed: %d\n", ret);
}
CLK_OF_DECLARE(gc1601, "galachip,gc1601-scu", gc1601_clk_init);
