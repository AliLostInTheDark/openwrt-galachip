// SPDX-License-Identifier: GPL-2.0-only
/*
 * Pin controller driver for the GalaChip GC1601 system control unit (SCU)
 *
 * Copyright (C) 2026 Ifran Ali <khanu.i.i.k.2000@gmail.com>
 */

#include <dm.h>
#include <dm/pinctrl.h>
#include <asm/io.h>
#include <linux/err.h>
#include <vsprintf.h>

#define GC1601_SCU_PINMUX		0x81c
#define GC1601_SCU_PINMUX_MASK(pin)	(3U << (2 * (pin)))
#define GC1601_SCU_PINMUX_VAL(pin, fn)	((u32)(fn) << (2 * (pin)))
#define GC1601_SCU_PINMUX_GPIO		2U
#define GC1601_NUM_PINS			16

struct gc1601_pinctrl_priv {
	void __iomem *base;
};

static int gc1601_pinctrl_set_state(struct udevice *dev, struct udevice *config)
{
	struct gc1601_pinctrl_priv *priv = dev_get_priv(dev);
	const char *func, *pin;
	int count, i;
	u32 mask = 0, val = 0;

	func = dev_read_string(config, "function");
	if (!func)
		return 0;
	if (strcmp(func, "gpio"))
		return -EINVAL;

	count = dev_read_string_count(config, "pins");
	if (count <= 0)
		return count;

	for (i = 0; i < count; i++) {
		char *end;
		ulong num;

		if (dev_read_string_index(config, "pins", i, &pin))
			return -EINVAL;
		if (strncmp(pin, "gpio", 4) || !pin[4])
			return -EINVAL;
		num = simple_strtoul(pin + 4, &end, 10);
		if (*end || num >= GC1601_NUM_PINS)
			return -EINVAL;

		mask |= GC1601_SCU_PINMUX_MASK(num);
		val |= GC1601_SCU_PINMUX_VAL(num, GC1601_SCU_PINMUX_GPIO);
	}

	clrsetbits_le32(priv->base + GC1601_SCU_PINMUX, mask, val);

	return 0;
}

static int gc1601_pinctrl_probe(struct udevice *dev)
{
	struct gc1601_pinctrl_priv *priv = dev_get_priv(dev);
	struct ofnode_phandle_args args;
	fdt_addr_t addr;
	int ret;

	ret = dev_read_phandle_with_args(dev, "galachip,scu", NULL, 0, 0, &args);
	if (ret)
		return ret;

	addr = ofnode_get_addr(args.node);
	if (addr == FDT_ADDR_T_NONE)
		return -EINVAL;

	priv->base = (void __iomem *)addr;

	return 0;
}

static const struct pinctrl_ops gc1601_pinctrl_ops = {
	.set_state = gc1601_pinctrl_set_state,
};

static const struct udevice_id gc1601_pinctrl_ids[] = {
	{ .compatible = "galachip,gc1601-pinctrl" },
	{ }
};

U_BOOT_DRIVER(gc1601_pinctrl) = {
	.name = "gc1601_pinctrl",
	.id = UCLASS_PINCTRL,
	.of_match = gc1601_pinctrl_ids,
	.probe = gc1601_pinctrl_probe,
	.ops = &gc1601_pinctrl_ops,
	.priv_auto = sizeof(struct gc1601_pinctrl_priv),
};
