// SPDX-License-Identifier: GPL-2.0-only
/*
 * Core PON platform driver for ZTE Sanechips ZX279125
 *
 * Copyright (C) 2026 Ifran Ali <khanu.i.i.k.2000@gmail.com>
 */

#include <linux/etherdevice.h>
#include <linux/ethtool.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/skbuff.h>
#include <linux/slab.h>
#include <linux/sysfs.h>
#include <net/xpon.h>

#include "zx279125-pon.h"

static int zx279125_xpon_set_mode(struct xpon_device *xpon, enum xpon_mode mode)
{
	struct zx279125_pon *pon = xpon_device_priv(xpon);

	if (mode != XPON_MODE_GPON)
		return -EOPNOTSUPP;

	return zx279125_gpon_start(pon);
}

static const struct xpon_device_ops zx279125_xpon_ops = {
	.set_mode = zx279125_xpon_set_mode,
};

/* ---- OMCI Device Operations ---- */

static int zx279125_omci_backend_begin(struct omci_device *odev)
{
	struct zx279125_pon *pon = omci_device_priv(odev);

	if (!mutex_trylock(&pon->config_lock))
		return -EBUSY;
	return 0;
}

static void zx279125_omci_backend_end(struct omci_device *odev)
{
	struct zx279125_pon *pon = omci_device_priv(odev);

	mutex_unlock(&pon->config_lock);
}

static int zx279125_omci_backend_identity(struct omci_device *odev,
					  const struct omci_identity *identity)
{
	struct zx279125_pon *pon = omci_device_priv(odev);

	if (READ_ONCE(pon->running))
		return -EBUSY;

	if (identity->valid & OMCI_IDENTITY_F_SERIAL_NUMBER)
		memcpy(pon->serial_number, identity->serial_number, sizeof(pon->serial_number));
	if (identity->valid & OMCI_IDENTITY_F_PASSWORD)
		memcpy(pon->password, identity->password, sizeof(pon->password));

	/* Program hardware GPON Serial Number register (0x920d8040) */
	if (pon->base_tx && (identity->valid & OMCI_IDENTITY_F_SERIAL_NUMBER)) {
		u32 sn_hi = get_unaligned_be32(pon->serial_number);
		u32 sn_lo = get_unaligned_be32(pon->serial_number + 4);

		writel(sn_hi, pon->base_tx + ZX_PON_SERIAL_NUM);
		writel(sn_lo, pon->base_tx + ZX_PON_SERIAL_NUM + 4);
	}

	return 0;
}

static int zx279125_omci_backend_reset(struct omci_device *odev)
{
	struct zx279125_pon *pon = omci_device_priv(odev);
	unsigned int i;

	/* OMCI reset must preserve PLOAM assignment and the active OMCC. */
	for (i = 0; i < ARRAY_SIZE(pon->gems); i++) {
		if (pon->gems[i].valid && pon->gems[i].gem != pon->omci_port)
			writel(0, pon->base_ports + pon->gems[i].gem * 4);
		pon->gems[i].valid = false;
	}
	pon->tcont_valid = 0;
	return 0;
}

static int zx279125_omci_start(struct omci_device *odev)
{
	struct zx279125_pon *pon = omci_device_priv(odev);

	return zx279125_gpon_start(pon);
}

static void zx279125_omci_stop(struct omci_device *odev)
{
	struct zx279125_pon *pon = omci_device_priv(odev);

	zx279125_gpon_exit(pon);
}

static int zx279125_omci_xmit(struct omci_device *odev, struct sk_buff *skb,
			      u16 gem_port_id)
{
	struct zx279125_pon *pon = omci_device_priv(odev);
	int ret;

	if (skb->len != 48)
		ret = -EMSGSIZE;
	else if (pon->state != ZX_ONU_STATE_O5_OPERATION || pon->tx_hold ||
		 gem_port_id != pon->omci_port)
		ret = -ENOLINK;
	else
		ret = zx279125_pp_send_control(skb->data, skb->len, gem_port_id);
	dev_kfree_skb_any(skb);
	return ret;
}

