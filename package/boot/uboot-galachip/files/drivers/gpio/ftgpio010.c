// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Faraday FTGPIO010 GPIO controller
 *
 * Register layout as in the Linux gpio-ftgpio010 driver. Only direction and
 * level are handled; U-Boot uses it for the board's LEDs and buttons.
 *
 * Copyright (C) 2026 Ifran Ali <khanu.i.i.k.2000@gmail.com>
 */

#include <dm.h>
#include <asm/gpio.h>
#include <asm/io.h>
#include <linux/bitops.h>

#define FTGPIO010_DATA_OUT	0x00
#define FTGPIO010_DATA_IN	0x04
#define FTGPIO010_DIR		0x08	/* 1 = output */
#define FTGPIO010_DATA_SET	0x10
#define FTGPIO010_DATA_CLR	0x14

struct ftgpio010_priv {
	void __iomem *base;
};

static int ftgpio010_direction_input(struct udevice *dev, unsigned int offset)
{
	struct ftgpio010_priv *priv = dev_get_priv(dev);

	clrbits_le32(priv->base + FTGPIO010_DIR, BIT(offset));
	return 0;
}

static int ftgpio010_set_value(struct udevice *dev, unsigned int offset, int value)
{
	struct ftgpio010_priv *priv = dev_get_priv(dev);

	writel(BIT(offset), priv->base + (value ? FTGPIO010_DATA_SET : FTGPIO010_DATA_CLR));
	return 0;
}

static int ftgpio010_direction_output(struct udevice *dev, unsigned int offset, int value)
{
	struct ftgpio010_priv *priv = dev_get_priv(dev);

	ftgpio010_set_value(dev, offset, value);
	setbits_le32(priv->base + FTGPIO010_DIR, BIT(offset));
	return 0;
}

static int ftgpio010_get_value(struct udevice *dev, unsigned int offset)
{
	struct ftgpio010_priv *priv = dev_get_priv(dev);

	if (readl(priv->base + FTGPIO010_DIR) & BIT(offset))
		return !!(readl(priv->base + FTGPIO010_DATA_OUT) & BIT(offset));
	return !!(readl(priv->base + FTGPIO010_DATA_IN) & BIT(offset));
}

static int ftgpio010_get_function(struct udevice *dev, unsigned int offset)
{
	struct ftgpio010_priv *priv = dev_get_priv(dev);

	return readl(priv->base + FTGPIO010_DIR) & BIT(offset) ? GPIOF_OUTPUT : GPIOF_INPUT;
}

static const struct dm_gpio_ops ftgpio010_ops = {
	.direction_input	= ftgpio010_direction_input,
	.direction_output	= ftgpio010_direction_output,
	.get_value		= ftgpio010_get_value,
	.set_value		= ftgpio010_set_value,
	.get_function		= ftgpio010_get_function,
};

static int ftgpio010_probe(struct udevice *dev)
{
	struct gpio_dev_priv *uc_priv = dev_get_uclass_priv(dev);
	struct ftgpio010_priv *priv = dev_get_priv(dev);

	priv->base = dev_read_addr_ptr(dev);
	if (!priv->base)
		return -EINVAL;
	uc_priv->gpio_count = 32;
	uc_priv->bank_name = dev->name;
	return 0;
}

static const struct udevice_id ftgpio010_ids[] = {
	{ .compatible = "faraday,ftgpio010" },
	{ }
};

U_BOOT_DRIVER(ftgpio010) = {
	.name		= "ftgpio010",
	.id		= UCLASS_GPIO,
	.of_match	= ftgpio010_ids,
	.ops		= &ftgpio010_ops,
	.probe		= ftgpio010_probe,
	.priv_auto	= sizeof(struct ftgpio010_priv),
};
