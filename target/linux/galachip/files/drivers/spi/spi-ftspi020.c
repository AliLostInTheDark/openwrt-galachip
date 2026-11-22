// SPDX-License-Identifier: GPL-2.0-only
/*
 * SPI memory controller of the Faraday FTSPI020, as used in the GalaChip GC1601
 *
 * The controller runs one flash command at a time. The address, the opcode and
 * address lengths, the dummy cycles and the data length go into four command
 * registers; writing the last one starts the command. Data passes through a
 * FIFO that the CPU reads or fills one 32-bit word at a time.
 *
 * Copyright (C) 2026 Ifran Ali <khanu.i.i.k.2000@gmail.com>
 *
 * The register layout and the access rules were established on the hardware
 * and with Faraday Technology's driver (Copyright (C) 2020 Faraday Technology
 * Corporation, Bing-Yao Luo), whose structure this driver partly follows.
 */

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/clk.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/limits.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/reset.h>
#include <linux/spi/spi.h>
#include <linux/spi/spi-mem.h>
#include <linux/string.h>
#include <linux/time64.h>
#include <linux/types.h>

#define FTSPI020_CMD0			0x00	/* flash address */
#define FTSPI020_CMD1			0x04	/* opcode and address lengths, dummy cycles */
#define FTSPI020_CMD2			0x08	/* data length */
#define FTSPI020_CMD3			0x0c	/* opcode, CS, protocol; starts the command */
#define FTSPI020_CTRL			0x10
#define FTSPI020_STS			0x18
#define FTSPI020_CR2			0x20
#define FTSPI020_ISR			0x24
#define FTSPI020_DATA			0x100	/* FIFO port, 32-bit accesses only */

#define FTSPI020_CMD1_OPCODE_LEN	GENMASK(25, 24)
#define FTSPI020_CMD1_DUMMY_CYCLES	GENMASK(23, 16)
#define FTSPI020_CMD1_ADDR_LEN		GENMASK(2, 0)

#define FTSPI020_CMD3_OPCODE		GENMASK(31, 24)
#define FTSPI020_CMD3_CS		GENMASK(9, 8)
#define FTSPI020_CMD3_PROTO		GENMASK(7, 5)
#define FTSPI020_CMD3_DIR		GENMASK(3, 1)
/* Without this bit the command never reports completion in ISR. */
#define FTSPI020_CMD3_REPORT_DONE	BIT(0)

#define FTSPI020_DIR_IN			0
#define FTSPI020_DIR_OUT		1

/* Protocol codes of CMD3, named command-address-data lines. */
#define FTSPI020_PROTO_1_1_1		0
#define FTSPI020_PROTO_1_1_2		1
#define FTSPI020_PROTO_1_1_4		2
#define FTSPI020_PROTO_1_2_2		3
#define FTSPI020_PROTO_1_4_4		4
#define FTSPI020_PROTO_2_2_2		5
#define FTSPI020_PROTO_4_4_4		6

#define FTSPI020_CTRL_READY_LOC		GENMASK(18, 16)
#define FTSPI020_CTRL_ABORT		BIT(8)
#define FTSPI020_CTRL_MODE3		BIT(4)
/* SCK is the clock divided by 2, 4, 6 or 8: the field holds divider / 2 - 1. */
#define FTSPI020_CTRL_DIVIDER		GENMASK(1, 0)

#define FTSPI020_STS_TX_READY		BIT(0)
#define FTSPI020_STS_RX_READY		BIT(1)

#define FTSPI020_CR2_DMA_RX_THRESHOLD	GENMASK(22, 20)
#define FTSPI020_CR2_RX_XFER_SIZE	GENMASK(13, 12)
#define FTSPI020_CR2_TX_XFER_SIZE	GENMASK(9, 8)
#define FTSPI020_CR2_DONE_ENABLE	BIT(1)
#define FTSPI020_CR2_DMA_ENABLE		BIT(0)

#define FTSPI020_ISR_DONE		BIT(0)	/* write 1 to clear */

