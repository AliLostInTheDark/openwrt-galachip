/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef GC1601_BOUNDS_H
#define GC1601_BOUNDS_H
/* All received files stay below relocated U-Boot, and outside the DMA hole. */
static inline int gc1601_load_range(unsigned long address, unsigned long size)
{
	if (!size)
		return 0;
	if (address >= 0x40080000UL && address < 0x40800000UL)
		return size <= 0x40800000UL - address;
	if (address >= 0x40912000UL && address < 0x41800000UL)
		return size <= 0x41800000UL - address;
	return 0;
}
#endif
