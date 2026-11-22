// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Reset of the GalaChip GC1601 in the TPL and the SPL
 *
 * U-Boot resets through the watchdog with driver model (wdt-reboot); the
 * stages before it do what ftwdt010_wdt_expire_now() does, on the registers.
 *
 * Copyright (C) 2026 Ifran Ali <khanu.i.i.k.2000@gmail.com>
 */

#include <cpu_func.h>
#include <asm/io.h>
#include <asm/arch/gc1601.h>
#include <faraday/ftwdt010_wdt.h>

void reset_cpu(void)
{
	struct ftwdt010_wdt __iomem *wdt = (void __iomem *)GC1601_WDT_BASE;

	writel(0, &wdt->wdcr);
	writel(FTWDT010_WDLOAD(0), &wdt->wdload);
	writel(FTWDT010_WDRESTART_MAGIC, &wdt->wdrestart);
	writel(FTWDT010_WDCR_RST | FTWDT010_WDCR_ENABLE, &wdt->wdcr);

	for (;;)
		;
}