static void zx279125_omci_receive(void *priv, struct sk_buff *skb, u16 gem)
{
	struct zx279125_pon *pon = priv;

	if (!pon->odev || READ_ONCE(pon->state) != ZX_ONU_STATE_O5_OPERATION ||
	    gem != READ_ONCE(pon->omci_port) || skb->len != 48) {
		dev_kfree_skb_any(skb);
		return;
	}
	omci_device_receive(pon->odev, skb, gem, 0);
}

static void zx279125_omci_set_operational(struct omci_device *odev, bool operational)
{
	struct zx279125_pon *pon = omci_device_priv(odev);

	pon->tx_allowed = operational;
}

static int zx279125_omci_get_ani_topology(struct omci_device *odev,
					  struct omci_ani_topology *topology)
{
	/* ZX279125 GPON: up to 32 T-CONTs in hardware BWMAP table, 8 priority queues each */
	*topology = (struct omci_ani_topology) {
		.tcont_base = 0x8000,
		.scheduler_base = 0x8000,
		.queue_base = 0x8000,
		.maximum_queue_size = 0xffff,
		.allocated_queue_size = 4,
		.tcont_count = 32,
		.queues_per_tcont = 8,
		.queue_config_option = 1,
		.scheduler_policy = 1,
		.gem_port_count = 256,
		.qos_configuration = BIT(3) | BIT(4),
		.downstream_queues_per_uni = 8,
		.flags = OMCI_TOPOLOGY_F_PROFILE_SERVICES,
	};
	return 0;
}

static int zx279125_omci_set_uni(struct omci_device *odev, u16 entity_id, bool enable)
{
	/* PPTP Ethernet UNI enable/disable; physical PHY is managed by PHY driver */
	return 0;
}

static int zx279125_omci_get_uni_link(struct omci_device *odev, u16 entity_id, bool *link_up)
{
	if (!link_up)
		return -EINVAL;
	*link_up = true;
	return 0;
}

static int zx279125_omci_set_tcont(struct omci_device *odev, u16 entity,
				 u16 alloc, bool valid)
{
	struct zx279125_pon *pon = omci_device_priv(odev);
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(pon->tcont_entities); i++) {
		if (!test_bit(i, &pon->tcont_valid) || pon->tcont_entities[i] != entity)
			continue;
		if (!valid) {
			clear_bit(i, &pon->tcont_valid);
			return 0;
		}
		if (test_bit(i, &pon->alloc_valid) && pon->alloc_ids[i] == alloc)
			return 0;
		return -EBUSY;
	}
	if (!valid)
		return 0;
	if (alloc > 0xfff)
		return -EINVAL;
	/* PLOAM owns BWMAP assignment. OMCI binds its entity to that resource. */
	for (i = 0; i < ARRAY_SIZE(pon->alloc_ids); i++) {
		if (!test_bit(i, &pon->alloc_valid) || pon->alloc_ids[i] != alloc)
			continue;
		if (test_bit(i, &pon->tcont_valid))
			return -EBUSY;
		pon->tcont_entities[i] = entity;
		set_bit(i, &pon->tcont_valid);
		return 0;
	}
	return -EAGAIN;
}

