// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The clock rates U-Boot runs with, from the SCU clock and timer drivers.
 * Their probes happen before the console works, so they are reported here.
 *
 * Copyright (C) 2026 Ifran Ali <khanu.i.i.k.2000@gmail.com>
 */

#include <clk.h>
#include <display_options.h>
#include <dm.h>
#include <init.h>
#include <timer.h>

#include <dt-bindings/clock/galachip,gc1601-clock.h>

static ulong gc1601_rate(struct udevice *scu, ulong id)
{
	struct clk clk = { .dev = scu, .id = id };

	return clk_get_rate(&clk);
}

int print_cpuinfo(void)
{
	struct udevice *scu, *timer;
	int ret;

	ret = uclass_get_device_by_driver(UCLASS_CLK, DM_DRIVER_GET(gc1601_clk), &scu);
	if (ret)
		return ret;
	ret = uclass_first_device_err(UCLASS_TIMER, &timer);
	if (ret)
		return ret;

	puts("CPU:   GalaChip GC1601 (FA626TE) at ");
	print_freq(gc1601_rate(scu, GC1601_CLK_CPU), "\nClocks: AHB/APB ");
	print_freq(gc1601_rate(scu, GC1601_CLK_APB), ", SPI ");
	print_freq(gc1601_rate(scu, GC1601_CLK_SPI), ", timer ");
	print_freq(timer_get_rate(timer), "\n");

	return 0;
}
