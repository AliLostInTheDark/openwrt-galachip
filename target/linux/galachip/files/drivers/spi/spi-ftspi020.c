// SPDX-License-Identifier: GPL-2.0+
/*
 * Faraday FTSPI020 SPI memory driver.
 *
 * Copyright (C) 2020 Faraday Technology Corporation.
 * Author: Bing-Yao, Luo <bjluo@faraday-tech.com>
 * Upstream adaptation: Ifran Ali <khanu.i.i.k.2000@gmail.com>
 */

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/reset.h>
#include <linux/sizes.h>
#include <linux/unaligned.h>

#include <linux/spi/spi.h>
#include <linux/spi/spi-mem.h>
#include <linux/mtd/spi-nor.h>

#define CONFIG_SPI_FTSPI020_V1_7_0 1

/******************************************************************************
 * FTSPI020 Registers
 *****************************************************************************/
#define FTSPI020_REG_CMD0               0x00	/* Flash address */
#define FTSPI020_REG_CMD1               0x04
#define FTSPI020_REG_CMD2               0x08
#define FTSPI020_REG_CMD3               0x0c
#define FTSPI020_REG_CTRL               0x10	/* Control */
#define FTSPI020_REG_AC_TIME            0x14
#define FTSPI020_REG_STS                0x18	/* TX/RX FIFO Status */
#define FTSPI020_REG_CR2                0x20
#define FTSPI020_REG_ISR                0x24	/* Interrupt Status */
#define FTSPI020_REG_XIPCMD             0x30
#define FTSPI020_REG_XIPFO              0x34
#define FTSPI020_REG_XIPAMB             0x38
#define FTSPI020_REG_XIPAOL             0x3c
#define FTSPI020_REG_REVISION           0x50
#define FTSPI020_REG_FEATURE            0x54
#define FTSPI020_REG_DATA_PORT          0x100

/*
 * Control Register offset 0x10
 */
#define FTSPI020_CTRL_READY_LOC         GENMASK(18, 16)
#define FTSPI020_CTRL_XIP_PORT_IDLE     BIT(7)
#define FTSPI020_CTRL_CLK_MODE_3        BIT(4)
#define FTSPI020_CTRL_CLK_DIVIDER       GENMASK(1, 0)

enum {
	CLK_DIVIDER_2 = 0,
	CLK_DIVIDER_4 = 1,
	CLK_DIVIDER_6 = 2,
	CLK_DIVIDER_8 = 3
};

/*
 * Status Register offset 0x18
 */
#define	FTSPI020_STS_RFR                BIT(1) /* RX FIFO ready */
#define	FTSPI020_STS_TFR                BIT(0) /* TX FIFO ready */

/*
 * Control 2 Register
 */
#define	FTSPI020_CR2_DMA_RX_THOD        GENMASK(22, 20)
#define	FTSPI020_CR2_DMA_TX_THOD        GENMASK(18, 16)
#define	FTSPI020_CR2_RX_XFR_SIZE        GENMASK(13, 12)
#define	FTSPI020_CR2_TX_XFR_SIZE        GENMASK(9, 8)
#define	FTSPI020_CR2_CMD_CMPL           BIT(1) /* Command complete enable */
#define	FTSPI020_CR2_DMA_EN             BIT(0) /* DMA handshake enable */

enum {
	DMA_THOD_4BYTES = 0,
	DMA_THOD_16BYTES = 1,
	DMA_THOD_32BYTES = 2,
	DMA_THOD_64BYTES = 3,
	DMA_THOD_128BYTES = 4,
	DMA_THOD_256BYTES = 5
};

enum {
	XFR_SIZE_32BYTES = 0,
	XFR_SIZE_64BYTES = 1,
	XFR_SIZE_128BYTES = 2,
	XFR_SIZE_256BYTES = 3
};

/*
 * Interrupt Status Register
 */
#define	FTSPI020_ISR_CMD_CMPL           BIT(0) /* Command complete status */

/*
 * Feature Register
 */
#define FTSPI020_FEATURE_DAMR           BIT(29)
#define FTSPI020_FEATURE_DTR_MODE       BIT(24)
#define FTSPI020_FEATURE_RXFIFO_DEPTH   GENMASK(15, 8)
#define FTSPI020_FEATURE_TXFIFO_DEPTH   GENMASK(7, 0)

