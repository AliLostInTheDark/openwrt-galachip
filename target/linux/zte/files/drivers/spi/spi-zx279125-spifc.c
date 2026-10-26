// SPDX-License-Identifier: GPL-2.0-only
/*
 * ZTE Sanechips ZX279125 SPI Flash Controller (SPIFC) Driver
 *
 * Copyright (C) 2026 Ifran Ali <khanu.i.i.k.2000@gmail.com>
 */

#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/spi/spi.h>
#include <linux/spi/spi-mem.h>
#include <linux/unaligned.h>

#define SPIFC_REG_START			0x04
#define  SPIFC_START_BIT		BIT(0)

#define SPIFC_REG_RESET			0x08
#define  SPIFC_RESET_FLUSH_FIFO		BIT(0)
#define  SPIFC_RESET_FIFO_BUSY		BIT(1)
#define  SPIFC_RESET_CLR_BIT2		BIT(2)

#define SPIFC_REG_CMD_CFG		0x0c
#define  SPIFC_CMD_CFG_FLAGS		0x1c440

#define SPIFC_REG_PHASE_EN		0x10
#define  SPIFC_PHASE_TX_DATA		BIT(0)
#define  SPIFC_PHASE_RX_DATA		BIT(1)
#define  SPIFC_PHASE_DUMMY		BIT(2)
#define  SPIFC_PHASE_ADDR		BIT(4)

#define SPIFC_REG_MODE			0x14
#define  SPIFC_MODE_ADDR_LEN_SHIFT	5
#define  SPIFC_MODE_DUMMY_LEN_SHIFT	12

#define SPIFC_REG_DATALEN		0x18

#define SPIFC_REG_ADDR			0x1c

#define SPIFC_REG_OPCODE		0x20

#define SPIFC_REG_STATUS		0x2c
#define  SPIFC_STATUS_DONE		BIT(0)
#define  SPIFC_STATUS_FORMAT_ERR	BIT(1)

#define SPIFC_REG_INT_STATUS_CLR	0x30
#define  SPIFC_INT_CLEAR_ALL		0x3f

#define SPIFC_REG_FIFO_STATUS		0x34
#define  SPIFC_FIFO_RX_AVAIL_MASK	0x00001f00
#define  SPIFC_FIFO_TX_FREE_MASK	0x001f0000

#define SPIFC_REG_FIFO_DATA		0x38

#define SPIFC_MAX_CHUNK_SIZE		256

struct zx279125_spifc {
	struct device *dev;
	void __iomem *base;
	struct clk *clk;
};

static void zx279125_spifc_reset_fifo(void __iomem *base)
{
	u32 val;

	val = readl(base + SPIFC_REG_RESET);
	val &= ~SPIFC_RESET_CLR_BIT2;
	writel(val, base + SPIFC_REG_RESET);

	val = readl(base + SPIFC_REG_RESET);
	if (!(val & SPIFC_RESET_FIFO_BUSY)) {
		val |= SPIFC_RESET_FLUSH_FIFO;
		writel(val, base + SPIFC_REG_RESET);
	}
}

static int zx279125_spifc_write_fifo(struct zx279125_spifc *spifc,
				     const u8 *buf, size_t len)
{
	void __iomem *base = spifc->base;
	size_t words = len >> 2;
	size_t rem = len & 3;
	size_t i;
	u32 val;
	int ret;

	for (i = 0; i < words; i++) {
		ret = readl_poll_timeout_atomic(base + SPIFC_REG_FIFO_STATUS, val,
						(val & SPIFC_FIFO_TX_FREE_MASK),
						1, 50000);
		if (ret) {
			dev_err(spifc->dev, "TX FIFO timeout (word %zu/%zu)\n",
				i, words);
			return ret;
		}
		writel(get_unaligned_le32(buf + i * 4), base + SPIFC_REG_FIFO_DATA);

		/* As soon as the first word is queued, start the transfer so the hardware
		 * drains the FIFO and does not deadlock on transfers > FIFO size (16 words).
		 */
		if (i == 0) {
			val = readl(base + SPIFC_REG_START);
			if (!(val & SPIFC_START_BIT))
				writel(val | SPIFC_START_BIT, base + SPIFC_REG_START);
		}
	}

	if (rem) {
		u32 tmp = 0;

		ret = readl_poll_timeout_atomic(base + SPIFC_REG_FIFO_STATUS, val,
						(val & SPIFC_FIFO_TX_FREE_MASK),
						1, 50000);
		if (ret) {
			dev_err(spifc->dev, "TX FIFO timeout (trailing bytes)\n");
			return ret;
		}
		memcpy(&tmp, buf + words * 4, rem);
		writel(tmp, base + SPIFC_REG_FIFO_DATA);

		val = readl(base + SPIFC_REG_START);
		if (!(val & SPIFC_START_BIT))
			writel(val | SPIFC_START_BIT, base + SPIFC_REG_START);
	}

	return 0;
}

