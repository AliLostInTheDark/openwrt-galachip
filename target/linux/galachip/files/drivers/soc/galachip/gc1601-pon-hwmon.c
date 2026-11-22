// SPDX-License-Identifier: GPL-2.0-only
/*
 * GalaChip GC1601 PON: the BOSA's SFF-8472 diagnostics as a hwmon device.
 *
 * The UX3320S is internally calibrated: the A2 page holds temperature, supply
 * voltage, laser bias and TX/RX power in SFF-8472 units, live at 0x60 and
 * with alarm and warning thresholds at 0x00. The channels and labels follow
 * the SFP driver (drivers/net/phy/sfp.c).
 *
 * Copyright (C) 2026 Ifran Ali <khanu.i.i.k.2000@gmail.com>
 */
#include <linux/hwmon.h>
#include <linux/unaligned.h>

#include "gc1601-pon.h"

#define BOSA_DDM_LIVE		0x60

/* Threshold blocks: high alarm, low alarm, high warning, low warning */
#define BOSA_DDM_TEMP_THR	0x00
#define BOSA_DDM_VCC_THR	0x08
#define BOSA_DDM_BIAS_THR	0x10
#define BOSA_DDM_TX_THR		0x18
#define BOSA_DDM_RX_THR		0x20

enum { DDM_TEMP, DDM_VCC, DDM_BIAS, DDM_TX, DDM_RX };

static const u8 gc1601_ddm_thr[] = {
	[DDM_TEMP] = BOSA_DDM_TEMP_THR,
	[DDM_VCC] = BOSA_DDM_VCC_THR,
	[DDM_BIAS] = BOSA_DDM_BIAS_THR,
	[DDM_TX] = BOSA_DDM_TX_THR,
	[DDM_RX] = BOSA_DDM_RX_THR,
};

static const char *const gc1601_ddm_label[] = {
	[DDM_VCC] = "VCC",
	[DDM_BIAS] = "bias",
	[DDM_TX] = "TX_power",
	[DDM_RX] = "RX_power",
};

/* SFF-8472 units to hwmon units: m°C, mV, mA, µW */
static long gc1601_ddm_scale(int ddm, u16 raw)
{
	switch (ddm) {
	case DDM_TEMP:
		return (s16)raw * 1000L / 256;
	case DDM_VCC:
		return raw / 10;
	case DDM_BIAS:
		return raw / 500;
	default:
		return raw / 10;
	}
}

static int gc1601_ddm_channel(enum hwmon_sensor_types type, int channel)
{
	switch (type) {
	case hwmon_temp:
		return DDM_TEMP;
	case hwmon_in:
		return DDM_VCC;
	case hwmon_curr:
		return DDM_BIAS;
	case hwmon_power:
		return channel ? DDM_RX : DDM_TX;
	default:
		return -EOPNOTSUPP;
	}
}

/* Offset of an attribute's value from its threshold block, or -1 for the live value */
static int gc1601_ddm_offset(enum hwmon_sensor_types type, u32 attr)
{
	switch (type) {
	case hwmon_temp:
		switch (attr) {
		case hwmon_temp_input: return -1;
		case hwmon_temp_crit: return 0;
		case hwmon_temp_lcrit: return 2;
		case hwmon_temp_max: return 4;
		case hwmon_temp_min: return 6;
		}
		break;
	case hwmon_in:
		switch (attr) {
		case hwmon_in_input: return -1;
		case hwmon_in_crit: return 0;
		case hwmon_in_lcrit: return 2;
		case hwmon_in_max: return 4;
		case hwmon_in_min: return 6;
		}
		break;
	case hwmon_curr:
		switch (attr) {
		case hwmon_curr_input: return -1;
		case hwmon_curr_crit: return 0;
		case hwmon_curr_lcrit: return 2;
		case hwmon_curr_max: return 4;
		case hwmon_curr_min: return 6;
		}
		break;
	case hwmon_power:
		switch (attr) {
		case hwmon_power_input: return -1;
		case hwmon_power_crit: return 0;
		case hwmon_power_lcrit: return 2;
		case hwmon_power_max: return 4;
		case hwmon_power_min: return 6;
		}
		break;
	default:
		break;
	}
	return -EOPNOTSUPP;
}

static umode_t gc1601_ddm_is_visible(const void *data, enum hwmon_sensor_types type,
				     u32 attr, int channel)
{
	return 0444;
}

static int gc1601_ddm_read(struct device *dev, enum hwmon_sensor_types type,
			   u32 attr, int channel, long *val)
{
	struct gc1601_pon *pon = dev_get_drvdata(dev);
	int ddm = gc1601_ddm_channel(type, channel);
	int off = gc1601_ddm_offset(type, attr);
	u8 reg, d[2];
	int ret;

	if (ddm < 0 || off == -EOPNOTSUPP)
		return -EOPNOTSUPP;
	/* The A2 page reads garbage until the BOSA is programmed. */
	if (READ_ONCE(pon->bosa_state) != GC1601_BOSA_READY)
		return -ENODATA;

	reg = off < 0 ? BOSA_DDM_LIVE + 2 * ddm : gc1601_ddm_thr[ddm] + off;
	ret = gc1601_bosa_read(pon, reg, d, sizeof(d));
	if (ret)
		return ret;
	*val = gc1601_ddm_scale(ddm, get_unaligned_be16(d));
	return 0;
}

static int gc1601_ddm_read_string(struct device *dev, enum hwmon_sensor_types type,
				  u32 attr, int channel, const char **str)
{
	int ddm = gc1601_ddm_channel(type, channel);

	if (ddm <= DDM_TEMP)
		return -EOPNOTSUPP;
	*str = gc1601_ddm_label[ddm];
	return 0;
}

#define DDM_LIMITS(t)	(t##_INPUT | t##_CRIT | t##_LCRIT | t##_MAX | t##_MIN)

static const struct hwmon_channel_info *const gc1601_ddm_info[] = {
	HWMON_CHANNEL_INFO(temp, DDM_LIMITS(HWMON_T)),
	HWMON_CHANNEL_INFO(in, DDM_LIMITS(HWMON_I) | HWMON_I_LABEL),
	HWMON_CHANNEL_INFO(curr, DDM_LIMITS(HWMON_C) | HWMON_C_LABEL),
	HWMON_CHANNEL_INFO(power, DDM_LIMITS(HWMON_P) | HWMON_P_LABEL,
			   DDM_LIMITS(HWMON_P) | HWMON_P_LABEL),
	NULL
};

static const struct hwmon_ops gc1601_ddm_ops = {
	.is_visible = gc1601_ddm_is_visible,
	.read = gc1601_ddm_read,
	.read_string = gc1601_ddm_read_string,
};

static const struct hwmon_chip_info gc1601_ddm_chip = {
	.ops = &gc1601_ddm_ops,
	.info = gc1601_ddm_info,
};

void gc1601_pon_hwmon_register(struct gc1601_pon *pon)
{
	struct device *hwmon;

	hwmon = hwmon_device_register_with_info(pon->dev, "bosa", pon,
						&gc1601_ddm_chip, NULL);
	if (IS_ERR(hwmon)) {
		dev_warn(pon->dev, "no hwmon device for the BOSA: %pe\n", hwmon);
		return;
	}
	pon->hwmon = hwmon;
}

void gc1601_pon_hwmon_unregister(struct gc1601_pon *pon)
{
	if (pon->hwmon)
		hwmon_device_unregister(pon->hwmon);
	pon->hwmon = NULL;
}
