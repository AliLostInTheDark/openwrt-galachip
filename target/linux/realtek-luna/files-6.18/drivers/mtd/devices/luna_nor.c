// SPDX-License-Identifier: GPL-2.0-only
/*
 * SPI-NOR flash behind the Luna (RTL960x) flash controller.
 *
 * Reads go through the controller's memory-mapped window, as map_rom did.
 * Erase and program go through its PIO registers, one command per chip-select
 * cycle: a 3-byte address, single I/O, standard JEDEC opcodes. Register
 * layout and the chip-select handshake as the vendor bootloader drives them.
 */
#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/mtd/mtd.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#define SFCSR			0x08
#define SFCSR_CSB0		BIT(31)		/* chip select 0, active low */
#define SFCSR_CSB1		BIT(30)
#define SFCSR_LEN		GENMASK(29, 28)	/* bytes per SFDR access - 1 */
#define SFCSR_RDY		BIT(27)
#define SFCSR_IO_WIDTH		GENMASK(26, 25)	/* 0: single I/O */
#define SFCSR_CSB0_STS		BIT(11)
#define SFCSR_CSB1_STS		BIT(10)
#define SFCSR_IDLE		BIT(4)
#define SFDR			0x0c		/* data, MSB first */

#define OP_WREN			0x06
#define OP_RDSR			0x05
#define OP_PP			0x02
#define OP_BE_64K		0xd8
#define OP_SE_4K		0x20
#define SR_WIP			BIT(0)
#define SR_BP			GENMASK(4, 2)

#define NOR_PAGE		256
#define NOR_BLOCK		SZ_64K

struct luna_nor {
	struct mtd_info mtd;
	void __iomem *window;
	void __iomem *regs;
	struct mutex lock;
	u8 erase_op;
};

/* readl/writel are native-endian on Luna (luna_wdt relies on it too); ioread32be swaps. */
static u32 sfcsr_read(struct luna_nor *nor)
{
	return readl(nor->regs + SFCSR);
}

static void sfcsr_write(struct luna_nor *nor, u32 v)
{
	writel(v, nor->regs + SFCSR);
}

static int wait_bits(struct luna_nor *nor, u32 bits)
{
	u32 v;

	return readx_poll_timeout_atomic(sfcsr_read, nor, v, (v & bits) == bits, 0, 20000);
}

/* End any cycle, including a memory-mapped read still holding CS low. */
static int cs_release(struct luna_nor *nor)
{
	u32 v = sfcsr_read(nor);

	sfcsr_write(nor, v & ~(SFCSR_CSB0 | SFCSR_CSB1));
	ndelay(200);
	sfcsr_write(nor, v | SFCSR_CSB0 | SFCSR_CSB1);
	ndelay(200);
	return wait_bits(nor, SFCSR_CSB0_STS | SFCSR_CSB1_STS | SFCSR_RDY | SFCSR_IDLE);
}

static int put(struct luna_nor *nor, u32 word)
{
	writel(word, nor->regs + SFDR);
	return wait_bits(nor, SFCSR_RDY | SFCSR_IDLE);
}

/* One command: opcode, optional 3-byte address, then len bytes out or in. */
static int nor_cmd(struct luna_nor *nor, u8 op, s64 addr, const u8 *out, u8 *in, size_t len)
{
	u32 sel, v;
	size_t i;
	int ret;

	ret = wait_bits(nor, SFCSR_RDY | SFCSR_IDLE);
	if (!ret)
		ret = cs_release(nor);
	if (ret)
		return ret;

	sel = sfcsr_read(nor) & ~(SFCSR_CSB0 | SFCSR_LEN | SFCSR_IO_WIDTH);
	sfcsr_write(nor, sel);
	ret = readx_poll_timeout_atomic(sfcsr_read, nor, v,
					(v & (SFCSR_CSB0_STS | SFCSR_CSB1_STS)) !=
					(SFCSR_CSB0_STS | SFCSR_CSB1_STS), 0, 20000);
	if (!ret)
		ret = put(nor, (u32)op << 24);
	if (!ret && addr >= 0) {
		sfcsr_write(nor, sel | FIELD_PREP(SFCSR_LEN, 2));
		ret = put(nor, (u32)addr << 8);
		sfcsr_write(nor, sel);
	}
	for (i = 0; !ret && i < len; i++) {
		if (out) {
			ret = put(nor, (u32)out[i] << 24);
		} else {
			in[i] = readl(nor->regs + SFDR) >> 24;
			ret = wait_bits(nor, SFCSR_RDY | SFCSR_IDLE);
		}
	}
	if (!ret)
		ret = cs_release(nor);
	else
		cs_release(nor);
	return ret;
}

