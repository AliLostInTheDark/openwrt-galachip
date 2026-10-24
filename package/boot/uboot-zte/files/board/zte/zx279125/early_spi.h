/* SPDX-License-Identifier: GPL-2.0+ */
#ifndef __ZX279125_EARLY_SPI_H
#define __ZX279125_EARLY_SPI_H

#include <linux/types.h>

/*
 * Stock controller reset/FIFO and divider sequence, without bus clock or DDR
 * setup. Same SRAM execution/stack requirements as the reader below.
 * A timeout leaves controller state changed; no flash write is issued.
 */
int zx279125_early_spi_setup(void);

/*
 * Read one 256-byte page using the vendor pre-relocation SPI sequence.
 * Execute outside DDR with an SRAM stack, caches/MMU disabled and interrupts
 * masked. Destination must be writable, word-aligned initialized DDR or SRAM;
 * this routine neither initializes DDR nor resets/enables the SPI controller.
 * Caller supplies bus clocks and calls zx279125_early_spi_setup() first.
 * The decoded NOR ROM path loads 8 KiB at 00200000. Integrated cold execution
 * and practical ROM recovery entry remain unverified.
 * No flash programming/erase operations are issued. On timeout the destination
 * may contain a partial page and controller state is not rolled back.
 */
int zx279125_early_spi_read(void *destination, u32 offset, size_t length);

#endif
