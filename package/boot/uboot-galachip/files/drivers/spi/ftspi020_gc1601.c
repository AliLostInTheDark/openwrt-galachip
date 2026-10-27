// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * FTSPI020 single-lane, read-only SPI-memory controller for GC1601.
 * Based on Faraday's GPL implementation, Copyright (C) 2020 Faraday Technology
 * Author: Bing-Yao Luo <bjluo@faraday-tech.com>
 */
#include <clk.h>
#include <dm.h>
#include <spi.h>
#include <spi-mem.h>
#include <asm/io.h>
#include <linux/err.h>
#include <linux/iopoll.h>

struct gc_spi { void *base; ulong input_rate; bool failed; };

static int gc_spi_abort(struct gc_spi *p)
{
	u32 val;
	int ret;

	setbits_le32(p->base + 0x10, BIT(8));
	ret = readl_poll_timeout(p->base + 0x10, val, !(val & BIT(8)), 100000);
	p->failed = !!ret;
	if (!ret)
		writel(readl(p->base + 0x24), p->base + 0x24);
	return ret;
}

static bool gc_spi_supports(struct spi_slave *slave, const struct spi_mem_op *op)
{
	return spi_mem_default_supports_op(slave, op) &&
		op->cmd.nbytes == 1 && op->cmd.buswidth == 1 &&
		op->addr.nbytes <= 3 && (!op->addr.nbytes || op->addr.buswidth == 1) &&
		(!op->dummy.nbytes || op->dummy.buswidth == 1) &&
		op->dummy.nbytes <= 31 &&
		(!op->data.nbytes || op->data.buswidth == 1);
}

static int gc_spi_exec(struct spi_slave *slave, const struct spi_mem_op *op)
{
	struct gc_spi *p = dev_get_priv(dev_get_parent(slave->dev));
	u32 word, val, left = op->data.nbytes, cmd, n, chunk;
	u8 *dst = op->data.buf.in;
	int ret;

	if (!gc_spi_supports(slave, op) || spi_chip_select(slave->dev) != 0)
		return -ENOTSUPP;
	/* No WREN, status-register writes, program or erase in the first release. */
	if (op->data.dir != SPI_MEM_DATA_IN ||
	    (op->cmd.opcode != 0x03 && op->cmd.opcode != 0x0b &&
	     op->cmd.opcode != 0x05 && op->cmd.opcode != 0x9f &&
	     op->cmd.opcode != 0x35 && op->cmd.opcode != 0x15 &&
	     op->cmd.opcode != 0x5a))
		return -EROFS;
	if (!left || left > 0x10000 ||
	    (op->addr.nbytes && (op->addr.val > 0xffffff ||
	     left > 0x1000000 - op->addr.val)))
		return -EINVAL;
	if (p->failed && gc_spi_abort(p))
		return -EIO;
	writel(1, p->base + 0x24);
	writel(op->addr.val, p->base);
	writel(BIT(24) | op->addr.nbytes | (op->dummy.nbytes * 8 << 16), p->base + 4);
	writel(left, p->base + 8);
	cmd = (u32)op->cmd.opcode << 24;
	writel(cmd | BIT(0), p->base + 0x0c);
	while (left) {
		ret = readl_poll_timeout(p->base + 0x18, val, val & BIT(1), 100000);
		if (ret)
			goto fail;
		/* CR2 selects a 32-byte PIO transfer; the FIFO port is word-wide. */
		chunk = min(left, 32U);
		left -= chunk;
		while (chunk) {
			word = readl(p->base + 0x100);
			n = min(chunk, 4U);
			memcpy(dst, &word, n);
			dst += n;
			chunk -= n;
		}
	}
	ret = readl_poll_timeout(p->base + 0x24, val, val & BIT(0), 100000);
	if (ret)
		goto fail;
	writel(1, p->base + 0x24);
	return 0;
fail:
	gc_spi_abort(p);
	return ret;
}

static int gc_spi_adjust(struct spi_slave *slave, struct spi_mem_op *op)
{
	op->data.nbytes = min(op->data.nbytes, 0x10000U);
	return 0;
}

static int gc_spi_speed(struct udevice *bus, uint hz)
{
	struct gc_spi *p = dev_get_priv(bus);
	unsigned int div;

	if (!hz)
		return -EINVAL;
	for (div = 2; div <= 8; div += 2)
		if (p->input_rate / div <= hz)
			break;
	if (div > 8)
		return -ERANGE;
	clrsetbits_le32(p->base + 0x10, 3, div / 2 - 1);
	return 0;
}

static int gc_spi_mode(struct udevice *bus, uint mode)
{
	struct gc_spi *p = dev_get_priv(bus);

	if (mode)
		return -ENOTSUPP;
	clrbits_le32(p->base + 0x10, BIT(4));
	return 0;
}

static int gc_spi_probe(struct udevice *bus)
{
	struct gc_spi *p = dev_get_priv(bus);
	struct clk clk;
	int ret;

	p->base = dev_read_addr_ptr(bus);
	if (!p->base)
		return -EINVAL;
	ret = clk_get_by_index(bus, 0, &clk);
	if (ret)
		return ret;
	p->input_rate = clk_get_rate(&clk);
	if (IS_ERR_VALUE(p->input_rate) || !p->input_rate)
		return -EINVAL;
	/* Polled PIO: no IRQ or DMA completion inherited from stock. */
	clrsetbits_le32(p->base + 0x20, 3 | (3 << 12), BIT(1));
	return gc_spi_abort(p);
}

static const struct spi_controller_mem_ops gc_spi_mem_ops = {
	.supports_op = gc_spi_supports, .exec_op = gc_spi_exec,
	.adjust_op_size = gc_spi_adjust,
};
static const struct dm_spi_ops gc_spi_ops = {
	.set_speed = gc_spi_speed, .set_mode = gc_spi_mode, .mem_ops = &gc_spi_mem_ops,
};
static const struct udevice_id gc_spi_ids[] = {
	{ .compatible = "faraday,ftspi020-gc1601" }, {}
};
U_BOOT_DRIVER(gc1601_spi) = {
	.name = "gc1601_spi", .id = UCLASS_SPI, .of_match = gc_spi_ids,
	.probe = gc_spi_probe, .ops = &gc_spi_ops, .priv_auto = sizeof(struct gc_spi),
};