static int read_sr(struct luna_nor *nor, u8 *sr)
{
	return nor_cmd(nor, OP_RDSR, -1, NULL, sr, 1);
}

static int wait_idle(struct luna_nor *nor, unsigned int timeout_ms)
{
	unsigned long end = jiffies + msecs_to_jiffies(timeout_ms);
	u8 sr;
	int ret;

	for (;;) {
		ret = read_sr(nor, &sr);
		if (ret || !(sr & SR_WIP))
			return ret;
		if (time_after(jiffies, end))
			return -ETIMEDOUT;
		usleep_range(50, 200);
	}
}

/* The same wait for a panic: no jiffies, no sleep. */
static int wait_idle_atomic(struct luna_nor *nor, unsigned int timeout_us)
{
	u8 sr;
	int ret;

	for (;;) {
		ret = read_sr(nor, &sr);
		if (ret || !(sr & SR_WIP))
			return ret;
		if (!timeout_us--)
			return -ETIMEDOUT;
		udelay(1);
	}
}

static int luna_nor_read(struct mtd_info *mtd, loff_t from, size_t len,
			 size_t *retlen, u_char *buf)
{
	struct luna_nor *nor = mtd->priv;

	mutex_lock(&nor->lock);
	memcpy_fromio(buf, nor->window + from, len);
	mutex_unlock(&nor->lock);
	*retlen = len;
	return 0;
}

static int program(struct luna_nor *nor, loff_t to, size_t len, size_t *retlen,
		   const u_char *buf, bool in_panic)
{
	int ret = 0;

	while (len && !ret) {
		size_t chunk = min_t(size_t, len, NOR_PAGE - (to & (NOR_PAGE - 1)));

		ret = nor_cmd(nor, OP_WREN, -1, NULL, NULL, 0);
		if (!ret)
			ret = nor_cmd(nor, OP_PP, to, buf, NULL, chunk);
		if (!ret)
			ret = in_panic ? wait_idle_atomic(nor, 20000) : wait_idle(nor, 20);
		if (!ret) {
			to += chunk;
			buf += chunk;
			len -= chunk;
			*retlen += chunk;
		}
	}
	return ret;
}

static int luna_nor_write(struct mtd_info *mtd, loff_t to, size_t len,
			  size_t *retlen, const u_char *buf)
{
	struct luna_nor *nor = mtd->priv;
	int ret;

	mutex_lock(&nor->lock);
	ret = program(nor, to, len, retlen, buf, false);
	mutex_unlock(&nor->lock);
	if (ret)
		dev_err(&mtd->dev, "program at 0x%llx failed: %d\n", (u64)to, ret);
	return ret;
}

/*
 * A panic owns the machine: no lock, only already-erased pages are programmed. An erase or a
 * program it interrupted must end first, or the chip ignores the write and the record is lost.
 */
static int luna_nor_panic_write(struct mtd_info *mtd, loff_t to, size_t len,
				size_t *retlen, const u_char *buf)
{
	int ret = wait_idle_atomic(mtd->priv, 3000000);

	return ret ? ret : program(mtd->priv, to, len, retlen, buf, true);
}

static int luna_nor_erase(struct mtd_info *mtd, struct erase_info *instr)
{
	struct luna_nor *nor = mtd->priv;
	loff_t addr = instr->addr;
	loff_t end = instr->addr + instr->len;
	int ret = 0;

	mutex_lock(&nor->lock);
	for (; addr < end && !ret; addr += mtd->erasesize) {
		ret = nor_cmd(nor, OP_WREN, -1, NULL, NULL, 0);
		if (!ret)
			ret = nor_cmd(nor, nor->erase_op, addr, NULL, NULL, 0);
		if (!ret)
			ret = wait_idle(nor, 3000);
	}
	mutex_unlock(&nor->lock);
	if (ret) {
		instr->fail_addr = addr - mtd->erasesize;
		dev_err(&mtd->dev, "erase at 0x%llx failed: %d\n", (u64)instr->fail_addr, ret);
	}
	return ret;
}

