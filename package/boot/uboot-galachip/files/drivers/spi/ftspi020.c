// SPDX-License-Identifier: GPL-2.0-only
/*
 * SPI memory controller driver for the Faraday FTSPI020 in the GalaChip GC1601
 *
 * One lane for everything, plus 1-1-2 dual reads when the flash asks for
 * SPI_RX_DUAL. Reads of FTSPI020_BULK_MIN_BYTES or more run at the flash's
 * spi-max-frequency; every other command (identification, status, programming,
 * erasing) is held to galachip,safe-max-frequency when the flash node sets one,
 * because only bulk reads were checked at the higher speeds.
 *
 * The driver passes the commands of the spi-mem layer to the
 * controller. The only thing it refuses is changing a range that the flash node
 * lists in galachip,protected-ranges (offset and size pairs), which the board
 * uses for the factory data; a chip erase is refused while any range is set.
 *
 * Copyright (C) 2026 Ifran Ali <khanu.i.i.k.2000@gmail.com>
 *
 * The register layout and the access rules were established on the hardware
 * and with Faraday Technology's driver (Copyright (C) 2020 Faraday Technology
 * Corporation, Bing-Yao Luo), whose structure this driver partly follows.
 */
#include <clk.h>
#include <dm.h>
#include <spi.h>
#include <spi-mem.h>
#include <asm/io.h>
#include <linux/err.h>
#include <linux/iopoll.h>

#define FTSPI020_CMD0			0x00	/* flash address */
#define FTSPI020_CMD1			0x04	/* opcode and address lengths, dummy cycles */
#define FTSPI020_CMD2			0x08	/* data length */
#define FTSPI020_CMD3			0x0c	/* opcode, protocol, direction; starts it */
#define FTSPI020_CTRL			0x10
#define FTSPI020_STS			0x18
#define FTSPI020_CR2			0x20
#define FTSPI020_ISR			0x24
#define FTSPI020_DATA			0x100	/* FIFO port, 32-bit accesses only */

#define FTSPI020_CMD1_OPCODE_1BYTE	BIT(24)
#define FTSPI020_CMD1_DUMMY_SHIFT	16

#define FTSPI020_CMD3_OPCODE_SHIFT	24
#define FTSPI020_CMD3_PROTO_1_1_2	BIT(5)
#define FTSPI020_CMD3_DIR_OUT		BIT(1)	/* also set when there is no data */
/* Without this bit the command never reports completion in ISR. */
#define FTSPI020_CMD3_REPORT_DONE	BIT(0)

#define FTSPI020_CTRL_ABORT		BIT(8)
#define FTSPI020_CTRL_MODE3		BIT(4)
/* SCK is the clock divided by 2, 4, 6 or 8: the field holds divider / 2 - 1. */
#define FTSPI020_CTRL_DIVIDER		3U
#define FTSPI020_MAX_DIVIDER		8

#define FTSPI020_STS_TX_READY		BIT(0)
#define FTSPI020_STS_RX_READY		BIT(1)

#define FTSPI020_CR2_DMA_ENABLE		BIT(0)
#define FTSPI020_CR2_DONE_ENABLE	BIT(1)
#define FTSPI020_CR2_TX_XFER_256B	(3U << 8)
#define FTSPI020_CR2_RX_XFER_256B	(3U << 12)

#define FTSPI020_ISR_DONE		BIT(0)	/* write 1 to clear */

/* The FIFO signals ready once it holds, or has room for, this many bytes. */
#define FTSPI020_XFER_BYTES		256U
/* The longest transfer of one command that was checked. */
#define FTSPI020_MAX_DATA		0x10000U
#define FTSPI020_MAX_DUMMY_CYCLES	255
#define FTSPI020_BULK_MIN_BYTES		64
#define FTSPI020_TIMEOUT_US		100000

/* Flash commands that change the array. */
#define FTSPI020_OP_PAGE_PROGRAM	0x02
#define FTSPI020_OP_ERASE_4K		0x20
#define FTSPI020_OP_ERASE_32K		0x52
#define FTSPI020_OP_ERASE_64K		0xd8
#define FTSPI020_OP_CHIP_ERASE		0xc7
#define FTSPI020_OP_CHIP_ERASE_ALT	0x60
#define FTSPI020_PAGE_SIZE		0x100U