static int zx279125_omci_set_gem(struct omci_device *odev, u16 entity,
			       u16 gem, u16 tcont, u8 direction,
			       bool valid, bool encrypted)
{
	struct zx279125_pon *pon = omci_device_priv(odev);
	unsigned int i, free_slot = ARRAY_SIZE(pon->gems);

	if (gem > 0xfff || !direction || direction > OMCI_GEM_PORT_DIRECTION_BIDIRECTIONAL)
		return -EINVAL;
	if (gem == READ_ONCE(pon->omci_port))
		return -EBUSY;
	if (valid && encrypted && !pon->aes_key_valid)
		return -EAGAIN;
	if (valid && (direction & OMCI_GEM_PORT_DIRECTION_UNI_TO_ANI)) {
		for (i = 0; i < ARRAY_SIZE(pon->tcont_entities); i++)
			if (test_bit(i, &pon->tcont_valid) && pon->tcont_entities[i] == tcont)
				break;
		if (i == ARRAY_SIZE(pon->tcont_entities))
			return -ENOENT;
	}
	for (i = 0; i < ARRAY_SIZE(pon->gems); i++) {
		if (!pon->gems[i].valid) {
			if (free_slot == ARRAY_SIZE(pon->gems))
				free_slot = i;
			continue;
		}
		if (pon->gems[i].entity == entity)
			break;
		if (valid && pon->gems[i].gem == gem)
			return -EEXIST;
	}
	if (!valid) {
		if (i < ARRAY_SIZE(pon->gems)) {
			writel(0, pon->base_ports + pon->gems[i].gem * 4);
			pon->gems[i].valid = false;
		}
		return 0;
	}
	if (i == ARRAY_SIZE(pon->gems))
		i = free_slot;
	if (i == ARRAY_SIZE(pon->gems))
		return -ENOSPC;
	if (pon->gems[i].valid && pon->gems[i].gem != gem)
		writel(0, pon->base_ports + pon->gems[i].gem * 4);
	/* SDK PortIdValid/AesPortIdSet share the two-bit downstream table entry. */
	writel(1 | (encrypted ? 2 : 0), pon->base_ports + gem * 4);
	pon->gems[i].entity = entity;
	pon->gems[i].gem = gem;
	pon->gems[i].tcont = tcont;
	pon->gems[i].valid = true;
	return 0;
}

static int zx279125_omci_get_telemetry(struct omci_device *odev,
				       struct omci_telemetry *t)
{
	struct zx279125_pon *pon = omci_device_priv(odev);
	u8 reg = ZX_BOSA_REG_TEMP;
	u8 d[10];
	struct i2c_msg msgs[2] = {
		{ .addr = ZX_BOSA_I2C_ADDR, .len = 1, .buf = &reg },
		{ .addr = ZX_BOSA_I2C_ADDR, .flags = I2C_M_RD, .len = sizeof(d), .buf = d },
	};

	if (!pon->i2c_adap)
		return -ENODEV;

	mutex_lock(&pon->bosa_lock);
	/* Read 10 bytes telemetry starting at 0x60 from BOSA I2C transceiver */
	if (i2c_transfer(pon->i2c_adap, msgs, 2) != 2) {
		mutex_unlock(&pon->bosa_lock);
		return -EIO;
	}
	mutex_unlock(&pon->bosa_lock);

	/* SFF-8472 conversions */
	t->bosa_temperature_mc = (s16)get_unaligned_be16(d) * 1000 / 256;
	t->bosa_voltage_uv = get_unaligned_be16(d + 2) * 100;
	t->bosa_bias_ua = get_unaligned_be16(d + 4) * 2;
	t->bosa_tx_power_nw = (u32)get_unaligned_be16(d + 6) * 100;
	t->bosa_rx_power_nw = (u32)get_unaligned_be16(d + 8) * 100;
	t->valid |= OMCI_TELEMETRY_F_BOSA_TEMPERATURE |
		    OMCI_TELEMETRY_F_BOSA_VOLTAGE |
		    OMCI_TELEMETRY_F_BOSA_BIAS |
		    OMCI_TELEMETRY_F_BOSA_TX_POWER |
		    OMCI_TELEMETRY_F_BOSA_RX_POWER;

	return 0;
}

static const struct omci_device_ops zx279125_omci_ops = {
	.backend_begin		= zx279125_omci_backend_begin,
	.backend_end		= zx279125_omci_backend_end,
	.backend_identity	= zx279125_omci_backend_identity,
	.backend_reset		= zx279125_omci_backend_reset,
	.start			= zx279125_omci_start,
	.stop			= zx279125_omci_stop,
	.xmit			= zx279125_omci_xmit,
	.get_ani_topology	= zx279125_omci_get_ani_topology,
	.set_tcont		= zx279125_omci_set_tcont,
	.set_gem_port		= zx279125_omci_set_gem,
	.set_uni		= zx279125_omci_set_uni,
	.get_telemetry		= zx279125_omci_get_telemetry,
	.get_uni_link		= zx279125_omci_get_uni_link,
	.set_operational	= zx279125_omci_set_operational,
};

