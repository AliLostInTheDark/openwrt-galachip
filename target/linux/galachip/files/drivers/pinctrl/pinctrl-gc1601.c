// SPDX-License-Identifier: GPL-2.0-only
/*
 * Pin controller driver for the GalaChip GC1601 system control unit (SCU)
 *
 * Copyright (C) 2026 Ifran Ali <khanu.i.i.k.2000@gmail.com>
 */

#include <linux/array_size.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/mfd/syscon.h>
#include <linux/mod_devicetable.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

#include <linux/pinctrl/pinconf-generic.h>
#include <linux/pinctrl/pinctrl.h>
#include <linux/pinctrl/pinmux.h>

#include "pinctrl-utils.h"

#define GC1601_SCU_PINMUX		0x81c
#define GC1601_SCU_PINMUX_MASK(pin)	(3U << (2 * (pin)))
#define GC1601_SCU_PINMUX_VAL(pin, fn)	((u32)(fn) << (2 * (pin)))
#define GC1601_SCU_PINMUX_GPIO		2U

static const struct pinctrl_pin_desc gc1601_pins[] = {
	PINCTRL_PIN(0, "gpio0"),
	PINCTRL_PIN(1, "gpio1"),
	PINCTRL_PIN(2, "gpio2"),
	PINCTRL_PIN(3, "gpio3"),
	PINCTRL_PIN(4, "gpio4"),
	PINCTRL_PIN(5, "gpio5"),
	PINCTRL_PIN(6, "gpio6"),
	PINCTRL_PIN(7, "gpio7"),
	PINCTRL_PIN(8, "gpio8"),
	PINCTRL_PIN(9, "gpio9"),
	PINCTRL_PIN(10, "gpio10"),
	PINCTRL_PIN(11, "gpio11"),
	PINCTRL_PIN(12, "gpio12"),
	PINCTRL_PIN(13, "gpio13"),
	PINCTRL_PIN(14, "gpio14"),
	PINCTRL_PIN(15, "gpio15"),
};

static const char *const gc1601_pin_names[] = {
	"gpio0", "gpio1", "gpio2", "gpio3",
	"gpio4", "gpio5", "gpio6", "gpio7",
	"gpio8", "gpio9", "gpio10", "gpio11",
	"gpio12", "gpio13", "gpio14", "gpio15",
};

static int gc1601_pinctrl_get_groups_count(struct pinctrl_dev *pctldev)
{
	return ARRAY_SIZE(gc1601_pins);
}

static const char *gc1601_pinctrl_get_group_name(struct pinctrl_dev *pctldev,
						 unsigned int selector)
{
	return gc1601_pins[selector].name;
}

static int gc1601_pinctrl_get_group_pins(struct pinctrl_dev *pctldev,
					 unsigned int selector,
					 const unsigned int **pins,
					 unsigned int *num_pins)
{
	*pins = &gc1601_pins[selector].number;
	*num_pins = 1;

	return 0;
}

static const struct pinctrl_ops gc1601_pinctrl_ops = {
	.get_groups_count = gc1601_pinctrl_get_groups_count,
	.get_group_name = gc1601_pinctrl_get_group_name,
	.get_group_pins = gc1601_pinctrl_get_group_pins,
	.dt_node_to_map = pinconf_generic_dt_node_to_map_pin,
	.dt_free_map = pinctrl_utils_free_map,
};

static const char *const gc1601_functions[] = {
	"gpio",
};

static int gc1601_pinmux_get_functions_count(struct pinctrl_dev *pctldev)
{
	return ARRAY_SIZE(gc1601_functions);
}

static const char *gc1601_pinmux_get_function_name(struct pinctrl_dev *pctldev,
						   unsigned int selector)
{
	return gc1601_functions[selector];
}

static int gc1601_pinmux_get_groups(struct pinctrl_dev *pctldev,
				    unsigned int selector,
				    const char *const **groups,
				    unsigned int *const num_groups)
{
	*groups = gc1601_pin_names;
	*num_groups = ARRAY_SIZE(gc1601_pin_names);

	return 0;
}

static int gc1601_pinmux_set_mux(struct pinctrl_dev *pctldev,
				 unsigned int func_selector,
				 unsigned int group_selector)
{
	struct regmap *scu = pinctrl_dev_get_drvdata(pctldev);

	if (func_selector != 0 || group_selector >= ARRAY_SIZE(gc1601_pins))
		return -EINVAL;

	return regmap_update_bits(scu, GC1601_SCU_PINMUX,
				  GC1601_SCU_PINMUX_MASK(group_selector),
				  GC1601_SCU_PINMUX_VAL(group_selector,
							GC1601_SCU_PINMUX_GPIO));
}

static const struct pinmux_ops gc1601_pinmux_ops = {
	.get_functions_count = gc1601_pinmux_get_functions_count,
	.get_function_name = gc1601_pinmux_get_function_name,
	.get_function_groups = gc1601_pinmux_get_groups,
	.set_mux = gc1601_pinmux_set_mux,
};

static struct pinctrl_desc gc1601_pinctrl_desc = {
	.name = "pinctrl-gc1601",
	.pins = gc1601_pins,
	.npins = ARRAY_SIZE(gc1601_pins),
	.pctlops = &gc1601_pinctrl_ops,
	.pmxops = &gc1601_pinmux_ops,
	.owner = THIS_MODULE,
};

static int gc1601_pinctrl_probe(struct platform_device *pdev)
{
	struct pinctrl_dev *pctldev;
	struct regmap *scu;

	scu = syscon_regmap_lookup_by_phandle(pdev->dev.of_node, "galachip,scu");
	if (IS_ERR(scu))
		return dev_err_probe(&pdev->dev, PTR_ERR(scu),
				     "failed to get SCU regmap\n");

	pctldev = devm_pinctrl_register(&pdev->dev, &gc1601_pinctrl_desc, scu);
	if (IS_ERR(pctldev))
		return dev_err_probe(&pdev->dev, PTR_ERR(pctldev),
				     "failed to register pinctrl\n");

	return 0;
}

static const struct of_device_id gc1601_pinctrl_of_match[] = {
	{ .compatible = "galachip,gc1601-pinctrl" },
	{ }
};

static struct platform_driver gc1601_pinctrl_driver = {
	.driver = {
		.name = "pinctrl-gc1601",
		.of_match_table = gc1601_pinctrl_of_match,
	},
	.probe = gc1601_pinctrl_probe,
};
builtin_platform_driver(gc1601_pinctrl_driver);