/* The FIFO signals ready once it holds, or has room for, this many bytes. */
#define FTSPI020_XFER_SIZE_CODE		3
#define FTSPI020_XFER_BYTES		256
#define FTSPI020_DMA_THRESHOLD_16B	1

#define FTSPI020_MAX_CS			4
#define FTSPI020_MAX_DIVIDER		8
#define FTSPI020_BULK_MIN_BYTES		64
#define FTSPI020_DONE_TIMEOUT_US	USEC_PER_SEC
#define FTSPI020_FIFO_TIMEOUT_US	(3 * USEC_PER_SEC)

/*
 * Speed limits of a chip select. Reads of at least FTSPI020_BULK_MIN_BYTES may
 * use the full spi-max-frequency. Everything else (identification, status,
 * programming, erasing) is held to galachip,safe-max-frequency when the board
 * sets one, because only bulk reads were checked at the higher speeds.
 */
struct ftspi020_cs {
	unsigned int bulk_hz;
	unsigned int safe_hz;
	bool mode3;
};

struct ftspi020 {
	void __iomem *regs;
	struct clk *clk;
	struct ftspi020_cs cs[FTSPI020_MAX_CS];
	bool wedged;	/* an abort failed; try again before the next command */
};

static const struct {
	u8 cmd, addr, data;
	u8 proto;
} ftspi020_protos[] = {
	{ 1, 1, 1, FTSPI020_PROTO_1_1_1 },
	{ 1, 1, 2, FTSPI020_PROTO_1_1_2 },
	{ 1, 1, 4, FTSPI020_PROTO_1_1_4 },
	{ 1, 2, 2, FTSPI020_PROTO_1_2_2 },
	{ 1, 4, 4, FTSPI020_PROTO_1_4_4 },
	{ 2, 2, 2, FTSPI020_PROTO_2_2_2 },
	{ 4, 4, 4, FTSPI020_PROTO_4_4_4 },
};

static int ftspi020_proto(const struct spi_mem_op *op)
{
	u8 cmd = op->cmd.buswidth ?: 1;
	u8 addr = op->addr.nbytes ? op->addr.buswidth : 1;
	u8 data = op->data.nbytes ? op->data.buswidth : 1;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(ftspi020_protos); i++)
		if (ftspi020_protos[i].cmd == cmd && ftspi020_protos[i].addr == addr &&
		    ftspi020_protos[i].data == data)
			return ftspi020_protos[i].proto;

	return -EINVAL;
}

static bool ftspi020_supports_op(struct spi_mem *mem, const struct spi_mem_op *op)
{
	u8 dummy_width = op->addr.nbytes ? op->addr.buswidth : op->cmd.buswidth;

	if (!spi_mem_default_supports_op(mem, op))
		return false;

	if (op->cmd.dtr || op->addr.dtr || op->dummy.dtr || op->data.dtr)
		return false;

	if (op->addr.nbytes > 4 || ftspi020_proto(op) < 0)
		return false;

	/* The command engine clocks dummy cycles at the width of the address phase. */
	if (op->dummy.nbytes &&
	    (op->dummy.buswidth != dummy_width ||
	     op->dummy.nbytes * 8 / op->dummy.buswidth > 255))
		return false;

	return true;
}

/* MTD device name: the controller's, plus the chip select when it has several flashes. */
static const char *ftspi020_get_name(struct spi_mem *mem)
{
	struct device *ctrl_dev = mem->spi->controller->dev.parent;

	if (of_get_available_child_count(ctrl_dev->of_node) == 1)
		return dev_name(ctrl_dev);

	return devm_kasprintf(&mem->spi->dev, GFP_KERNEL, "%s-%d", dev_name(ctrl_dev),
			      spi_get_chipselect(mem->spi, 0));
}

