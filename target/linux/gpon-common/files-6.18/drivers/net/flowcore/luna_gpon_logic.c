// SPDX-License-Identifier: GPL-2.0-only
/* See luna_gpon_logic.h. */
#include <linux/types.h>
#include "gpon_ddm.h"	/* the one 0.1 uW -> centi-dBm conversion */
#include <linux/bitops.h>
#include <linux/limits.h>

#include "luna_gpon_logic.h"

/* Map an RTL8290B 12-bit register number to its paging I2C slave address. */
u8 bosa_slave_for(u16 reg)
{
	switch (reg >> 8) {
	case 0:  return 0x50;
	case 1:  return 0x51;
	case 2:  return 0x54;
	default: return 0x55;		/* page 3 and up */
	}
}

/* Raw SFF-8472 DDM power word (0.1 uW/LSB) -> the ANI-G level encoding.
 * INT_MIN when the word reads "not available" (0x0000/0xffff), so the caller
 * keeps its cache. */
s32 ddm_word_to_level(int raw)
{
	s32 level;

	if (raw <= 0 || raw >= 0xffff)
		return INT_MIN;
	level = gpon_ddm_cdbm_to_anig(gpon_ddm_uw10_to_cdbm((u32)raw));
	return level;
}

/* Linear power code (0.1 uW) -> centi-dBm. A dark or "n/a" read floors at
 * -40.00 dBm rather than returning a sentinel: this family's reporting policy. */
s32 bosa_code_to_cdbm(u32 code)
{
	if (code < 1)
		return -4000;
	return gpon_ddm_uw10_to_cdbm(code);
}

/* BOSA/DDM optical measurement logic: the shell samples the RTL8290B over I2C
 * and these functions decide what the samples MEAN. math64 because MIPS32 has
 * no libgcc 64-bit divide. */
#include <linux/math64.h>

#define BOSA_ADC_VREF_UV	3300000		/* stock 3.3V ADC full-scale (0x325aa0) */

/* RX power code (0.1 uW) from the raw ratiometric ADC samples ...
 * dev/MEASURED-luna_gpon_logic.c.md sec 1. */
u32 bosa_rx_code_calc(u32 rssi, u32 tap_lo, u32 tap_hi,
		      const struct bosa_optical_cal *cal)
{
	u32 span;
	u64 v_uv, irssi, code;
	u32 s1, q;

	if (tap_hi <= tap_lo)			/* dead/floating I2C: taps read alike */
		return BOSA_RX_CODE_NA;
	span = tap_hi - tap_lo;
	if (rssi <= tap_lo)			/* below the low reference tap        */
		return BOSA_RX_CODE_NA;
	v_uv = div64_u64((u64)(rssi - tap_lo) * BOSA_ADC_VREF_UV, span);
	if (v_uv <= cal->rx_vthr)		/* at/below the dark level -> no light */
		return BOSA_RX_CODE_NA;
	irssi = div64_u64((u64)(v_uv - cal->rx_vthr) * 1000 * (cal->rx_r1 + cal->rx_r2),
			  (u64)cal->rx_r1 * cal->rx_r2);
	s1 = (irssi < 65536) ? 10 : 100;
	q  = (u32)div64_u64(irssi, s1);
	code = (((u64)cal->rx_poly_b * q) >> 13) * s1 + ((1000u * (u32)cal->rx_poly_c) >> 12);
	code = div64_u64(code, 100);
	return code < 11 ? 11 : (u32)code;
}

/* Module temperature in deci-degC from 14 raw (0x302, 0x303) register pairs:
 * Kelvin code a[7:0]<<1 | b[7] (233..383 K = -40..+110 C), per-sample clamp
 * against torn reads, sort, drop 2 low + 2 high, mean the middle 10, minus the
 * per-board Kelvin trim. A negative sample is a failed I2C read -> INT_MIN. */
s32 bosa_temp_dc_calc(const int *a, const int *b, s16 temp_off)
{
	u16 s[14];
	u32 sum = 0;
	int i, j;

	for (i = 0; i < 14; i++) {
		u16 code;

		if (a[i] < 0 || b[i] < 0)
			return INT_MIN;
		code = ((u16)(a[i] & 0xff) << 1) | ((b[i] >> 7) & 1);
		if (code < 233) code = 233;
		if (code > 383) code = 383;
		s[i] = code;
	}
	for (i = 0; i < 13; i++)
		for (j = 0; j < 13 - i; j++)
			if (s[j + 1] < s[j]) { u16 t = s[j]; s[j] = s[j + 1]; s[j + 1] = t; }
	for (i = 2; i < 12; i++)
		sum += s[i];
	sum /= 10;
	return ((int)sum - temp_off - 273) * 10;
}

