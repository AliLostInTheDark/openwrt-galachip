// SPDX-License-Identifier: GPL-2.0-only
/*
 * DDR controller setup of the GalaChip GC1601, run by the TPL from SRAM
 *
 * The sequence is the one of the boot loader installed on the boards and of
 * the Faraday GPL: write the board's register table, start the controller,
 * wait for it, then check a few words of DDR.
 *
 * Copyright (C) 2026 Ifran Ali <khanu.i.i.k.2000@gmail.com>
 */

#include <asm/barriers.h>
#include <asm/io.h>
#include <asm/arch/gc1601.h>
#include <asm/arch/sdram.h>
#include <linux/bitops.h>
#include <linux/errno.h>
#include <linux/kernel.h>

#define GC1601_DDRC_CMD		0x04	/* write 1 to start */
#define GC1601_DDRC_READY	0xc100	/* what GC1601_DDRC_CMD reads once it runs */
#define GC1601_DDRC_STATUS	0x20
/* A healthy controller sets both; the GPL accepts either, and so do we. */
#define GC1601_DDRC_LOCKED	GENMASK(25, 24)

/* Below the packet engine's DMA area, apart from everything U-Boot loads */
#define GC1601_DDR_SCRATCH	0x407f0000
/* Polls, not time: there is no timer before the SPL. */
#define GC1601_DDRC_POLLS	10000000

static int gc1601_ddrc_wait(void __iomem *reg, u32 mask, u32 val)
{
	unsigned int n;

	for (n = 0; n < GC1601_DDRC_POLLS; n++)
		if ((readl(reg) & mask) == val)
			return 0;

	return -ETIMEDOUT;
}

static int gc1601_ddrc_wait_locked(void __iomem *ddrc)
{
	unsigned int n;

	for (n = 0; n < GC1601_DDRC_POLLS; n++)
		if (readl(ddrc + GC1601_DDRC_STATUS) & GC1601_DDRC_LOCKED)
			return 0;

	return -ETIMEDOUT;
}

int gc1601_sdram_init(void)
{
	static const u32 patterns[] = { 0x55555555, 0xaaaaaaaa, 0, ~0U };
	void __iomem *ddrc = (void __iomem *)GC1601_DDRC_BASE;
	const struct gc1601_ddr_params *p = &gc1601_ddr_params;
	u32 __iomem *scratch = (u32 __iomem *)GC1601_DDR_SCRATCH;
	unsigned int i;

	/* A warm reset can keep DDR running: never set it up a second time. */
	if (readl(ddrc + GC1601_DDRC_CMD) != GC1601_DDRC_READY) {
		for (i = 0; i < p->count; i++)
			writel(p->regs[i].value, ddrc + p->regs[i].offset);
		writel(1, ddrc + GC1601_DDRC_CMD);
	}

	if (gc1601_ddrc_wait(ddrc + GC1601_DDRC_CMD, ~0U, GC1601_DDRC_READY) ||
	    gc1601_ddrc_wait_locked(ddrc))
		return -ETIMEDOUT;

	for (i = 0; i < ARRAY_SIZE(patterns); i++) {
		writel(patterns[i], &scratch[i]);
		dsb();
		if (readl(&scratch[i]) != patterns[i])
			return -EIO;
	}

	return 0;
}