static int luna_nor_erase_rom(struct mtd_info *mtd, struct erase_info *instr)
{
	return -EROFS;
}

static int luna_nor_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct resource *res;
	struct luna_nor *nor;
	u8 sr;
	int ret;

	nor = devm_kzalloc(&pdev->dev, sizeof(*nor), GFP_KERNEL);
	if (!nor)
		return -ENOMEM;
	nor->window = devm_platform_get_and_ioremap_resource(pdev, 0, &res);
	if (IS_ERR(nor->window))
		return PTR_ERR(nor->window);
	nor->regs = devm_platform_ioremap_resource(pdev, 1);
	if (IS_ERR(nor->regs))
		return PTR_ERR(nor->regs);
	mutex_init(&nor->lock);

	/* The rootfs lives on this flash: whatever the controller says, register it. */
	ret = read_sr(nor, &sr);
	if (ret)
		dev_warn(&pdev->dev, "status register unreadable (%d): read-only\n", ret);
	else if (sr & SR_BP)
		dev_warn(&pdev->dev, "block protection set (SR 0x%02x): read-only\n", sr);

	nor->mtd.name = dev_name(&pdev->dev);
	nor->mtd.size = resource_size(res);
	/* A board whose partitions sit on 4 KiB boundaries (G24W: config ends at
	 * 0x84000) declares erase-size: a 64 KiB erase there would reach into the
	 * partition before. */
	if (of_property_read_u32(np, "erase-size", &nor->mtd.erasesize) || nor->mtd.erasesize != SZ_4K)
		nor->mtd.erasesize = NOR_BLOCK;
	nor->erase_op = nor->mtd.erasesize == SZ_4K ? OP_SE_4K : OP_BE_64K;
	nor->mtd.writesize = 1;
	nor->mtd.writebufsize = NOR_PAGE;
	nor->mtd.owner = THIS_MODULE;
	nor->mtd.priv = nor;
	nor->mtd.dev.parent = &pdev->dev;
	nor->mtd._read = luna_nor_read;
	if (IS_ENABLED(CONFIG_MTD_LUNA_NOR_WRITE) && !ret && !(sr & SR_BP)) {
		nor->mtd.type = MTD_NORFLASH;
		nor->mtd.flags = MTD_CAP_NORFLASH;
		nor->mtd._write = luna_nor_write;
		nor->mtd._panic_write = luna_nor_panic_write;
		nor->mtd._erase = luna_nor_erase;
	} else {
		nor->mtd.type = MTD_ROM;
		nor->mtd.flags = MTD_CAP_ROM;
		nor->mtd._erase = luna_nor_erase_rom;	/* the MTD core wants one */
	}
	mtd_set_of_node(&nor->mtd, np);
	platform_set_drvdata(pdev, nor);
	dev_info(&pdev->dev, "%llu MiB SPI-NOR, %s\n", (u64)nor->mtd.size >> 20,
		 nor->mtd._write ? "read-write" : "read-only");
	return mtd_device_register(&nor->mtd, NULL, 0);
}

static void luna_nor_remove(struct platform_device *pdev)
{
	struct luna_nor *nor = platform_get_drvdata(pdev);

	WARN_ON(mtd_device_unregister(&nor->mtd));
}

/* A reboot must not cut an erase or a program in half. */
static void luna_nor_shutdown(struct platform_device *pdev)
{
	struct luna_nor *nor = platform_get_drvdata(pdev);

	mutex_lock(&nor->lock);
}

static const struct of_device_id luna_nor_of_match[] = {
	{ .compatible = "realtek,luna-spi-nor" },
	{ }
};
MODULE_DEVICE_TABLE(of, luna_nor_of_match);

static struct platform_driver luna_nor_driver = {
	.probe = luna_nor_probe,
	.remove = luna_nor_remove,
	.shutdown = luna_nor_shutdown,
	.driver = {
		.name = "luna-spi-nor",
		.of_match_table = luna_nor_of_match,
	},
};
module_platform_driver(luna_nor_driver);

MODULE_DESCRIPTION("Luna (RTL960x) SPI-NOR flash: memory-mapped read, PIO program and erase");
MODULE_LICENSE("GPL");
