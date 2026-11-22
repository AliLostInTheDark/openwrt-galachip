// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Faraday FTTMR010 timer
 *
 * U-Boot runs timer 2 as a free-running counter, without interrupts. On the
 * GalaChip GC1601 it counts down from the reload value, the interrupt status
 * is cleared by writing a one and a one in the mask register masks an
 * interrupt.
 *
 * Based on the Linux timer-fttmr010 driver:
 * Copyright (C) 2009 Faraday Technology
 * Copyright (C) 2009 Po-Yu Chuang <ratbert@faraday-tech.com>
 */

#include <clk.h>
#include <dm.h>
#include <timer.h>
#include <asm/io.h>
#include <dm/device_compat.h>
#include <linux/bitops.h>
#include <linux/err.h>

#define TIMER2_COUNT		0x10
#define TIMER2_LOAD		0x14
#define TIMER2_MATCH1		0x18
#define TIMER2_MATCH2		0x1c
#define TIMER_CR		0x30
#define TIMER_INTR_STATE	0x34
#define TIMER_INTR_MASK		0x38

#define TIMER_2_CR_ENABLE	BIT(3)
#define TIMER_2_CR_CLOCK	BIT(4)	/* external clock instead of PCLK */
#define TIMER_2_CR_INT		BIT(5)
#define TIMER_2_CR_UPDOWN	BIT(10)	/* count up */
#define TIMER_2_CR_ALL		(TIMER_2_CR_ENABLE | TIMER_2_CR_CLOCK | \
				 TIMER_2_CR_INT | TIMER_2_CR_UPDOWN)
#define TIMER_2_INT_ALL		GENMASK(5, 3)

struct fttmr010_timer_priv {
	void __iomem *base;
};

static u64 fttmr010_timer_get_count(struct udevice *dev)
{
	struct fttmr010_timer_priv *priv = dev_get_priv(dev);

	return timer_conv_64(~readl(priv->base + TIMER2_COUNT));
}

static int fttmr010_timer_probe(struct udevice *dev)
{
	struct timer_dev_priv *uc_priv = dev_get_uclass_priv(dev);
	struct fttmr010_timer_priv *priv = dev_get_priv(dev);
	struct clk clk;
	ulong rate;
	int ret;

	priv->base = dev_read_addr_ptr(dev);
	if (!priv->base)
		return -EINVAL;

	ret = clk_get_by_index(dev, 0, &clk);
	if (ret)
		return ret;
	rate = clk_get_rate(&clk);
	if (IS_ERR_VALUE(rate) || !rate)
		return -EINVAL;
	uc_priv->clock_rate = rate;

	/* Restart timer 2 from the top on PCLK, counting down, interrupts masked */
	clrbits_le32(priv->base + TIMER_CR, TIMER_2_CR_ALL);
	setbits_le32(priv->base + TIMER_INTR_MASK, TIMER_2_INT_ALL);
	writel(TIMER_2_INT_ALL, priv->base + TIMER_INTR_STATE);
	writel(0, priv->base + TIMER2_MATCH1);
	writel(0, priv->base + TIMER2_MATCH2);
	writel(~0, priv->base + TIMER2_LOAD);
	writel(~0, priv->base + TIMER2_COUNT);
	setbits_le32(priv->base + TIMER_CR, TIMER_2_CR_ENABLE);

	dev_dbg(dev, "timer 2 counting down at %lu Hz\n", rate);

	return 0;
}

static const struct timer_ops fttmr010_timer_ops = {
	.get_count = fttmr010_timer_get_count,
};

static const struct udevice_id fttmr010_timer_ids[] = {
	{ .compatible = "galachip,gc1601-timer" },
	{ }
};

U_BOOT_DRIVER(fttmr010_timer) = {
	.name = "fttmr010_timer",
	.id = UCLASS_TIMER,
	.of_match = fttmr010_timer_ids,
	.probe = fttmr010_timer_probe,
	.ops = &fttmr010_timer_ops,
	.priv_auto = sizeof(struct fttmr010_timer_priv),
	.flags = DM_FLAG_PRE_RELOC,
};
