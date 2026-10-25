/* SPDX-License-Identifier: GPL-2.0-only */
/* luna_gpon_logic.h -- the Luna GPON shell's hoisted pure ...
 * dev/MEASURED-luna_gpon_logic.h.md sec 1. */
#ifndef _LUNA_GPON_LOGIC_H
#define _LUNA_GPON_LOGIC_H

#include <linux/types.h>

u8 bosa_slave_for(u16 reg);
s32 ddm_word_to_level(int raw);
s32 bosa_code_to_cdbm(u32 code);

/* "Could not measure" sentinel for the RX optical chain. ...
 * dev/MEASURED-luna_gpon_logic.h.md sec 2. */
#define BOSA_RX_CODE_NA		0u

/* Per-board optical calibration. Stock loads this from rtl8290b.data into its
 * europa_param struct; the shell owns the instance (compiled defaults are that
 * board's confirmed values) and passes it down BY ARGUMENT -- the logic never
 * reaches back for file-scope state. */
struct bosa_optical_cal {
	/* Faithful RTL8290B RX-power chain (re-expressed from ...
	 * dev/MEASURED-luna_gpon_logic.h.md sec 3. */
	u32 rx_vthr;		/* RSSI detection threshold / dark level, uV (data @0x552) */
	u32 rx_r1, rx_r2;	/* RSSI load resistors, ohm (data @0x5df, @0x5e1, x10) */
	s32 rx_poly_b, rx_poly_c;	/* code poly, a=0 on this board (data @0x54a, @0x54e) */
	s16 temp_off;		/* temperature offset, degC (data @0x568) */
	s32 tx_slope, tx_offset;	/* TX word = (mpd*slope*10)>>8 + (offset*10>>5) (data @0x55c/0x560) */
};

u32 bosa_rx_code_calc(u32 rssi, u32 tap_lo, u32 tap_hi,
		      const struct bosa_optical_cal *cal);
s32 bosa_temp_dc_calc(const int *a, const int *b, s16 temp_off);
u32 bosa_bias_ua_calc(int h, int l);
u32 bosa_tx_sample_contrib(s32 vmpd, s32 dark, int iavg, int range);
u32 bosa_tx_word_calc(u64 sum, int n, s32 tx_slope, s32 tx_offset);

/* pi_packed_locate/insert/extract + struct pi_packed_slot ...
 * dev/MEASURED-luna_gpon_logic.h.md sec 5. */
#include "flowcore.h"

/* ===== round 2 (2026-09-02): module identity + sample ...
 * dev/MEASURED-luna_gpon_logic.h.md sec 4. */
enum bosa_module_verdict {
	BOSA_MODULE_NAMED_OURS,		/* strings name REALTEK/RTL8290: path stays enabled */
	BOSA_MODULE_FOREIGN,		/* plausible SFF-8024 ident AND a foreign name:
					 * refuse the register writes (they would land in
					 * an identity EEPROM) */
	BOSA_MODULE_COULD_NOT_TELL,	/* no plausible identity read: path stays enabled */
};

enum bosa_module_verdict bosa_module_classify(int ident, int extid,
					      const char *vend, const char *part);

/* Render one SFF-8472 string field: n sampled bytes (a negative byte is a
 * failed read) -> printable ASCII, '.' elsewhere, NUL-terminated (dst[n+1]). */
void bosa_sff_text(char *dst, const int *raw, unsigned int n);

/* Median of the first n samples (insertion sort in place, ...
 * dev/MEASURED-luna_gpon_logic.h.md sec 6. */
u32 bosa_median_u32(u32 *v, unsigned int n);

/* The MPD sample validity test + ratiometric mV conversion (the one
 * un-hoisted link of the TX-power chain): hi == 0 or code at/below the zero
 * tap is the taps-agree dead-bus shape -> INT_MIN ("not a measurement");
 * else mV = (hi - zero) * 1200 / (code - zero). */
s32 bosa_vmpd_mv_calc(u32 code, s32 hi, s32 zero);

#endif /* _LUNA_GPON_LOGIC_H */