/*
 * CMD1 Register offset 4: Command queue Second Word
 */
#define FTSPI020_CMD1_CONT_READMODE_EN  BIT(28)
#define FTSPI020_CMD1_OPCODE_BYTE       GENMASK(25, 24)
#define FTSPI020_CMD1_DUMMY_CYCLE       GENMASK(23, 16)
#define FTSPI020_CMD1_ADDR_BYTES        GENMASK(2, 0)

/*
 * CMD3 Register offset 0xc: Command queue Fourth Word
 */
#define FTSPI020_CMD3_INSTR_CODE        GENMASK(31, 24)
#define FTSPI020_CMD3_CONTREAD_CODE     GENMASK(23, 16)
#define FTSPI020_CMD3_CHIP_SELECT       GENMASK(9, 8)
#define FTSPI020_CMD3_PROTO             GENMASK(7, 5)
enum {
	CMD3_PROTO_SERIAL = 0,  /* 1-1-1 */
	CMD3_PROTO_DUAL,        /* 1-1-2 */
	CMD3_PROTO_QUAD,        /* 1-1-4 */
	CMD3_PROTO_DUALIO,      /* 1-2-2 */
	CMD3_PROTO_QUADIO,      /* 1-4-4 */
	CMD3_PROTO_OPCODE_2BIT, /* 2-2-2 */
	CMD3_PROTO_OPCODE_4BIT, /* 4-4-4 */
};

#define FTSPI020_CMD3_DTR_MODE_EN       BIT(4)
#define FTSPI020_CMD3_OP_MODE           GENMASK(3, 1)
enum {
	CMD3_OP_MODE_DIR_IN = 0,
	CMD3_OP_MODE_DIR_OUT,
	CMD3_OP_MODE_NODATA,
};

#define FTSPI020_CMD3_SPI_NAND          BIT(0)

struct ftspi020_priv {
	void __iomem *io_base;
	struct clk *clk;
	struct device *dev;
	struct reset_control *rstc;
	struct spi_controller *controller;
	struct mutex lock;
	u32 tx_transfer_size;
	u32 rx_transfer_size;
	u8 txfifo_depth;
	u8 rxfifo_depth;
	phys_addr_t phys_io_base;
};

static int ftspi020_wait_complete(struct ftspi020_priv *ctrl, unsigned long timeout_ms)
{
	unsigned long expire = jiffies + msecs_to_jiffies(timeout_ms);
	u32 isr;

	do {
		isr = readl(ctrl->io_base + FTSPI020_REG_ISR);
		if (isr & FTSPI020_ISR_CMD_CMPL) {
			writel(isr, ctrl->io_base + FTSPI020_REG_ISR);
			return 0;
		}

		udelay(10);
	} while (time_before(jiffies, expire));

	return -ETIMEDOUT;
}

/*
 * Access the data port a word at a time, as the vendor loader does (one
 * readl for a 1-byte status read, one writel for a 2-byte status write).
 * Byte accesses inside a transfer, used for unaligned buffers, returned
 * corrupt jffs2 data and then stalled the FIFO. A partial last word
 * carries the tail.
 */
static void ftspi020_write_fifo(void __iomem *port, const u8 *buf, u32 len)
{
	u32 val = 0;

	for (; len >= 4; len -= 4, buf += 4)
		iowrite32(get_unaligned((const u32 *)buf), port);

	if (len) {
		memcpy(&val, buf, len);
		iowrite32(val, port);
	}
}

static void ftspi020_read_fifo(void __iomem *port, u8 *buf, u32 len)
{
	u32 val;

	for (; len >= 4; len -= 4, buf += 4)
		put_unaligned(ioread32(port), (u32 *)buf);

	if (len) {
		val = ioread32(port);
		memcpy(buf, &val, len);
	}
}

