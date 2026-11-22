/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Configuration for the GalaChip GC1601
 *
 * Copyright (C) 2026 Ifran Ali <khanu.i.i.k.2000@gmail.com>
 */

#ifndef __CONFIG_GC1601_H
#define __CONFIG_GC1601_H

/* DDR starts here; its size is the device tree's memory node. */
#define CFG_SYS_SDRAM_BASE	0x40000000

/*
 * The UART runs from a fixed 33 MHz clock (divisor = 33 MHz / 16 / baud):
 * 115200 and 230400 are within 0.6 %; 1031250 is exact but non-standard.
 * 460800 and 921600 have no usable divisor.
 */
/* The TPL's console and timer, without driver model */
#define CFG_SYS_NS16550_COM1	0x90400000
#define CFG_SYS_NS16550_CLK	33000000
#ifdef CONFIG_TPL_BUILD
/* FTTMR010 timer 2 on PCLK, 133.33 MHz as the SCU is left by the mask ROM */
#define CFG_SYS_TIMER_COUNTER	0x90200010
#define CFG_SYS_TIMER_RATE	133333333
#endif

#define CFG_SYS_BAUDRATE_TABLE	{ 9600, 19200, 38400, 57600, 115200, 230400, 1031250 }

#endif