/* Sysfs Optical Telemetry Attributes */
static int zx279125_read_bosa_raw(struct zx279125_pon *pon, u8 d[10])
{
	u8 reg = ZX_BOSA_REG_TEMP;
	struct i2c_msg msgs[2] = {
		{ .addr = ZX_BOSA_I2C_ADDR, .len = 1, .buf = &reg },
		{ .addr = ZX_BOSA_I2C_ADDR, .flags = I2C_M_RD, .len = 10, .buf = d },
	};
	int ret;

	if (!pon->i2c_adap)
		return -ENODEV;

	mutex_lock(&pon->bosa_lock);
	ret = i2c_transfer(pon->i2c_adap, msgs, 2);
	mutex_unlock(&pon->bosa_lock);

	return ret == 2 ? 0 : -EIO;
}

static ssize_t optical_state_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct zx279125_pon *pon = dev_get_drvdata(dev);

	return sysfs_emit(buf, "O%d\n", pon->state);
}
static DEVICE_ATTR_RO(optical_state);

static ssize_t temperature_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct zx279125_pon *pon = dev_get_drvdata(dev);
	u8 d[10];
	int ret, temp_mc;

	ret = zx279125_read_bosa_raw(pon, d);
	if (ret)
		return ret;

	temp_mc = (s16)get_unaligned_be16(d) * 1000 / 256;
	return sysfs_emit(buf, "%d.%02d C\n", temp_mc / 1000,
			  abs(temp_mc % 1000) / 10);
}
static DEVICE_ATTR_RO(temperature);

static ssize_t voltage_show(struct device *dev,
			    struct device_attribute *attr, char *buf)
{
	struct zx279125_pon *pon = dev_get_drvdata(dev);
	u8 d[10];
	int ret, vcc_uv;

	ret = zx279125_read_bosa_raw(pon, d);
	if (ret)
		return ret;

	vcc_uv = (u32)get_unaligned_be16(d + 2) * 100;
	return sysfs_emit(buf, "%u.%03u V\n", vcc_uv / 1000000,
			  (vcc_uv % 1000000) / 1000);
}
static DEVICE_ATTR_RO(voltage);

static ssize_t bias_current_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct zx279125_pon *pon = dev_get_drvdata(dev);
	u8 d[10];
	int ret, bias_ua;

	ret = zx279125_read_bosa_raw(pon, d);
	if (ret)
		return ret;

	bias_ua = (u32)get_unaligned_be16(d + 4) * 2;
	return sysfs_emit(buf, "%u.%03u mA\n", bias_ua / 1000,
			  bias_ua % 1000);
}
static DEVICE_ATTR_RO(bias_current);

static ssize_t rx_power_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	struct zx279125_pon *pon = dev_get_drvdata(dev);
	u8 d[10];
	int ret;
	u16 raw;

	ret = zx279125_read_bosa_raw(pon, d);
	if (ret)
		return ret;

	raw = get_unaligned_be16(d + 8);
	return sysfs_emit(buf, "%u uW\n", (unsigned int)raw / 10);
}
static DEVICE_ATTR_RO(rx_power);

static ssize_t tx_power_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	struct zx279125_pon *pon = dev_get_drvdata(dev);
	u8 d[10];
	int ret;
	u16 raw;

	ret = zx279125_read_bosa_raw(pon, d);
	if (ret)
		return ret;

	raw = get_unaligned_be16(d + 6);
	return sysfs_emit(buf, "%u uW\n", (unsigned int)raw / 10);
}
static DEVICE_ATTR_RO(tx_power);