static int ftspi020_do_data_pio(struct ftspi020_priv *ctrl, const struct spi_mem_op *op)
{
	void __iomem *port = ctrl->io_base + FTSPI020_REG_DATA_PORT;
	bool out = op->data.dir == SPI_MEM_DATA_OUT;
	u32 len = op->data.nbytes;
	u32 transfer_size, chunk, val;
	u8 fifo_ready_mask;
	u8 *buf;
	int ret;

	if (out) {
		buf = (u8 *)op->data.buf.out;
		transfer_size = ctrl->tx_transfer_size;
		fifo_ready_mask = FTSPI020_STS_TFR;
	} else {
		buf = op->data.buf.in;
		transfer_size = ctrl->rx_transfer_size;
		fifo_ready_mask = FTSPI020_STS_RFR;
	}

	while (len > 0) {
		chunk = min(len, transfer_size);

		/* Wait for txfifo or rxfifo ready */
		ret = readl_relaxed_poll_timeout_atomic(ctrl->io_base + FTSPI020_REG_STS, val,
							(val & fifo_ready_mask), 1,
							3000000);
		if (ret) {
			dev_err(ctrl->dev, "wait fifo timeout(len:%d(%d) sts:%#x(%#x))\n",
				len, op->data.nbytes, val, fifo_ready_mask);
			return ret;
		}

		if (out)
			ftspi020_write_fifo(port, buf, chunk);
		else
			ftspi020_read_fifo(port, buf, chunk);

		buf += chunk;
		len -= chunk;
	}

	return 0;
}

static int ftspi020_firecmd(struct ftspi020_priv *ctrl, u32 cmd3, const struct spi_mem_op *op)
{
	int err = 0;

	/* Clear any pending ISR status before firing command */
	writel(readl(ctrl->io_base + FTSPI020_REG_ISR), ctrl->io_base + FTSPI020_REG_ISR);

	/* HW starts the command */
	iowrite32(cmd3, ctrl->io_base + FTSPI020_REG_CMD3);

	if (op->data.nbytes) {
		dev_dbg(ctrl->dev, "%s: nbytes %d, dir %s\n",
			__func__, op->data.nbytes,
			(op->data.dir == SPI_MEM_DATA_IN) ? "IN" : "OUT");
		err = ftspi020_do_data_pio(ctrl, op);
		if (err)
			return err;
	}

	if (ftspi020_wait_complete(ctrl, 1000)) {
		dev_err(ctrl->dev, "wait cmd complete timeout (cmd3=%08x isr=%08x sts=%08x)\n",
			cmd3, readl(ctrl->io_base + FTSPI020_REG_ISR),
			readl(ctrl->io_base + FTSPI020_REG_STS));
		return -ETIMEDOUT;
	}

	return 0;
}

static int ftspi020_decode_proto(struct spi_mem *mem, const struct spi_mem_op *op)
{
	struct ftspi020_priv *ctrl = spi_controller_get_devdata(mem->spi->controller);
	int cmd_width, addr_width, data_width, ret;
	u32 proto;

	cmd_width = (op->cmd.buswidth) ? op->cmd.buswidth : 1;
	addr_width = (op->addr.nbytes) ? op->addr.buswidth : 1;
	data_width = (op->data.nbytes) ? op->data.buswidth : 1;

	proto = SNOR_PROTO_STR(cmd_width, addr_width, data_width);

	switch (proto) {
	case SNOR_PROTO_1_1_1:
		ret = CMD3_PROTO_SERIAL;
		break;
	case SNOR_PROTO_1_1_2:
		ret = CMD3_PROTO_DUAL;
		break;
	case SNOR_PROTO_1_1_4:
		ret = CMD3_PROTO_QUAD;
		break;
	case SNOR_PROTO_1_2_2:
		ret = CMD3_PROTO_DUALIO;
		break;
	case SNOR_PROTO_1_4_4:
		ret = CMD3_PROTO_QUADIO;
		break;
	case SNOR_PROTO_2_2_2:
		ret = CMD3_PROTO_OPCODE_2BIT;
		break;
	case SNOR_PROTO_4_4_4:
		ret = CMD3_PROTO_OPCODE_4BIT;
		break;
	default:
		dev_err(ctrl->dev, "Decode protocol %d-%d-%d error\n",
			cmd_width, addr_width, data_width);
		ret = -EINVAL;
	}

	return ret;
}

static u32 ftspi020_get_op_mode(const struct spi_mem_op *op)
{
	u32 ret = CMD3_OP_MODE_DIR_OUT;

	if (op->data.nbytes && (op->data.dir == SPI_MEM_DATA_IN))
		ret = CMD3_OP_MODE_DIR_IN;

	return ret;
}

