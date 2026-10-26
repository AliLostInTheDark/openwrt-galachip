// SPDX-License-Identifier: GPL-2.0+
/* Cold SPI-to-DDR path reconstructed from stock 0x41f00774..0x41f00868. */
#include <asm/io.h>
#include <linux/errno.h>

#include "ddr.h"
#include "early_boot.h"
#include "early_spi.h"

#define ZX_BUS_BASE	0x94200000UL

int zx279125_bootstrap_load(unsigned long destination, u32 offset, u32 size)
{
	u32 value, copied;
	int ret;
	int stock_restore = destination == 0x41f00000 &&
			     offset == 0 && size == 0x40000;

	/* Stock recovery uses its observed high-DRAM link address verbatim. */
	if (!stock_restore &&
	    (destination < 0x40000000 || destination >= 0x41000000 ||
	     !size || size > 0x41000000 - destination ||
	     offset >= 0x40000 || size > 0x40000 - offset ||
	     (destination & 3) || (offset & 255) || (size & 255)))
		return -EINVAL;

	/* This checks the actual execution address and stack before any MMIO. */
	ret = zx279125_ddr_init();
	if (ret)
		return ret;

	value = readl((void __iomem *)ZX_BUS_BASE);
	writel(value & ~0x3c030000, (void __iomem *)ZX_BUS_BASE);
	value = readl((void __iomem *)(ZX_BUS_BASE + 4));
	writel(value | 0x3c0, (void __iomem *)(ZX_BUS_BASE + 4));
	ret = zx279125_early_spi_setup();
	if (ret)
		return ret;

	for (copied = 0; copied < size; copied += 256) {
		ret = zx279125_early_spi_read((void *)(destination + copied),
					    offset + copied, 256);
		if (ret)
			return ret;
	}

	return 0;
}
