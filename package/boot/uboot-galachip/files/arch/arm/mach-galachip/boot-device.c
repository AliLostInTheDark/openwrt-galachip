// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Boot order of the GalaChip GC1601 TPL and SPL
 *
 * Each loads the next stage from the SPI flash, or over YMODEM when that
 * fails. A key held down in the terminal at power-on puts YMODEM first, for
 * a next stage that loads from the flash but does not run.
 *
 * Copyright (C) 2026 Ifran Ali <khanu.i.i.k.2000@gmail.com>
 */

#include <spl.h>
#include <stdio.h>
#include <time.h>

/* Longer than the gap between two keys of a terminal's autorepeat */
#define GC1601_KEY_WAIT_MS	50

u32 spl_boot_device(void)
{
	return BOOT_DEVICE_SPI;
}

void board_boot_order(u32 *spl_boot_list)
{
	ulong start = get_timer(0);
	int i = 0;

	while (get_timer(start) < GC1601_KEY_WAIT_MS) {
		if (tstc()) {
			spl_boot_list[i++] = BOOT_DEVICE_UART;
			break;
		}
	}

	spl_boot_list[i++] = BOOT_DEVICE_SPI;
	spl_boot_list[i] = BOOT_DEVICE_UART;
}