static ssize_t bosa_telemetry_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	static const char * const states[] = {
		"O1", "O2", "O3", "O4", "O5", "O6", "O7"
	};
	struct zx279125_pon *pon = dev_get_drvdata(dev);
	u8 d[10];
	int ret, temp_mc, vcc_uv, bias_ua, tx_uw, rx_uw;
	const char *state_str = "Unknown";

	ret = zx279125_read_bosa_raw(pon, d);
	if (ret)
		return ret;

	temp_mc = (s16)get_unaligned_be16(d) * 1000 / 256;
	vcc_uv = (u32)get_unaligned_be16(d + 2) * 100;
	bias_ua = (u32)get_unaligned_be16(d + 4) * 2;
	tx_uw = (u32)get_unaligned_be16(d + 6) / 10;
	rx_uw = (u32)get_unaligned_be16(d + 8) / 10;

	if (pon->state >= 1 && pon->state <= 7)
		state_str = states[pon->state - 1];

	return sysfs_emit(buf,
			  "Optical State : %s\n"
			  "Laser Safety  : %s\n"
			  "Temperature   : %d.%02d C\n"
			  "Supply VCC    : %u.%03u V\n"
			  "Laser Bias    : %u.%03u mA\n"
			  "TX Power      : %u uW\n"
			  "RX Power      : %u uW\n",
			  state_str,
			  pon->laser_allowed ? "ENABLED" : "DISABLED (Safety Hold Active)",
			  temp_mc / 1000, abs(temp_mc % 1000) / 10,
			  vcc_uv / 1000000, (vcc_uv % 1000000) / 1000,
			  bias_ua / 1000, bias_ua % 1000,
			  tx_uw, rx_uw);
}
static DEVICE_ATTR_RO(bosa_telemetry);

static bool laser_enable;
module_param_named(laser_enable, laser_enable, bool, 0644);
MODULE_PARM_DESC(laser_enable, "Allow optical TX laser emission (default: 0 / safety hold)");

static ssize_t laser_enable_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct zx279125_pon *pon = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%d\n", pon->laser_allowed ? 1 : 0);
}

static ssize_t laser_enable_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t count)
{
	struct zx279125_pon *pon = dev_get_drvdata(dev);
	bool enable;
	int ret;

	ret = kstrtobool(buf, &enable);
	if (ret)
		return ret;

	pon->laser_allowed = enable;
	if (!enable)
		zx279125_gpon_hold_tx(pon, true);

	dev_info(dev, "optical TX laser %s\n", enable ? "ENABLED" : "DISABLED");
	return count;
}
static DEVICE_ATTR_RW(laser_enable);

static struct attribute *zx279125_pon_attrs[] = {
	&dev_attr_optical_state.attr,
	&dev_attr_laser_enable.attr,
	&dev_attr_temperature.attr,
	&dev_attr_voltage.attr,
	&dev_attr_bias_current.attr,
	&dev_attr_rx_power.attr,
	&dev_attr_tx_power.attr,
	&dev_attr_bosa_telemetry.attr,
	NULL,
};
ATTRIBUTE_GROUPS(zx279125_pon);

/* ---- pon0 Ethernet Device ---- */

static int zx279125_pon0_open(struct net_device *dev)
{
	netif_start_queue(dev);
	return 0;
}

static int zx279125_pon0_stop(struct net_device *dev)
{
	netif_stop_queue(dev);
	return 0;
}

static netdev_tx_t zx279125_pon0_xmit(struct sk_buff *skb, struct net_device *dev)
{
	dev->stats.tx_dropped++;
	dev_kfree_skb_any(skb);
	return NETDEV_TX_OK;
}

static const struct net_device_ops zx279125_pon0_ops = {
	.ndo_open = zx279125_pon0_open,
	.ndo_stop = zx279125_pon0_stop,
	.ndo_start_xmit = zx279125_pon0_xmit,
	.ndo_set_mac_address = eth_mac_addr,
	.ndo_validate_addr = eth_validate_addr,
};

static int zx279125_pon0_get_link_ksettings(struct net_device *dev,
					    struct ethtool_link_ksettings *cmd)
{
	struct zx279125_pon *pon = *(struct zx279125_pon **)netdev_priv(dev);

