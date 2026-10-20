// SPDX-License-Identifier: GPL-2.0-only
/*
 * Core PON platform driver for ZTE Sanechips ZX279125
 *
 * Copyright (C) 2026 Ifran Ali <khanu.i.i.k.2000@gmail.com>
 */

#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/sysfs.h>
#include <net/xpon.h>

#include "zx279125-pon.h"

static int zx279125_xpon_set_mode(struct xpon_device *xpon, enum xpon_mode mode)
{
	struct zx279125_pon *pon = xpon_device_priv(xpon);

	if (mode != XPON_MODE_GPON)
		return -EOPNOTSUPP;

	return zx279125_gpon_set_state(pon, ZX_ONU_STATE_O1_INITIAL);
}

static const struct xpon_device_ops zx279125_xpon_ops = {
	.set_mode = zx279125_xpon_set_mode,
};

/* Sysfs Optical Telemetry Attributes */
static ssize_t optical_state_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct zx279125_pon *pon = dev_get_drvdata(dev);

	return sysfs_emit(buf, "O%d\n", pon->state);
}
static DEVICE_ATTR_RO(optical_state);

static ssize_t rx_power_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	struct zx279125_pon *pon = dev_get_drvdata(dev);
	u8 msb = 0, lsb = 0;
	u16 raw = 0;

	if (pon->bosa_client) {
		mutex_lock(&pon->bosa_lock);
		msb = i2c_smbus_read_byte_data(pon->bosa_client, ZX_BOSA_REG_RX_POWER);
		lsb = i2c_smbus_read_byte_data(pon->bosa_client, ZX_BOSA_REG_RX_POWER + 1);
		mutex_unlock(&pon->bosa_lock);
		raw = (msb << 8) | lsb;
	}

	return sysfs_emit(buf, "%u uW\n", (unsigned int)raw / 10);
}
static DEVICE_ATTR_RO(rx_power);

static ssize_t tx_power_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	struct zx279125_pon *pon = dev_get_drvdata(dev);
	u8 msb = 0, lsb = 0;
	u16 raw = 0;

	if (pon->bosa_client) {
		mutex_lock(&pon->bosa_lock);
		msb = i2c_smbus_read_byte_data(pon->bosa_client, ZX_BOSA_REG_TX_POWER);
		lsb = i2c_smbus_read_byte_data(pon->bosa_client, ZX_BOSA_REG_TX_POWER + 1);
		mutex_unlock(&pon->bosa_lock);
		raw = (msb << 8) | lsb;
	}

	return sysfs_emit(buf, "%u uW\n", (unsigned int)raw / 5);
}
static DEVICE_ATTR_RO(tx_power);

static struct attribute *zx279125_pon_attrs[] = {
	&dev_attr_optical_state.attr,
	&dev_attr_rx_power.attr,
	&dev_attr_tx_power.attr,
	NULL,
};
ATTRIBUTE_GROUPS(zx279125_pon);

static int zx279125_pon_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct zx279125_pon *pon;
	struct xpon_device_desc desc = {
		.modes = XPON_MODE_CAP(XPON_MODE_GPON),
		.mode = XPON_MODE_GPON,
		.ops = &zx279125_xpon_ops,
	};
	int ret;

	pon = devm_kzalloc(dev, sizeof(*pon), GFP_KERNEL);
	if (!pon)
		return -ENOMEM;

	pon->dev = dev;
	spin_lock_init(&pon->lock);
	mutex_init(&pon->bosa_lock);

	/* Map 6 hardware register blocks */
	pon->base_gmac_int = devm_platform_ioremap_resource_byname(pdev, "intc");
	if (IS_ERR(pon->base_gmac_int))
		return PTR_ERR(pon->base_gmac_int);

	pon->base_gpon = devm_platform_ioremap_resource_byname(pdev, "gpon");
	if (IS_ERR(pon->base_gpon))
		return PTR_ERR(pon->base_gpon);

	pon->base_gem = devm_platform_ioremap_resource_byname(pdev, "gem");
	if (IS_ERR(pon->base_gem))
		return PTR_ERR(pon->base_gem);

	pon->base_onu = devm_platform_ioremap_resource_byname(pdev, "onu");
	if (IS_ERR(pon->base_onu))
		return PTR_ERR(pon->base_onu);

	pon->base_tx = devm_platform_ioremap_resource_byname(pdev, "tx");
	if (IS_ERR(pon->base_tx))
		return PTR_ERR(pon->base_tx);

	pon->base_aes = devm_platform_ioremap_resource_byname(pdev, "aes");
	if (IS_ERR(pon->base_aes))
		return PTR_ERR(pon->base_aes);

	pon->irq = platform_get_irq(pdev, 0);
	if (pon->irq < 0)
		return pon->irq;

	platform_set_drvdata(pdev, pon);

	/* Request shared GMAC / GPON interrupt */
	ret = devm_request_irq(dev, pon->irq, zx279125_pon_isr,
			       IRQF_SHARED, "zx279125-pon", pon);
	if (ret)
		return dev_err_probe(dev, ret, "failed to request PON IRQ\n");

	/* Initialize GPON MAC hardware in State O1 */
	ret = zx279125_gpon_init(pon);
	if (ret)
		return ret;

	/* Register with xPON Subsystem */
	desc.priv = pon;
	pon->xpon = xpon_device_register(dev, &desc);
	if (IS_ERR(pon->xpon)) {
		zx279125_gpon_exit(pon);
		return dev_err_probe(dev, PTR_ERR(pon->xpon), "xPON registration failed\n");
	}

	dev_info(dev, "ZTE Sanechips ZX279125 GPON MAC ready (IRQ %d)\n", pon->irq);
	return 0;
}

static void zx279125_pon_remove(struct platform_device *pdev)
{
	struct zx279125_pon *pon = platform_get_drvdata(pdev);

	if (pon->xpon)
		xpon_device_unregister(pon->xpon);

	zx279125_gpon_exit(pon);
}

static const struct of_device_id zx279125_pon_of_match[] = {
	{ .compatible = "zte,zx279125-pon" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, zx279125_pon_of_match);

static struct platform_driver zx279125_pon_driver = {
	.probe	= zx279125_pon_probe,
	.remove = zx279125_pon_remove,
	.driver	= {
		.name		= "zx279125-pon",
		.of_match_table	= zx279125_pon_of_match,
		.dev_groups	= zx279125_pon_groups,
	},
};
module_platform_driver(zx279125_pon_driver);

MODULE_AUTHOR("Ifran Ali <khanu.i.i.k.2000@gmail.com>");
MODULE_DESCRIPTION("ZTE Sanechips ZX279125 GPON MAC and Optical Controller");
MODULE_LICENSE("GPL");