static u32 ftspi020_setup_cmd(struct ftspi020_priv *ctrl,
			      const struct spi_mem_op *op,
			      u32 chip_select, int proto)
{
	u32 cmd;

	if (op->addr.nbytes)
		iowrite32(op->addr.val, ctrl->io_base + FTSPI020_REG_CMD0);
	else
		iowrite32(0, ctrl->io_base + FTSPI020_REG_CMD0);

	cmd = FIELD_PREP(FTSPI020_CMD1_OPCODE_BYTE, op->cmd.nbytes ? op->cmd.nbytes : 1);
	if (op->addr.nbytes)
		cmd |= FIELD_PREP(FTSPI020_CMD1_ADDR_BYTES, op->addr.nbytes);
	if (op->dummy.nbytes && op->dummy.buswidth)
		cmd |= FIELD_PREP(FTSPI020_CMD1_DUMMY_CYCLE,
				  op->dummy.nbytes * 8 / op->dummy.buswidth);

	iowrite32(cmd, ctrl->io_base + FTSPI020_REG_CMD1);
	iowrite32(op->data.nbytes, ctrl->io_base + FTSPI020_REG_CMD2);

	cmd = (FIELD_PREP(FTSPI020_CMD3_CHIP_SELECT, chip_select) |
	       FIELD_PREP(FTSPI020_CMD3_PROTO, proto) |
	       FIELD_PREP(FTSPI020_CMD3_OP_MODE, ftspi020_get_op_mode(op)) |
	       FIELD_PREP(FTSPI020_CMD3_INSTR_CODE, op->cmd.opcode) |
	       BIT(0)); /* CMD3_CMDIRQ: trigger completion interrupt / status */

	return cmd;
}

static int ftspi020_exec_op_by_cmd(struct ftspi020_priv *ctrl,
				   const struct spi_mem_op *op,
				   u32 ce, int proto)
{
	u32 cmd;

	cmd = ftspi020_setup_cmd(ctrl, op, ce, proto);
	return ftspi020_firecmd(ctrl, cmd, op);
}

static int ftspi020_exec_op(struct spi_mem *mem, const struct spi_mem_op *op)
{
	struct ftspi020_priv *ctrl = spi_controller_get_devdata(mem->spi->controller);
	struct spi_device *spi = mem->spi;
	int err, proto;
	u32 ce;

	mutex_lock(&ctrl->lock);

	dev_dbg(ctrl->dev, "cmd:%#x mode:%d.%d.%d.%d addr:%#llx.%d len:%#x\n",
		op->cmd.opcode, op->cmd.buswidth, op->addr.buswidth,
		op->dummy.buswidth, op->data.buswidth,
		op->addr.val, op->addr.nbytes, op->data.nbytes);

	proto = ftspi020_decode_proto(mem, op);
	if (proto < 0) {
		err = proto;
		goto error;
	}

	ce = spi_get_chipselect(spi, 0);
	err = ftspi020_exec_op_by_cmd(ctrl, op, ce, proto);

error:
	mutex_unlock(&ctrl->lock);
	return err;
}

static int ftspi020_check_buswidth(struct ftspi020_priv *ctrl, u8 width)
{
	switch (width) {
	case 1:
	case 2:
	case 4:
		return 0;
	}

	return -ENOTSUPP;
}

static bool ftspi020_supports_op(struct spi_mem *mem,
				 const struct spi_mem_op *op)
{
	struct ftspi020_priv *ctrl = spi_controller_get_devdata(mem->spi->controller);
	int ret;

	ret = ftspi020_check_buswidth(ctrl, op->cmd.buswidth);

	if (op->addr.nbytes)
		ret |= ftspi020_check_buswidth(ctrl, op->addr.buswidth);

	if (op->dummy.nbytes)
		ret |= ftspi020_check_buswidth(ctrl, op->dummy.buswidth);

	if (op->data.nbytes)
		ret |= ftspi020_check_buswidth(ctrl, op->data.buswidth);

	if (ret)
		return false;

	if (op->addr.nbytes > 4)
		return false;

	if (op->dummy.nbytes &&
	    (op->dummy.nbytes * 8 / op->dummy.buswidth > 255))
		return false;

	return true;
}