static int zx279125_spifc_read_fifo(struct zx279125_spifc *spifc,
				    u8 *buf, size_t len)
{
	void __iomem *base = spifc->base;
	size_t words = len >> 2;
	size_t rem = len & 3;
	size_t i;
	u32 val;
	int ret;

	for (i = 0; i < words; i++) {
		ret = readl_poll_timeout_atomic(base + SPIFC_REG_FIFO_STATUS, val,
						(val & SPIFC_FIFO_RX_AVAIL_MASK),
						1, 50000);
		if (ret) {
			dev_err(spifc->dev, "RX FIFO timeout (word %zu/%zu)\n",
				i, words);
			return ret;
		}
		put_unaligned_le32(readl(base + SPIFC_REG_FIFO_DATA), buf + i * 4);
	}

	if (rem) {
		u32 tmp;

		ret = readl_poll_timeout_atomic(base + SPIFC_REG_FIFO_STATUS, val,
						(val & SPIFC_FIFO_RX_AVAIL_MASK),
						1, 50000);
		if (ret) {
			dev_err(spifc->dev, "RX FIFO timeout (trailing bytes)\n");
			return ret;
		}
		tmp = readl(base + SPIFC_REG_FIFO_DATA);
		memcpy(buf + words * 4, &tmp, rem);
	}

	return 0;
}

static int zx279125_spifc_adjust_op_size(struct spi_mem *mem,
					 struct spi_mem_op *op)
{
	if (op->data.dir != SPI_MEM_NO_DATA &&
	    op->data.nbytes > SPIFC_MAX_CHUNK_SIZE)
		op->data.nbytes = SPIFC_MAX_CHUNK_SIZE;

	return 0;
}

static bool zx279125_spifc_supports_op(struct spi_mem *mem,
				       const struct spi_mem_op *op)
{
	if (op->cmd.buswidth > 1 || op->addr.buswidth > 1 ||
	    op->dummy.buswidth > 1 || op->data.buswidth > 1)
		return false;

	if (op->addr.nbytes > 4)
		return false;

	if (op->dummy.nbytes > 15)
		return false;

	return spi_mem_default_supports_op(mem, op);
}

