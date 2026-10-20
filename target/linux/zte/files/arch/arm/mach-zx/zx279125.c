// SPDX-License-Identifier: GPL-2.0-only
/*
 * Machine support for ZTE Sanechips ZX279125 SoC
 *
 * Copyright (C) 2026 Ifran Ali <khanu.i.i.k.2000@gmail.com>
 */

#include <linux/init.h>
#include <linux/io.h>
#include <linux/reboot.h>
#include <linux/sizes.h>
#include <asm/mach/arch.h>

#define ZX279125_RST_BASE	0x94000000
#define ZX279125_RST_SOFT	0x08

static void __iomem *zx279125_rst_base;

static void __init zx279125_init_machine(void)
{
	zx279125_rst_base = ioremap(ZX279125_RST_BASE, SZ_4K);
}

static void zx279125_restart(enum reboot_mode mode, const char *cmd)
{
	if (!zx279125_rst_base)
		return;

	/* Soft reset register at 0x94000008 */
	writel(1, zx279125_rst_base + ZX279125_RST_SOFT);
}

static const char *const zx279125_dt_compat[] __initconst = {
	"zte,zx279125",
	NULL,
};

DT_MACHINE_START(ZX279125_DT, "ZTE Sanechips ZX279125")
	.dt_compat	= zx279125_dt_compat,
	.init_machine	= zx279125_init_machine,
	.restart	= zx279125_restart,
MACHINE_END
