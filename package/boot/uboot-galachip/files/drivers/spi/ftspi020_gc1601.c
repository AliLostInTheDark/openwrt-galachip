// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * FTSPI020 single-lane SPI-memory controller for GC1601. Program and erase
 * stay inside the firmware partition; boot and factory regions are refused.
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

/* PIO burst per FIFO-ready wait, as CR2 selects and the Linux driver uses. */
#define XFR_SIZE 256U
#define WRITABLE_START 0x20000U
#define WRITABLE_END 0x3d0000U

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

static bool gc_spi_read_op(u8 opcode)
{
	return opcode == 0x03 || opcode == 0x0b || opcode == 0x05 ||
	       opcode == 0x9f || opcode == 0x35 || opcode == 0x15 ||
	       opcode == 0x5a;
}

/* Bytes a program or erase touches, or 0 for any other opcode. */
static u32 gc_spi_write_span(const struct spi_mem_op *op)
{
	switch (op->cmd.opcode) {
	case 0x02:
		return op->data.nbytes;
	case 0x20:
		return 0x1000;
	case 0xd8:
		return 0x10000;
	}
	return 0;
}

static int gc_spi_check(const struct spi_mem_op *op)
{
	u32 span = gc_spi_write_span(op);
	bool out = op->data.dir == SPI_MEM_DATA_OUT;

	if (op->cmd.opcode == 0x06 || op->cmd.opcode == 0x04)
		return op->addr.nbytes || op->data.nbytes ? -EINVAL : 0;
	if (span) {
		if (op->addr.nbytes != 3 || (op->cmd.opcode == 0x02) != out ||
		    (op->cmd.opcode != 0x02 && op->data.nbytes))
			return -EINVAL;
		if (op->addr.val < WRITABLE_START || op->addr.val >= WRITABLE_END ||
		    span > WRITABLE_END - op->addr.val)
			return -EROFS;
		return 0;
	}
	if (out || !gc_spi_read_op(op->cmd.opcode))
		return -EROFS;
	if (!op->data.nbytes || (op->addr.nbytes &&
	    (op->addr.val > 0xffffff ||
	     op->data.nbytes > 0x1000000 - op->addr.val)))
		return -EINVAL;
	return 0;
}

static int gc_spi_exec(struct spi_slave *slave, const struct spi_mem_op *op)
{
	struct gc_spi *p = dev_get_priv(dev_get_parent(slave->dev));
	u32 word, val, left = op->data.nbytes, n, chunk;
	bool out = op->data.dir == SPI_MEM_DATA_OUT;
	const u8 *src = op->data.buf.out;
	u8 *dst = op->data.buf.in;
	int ret;

	if (!gc_spi_supports(slave, op) || spi_chip_select(slave->dev) != 0)
		return -ENOTSUPP;
	if (left > 0x10000)
		return -EINVAL;
	ret = gc_spi_check(op);
	if (ret)
		return ret;
	if (p->failed && gc_spi_abort(p))
		return -EIO;
	writel(1, p->base + 0x24);
	writel(op->addr.val, p->base);
	writel(BIT(24) | op->addr.nbytes | (op->dummy.nbytes * 8 << 16), p->base + 4);
	writel(left, p->base + 8);
	/* CMD3: opcode, direction (bits 3:1, 1 = out or no data), completion. */
	writel((u32)op->cmd.opcode << 24 | (left && !out ? 0 : BIT(1)) | BIT(0),
	       p->base + 0x0c);
	while (left) {
		ret = readl_poll_timeout(p->base + 0x18, val,
					 val & (out ? BIT(0) : BIT(1)), 100000);
		if (ret)
			goto fail;
		chunk = min(left, XFR_SIZE);
		left -= chunk;
		/* The data port is word-wide; a partial last word carries the tail. */
		while (chunk) {
			n = min(chunk, 4U);
			if (out) {
				word = 0;
				memcpy(&word, src, n);
				writel(word, p->base + 0x100);
				src += n;
			} else {
				word = readl(p->base + 0x100);
				memcpy(dst, &word, n);
				dst += n;
			}
			chunk -= n;
		}
	}
	ret = readl_poll_timeout(p->base + 0x24, val, val & BIT(0), 100000);
	if (ret)
		goto fail;
	writel(1, p->base + 0x24);
	return 0;
fail:
	/* Never retry a program or erase here. */
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
	/* Polled PIO with 256-byte RX/TX bursts; no IRQ or DMA from stock. */
	clrsetbits_le32(p->base + 0x20, 3 | (3 << 8) | (3 << 12),
			BIT(1) | (3 << 8) | (3 << 12));
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