static void ftspi020_set_sck(struct ftspi020 *spi, const struct ftspi020_cs *cs,
			     unsigned int hz)
{
	unsigned long rate = clk_get_rate(spi->clk);
	unsigned int divider = 2;
	u32 ctrl, val;

	while (divider < FTSPI020_MAX_DIVIDER && rate / divider > hz)
		divider += 2;

	ctrl = readl(spi->regs + FTSPI020_CTRL);
	val = (ctrl & ~(FTSPI020_CTRL_DIVIDER | FTSPI020_CTRL_MODE3)) |
	      FIELD_PREP(FTSPI020_CTRL_DIVIDER, divider / 2 - 1) |
	      (cs->mode3 ? FTSPI020_CTRL_MODE3 : 0);
	if (val != ctrl)
		writel(val, spi->regs + FTSPI020_CTRL);
}

static unsigned int ftspi020_op_hz(const struct ftspi020_cs *cs, const struct spi_mem_op *op)
{
	bool bulk = op->data.dir == SPI_MEM_DATA_IN && op->data.nbytes >= FTSPI020_BULK_MIN_BYTES;
	unsigned int hz = bulk ? cs->bulk_hz : cs->safe_hz;

	if (op->max_freq)
		hz = min(hz, op->max_freq);

	return hz;
}

static void ftspi020_start(struct ftspi020 *spi, const struct spi_mem_op *op,
			   unsigned int cs, int proto)
{
	u32 cmd1 = FIELD_PREP(FTSPI020_CMD1_OPCODE_LEN, op->cmd.nbytes ?: 1) |
		   FIELD_PREP(FTSPI020_CMD1_ADDR_LEN, op->addr.nbytes);
	bool in = op->data.nbytes && op->data.dir == SPI_MEM_DATA_IN;
	u32 cmd3;

	if (op->dummy.nbytes)
		cmd1 |= FIELD_PREP(FTSPI020_CMD1_DUMMY_CYCLES,
				   op->dummy.nbytes * 8 / op->dummy.buswidth);

	cmd3 = FIELD_PREP(FTSPI020_CMD3_OPCODE, op->cmd.opcode) |
	       FIELD_PREP(FTSPI020_CMD3_CS, cs) |
	       FIELD_PREP(FTSPI020_CMD3_PROTO, proto) |
	       FIELD_PREP(FTSPI020_CMD3_DIR, in ? FTSPI020_DIR_IN : FTSPI020_DIR_OUT) |
	       FTSPI020_CMD3_REPORT_DONE;

	writel(op->addr.nbytes ? op->addr.val : 0, spi->regs + FTSPI020_CMD0);
	writel(cmd1, spi->regs + FTSPI020_CMD1);
	writel(op->data.nbytes, spi->regs + FTSPI020_CMD2);

	/* Forget the status of the previous command, then start this one. */
	writel(readl(spi->regs + FTSPI020_ISR), spi->regs + FTSPI020_ISR);
	writel(cmd3, spi->regs + FTSPI020_CMD3);
}

/*
 * Move the data a word at a time. Byte accesses to the FIFO port corrupted
 * data and stalled the FIFO, so a partial last word is read or written whole.
 */
static int ftspi020_transfer(struct ftspi020 *spi, const struct spi_mem_op *op)
{
	void __iomem *fifo = spi->regs + FTSPI020_DATA;
	bool in = op->data.dir == SPI_MEM_DATA_IN;
	u32 ready = in ? FTSPI020_STS_RX_READY : FTSPI020_STS_TX_READY;
	unsigned int left = op->data.nbytes;
	const u8 *out = op->data.buf.out;
	u8 *buf = op->data.buf.in;
	u32 sts, tail;
	int ret;

	while (left) {
		unsigned int n = min(left, FTSPI020_XFER_BYTES);

		ret = readl_poll_timeout_atomic(spi->regs + FTSPI020_STS, sts, sts & ready, 0,
						FTSPI020_FIFO_TIMEOUT_US);
		if (ret)
			return ret;

		if (in) {
			ioread32_rep(fifo, buf, n / 4);
			buf += n & ~3;
			if (n & 3) {
				tail = ioread32(fifo);
				memcpy(buf, &tail, n & 3);
				buf += n & 3;
			}
		} else {
			iowrite32_rep(fifo, out, n / 4);
			out += n & ~3;
			if (n & 3) {
				tail = 0;
				memcpy(&tail, out, n & 3);
				iowrite32(tail, fifo);
				out += n & 3;
			}
		}
		left -= n;
	}

	return 0;
}