#define FTSPI020_MAX_PROTECTED		4

struct ftspi020 {
	void *base;
	struct clk clk;
	ulong input_rate;
	uint bulk_hz;		/* reads of FTSPI020_BULK_MIN_BYTES or more */
	uint safe_hz;		/* everything else */
	uint dt_safe_hz;	/* galachip,safe-max-frequency, 0 when unset */
	bool wedged;	/* an abort failed; try again before the next command */
	unsigned int nprotected;
	struct {
		u32 start, end;
	} protected[FTSPI020_MAX_PROTECTED];
};

static int ftspi020_abort(struct ftspi020 *spi)
{
	u32 val;
	int ret;

	setbits_le32(spi->base + FTSPI020_CTRL, FTSPI020_CTRL_ABORT);
	ret = readl_poll_timeout(spi->base + FTSPI020_CTRL, val, !(val & FTSPI020_CTRL_ABORT),
				 FTSPI020_TIMEOUT_US);
	spi->wedged = !!ret;
	if (!ret)
		writel(readl(spi->base + FTSPI020_ISR), spi->base + FTSPI020_ISR);

	return ret;
}

static bool ftspi020_supports_op(struct spi_slave *slave, const struct spi_mem_op *op)
{
	if (!spi_mem_default_supports_op(slave, op))
		return false;

	/* One lane for the opcode, address and dummy cycles; the data may be dual when read. */
	if (op->cmd.nbytes != 1 || op->cmd.buswidth != 1)
		return false;
	if (op->addr.nbytes > 3 || (op->addr.nbytes && op->addr.buswidth != 1))
		return false;
	if (op->dummy.nbytes &&
	    (op->dummy.buswidth != 1 || op->dummy.nbytes * 8 > FTSPI020_MAX_DUMMY_CYCLES))
		return false;
	if (op->data.nbytes && op->data.buswidth != 1 &&
	    !(op->data.buswidth == 2 && op->data.dir == SPI_MEM_DATA_IN))
		return false;

	return true;
}

static int ftspi020_adjust_op_size(struct spi_slave *slave, struct spi_mem_op *op)
{
	op->data.nbytes = min(op->data.nbytes, FTSPI020_MAX_DATA);

	return 0;
}

/* The bytes a program or erase command changes, as [*start, *end); false for other commands. */
static bool ftspi020_write_span(const struct spi_mem_op *op, u32 *start, u32 *end)
{
	u32 addr = op->addr.val, size;

	switch (op->cmd.opcode) {
	case FTSPI020_OP_PAGE_PROGRAM:
		/* A program that runs past its page wraps inside it. */
		*start = addr & ~(FTSPI020_PAGE_SIZE - 1);
		*end = max(*start + FTSPI020_PAGE_SIZE, addr + op->data.nbytes);
		return true;
	case FTSPI020_OP_ERASE_4K:
		size = 0x1000;
		break;
	case FTSPI020_OP_ERASE_32K:
		size = 0x8000;
		break;
	case FTSPI020_OP_ERASE_64K:
		size = 0x10000;
		break;
	default:
		return false;
	}

	/* An erase clears its whole block, whatever the low address bits. */
	*start = addr & ~(size - 1);
	*end = *start + size;

	return true;
}

static int ftspi020_check_protected(const struct ftspi020 *spi, const struct spi_mem_op *op)
{
	unsigned int i;
	u32 start, end;

	if (!spi->nprotected)
		return 0;

	if (op->cmd.opcode == FTSPI020_OP_CHIP_ERASE ||
	    op->cmd.opcode == FTSPI020_OP_CHIP_ERASE_ALT)
		return -EROFS;

	if (!ftspi020_write_span(op, &start, &end))
		return 0;

	for (i = 0; i < spi->nprotected; i++)
		if (start < spi->protected[i].end && end > spi->protected[i].start)
			return -EROFS;

	return 0;
}

/*
 * Move the data a word at a time. Byte accesses to the FIFO port corrupted
 * data and stalled the FIFO, so a partial last word is read or written whole.
 * The CPU moves every word itself, so the data cache needs no maintenance.
 */
