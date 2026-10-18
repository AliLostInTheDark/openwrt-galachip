// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Faraday FTIIC010 I2C controller, polled master mode.
 *
 * Based on the Faraday BSP driver by Po-Yu Chuang. Bytes are moved one at a
 * time through the data register; the controller raises TD when a byte has
 * been shifted and NACK when the target did not acknowledge it.
 */

#include <linux/clk.h>
#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#define FTIIC010_CR		0x00
#define FTIIC010_SR		0x04
#define FTIIC010_CDR		0x08
#define FTIIC010_DR		0x0c
#define FTIIC010_TGSR		0x14

#define FTIIC010_CR_RST		BIT(0)
#define FTIIC010_CR_EN		BIT(1)
#define FTIIC010_CR_MST_EN	BIT(2)
#define FTIIC010_CR_START	BIT(4)
#define FTIIC010_CR_STOP	BIT(5)
#define FTIIC010_CR_NACK	BIT(6)
#define FTIIC010_CR_TB_EN	BIT(7)
#define FTIIC010_CR_ALI_EN	BIT(13)

#define FTIIC010_SR_TD		BIT(5)
#define FTIIC010_SR_NACK	BIT(6)
#define FTIIC010_SR_AL		BIT(10)

#define FTIIC010_CDR_COUNT	GENMASK(19, 0)
/* COUNTH and duty fields as programmed by the vendor driver. */
#define FTIIC010_CDR_FIXED	(0xa10 << 20)

#define FTIIC010_TSR		1
#define FTIIC010_GSR		2
#define FTIIC010_TGSR_VAL	(FTIIC010_TSR | (FTIIC010_GSR << 10))

#define FTIIC010_BYTE_TIMEOUT_US	2000

struct ftiic010 {
	struct i2c_adapter adapter;
	void __iomem *base;
	unsigned long pclk;
	u32 bus_hz;
};

static int ftiic010_reset(struct ftiic010 *i2c)
{
	u32 cr;
	int ret;

	writel(FTIIC010_CR_RST, i2c->base + FTIIC010_CR);
	ret = readl_poll_timeout_atomic(i2c->base + FTIIC010_CR, cr,
					!(cr & FTIIC010_CR_RST), 1, 1000);
	writel(FTIIC010_TGSR_VAL, i2c->base + FTIIC010_TGSR);
	writel(FTIIC010_CDR_FIXED |
	       (((i2c->pclk / i2c->bus_hz - FTIIC010_GSR) / 2 - 2) & FTIIC010_CDR_COUNT),
	       i2c->base + FTIIC010_CDR);
	return ret;
}

/* Shift one byte with the given control flags and wait for it to complete. */
static int ftiic010_xfer_byte(struct ftiic010 *i2c, u32 flags, bool check_ack)
{
	u32 sr;
	int ret;

	writel(FTIIC010_CR_EN | FTIIC010_CR_MST_EN | FTIIC010_CR_TB_EN |
	       FTIIC010_CR_ALI_EN | flags, i2c->base + FTIIC010_CR);

	ret = readl_poll_timeout_atomic(i2c->base + FTIIC010_SR, sr,
					sr & (FTIIC010_SR_TD | FTIIC010_SR_NACK |
					      FTIIC010_SR_AL),
					1, FTIIC010_BYTE_TIMEOUT_US);
	writel(sr, i2c->base + FTIIC010_SR);
	if (ret)
		return ret;
	if (sr & FTIIC010_SR_AL)
		return -EAGAIN;
	if (check_ack && (sr & FTIIC010_SR_NACK))
		return -ENXIO;
	return 0;
}

static int ftiic010_write_byte(struct ftiic010 *i2c, u8 data, u32 flags)
{
	writel(data, i2c->base + FTIIC010_DR);
	return ftiic010_xfer_byte(i2c, flags, true);
}