static int ftspi020_wait_done(struct ftspi020 *spi)
{
	u32 isr;
	int ret;

	ret = readl_poll_timeout_atomic(spi->regs + FTSPI020_ISR, isr, isr & FTSPI020_ISR_DONE, 1,
					FTSPI020_DONE_TIMEOUT_US);
	if (!ret)
		writel(isr, spi->regs + FTSPI020_ISR);

	return ret;
}

static int ftspi020_abort(struct ftspi020 *spi)
{
	u32 ctrl;
	int ret;

	writel(readl(spi->regs + FTSPI020_CTRL) | FTSPI020_CTRL_ABORT, spi->regs + FTSPI020_CTRL);
	ret = readl_poll_timeout(spi->regs + FTSPI020_CTRL, ctrl, !(ctrl & FTSPI020_CTRL_ABORT),
				 10, USEC_PER_SEC);
	spi->wedged = ret;
	if (!ret)
		writel(readl(spi->regs + FTSPI020_ISR), spi->regs + FTSPI020_ISR);

	return ret;
}

static int ftspi020_exec_op(struct spi_mem *mem, const struct spi_mem_op *op)
{
	struct spi_device *spidev = mem->spi;
	struct ftspi020 *spi = spi_controller_get_devdata(spidev->controller);
	unsigned int cs = spi_get_chipselect(spidev, 0);
	int proto = ftspi020_proto(op);
	int ret = 0;

	if (proto < 0)
		return proto;

	if (spi->wedged) {
		ret = ftspi020_abort(spi);
		if (ret)
			return ret;
	}

	ftspi020_set_sck(spi, &spi->cs[cs], ftspi020_op_hz(&spi->cs[cs], op));
	ftspi020_start(spi, op, cs, proto);

	if (op->data.nbytes)
		ret = ftspi020_transfer(spi, op);
	if (!ret)
		ret = ftspi020_wait_done(spi);
	if (ret) {
		dev_err(&spidev->dev, "command %#x failed: %d\n", op->cmd.opcode, ret);
		/* The command is dropped, never repeated: a repeated write could do harm. */
		ftspi020_abort(spi);
	}

	return ret;
}

static const struct spi_controller_mem_ops ftspi020_mem_ops = {
	.supports_op = ftspi020_supports_op,
	.exec_op = ftspi020_exec_op,
	.get_name = ftspi020_get_name,
};

static int ftspi020_setup(struct spi_device *spidev)
{
	struct ftspi020 *spi = spi_controller_get_devdata(spidev->controller);
	struct ftspi020_cs *cs = &spi->cs[spi_get_chipselect(spidev, 0)];
	u32 safe_hz = 0;
	int ret;

	/* Only SPI modes 0 and 3 exist. */
	if (!(spidev->mode & SPI_CPOL) != !(spidev->mode & SPI_CPHA))
		return -EINVAL;

	cs->mode3 = spidev->mode & SPI_CPHA;
	cs->bulk_hz = spidev->max_speed_hz;
	of_property_read_u32(spidev->dev.of_node, "galachip,safe-max-frequency", &safe_hz);
	cs->safe_hz = safe_hz ? min(safe_hz, cs->bulk_hz) : cs->bulk_hz;

	/* SCK is the clock divided by at least 2: ask for the clock the fastest read needs. */
	ret = clk_set_rate(spi->clk, 2UL * cs->bulk_hz);
	if (ret)
		dev_warn(&spidev->dev, "SPI clock stays at %lu Hz: %d\n",
			 clk_get_rate(spi->clk), ret);

	dev_dbg(&spidev->dev, "large reads up to %u Hz, other commands up to %u Hz, SPI clock %lu Hz\n",
		cs->bulk_hz, cs->safe_hz, clk_get_rate(spi->clk));

	return 0;
}

