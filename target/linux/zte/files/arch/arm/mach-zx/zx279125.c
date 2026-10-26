// SPDX-License-Identifier: GPL-2.0-only
/*
 * Machine support for ZTE Sanechips ZX279125 SoC
 *
 * Copyright (C) 2026 Ifran Ali <khanu.i.i.k.2000@gmail.com>
 */

#include <linux/init.h>
#include <linux/io.h>
#include <linux/irqchip.h>
#include <linux/reboot.h>
#include <linux/sizes.h>
#include <asm/mach/arch.h>

#define ZX279125_RST_BASE	0x94000000
#define ZX279125_RST_SOFT	0x3c

static void __iomem *zx279125_rst_base;

static void __init zx279125_init_irq(void)
{
	void __iomem *routing = ioremap(0x00803000, SZ_4K);

	/* Stock zx_init_irq configures this block before the GIC. */
	if (routing) {
		writel(0x80000000, routing + 0x50);
		writel(0x5500000a, routing + 0x54);
		iounmap(routing);
	}

	irqchip_init();
}

static void __init zx279125_init_machine(void)
{
	void __iomem *pp;

	zx279125_rst_base = ioremap(ZX279125_RST_BASE, SZ_4K);

	/* Observe the inherited DMA layout without changing its ownership. */
	pp = ioremap(0x92144000, SZ_256);
	if (pp) {
		pr_info("ZX279125 inherited PP: bp=%#x data=%#x count=%u desc=%#x\n",
			readl(pp + 0x18), readl(pp + 0x20),
			readl(pp + 0x24), readl(pp + 0x1c));
		iounmap(pp);
	}
}

static void zx279125_restart(enum reboot_mode mode, const char *cmd)
{
	if (!zx279125_rst_base)
		return;

	/* Stock zx_board_reset writes the whole-board reset register. */
	writel(1, zx279125_rst_base + ZX279125_RST_SOFT);
}

static const char *const zx279125_dt_compat[] __initconst = {
	"zte,zx279125",
	NULL,
};

DT_MACHINE_START(ZX279125_DT, "ZTE Sanechips ZX279125")
	.dt_compat	= zx279125_dt_compat,
	.init_irq	= zx279125_init_irq,
	.init_machine	= zx279125_init_machine,
	.restart	= zx279125_restart,
MACHINE_END
