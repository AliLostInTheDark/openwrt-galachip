/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * GalaChip GC1601 SoC definitions shared by U-Boot, its SPL and TPL
 *
 * Copyright (C) 2026 Ifran Ali <khanu.i.i.k.2000@gmail.com>
 */

#ifndef __ASM_ARCH_GC1601_H
#define __ASM_ARCH_GC1601_H

#include <linux/types.h>

/* Register bases used before driver model, in the TPL and SPL */
#define GC1601_TIMER_BASE	0x90200000
#define GC1601_WDT_BASE		0x90800000
#define GC1601_SCU_BASE		0x90a00000
#define GC1601_DDRC_BASE	0x90b00000
#define GC1601_SPI_BASE		0xa0000000

/* The packet engine's descriptors and buffers, fixed since the stock firmware */
#define GC1601_ETH_DMA_BASE	0x40800000
#define GC1601_ETH_DMA_SIZE	0x00112000

/**
 * gc1601_ddr_load_range() - Check that a load fits a DDR window
 * @base:	Start of DDR
 * @top:	End of the usable DDR, exclusive
 * @address:	Start of the load
 * @size:	Size of the load in bytes
 *
 * The load must stay within [@base + 512 KiB, @top) and outside the packet
 * engine's DMA area.
 *
 * Return: 1 if the load fits, 0 if not
 */
static inline int gc1601_ddr_load_range(ulong base, ulong top, ulong address, ulong size)
{
	if (!size || address < base + 0x80000 || address >= top || size > top - address)
		return 0;

	return address + size <= GC1601_ETH_DMA_BASE ||
	       address >= GC1601_ETH_DMA_BASE + GC1601_ETH_DMA_SIZE;
}

/**
 * gc1601_load_range() - Check that a load fits the DDR U-Boot leaves free
 * @address:	Start of the load
 * @size:	Size of the load in bytes
 *
 * Only in U-Boot proper: the window ends 8 MiB below the top of DDR, where
 * U-Boot relocates to.
 *
 * Return: 1 if the load fits, 0 if not
 */
int gc1601_load_range(ulong address, ulong size);

/**
 * gc1601_fw_partition() - Find the firmware partition in the device tree
 * @offset:	Returns the partition's offset in the flash
 * @size:	Returns the partition's size
 *
 * The firmware partition is the flash partition with the compatible
 * "galachip,gc1601-fw": the stock firmware tag, then the kernel.
 *
 * Return: 0 if found, -ENOENT if there is none, -EINVAL if its reg is invalid
 */
int gc1601_fw_partition(u32 *offset, u32 *size);

/**
 * gc1601_dcache_enable() - Turn the MMU and the D-cache on, in the SPL
 * @ttb:	16 KiB-aligned first-level table of 4096 section entries
 */
void gc1601_dcache_enable(u32 *ttb);

/**
 * gc1601_dcache_disable() - Write the D-cache back, turn it and the MMU off
 *
 * The SPL calls it before entering the next stage, whose entry refuses a
 * D-cache or MMU left on.
 */
void gc1601_dcache_disable(void);

/**
 * gc1601_eth_handoff() - Stop the packet engine's DMA for Linux
 *
 * Return: 0 on success, a negative error if the DMA did not stop
 */
int gc1601_eth_handoff(void);

/**
 * gc1601_eth_diagnostic() - Print the packet engine's state
 * @verbose:	Also print the raw MAC registers and the stop and transmit traces
 */
void gc1601_eth_diagnostic(bool verbose);

#endif
