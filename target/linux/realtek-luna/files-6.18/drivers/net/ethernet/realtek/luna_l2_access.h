/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LUNA_L2_ACCESS_H
#define _LUNA_L2_ACCESS_H

#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/io.h>

struct luna_l2_layout {
	u8 method_shift, address_shift, hash_bits;
};

static const struct luna_l2_layout luna_l2_rtl9602c = { 4, 9, 10 };
static const struct luna_l2_layout luna_l2_rtl9603cvd = { 5, 12, 11 };

static inline int luna_l2_wait(void __iomem *sw)
{
	unsigned int i;

	for (i = 0; i < 2000; i++) {
		if (!(readl(sw + 0x12004) & (1u << 13)))
			return 0;
		udelay(1);
	}
	return -ETIMEDOUT;
}

/* Writes use the complete MAC/VLAN key; address mode is read-only. */
static inline int luna_l2_access(void __iomem *sw,
				 const struct luna_l2_layout *l, u16 index,
				 u32 words[3], bool by_mac, bool write)
{
	u32 cmd = 0, status, hash_size;
	unsigned int i;
	int ret;

	if (!sw || !l)
		return -ENXIO;
	if (!words || (l->hash_bits != 10 && l->hash_bits != 11) ||
	    l->method_shift >= 32 || l->address_shift > 16)
		return -EINVAL;
	hash_size = 1u << l->hash_bits;
	if (!by_mac && !write) {
		if (index >= hash_size + 64)
			return -EINVAL;
		cmd = (1u << l->method_shift) | ((u32)index << l->address_shift);
	}
	ret = luna_l2_wait(sw);
	if (ret)
		return ret;
	if (by_mac || write)
		for (i = 0; i < 3; i++)
			writel(words[i], sw + 0x12008 + 4 * i);
	writel(cmd | (write ? (1u << 3) : 0), sw + 0x12000);
	ret = luna_l2_wait(sw);
	if (ret)
		return ret;
	status = readl(sw + 0x12004);
	if (!(status & (1u << 12)))
		return -ENOENT;
	index = (status & (hash_size - 1)) |
		((status & (1u << 11)) ? hash_size : 0);
	if (index >= hash_size + 64)
		return -EIO;
	if (!write)
		for (i = 0; i < 3; i++)
			words[i] = readl(sw + 0x1201c + 4 * i);
	return index;
}

#endif