/* Laser bias current in micro-amps from the raw (0x321, 0x322) register pair.
 * 12-bit monitor code (h[7:0]<<4 | l[3:0]), full-scale ~100 mA at code 8192
 * (stock A2 word is 2 uA/LSB). A negative input is a failed read -> 0. */
u32 bosa_bias_ua_calc(int h, int l)
{
	u32 code12;

	if (h < 0 || l < 0)
		return 0;
	code12 = ((u32)(h & 0xff) << 4) | (u32)(l & 0x0f);
	return (u32)div_u64((u64)code12 * 100000, 8192) * 2;
}

/* One TX-power sample's contribution to the 10-sample accumulator (europa_drv
 * update_ddmi_tx_power): MPD-minus-dark voltage -> power code, then a bias class
 * from iavg (0x23A[7:0]) and a range shift from 0x246[7:6]. vmpd and dark are
 * millivolts from the shell's SD-ADC ch2 sequence. */
u32 bosa_tx_sample_contrib(s32 vmpd, s32 dark, int iavg, int range)
{
	s32 c = (((vmpd - dark) * 1000 / 1374) >> 4) + 50;
	int cls, shift;

	if (c < 0)
		c = 0;
	cls   = iavg < 64 ? 0 : iavg < 96 ? 1 : iavg < 128 ? 2 : iavg < 160 ? 3 : 4;
	shift = cls - (range == 1 ? 1 : range == 2 ? 2 : 0);
	return shift >= 0 ? (u32)c << shift : (u32)c >> -shift;
}

/* Fold the accumulated TX contributions into the per-board 0.1 uW word for
 * bosa_code_to_cdbm. n must be >= 1. */
u32 bosa_tx_word_calc(u64 sum, int n, s32 tx_slope, s32 tx_offset)
{
	return (u32)((((u64)div_u64(sum, n) * tx_slope * 10) >> 8) +
		     ((tx_offset * 10) >> 5));
}

/* pi_packed_locate/insert/extract live in flowcore_hash.c, declared through
 * flowcore.h: the Cortina flow engine needs them too and this object is gated
 * on CONFIG_LUNA_GPON, which that board never sets. */
#include <linux/string.h>

/* The test ORDER is load-bearing. An RTL8290B may ship an ...
 * dev/MEASURED-luna_gpon_logic.c.md sec 2. */
enum bosa_module_verdict bosa_module_classify(int ident, int extid,
					      const char *vend, const char *part)
{
	if (strstr(vend, "REALTEK") || strstr(part, "RTL8290"))
		return BOSA_MODULE_NAMED_OURS;
	if (ident > 0 && ident < 0x30 && extid >= 0)
		return BOSA_MODULE_FOREIGN;
	return BOSA_MODULE_COULD_NOT_TELL;
}

/* One SFF-8472 string field, sanitized for the log: printable ASCII kept, any
 * other byte -- a failed (negative) I2C read included -- rendered '.'. */
void bosa_sff_text(char *dst, const int *raw, unsigned int n)
{
	unsigned int i;

	for (i = 0; i < n; i++)
		dst[i] = (raw[i] >= 0x20 && raw[i] < 0x7f) ? (char)raw[i] : '.';
	dst[n] = 0;
}

/* Which of up to n samples is THE reading: insertion-sort ascending in place,
 * return the upper median. BOSA_RX_CODE_NA == 0 sorts to the bottom, so one
 * glitched or dark sample is discarded and only 2-of-3 NA makes the verdict NA.
 * n must be >= 1. */
u32 bosa_median_u32(u32 *v, unsigned int n)
{
	unsigned int i, j;

	for (i = 1; i < n; i++) {
		u32 key = v[i];

		for (j = i; j > 0 && v[j - 1] > key; j--)
			v[j] = v[j - 1];
		v[j] = key;
	}
	return v[n / 2];
}

/* Whether an MPD ADC sample is a measurement, and its ratiometric mV if so (the
 * vmpd input of bosa_tx_sample_contrib). hi == 0 or code at/below the zero tap is
 * the same dead-bus shape bosa_rx_code_calc owns -> INT_MIN. */
s32 bosa_vmpd_mv_calc(u32 code, s32 hi, s32 zero)
{
	if (hi == 0 || (s32)code <= zero)
		return INT_MIN;
	return (s32)div_u64((u64)(u32)(hi - zero) * 1200, (u32)((s32)code - zero));
}