static int ftiic010_do_msg(struct ftiic010 *i2c, struct i2c_msg *msg, bool last)
{
	bool rd = msg->flags & I2C_M_RD;
	u32 stop = last ? FTIIC010_CR_STOP : 0;
	int ret;
	u16 i;

	ret = ftiic010_write_byte(i2c, i2c_8bit_addr_from_msg(msg),
				  FTIIC010_CR_START | (msg->len ? 0 : stop));
	if (ret)
		return ret;

	for (i = 0; i < msg->len; i++) {
		bool final = i + 1 == msg->len;

		if (rd) {
			ret = ftiic010_xfer_byte(i2c, final ? FTIIC010_CR_NACK | stop : 0,
						 false);
			if (ret)
				return ret;
			msg->buf[i] = readl(i2c->base + FTIIC010_DR) & 0xff;
		} else {
			ret = ftiic010_write_byte(i2c, msg->buf[i], final ? stop : 0);
			if (ret)
				return ret;
		}
	}
	return 0;
}

static int ftiic010_master_xfer(struct i2c_adapter *adap, struct i2c_msg *msgs,
				int num)
{
	struct ftiic010 *i2c = i2c_get_adapdata(adap);
	int i, ret;

	for (i = 0; i < num; i++) {
		ret = ftiic010_do_msg(i2c, &msgs[i], i + 1 == num);
		if (ret) {
			/* Release the bus; a reset also clears a stuck transfer. */
			ftiic010_reset(i2c);
			return ret;
		}
	}
	return num;
}

static u32 ftiic010_functionality(struct i2c_adapter *adap)
{
	return I2C_FUNC_I2C | I2C_FUNC_SMBUS_EMUL;
}

static const struct i2c_algorithm ftiic010_algorithm = {
	.xfer = ftiic010_master_xfer,
	.functionality = ftiic010_functionality,
};

static int ftiic010_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct ftiic010 *i2c;
	struct clk *clk;
	int ret;

	i2c = devm_kzalloc(dev, sizeof(*i2c), GFP_KERNEL);
	if (!i2c)
		return -ENOMEM;

	i2c->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(i2c->base))
		return PTR_ERR(i2c->base);

	clk = devm_clk_get_enabled(dev, NULL);
	if (IS_ERR(clk))
		return dev_err_probe(dev, PTR_ERR(clk), "no clock\n");
	i2c->pclk = clk_get_rate(clk);

	i2c->bus_hz = I2C_MAX_STANDARD_MODE_FREQ;
	of_property_read_u32(dev->of_node, "clock-frequency", &i2c->bus_hz);
	if (!i2c->bus_hz || i2c->pclk < 4 * i2c->bus_hz)
		return dev_err_probe(dev, -EINVAL, "bad bus frequency %u\n",
				     i2c->bus_hz);

	ret = ftiic010_reset(i2c);
	if (ret)
		return dev_err_probe(dev, ret, "controller reset timed out\n");

	i2c->adapter.owner = THIS_MODULE;
	i2c->adapter.algo = &ftiic010_algorithm;
	i2c->adapter.timeout = HZ;
	i2c->adapter.dev.parent = dev;
	i2c->adapter.dev.of_node = dev->of_node;
	strscpy(i2c->adapter.name, dev_name(dev), sizeof(i2c->adapter.name));
	i2c_set_adapdata(&i2c->adapter, i2c);

	return devm_i2c_add_adapter(dev, &i2c->adapter);
}

static const struct of_device_id ftiic010_of_match[] = {
	{ .compatible = "faraday,ftiic010" },
	{ }
};
MODULE_DEVICE_TABLE(of, ftiic010_of_match);

static struct platform_driver ftiic010_driver = {
	.probe = ftiic010_probe,
	.driver = {
		.name = "ftiic010",
		.of_match_table = ftiic010_of_match,
	},
};
module_platform_driver(ftiic010_driver);

MODULE_DESCRIPTION("Faraday FTIIC010 I2C controller");
MODULE_LICENSE("GPL");
