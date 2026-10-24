// SPDX-License-Identifier: GPL-2.0+
/* V2801SG stock bootloader 0x41f00688..0x41f0076c, reconstructed as source. */
#include <asm/io.h>
#include <linux/errno.h>
#include "early_spi.h"

#define ZX_EARLY_SPI_BASE	0x94407000UL
#define ZX_SPI_FIFO_COUNT	0x00001f00U
#define ZX_SPI_POLL_LIMIT	1000000U

static inline u32 zx_spi_read(unsigned int offset)
{
	return readl((void __iomem *)(ZX_EARLY_SPI_BASE + offset));
}

static inline void zx_spi_write(unsigned int offset, u32 value)
{
	writel(value, (void __iomem *)(ZX_EARLY_SPI_BASE + offset));
}

static int zx_spi_wait(unsigned int offset, u32 mask, int set)
{
	unsigned int count;

	for (count = 0; count < ZX_SPI_POLL_LIMIT; count++) {
		if (!!(zx_spi_read(offset) & mask) == set)
			return 0;
	}

	return -ETIMEDOUT;
}

int zx279125_early_spi_setup(void)
{
	unsigned long pc, sp;
	u32 value;
	int ret;

	asm volatile("mov %0, pc\n\tmov %1, sp" : "=r" (pc), "=r" (sp));
	if ((pc & 0xf0000000UL) == 0x40000000UL ||
	    (sp & 0xf0000000UL) == 0x40000000UL)
		return -EPERM;

	value = zx_spi_read(0x08);
	zx_spi_write(0x08, value & ~4);
	value = zx_spi_read(0x08);
	zx_spi_write(0x08, value | 1);
	ret = zx_spi_wait(0x08, 2, 1);
	if (ret)
		return ret;
	value = zx_spi_read(0x24);
	zx_spi_write(0x24, value & ~0x30000);
	value = zx_spi_read(0x24);
	zx_spi_write(0x24, value | 0x10000);

	return 0;
}

int zx279125_early_spi_read(void *destination, u32 offset, size_t length)
{
	unsigned long pc, sp;
	u32 *output = destination;
	unsigned int word;
	u32 value;
	int ret;

	asm volatile("mov %0, pc\n\tmov %1, sp" : "=r" (pc), "=r" (sp));
	if ((pc & 0xf0000000UL) == 0x40000000UL ||
	    (sp & 0xf0000000UL) == 0x40000000UL)
		return -EPERM;
	/* Only the aligned page-sized contract used by the stock loader. */
	if (!destination || ((unsigned long)destination & 3) ||
	    length != 256 || (offset & 255) || offset > 0x00ffff00)
		return -EINVAL;

	zx_spi_write(0x14, 0);
	value = zx_spi_read(0x14);
	zx_spi_write(0x14, value);
	value = zx_spi_read(0x14);
	zx_spi_write(0x14, value & ~0x00000f00);
	value = zx_spi_read(0x14);
	zx_spi_write(0x14, value);
	value = zx_spi_read(0x14);
	zx_spi_write(0x14, value | 0x40);
	zx_spi_write(0x18, length - 1);
	zx_spi_write(0x1c, offset);
	zx_spi_write(0x20, 0x03);
	zx_spi_write(0x10, 0);
	value = zx_spi_read(0x10);
	zx_spi_write(0x10, value | 0x12);
	value = zx_spi_read(0x0c);
	zx_spi_write(0x0c, value | 0x0001c000);

	ret = zx_spi_wait(0x34, ZX_SPI_FIFO_COUNT, 0);
	if (ret)
		return ret;
	zx_spi_write(0x30, 0x3f);
	value = zx_spi_read(0x04);
	zx_spi_write(0x04, value | 1);

	for (word = 0; word < 64; word++) {
		ret = zx_spi_wait(0x34, ZX_SPI_FIFO_COUNT, 1);
		if (ret)
			return ret;
		output[word] = zx_spi_read(0x38);
	}

	return zx_spi_wait(0x2c, 1, 1);
}