static int ftspi020_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct reset_control *reset;
	struct spi_controller *host;
	struct ftspi020 *spi;
	long max_clk;
	u32 val;

	host = devm_spi_alloc_host(dev, sizeof(*spi));
	if (!host)
		return -ENOMEM;
	spi = spi_controller_get_devdata(host);

	spi->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(spi->regs))
		return PTR_ERR(spi->regs);

	spi->clk = devm_clk_get_enabled(dev, "spiclk");
	if (IS_ERR(spi->clk))
		return dev_err_probe(dev, PTR_ERR(spi->clk), "cannot get the SPI clock\n");

	reset = devm_reset_control_get_optional_shared_deasserted(dev, NULL);
	if (IS_ERR(reset))
		return dev_err_probe(dev, PTR_ERR(reset), "cannot release the reset\n");

	/* Start at the slowest SCK; setup() picks the speed of each flash. */
	val = readl(spi->regs + FTSPI020_CTRL);
	val &= ~(FTSPI020_CTRL_READY_LOC | FTSPI020_CTRL_DIVIDER);
	val |= FIELD_PREP(FTSPI020_CTRL_DIVIDER, FTSPI020_MAX_DIVIDER / 2 - 1);
	writel(val, spi->regs + FTSPI020_CTRL);

	/* Report completion in ISR, signal the FIFO per 256 bytes, no DMA. */
	val = readl(spi->regs + FTSPI020_CR2);
	val &= ~(FTSPI020_CR2_DMA_ENABLE | FTSPI020_CR2_DMA_RX_THRESHOLD |
		 FTSPI020_CR2_RX_XFER_SIZE | FTSPI020_CR2_TX_XFER_SIZE);
	val |= FTSPI020_CR2_DONE_ENABLE |
	       FIELD_PREP(FTSPI020_CR2_DMA_RX_THRESHOLD, FTSPI020_DMA_THRESHOLD_16B) |
	       FIELD_PREP(FTSPI020_CR2_RX_XFER_SIZE, FTSPI020_XFER_SIZE_CODE) |
	       FIELD_PREP(FTSPI020_CR2_TX_XFER_SIZE, FTSPI020_XFER_SIZE_CODE);
	writel(val, spi->regs + FTSPI020_CR2);

	host->mode_bits = SPI_CPOL | SPI_CPHA | SPI_RX_DUAL | SPI_RX_QUAD | SPI_TX_DUAL |
			  SPI_TX_QUAD;
	/* The core limits each flash to this: the fastest SCK the clock can give. */
	max_clk = clk_round_rate(spi->clk, ULONG_MAX);
	host->max_speed_hz = (max_clk > 0 ? max_clk : clk_get_rate(spi->clk)) / 2;
	host->num_chipselect = FTSPI020_MAX_CS;
	host->bus_num = -1;
	host->setup = ftspi020_setup;
	host->mem_ops = &ftspi020_mem_ops;
	host->dev.of_node = dev->of_node;

	return devm_spi_register_controller(dev, host);
}

static const struct of_device_id ftspi020_of_match[] = {
	{ .compatible = "faraday,ftspi020" },
	{ }
};
MODULE_DEVICE_TABLE(of, ftspi020_of_match);

static struct platform_driver ftspi020_driver = {
	.probe = ftspi020_probe,
	.driver = {
		.name = "ftspi020",
		.of_match_table = ftspi020_of_match,
	},
};
module_platform_driver(ftspi020_driver);

MODULE_DESCRIPTION("Faraday FTSPI020 SPI memory controller driver");
MODULE_AUTHOR("Ifran Ali <khanu.i.i.k.2000@gmail.com>");
MODULE_LICENSE("GPL");