static int zx279125_spifc_exec_op(struct spi_mem *mem,
				   const struct spi_mem_op *op)
{
	struct zx279125_spifc *spifc = spi_controller_get_devdata(mem->spi->controller);
	void __iomem *base = spifc->base;
	u32 phase_en = 0;
	u32 mode = 0;
	u32 status;
	int ret;

	zx279125_spifc_reset_fifo(base);

	/* Set Opcode */
	writel(op->cmd.opcode, base + SPIFC_REG_OPCODE);

	/* Set Address if present; default address length field is 2 (3-byte) */
	if (op->addr.nbytes) {
		phase_en |= SPIFC_PHASE_ADDR;
		mode |= ((op->addr.nbytes - 1) & 0x7) << SPIFC_MODE_ADDR_LEN_SHIFT;
		writel((u32)op->addr.val, base + SPIFC_REG_ADDR);
	} else {
		mode |= (2 & 0x7) << SPIFC_MODE_ADDR_LEN_SHIFT;
		writel(0, base + SPIFC_REG_ADDR);
	}

	/* Set Dummy cycles if present */
	if (op->dummy.nbytes) {
		phase_en |= SPIFC_PHASE_DUMMY;
		mode |= (op->dummy.nbytes & 0xf) << SPIFC_MODE_DUMMY_LEN_SHIFT;
	}
	writel(mode, base + SPIFC_REG_MODE);

	/* Set Data phase if present */
	if (op->data.nbytes) {
		if (op->data.dir == SPI_MEM_DATA_IN)
			phase_en |= SPIFC_PHASE_RX_DATA;
		else if (op->data.dir == SPI_MEM_DATA_OUT)
			phase_en |= SPIFC_PHASE_TX_DATA;

		writel(op->data.nbytes - 1, base + SPIFC_REG_DATALEN);
	} else {
		writel(0, base + SPIFC_REG_DATALEN);
	}
	writel(phase_en, base + SPIFC_REG_PHASE_EN);

	/* Command Config Flags (0x1c440 includes bit 16 as required by hardware) */
	writel(readl(base + SPIFC_REG_CMD_CFG) | SPIFC_CMD_CFG_FLAGS,
	       base + SPIFC_REG_CMD_CFG);

	/* Acknowledge / clear any pending status flags */
	writel(SPIFC_INT_CLEAR_ALL, base + SPIFC_REG_INT_STATUS_CLR);

	/* If writing data, write FIFO (starts transaction on first word) */
	if (op->data.dir == SPI_MEM_DATA_OUT && op->data.nbytes) {
		ret = zx279125_spifc_write_fifo(spifc, op->data.buf.out,
						op->data.nbytes);
		if (ret)
			return ret;
	} else {
		/* Start transaction for operations with no TX data (or READ) */
		writel(readl(base + SPIFC_REG_START) | SPIFC_START_BIT,
		       base + SPIFC_REG_START);
	}

	/* If reading data, drain FIFO as words arrive from the SPI bus */
	if (op->data.dir == SPI_MEM_DATA_IN && op->data.nbytes) {
		ret = zx279125_spifc_read_fifo(spifc, op->data.buf.in,
					       op->data.nbytes);
		if (ret)
			return ret;
	}

	/* Wait for command done */
	ret = readl_poll_timeout_atomic(base + SPIFC_REG_STATUS, status,
					(status & SPIFC_STATUS_DONE), 10, 500000);
	if (ret) {
		dev_err(spifc->dev, "Opcode 0x%02x timed out (status=0x%08x)\n",
			op->cmd.opcode, readl(base + SPIFC_REG_STATUS));
		return ret;
	}

	if (status & SPIFC_STATUS_FORMAT_ERR)
		dev_warn(spifc->dev, "Opcode 0x%02x reported format error\n",
			 op->cmd.opcode);

	return 0;
}

static const struct spi_controller_mem_ops zx279125_spifc_mem_ops = {
	.adjust_op_size = zx279125_spifc_adjust_op_size,
	.supports_op = zx279125_spifc_supports_op,
	.exec_op = zx279125_spifc_exec_op,
};

static int zx279125_spifc_probe(struct platform_device *pdev)
{
	struct spi_controller *ctlr;
	struct zx279125_spifc *spifc;
	int ret;

	ctlr = devm_spi_alloc_host(&pdev->dev, sizeof(*spifc));
	if (!ctlr)
		return -ENOMEM;

	spifc = spi_controller_get_devdata(ctlr);
	spifc->dev = &pdev->dev;

	spifc->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(spifc->base))
		return PTR_ERR(spifc->base);

	spifc->clk = devm_clk_get_optional_enabled(&pdev->dev, NULL);
	if (IS_ERR(spifc->clk))
		return dev_err_probe(&pdev->dev, PTR_ERR(spifc->clk),
				     "Failed to get and enable clock\n");

	ctlr->mode_bits = SPI_CPOL | SPI_CPHA;
	ctlr->bus_num = -1;
	ctlr->mem_ops = &zx279125_spifc_mem_ops;
	ctlr->num_chipselect = 1;
	ctlr->dev.of_node = pdev->dev.of_node;
	ctlr->bits_per_word_mask = SPI_BPW_MASK(8);

	platform_set_drvdata(pdev, ctlr);

	ret = devm_spi_register_controller(&pdev->dev, ctlr);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "Failed to register SPI controller\n");

	dev_info(&pdev->dev, "ZTE ZX279125 SPIFC initialized\n");
	return 0;
}

static const struct of_device_id zx279125_spifc_of_match[] = {
	{ .compatible = "zte,zx279125-spifc" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, zx279125_spifc_of_match);

static struct platform_driver zx279125_spifc_driver = {
	.probe = zx279125_spifc_probe,
	.driver = {
		.name = "zx279125-spifc",
		.of_match_table = zx279125_spifc_of_match,
	},
};
module_platform_driver(zx279125_spifc_driver);

MODULE_AUTHOR("Ifran Ali <khanu.i.i.k.2000@gmail.com>");
MODULE_DESCRIPTION("ZTE Sanechips ZX279125 SPI Flash Controller Driver");
MODULE_LICENSE("GPL");