static const char *ftspi020_get_name(struct spi_mem *mem)
{
	struct ftspi020_priv *ctrl = spi_controller_get_devdata(mem->spi->controller);
	struct device *dev = &mem->spi->dev;
	const char *name;

	if (of_get_available_child_count(ctrl->dev->of_node) == 1)
		return dev_name(ctrl->dev);

	name = devm_kasprintf(dev, GFP_KERNEL,
			      "%s-%d", dev_name(ctrl->dev),
			      spi_get_chipselect(mem->spi, 0));
	if (!name)
		return ERR_PTR(-ENOMEM);

	return name;
}

static const struct spi_controller_mem_ops ftspi020_mem_ops = {
	.exec_op = ftspi020_exec_op,
	.supports_op = ftspi020_supports_op,
	.get_name = ftspi020_get_name,
};

static int ftspi020_setup(struct spi_device *spi)
{
	struct spi_controller *master = spi->controller;
	struct ftspi020_priv *ctrl = spi_controller_get_devdata(master);
	u32 clk_rate, divider, val;

	if (master->busy)
		return -EBUSY;

	if (!spi->max_speed_hz)
		return 0;

	clk_rate = clk_get_rate(ctrl->clk);
	if (!clk_rate)
		return 0;

	divider = 2;
	while ((clk_rate / divider) > spi->max_speed_hz) {
		divider += 2;
		if (divider > 8) {
			divider = 8;
			break;
		}
	}

	val = ioread32(ctrl->io_base + FTSPI020_REG_CTRL);
	val &= ~FTSPI020_CTRL_CLK_DIVIDER;

	switch (divider) {
	case 2:
		val |= FIELD_PREP(FTSPI020_CTRL_CLK_DIVIDER, CLK_DIVIDER_2);
		break;
	case 4:
		val |= FIELD_PREP(FTSPI020_CTRL_CLK_DIVIDER, CLK_DIVIDER_4);
		break;
	case 6:
		val |= FIELD_PREP(FTSPI020_CTRL_CLK_DIVIDER, CLK_DIVIDER_6);
		break;
	case 8:
		val |= FIELD_PREP(FTSPI020_CTRL_CLK_DIVIDER, CLK_DIVIDER_8);
		break;
	}

	if (spi->mode & SPI_CPHA)
		val |= FTSPI020_CTRL_CLK_MODE_3;
	else
		val &= ~FTSPI020_CTRL_CLK_MODE_3;

	iowrite32(val, ctrl->io_base + FTSPI020_REG_CTRL);

	dev_dbg(ctrl->dev, "spi clock %u, divider %d, ctrl %08x\n",
		clk_rate / divider, divider, ioread32(ctrl->io_base + FTSPI020_REG_CTRL));

	return 0;
}

static int ftspi020_default_setup(struct ftspi020_priv *ctrl)
{
	u32 val;

	val = ioread32(ctrl->io_base + FTSPI020_REG_CTRL);
	val &= ~(FTSPI020_CTRL_READY_LOC | FTSPI020_CTRL_CLK_DIVIDER);
	val |= (FIELD_PREP(FTSPI020_CTRL_READY_LOC, 0) |
		FIELD_PREP(FTSPI020_CTRL_CLK_DIVIDER, CLK_DIVIDER_4));
	iowrite32(val, ctrl->io_base + FTSPI020_REG_CTRL);

	val = ioread32(ctrl->io_base + FTSPI020_REG_FEATURE);
	ctrl->rxfifo_depth = FIELD_GET(FTSPI020_FEATURE_RXFIFO_DEPTH, val);
	ctrl->txfifo_depth = FIELD_GET(FTSPI020_FEATURE_TXFIFO_DEPTH, val);

	val = ioread32(ctrl->io_base + FTSPI020_REG_CR2);
	val &= ~(FTSPI020_CR2_CMD_CMPL | FTSPI020_CR2_DMA_EN);
	val |= FIELD_PREP(FTSPI020_CR2_CMD_CMPL, 1);
	val |= FIELD_PREP(FTSPI020_CR2_RX_XFR_SIZE, XFR_SIZE_256BYTES);
	val |= FIELD_PREP(FTSPI020_CR2_TX_XFR_SIZE, XFR_SIZE_256BYTES);
	val |= FIELD_PREP(FTSPI020_CR2_DMA_RX_THOD, DMA_THOD_16BYTES);
	iowrite32(val, ctrl->io_base + FTSPI020_REG_CR2);

	ctrl->rx_transfer_size = (32 << FIELD_GET(FTSPI020_CR2_RX_XFR_SIZE, val));
	ctrl->tx_transfer_size = (32 << FIELD_GET(FTSPI020_CR2_TX_XFR_SIZE, val));

	dev_info(ctrl->dev, "CR=%08x, CR2=%08x\n",
		 ioread32(ctrl->io_base + FTSPI020_REG_CTRL),
		 ioread32(ctrl->io_base + FTSPI020_REG_CR2));

	return 0;
}

