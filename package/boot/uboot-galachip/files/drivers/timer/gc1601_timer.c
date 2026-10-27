// SPDX-License-Identifier: GPL-2.0-or-later
/* FTTMR010 timer 2; no IRQs and no assumptions about inherited timer setup. */
/*
 * Based on the Faraday/Linux FTTMR010 sequence:
 * Copyright (C) 2009 Faraday Technology
 * Copyright (C) 2009 Po-Yu Chuang <ratbert@faraday-tech.com>
 */
#include <clk.h>
#include <dm.h>
#include <timer.h>
#include <asm/io.h>
#include <linux/err.h>

struct gc1601_timer { void *base; };

static int gc1601_timer_probe(struct udevice *dev)
{
	struct gc1601_timer *priv = dev_get_priv(dev);
	struct timer_dev_priv *uc = dev_get_uclass_priv(dev);
	struct clk clk;
	int ret;

	priv->base = dev_read_addr_ptr(dev);
	if (!priv->base)
		return -EINVAL;
	ret = clk_get_by_index(dev, 0, &clk);
	if (ret)
		return ret;
	uc->clock_rate = clk_get_rate(&clk);
	if (IS_ERR_VALUE(uc->clock_rate) || !uc->clock_rate)
		return -EINVAL;
	writel(0, priv->base + 0x30);
	writel(0x1ff, priv->base + 0x38);
	writel(0x1ff, priv->base + 0x34);
	writel(0, priv->base + 0x18);
	writel(0, priv->base + 0x1c);
	writel(~0U, priv->base + 0x14);
	writel(~0U, priv->base + 0x10);
	writel(BIT(3), priv->base + 0x30);
	return 0;
}

static u64 gc1601_timer_count(struct udevice *dev)
{
	struct gc1601_timer *priv = dev_get_priv(dev);

	return timer_conv_64(~readl(priv->base + 0x10));
}

static const struct timer_ops gc1601_timer_ops = { .get_count = gc1601_timer_count };
static const struct udevice_id gc1601_timer_ids[] = {
	{ .compatible = "galachip,gc1601-timer" }, {}
};
U_BOOT_DRIVER(gc1601_timer) = {
	.name = "gc1601_timer", .id = UCLASS_TIMER,
	.of_match = gc1601_timer_ids, .ops = &gc1601_timer_ops,
	.probe = gc1601_timer_probe, .priv_auto = sizeof(struct gc1601_timer),
	.flags = DM_FLAG_PRE_RELOC,
};