	ethtool_link_ksettings_zero_link_mode(cmd, supported);
	ethtool_link_ksettings_add_link_mode(cmd, supported, FIBRE);
	ethtool_link_ksettings_zero_link_mode(cmd, advertising);
	cmd->base.port = PORT_FIBRE;
	cmd->base.autoneg = AUTONEG_DISABLE;
	if (!netif_carrier_ok(dev)) {
		cmd->base.speed = SPEED_UNKNOWN;
		cmd->base.duplex = DUPLEX_UNKNOWN;
		return 0;
	}
	cmd->base.speed = (pon->state == ZX_ONU_STATE_O5_OPERATION) ? SPEED_2500 : SPEED_UNKNOWN;
	cmd->base.duplex = DUPLEX_FULL;
	return 0;
}

static const struct ethtool_ops zx279125_pon0_ethtool_ops = {
	.get_link = ethtool_op_get_link,
	.get_link_ksettings = zx279125_pon0_get_link_ksettings,
};

static int zx279125_pon0_register(struct zx279125_pon *pon)
{
	struct net_device *dev;
	int ret;

	dev = alloc_etherdev(sizeof(pon));
	if (!dev)
		return -ENOMEM;

	*(struct zx279125_pon **)netdev_priv(dev) = pon;
	strscpy(dev->name, "pon%d", sizeof(dev->name));
	dev->netdev_ops = &zx279125_pon0_ops;
	dev->ethtool_ops = &zx279125_pon0_ethtool_ops;
	dev->min_mtu = ETH_MIN_MTU;
	dev->max_mtu = 2000;
	SET_NETDEV_DEV(dev, pon->dev);

	if (pon->has_pon_mac)
		eth_hw_addr_set(dev, pon->pon_mac);
	else
		eth_hw_addr_random(dev);

	netif_carrier_off(dev);

	ret = register_netdev(dev);
	if (ret) {
		free_netdev(dev);
		return ret;
	}

	pon->netdev = dev;
	return 0;
}

static void zx279125_pon0_unregister(struct zx279125_pon *pon)
{
	if (pon->netdev) {
		unregister_netdev(pon->netdev);
		free_netdev(pon->netdev);
		pon->netdev = NULL;
	}
}