static int ftspi020_probe(struct platform_device *pdev)
{
	struct spi_controller *master;
	struct device *dev = &pdev->dev;
	struct resource *res;
	struct ftspi020_priv *ctrl;
	int ret;

	master = devm_spi_alloc_host(&pdev->dev, sizeof(*ctrl));
	if (!master)
		return -ENOMEM;

	ctrl = spi_controller_get_devdata(master);
	ctrl->dev = dev;
	platform_set_drvdata(pdev, ctrl);
	ctrl->controller = master;

	/* find the resources */
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "ctrl-port");
	if (!res)
		res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	ctrl->io_base = devm_ioremap_resource(dev, res);
	if (IS_ERR(ctrl->io_base))
		return PTR_ERR(ctrl->io_base);

	ctrl->phys_io_base = res->start;

	ctrl->clk = devm_clk_get(dev, "spiclk");
	if (IS_ERR(ctrl->clk))
		return PTR_ERR(ctrl->clk);

	ret = clk_prepare_enable(ctrl->clk);
	if (ret) {
		dev_err(dev, "cannot enable SPI clock\n");
		return ret;
	}

	ctrl->rstc = devm_reset_control_get_optional_shared(dev, "rstn");
	if (!IS_ERR_OR_NULL(ctrl->rstc))
		reset_control_deassert(ctrl->rstc);

	mutex_init(&ctrl->lock);

	ret = ftspi020_default_setup(ctrl);
	if (ret)
		goto err_disable_clk;

	dev_info(dev, "reg base %px, tx fifo %d rx fifo %d, tx xfr sz %d, rx xfr sz %d\n",
		 ctrl->io_base, ctrl->txfifo_depth, ctrl->rxfifo_depth,
		 ctrl->tx_transfer_size, ctrl->rx_transfer_size);

	master->mode_bits = SPI_RX_DUAL | SPI_RX_QUAD |
			    SPI_TX_DUAL | SPI_TX_QUAD;
	master->setup = ftspi020_setup;
	master->bus_num = -1;
	master->num_chipselect = 4;
	master->mem_ops = &ftspi020_mem_ops;
	master->dev.of_node = dev->of_node;

	ret = devm_spi_register_controller(dev, master);
	if (ret)
		goto err_destroy_mutex;

	return 0;

err_destroy_mutex:
	mutex_destroy(&ctrl->lock);
err_disable_clk:
	clk_disable_unprepare(ctrl->clk);
	return ret;
}

static void ftspi020_remove(struct platform_device *pdev)
{
	struct ftspi020_priv *ctrl = platform_get_drvdata(pdev);

	clk_disable_unprepare(ctrl->clk);
	mutex_destroy(&ctrl->lock);
}

static int ftspi020_suspend(struct device *dev)
{
	return 0;
}

static int ftspi020_resume(struct device *dev)
{
	return 0;
}

static const struct of_device_id ftspi020_dt_ids[] = {
	{ .compatible = "faraday,ftspi020" },
	{ }
};
MODULE_DEVICE_TABLE(of, ftspi020_dt_ids);

static DEFINE_SIMPLE_DEV_PM_OPS(ftspi020_pm_ops, ftspi020_suspend, ftspi020_resume);

static struct platform_driver ftspi020_driver = {
	.driver = {
		.name	= "ftspi020",
		.of_match_table = ftspi020_dt_ids,
		.pm = pm_sleep_ptr(&ftspi020_pm_ops),
	},
	.probe		= ftspi020_probe,
	.remove		= ftspi020_remove,
};
module_platform_driver(ftspi020_driver);

MODULE_DESCRIPTION("Faraday FTSPI020 SPI Memory Controller Driver");
MODULE_AUTHOR("Bing-Yao, Luo <bjluo@faraday-tech.com>");
MODULE_LICENSE("GPL");
