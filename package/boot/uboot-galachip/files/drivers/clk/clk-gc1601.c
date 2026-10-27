// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * GC1601 SCU fixed factors, from the Faraday GPL clock implementation.
 * Copyright (C) 2015, 2020 Faraday Technology
 * Bing-Yao Luo <bjluo@faraday-tech.com>, Bo-Cun Chen <bcchen@faraday-tech.com>
 */
#include <clk-uclass.h>
#include <dm.h>
#include <asm/io.h>
#include <linux/err.h>

static ulong gc1601_get_rate(struct clk *clk)
{
	void *base = dev_read_addr_ptr(clk->dev);
	u32 ctrl = readl(base + 0x20), pll = readl(base + 0x30);
	u64 rate = 25000000ULL * (((pll >> 24) & 255) * 4);
	u32 select, div;

	if (!rate)
		return -EINVAL;
	switch (clk->id) {
	case 0:
		div = 3;
		break;
	case 1:
		div = 4;
		break;
	case 2:
		div = 5;
		break;
	case 3:
		select = (ctrl >> 16) & 3;
		div = 3 * (select == 2 ? 1 : select == 1 ? 2 : 4);
		break;
	case 4:
	case 5:
		div = 3 * (((ctrl >> 20) & 3) == 1 ? 4 : 8);
		break;
	case 6:
		div = 4 * (readl(base + 0x810) & BIT(11) ? 2 : 8);
		break;
	default:
		return -EINVAL;
	}
	rate /= div;
	return rate > 0xffffffffULL ? -ERANGE : (ulong)rate;
}

static const struct clk_ops gc1601_clk_ops = { .get_rate = gc1601_get_rate };
static const struct udevice_id gc1601_clk_ids[] = {
	{ .compatible = "galachip,gc1601-scu" }, {}
};
U_BOOT_DRIVER(gc1601_clk) = {
	.name = "gc1601_clk", .id = UCLASS_CLK,
	.of_match = gc1601_clk_ids, .ops = &gc1601_clk_ops,
	.flags = DM_FLAG_PRE_RELOC,
};