static int ftspi020_transfer(struct ftspi020 *spi, const struct spi_mem_op *op)
{
	bool out = op->data.dir == SPI_MEM_DATA_OUT;
	u32 ready = out ? FTSPI020_STS_TX_READY : FTSPI020_STS_RX_READY;
	const u8 *src = op->data.buf.out;
	u8 *dst = op->data.buf.in;
	u32 left = op->data.nbytes;
	u32 chunk, word, val, n;
	int ret;

	while (left) {
		ret = readl_poll_timeout(spi->base + FTSPI020_STS, val, val & ready,
					 FTSPI020_TIMEOUT_US);
		if (ret)
			return ret;

		chunk = min(left, FTSPI020_XFER_BYTES);
		left -= chunk;

		if (out && !((uintptr_t)src & 3)) {
			for (; chunk >= 4; chunk -= 4, src += 4)
				writel(*(const u32 *)src, spi->base + FTSPI020_DATA);
		} else if (!out && !((uintptr_t)dst & 3)) {
			for (; chunk >= 4; chunk -= 4, dst += 4)
				*(u32 *)dst = readl(spi->base + FTSPI020_DATA);
		}

		while (chunk) {
			n = min(chunk, 4U);
			if (out) {
				word = 0;
				memcpy(&word, src, n);
				writel(word, spi->base + FTSPI020_DATA);
				src += n;
			} else {
				word = readl(spi->base + FTSPI020_DATA);
				memcpy(dst, &word, n);
				dst += n;
			}
			chunk -= n;
		}
	}

	return 0;
}

static void ftspi020_set_sck(struct ftspi020 *spi, uint hz)
{
	unsigned int divider = 2;

	while (divider < FTSPI020_MAX_DIVIDER && spi->input_rate / divider > hz)
		divider += 2;

	clrsetbits_le32(spi->base + FTSPI020_CTRL, FTSPI020_CTRL_DIVIDER, divider / 2 - 1);
}

static int ftspi020_exec_op(struct spi_slave *slave, const struct spi_mem_op *op)
{
	struct ftspi020 *spi = dev_get_priv(dev_get_parent(slave->dev));
	bool in = op->data.nbytes && op->data.dir == SPI_MEM_DATA_IN;
	u32 cmd3, val;
	int ret;

	if (!ftspi020_supports_op(slave, op) || spi_chip_select(slave->dev) != 0)
		return -ENOTSUPP;
	if (op->data.nbytes > FTSPI020_MAX_DATA)
		return -EINVAL;
	ret = ftspi020_check_protected(spi, op);
	if (ret)
		return ret;
	if (spi->wedged && ftspi020_abort(spi))
		return -EIO;

	ftspi020_set_sck(spi, in && op->data.nbytes >= FTSPI020_BULK_MIN_BYTES ?
			 spi->bulk_hz : spi->safe_hz);

	/* Forget the status of the previous command, then describe and start this one. */
	writel(FTSPI020_ISR_DONE, spi->base + FTSPI020_ISR);
	writel(op->addr.val, spi->base + FTSPI020_CMD0);
	writel(FTSPI020_CMD1_OPCODE_1BYTE | op->addr.nbytes |
	       (op->dummy.nbytes * 8) << FTSPI020_CMD1_DUMMY_SHIFT, spi->base + FTSPI020_CMD1);
	writel(op->data.nbytes, spi->base + FTSPI020_CMD2);

	cmd3 = (u32)op->cmd.opcode << FTSPI020_CMD3_OPCODE_SHIFT | FTSPI020_CMD3_REPORT_DONE;
	if (op->data.buswidth == 2)
		cmd3 |= FTSPI020_CMD3_PROTO_1_1_2;
	if (!in)
		cmd3 |= FTSPI020_CMD3_DIR_OUT;
	writel(cmd3, spi->base + FTSPI020_CMD3);

	ret = op->data.nbytes ? ftspi020_transfer(spi, op) : 0;
	if (!ret)
		ret = readl_poll_timeout(spi->base + FTSPI020_ISR, val, val & FTSPI020_ISR_DONE,
					 FTSPI020_TIMEOUT_US);
	if (ret) {
		/* The command is dropped, never repeated: a repeated write could do harm. */
		ftspi020_abort(spi);
		return ret;
	}

	writel(FTSPI020_ISR_DONE, spi->base + FTSPI020_ISR);

	return 0;
}