static int zx279125_pon_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *i2c_np;
	struct zx279125_pon *pon;
	struct omci_identity id;
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
	pon->laser_allowed = laser_enable;
	spin_lock_init(&pon->lock);
	mutex_init(&pon->bosa_lock);
	mutex_init(&pon->config_lock);

	/* Map 6 hardware register blocks */
	pon->base_gmac_int = devm_platform_ioremap_resource_byname(pdev, "intc");
	if (IS_ERR(pon->base_gmac_int))
		return PTR_ERR(pon->base_gmac_int);

	pon->base_gpon = devm_platform_ioremap_resource_byname(pdev, "gpon");
	if (IS_ERR(pon->base_gpon))
		return PTR_ERR(pon->base_gpon);
	pon->base_rx = devm_platform_ioremap_resource_byname(pdev, "rx");
	if (IS_ERR(pon->base_rx))
		return PTR_ERR(pon->base_rx);
	pon->base_drs = devm_platform_ioremap_resource_byname(pdev, "drs");
	if (IS_ERR(pon->base_drs))
		return PTR_ERR(pon->base_drs);
	pon->base_ports = devm_platform_ioremap_resource_byname(pdev, "ports");
	if (IS_ERR(pon->base_ports))
		return PTR_ERR(pon->base_ports);

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

	/* Ingest authentic factory calibration directly from SPI NOR partition mtd5 */
	ret = zx279125_tagparam_load(pon, &id);
	if (ret == -EPROBE_DEFER)
		return ret;
	if (ret)
		dev_warn(dev, "failed to load factory tag parameters: %d\n", ret);

	if (pon->mode_pref != XPON_MODE_GPON)
		dev_info(dev, "GPON provider only; factory auto/EPON mode is not applied\n");

	/* Attach BOSA I2C adapter if referenced in Device Tree */
	i2c_np = of_parse_phandle(dev->of_node, "zte,bosa-i2c", 0);
	if (i2c_np) {
		pon->i2c_adap = of_find_i2c_adapter_by_node(i2c_np);
		of_node_put(i2c_np);
		if (!pon->i2c_adap)
			return -EPROBE_DEFER;
	}

	platform_set_drvdata(pdev, pon);

	/* Register pon0 ethernet network device */
	ret = zx279125_pon0_register(pon);
	if (ret)
		goto err_bosa;

	/* Mask optical sources before registering their shared interrupt handler. */
	ret = zx279125_gpon_init(pon);
	if (ret)
		goto err_netdev;

	/* Keep laser force-close held during bringup for safety. */
	zx279125_gpon_hold_tx(pon, true);

	ret = devm_request_irq(dev, pon->irq, zx279125_pon_isr,
			       IRQF_SHARED, "zx279125-pon", pon);
	if (ret)
		goto err_netdev;

	/* Register with xPON Subsystem */
	desc.netdev = pon->netdev;
	desc.priv = pon;
	pon->xpon = xpon_device_register(dev, &desc);
	if (IS_ERR(pon->xpon)) {
		ret = dev_err_probe(dev, PTR_ERR(pon->xpon), "xPON registration failed\n");
		pon->xpon = NULL;
		goto err_gpon;
	}

	/* Register OMCI device with telemetry capability */
	pon->odev = omci_device_register(pon->xpon,
					 OMCI_CAP_TELEMETRY,
					 &zx279125_omci_ops, pon);
	if (IS_ERR(pon->odev)) {
		ret = dev_err_probe(dev, PTR_ERR(pon->odev), "OMCI registration failed\n");
		pon->odev = NULL;
		goto err_xpon;
	}

	/* Push factory identity loaded from partition into OMCI stack */
	if (id.valid & OMCI_IDENTITY_F_SERIAL_NUMBER) {
		omci_device_set_identity_info(pon->odev, &id);
		omci_device_set_identity(pon->odev, id.serial_number,
					 (id.valid & OMCI_IDENTITY_F_PASSWORD) ? id.password : NULL);
		memcpy(pon->serial_number, id.serial_number, sizeof(pon->serial_number));
		if (id.valid & OMCI_IDENTITY_F_PASSWORD)
			memcpy(pon->password, id.password, sizeof(pon->password));

		/* Write hardware GPON SN register */
		writel(get_unaligned_be32(pon->serial_number), pon->base_tx + ZX_PON_SERIAL_NUM);
		writel(get_unaligned_be32(pon->serial_number + 4), pon->base_tx + ZX_PON_SERIAL_NUM + 4);
	}
	pon->receiver.receive_control = zx279125_omci_receive;
	pon->receiver.priv = pon;
	ret = zx279125_pp_register_receiver(&pon->receiver);
	if (ret)
		goto err_omci;

	dev_info(dev, "ZTE Sanechips ZX279125 xPON controller ready (IRQ %d, %s mode)\n",
		 pon->irq, desc.mode == XPON_MODE_GPON ? "GPON" : "EPON");
	return 0;

err_omci:
	omci_device_unregister(pon->odev);
	pon->odev = NULL;
err_xpon:
	zx279125_gpon_exit(pon);
	xpon_device_unregister(pon->xpon);
	pon->xpon = NULL;
err_gpon:
	zx279125_gpon_exit(pon);
err_netdev:
	zx279125_pon0_unregister(pon);
err_bosa:
	if (pon->i2c_adap) {
		i2c_put_adapter(pon->i2c_adap);
		pon->i2c_adap = NULL;
	}
	return ret;
}

static void zx279125_pon_remove(struct platform_device *pdev)
{
	struct zx279125_pon *pon = platform_get_drvdata(pdev);

	zx279125_pp_unregister_receiver(&pon->receiver);
	zx279125_gpon_exit(pon);
	devm_free_irq(&pdev->dev, pon->irq, pon);
	if (pon->odev)
		omci_device_unregister(pon->odev);

	if (pon->xpon)
		xpon_device_unregister(pon->xpon);

	zx279125_pon0_unregister(pon);

	if (pon->i2c_adap) {
		i2c_put_adapter(pon->i2c_adap);
		pon->i2c_adap = NULL;
	}
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
