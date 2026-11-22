// SPDX-License-Identifier: GPL-2.0-only
/*
 * Load the U-Boot SPL from the SPI NOR of the GalaChip GC1601, in the TPL
 *
 * The TPL runs from SRAM without driver model, so this reads the flash with
 * dual-output Fast Read commands on the FTSPI020 registers directly, the way
 * the ftspi020 driver programs bulk reads: 1-1-2 at 100 MHz, polled, in
 * 256-byte FIFO bursts.
 *
 * Copyright (C) 2026 Ifran Ali <khanu.i.i.k.2000@gmail.com>
 */

#include <errno.h>
#include <image.h>
#include <spl.h>
#include <asm/io.h>
#include <asm/arch/gc1601.h>
#include <linux/bitops.h>
#include <linux/kernel.h>

#define FTSPI020_CMD0			0x00	/* flash address */
#define FTSPI020_CMD1			0x04	/* opcode and address lengths, dummy cycles */
#define FTSPI020_CMD2			0x08	/* data length */
#define FTSPI020_CMD3			0x0c	/* opcode, protocol, direction; starts it */
#define FTSPI020_CTRL			0x10
#define FTSPI020_STS			0x18
#define FTSPI020_CR2			0x20
#define FTSPI020_ISR			0x24
#define FTSPI020_DATA			0x100

#define FTSPI020_CMD1_OPCODE_1BYTE	BIT(24)
#define FTSPI020_CMD1_DUMMY_SHIFT	16
#define FTSPI020_CMD3_OPCODE_SHIFT	24
#define FTSPI020_CMD3_PROTO_1_1_2	BIT(5)
#define FTSPI020_CMD3_REPORT_DONE	BIT(0)
#define FTSPI020_CTRL_ABORT		BIT(8)
#define FTSPI020_CTRL_MODE3		BIT(4)
#define FTSPI020_CTRL_DIVIDER		3U
#define FTSPI020_STS_RX_READY		BIT(1)
#define FTSPI020_CR2_DMA_ENABLE		BIT(0)
#define FTSPI020_CR2_DONE_ENABLE	BIT(1)
#define FTSPI020_CR2_RX_XFER_256B	(3U << 12)
#define FTSPI020_ISR_DONE		BIT(0)	/* write 1 to clear */

#define FTSPI020_XFER_BYTES		256U
#define FTSPI020_MAX_DATA		0x10000U

/* SPI clock = PLL0 output 2 / 2 (200 MHz) instead of / 8 */
#define GC1601_SCU_SPI			0x810
#define GC1601_SCU_SPI_DIV2		BIT(11)

#define SPINOR_OP_READ_1_1_2		0x3b
/* Polls, not time: there is no timer before the SPL. */
#define FTSPI020_POLLS			10000000

static int ftspi020_wait(void __iomem *reg, u32 bit, u32 val)
{
	unsigned int n;

	for (n = 0; n < FTSPI020_POLLS; n++)
		if ((readl(reg) & bit) == val)
			return 0;

	return -ETIMEDOUT;
}

static int gc1601_spi_init(void __iomem *base)
{
	setbits_le32(base + FTSPI020_CTRL, FTSPI020_CTRL_ABORT);
	if (ftspi020_wait(base + FTSPI020_CTRL, FTSPI020_CTRL_ABORT, 0))
		return -ETIMEDOUT;

	/* Mode 0, SCK = 200 MHz SPI clock / 2, polled with completion in ISR */
	setbits_le32(GC1601_SCU_BASE + GC1601_SCU_SPI, GC1601_SCU_SPI_DIV2);
	clrbits_le32(base + FTSPI020_CTRL, FTSPI020_CTRL_MODE3 | FTSPI020_CTRL_DIVIDER);
	clrsetbits_le32(base + FTSPI020_CR2,
			FTSPI020_CR2_DMA_ENABLE | FTSPI020_CR2_DONE_ENABLE |
			FTSPI020_CR2_RX_XFER_256B,
			FTSPI020_CR2_DONE_ENABLE | FTSPI020_CR2_RX_XFER_256B);
	writel(FTSPI020_ISR_DONE, base + FTSPI020_ISR);

	return 0;
}

/* Read whole FIFO bursts: @len is rounded up to 256 bytes. */
static int gc1601_spi_read(void __iomem *base, u32 offset, u32 len, void *buf)
{
	u32 *dst = buf;
	unsigned int i;

	len = ALIGN(len, FTSPI020_XFER_BYTES);
	if (len > FTSPI020_MAX_DATA)
		return -EINVAL;

	writel(offset, base + FTSPI020_CMD0);
	writel(FTSPI020_CMD1_OPCODE_1BYTE | 8 << FTSPI020_CMD1_DUMMY_SHIFT | 3,
	       base + FTSPI020_CMD1);
	writel(len, base + FTSPI020_CMD2);
	writel(SPINOR_OP_READ_1_1_2 << FTSPI020_CMD3_OPCODE_SHIFT | FTSPI020_CMD3_PROTO_1_1_2 |
	       FTSPI020_CMD3_REPORT_DONE, base + FTSPI020_CMD3);

	for (i = 0; i < len / 4; i++) {
		if (!(i % (FTSPI020_XFER_BYTES / 4)) &&
		    ftspi020_wait(base + FTSPI020_STS, FTSPI020_STS_RX_READY,
				  FTSPI020_STS_RX_READY))
			return -ETIMEDOUT;
		*dst++ = readl(base + FTSPI020_DATA);
	}

	if (ftspi020_wait(base + FTSPI020_ISR, FTSPI020_ISR_DONE, FTSPI020_ISR_DONE))
		return -ETIMEDOUT;
	writel(FTSPI020_ISR_DONE, base + FTSPI020_ISR);

	return 0;
}

static int gc1601_spi_load_image(struct spl_image_info *spl_image,
				 struct spl_boot_device *bootdev)
{
	void __iomem *base = (void __iomem *)GC1601_SPI_BASE;
	struct legacy_img_hdr *header;
	int ret;

	ret = gc1601_spi_init(base);
	if (ret)
		return ret;

	/* The header lands just below the SPL's load address, the data at it. */
	header = (void *)(CONFIG_SPL_TEXT_BASE - sizeof(*header));
	ret = gc1601_spi_read(base, CONFIG_GC1601_SPL_SPI_OFFS, sizeof(*header), header);
	if (ret)
		return ret;

	ret = spl_parse_image_header(spl_image, bootdev, header);
	if (ret)
		return ret;
	if (spl_image->load_addr != (ulong)header ||
	    spl_image->size > sizeof(*header) + CONFIG_SPL_MAX_SIZE)
		return -EINVAL;

	return gc1601_spi_read(base, CONFIG_GC1601_SPL_SPI_OFFS, spl_image->size, header);
}

SPL_LOAD_IMAGE_METHOD("SPI", 0, BOOT_DEVICE_SPI, gc1601_spi_load_image);