static int ftspi020_set_speed(struct udevice *bus, uint hz)
{
	struct ftspi020 *spi = dev_get_priv(bus);
	ulong rate;

	if (!hz)
		return -EINVAL;

	/* SCK is the clock divided by at least 2: ask for the clock the fastest read needs. */
	rate = clk_set_rate(&spi->clk, 2UL * hz);
	if (!IS_ERR_VALUE(rate) && rate)
		spi->input_rate = rate;

	spi->bulk_hz = hz;
	spi->safe_hz = spi->dt_safe_hz ? min(spi->dt_safe_hz, hz) : hz;
	if (spi->input_rate / FTSPI020_MAX_DIVIDER > spi->safe_hz)
		return -ERANGE;

	if (!IS_ENABLED(CONFIG_XPL_BUILD))
		printf("%s: reads up to %u Hz, other commands up to %u Hz, SPI clock %lu Hz\n",
		       bus->name, spi->bulk_hz, spi->safe_hz, spi->input_rate);

	return 0;
}

static int ftspi020_set_mode(struct udevice *bus, uint mode)
{
	struct ftspi020 *spi = dev_get_priv(bus);

	if (mode & ~SPI_RX_DUAL)
		return -ENOTSUPP;

	clrbits_le32(spi->base + FTSPI020_CTRL, FTSPI020_CTRL_MODE3);

	return 0;
}

/*
 * The flash node's slow speed, and the ranges the board keeps the flash driver
 * away from, as offset and size pairs.
 */
static void ftspi020_read_flash(struct udevice *bus, struct ftspi020 *spi)
{
	static const char prop[] = "galachip,protected-ranges";
	ofnode node;
	int pairs, i;
	u32 offset, size;

	dev_for_each_subnode(node, bus) {
		spi->dt_safe_hz = ofnode_read_u32_default(node, "galachip,safe-max-frequency", 0);
		pairs = ofnode_read_size(node, prop) / (2 * sizeof(u32));

		for (i = 0; i < pairs && spi->nprotected < FTSPI020_MAX_PROTECTED; i++) {
			if (ofnode_read_u32_index(node, prop, 2 * i, &offset) ||
			    ofnode_read_u32_index(node, prop, 2 * i + 1, &size) ||
			    !size)
				continue;

			spi->protected[spi->nprotected].start = offset;
			spi->protected[spi->nprotected].end = offset + size;
			spi->nprotected++;
		}
	}
}

static int ftspi020_probe(struct udevice *bus)
{
	struct ftspi020 *spi = dev_get_priv(bus);
	int ret;

	spi->base = dev_read_addr_ptr(bus);
	if (!spi->base)
		return -EINVAL;

	ret = clk_get_by_index(bus, 0, &spi->clk);
	if (ret)
		return ret;
	spi->input_rate = clk_get_rate(&spi->clk);
	if (IS_ERR_VALUE(spi->input_rate) || !spi->input_rate)
		return -EINVAL;

	/* Polled PIO in 256-byte bursts, completion reported in ISR; no interrupt or DMA. */
	clrsetbits_le32(spi->base + FTSPI020_CR2,
			FTSPI020_CR2_DMA_ENABLE | FTSPI020_CR2_DONE_ENABLE | (3U << 8) | (3U << 12),
			FTSPI020_CR2_DONE_ENABLE | FTSPI020_CR2_TX_XFER_256B |
			FTSPI020_CR2_RX_XFER_256B);

	ftspi020_read_flash(bus, spi);

	return ftspi020_abort(spi);
}

static const struct spi_controller_mem_ops ftspi020_mem_ops = {
	.supports_op = ftspi020_supports_op,
	.exec_op = ftspi020_exec_op,
	.adjust_op_size = ftspi020_adjust_op_size,
};

static const struct dm_spi_ops ftspi020_ops = {
	.set_speed = ftspi020_set_speed,
	.set_mode = ftspi020_set_mode,
	.mem_ops = &ftspi020_mem_ops,
};

static const struct udevice_id ftspi020_ids[] = {
	{ .compatible = "faraday,ftspi020" },
	{ }
};

U_BOOT_DRIVER(ftspi020) = {
	.name = "ftspi020",
	.id = UCLASS_SPI,
	.of_match = ftspi020_ids,
	.probe = ftspi020_probe,
	.ops = &ftspi020_ops,
	.priv_auto = sizeof(struct ftspi020),
};
