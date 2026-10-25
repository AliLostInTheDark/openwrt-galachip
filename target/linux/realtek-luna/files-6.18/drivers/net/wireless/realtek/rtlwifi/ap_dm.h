/* SPDX-License-Identifier: GPL-2.0 */
/* AP-mode initial gain and EDCCA as the vendor's phydm runs them, shared by
 * rtl8192fe (X111W, G24W) and rtl8192ee (V2801RGW). Mainline's dm.c carries a
 * STATION's recipe: rx_gain_max = rssi_min + 10, and EDCCA armed at L2H 0x03 /
 * H2L 0x00 once the IGI passes 0x28 -- a CCA threshold so low the MAC defers
 * every frame, beacons included (field X111W 2026-10-06: 0 own beacons at
 * 0x303, 252 at 0x7f037f, 0 again restored). Bounds: stock's boundary helpers
 * run under emulation (rtl9607c-test/rtl8192fe_dig_golden.h); EDCCA:
 * phydm_adaptivity.c normal mode. The includer provides DM_DIG_FA_TH0..2,
 * ROFDM0_ECCATHRESHOLD and its own write_dig.
 */
#ifndef __RTLWIFI_AP_DM_H__
#define __RTLWIFI_AP_DM_H__

static inline void rtl_ap_dm_dig(struct ieee80211_hw *hw,
				 void (*write_dig)(struct ieee80211_hw *hw, u8 igi))
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	struct dig_t *dm_dig = &rtlpriv->dm_digtable;
	u32 fa = rtlpriv->falsealm_cnt.cnt_all;
	long rssi = dm_dig->rssi_val_min;
	u8 lo = 0x1c, hi = 0x26, igi = dm_dig->cur_igvalue;

	if (rtlpriv->dm.entry_min_undec_sm_pwdb) {
		lo = (u8)(rssi < 0x20 ? 0x20 : rssi > 0x40 ? 0x40 : rssi);
		hi = (u8)((rssi > 0x40 ? 0x40 : rssi) + 15);
		if (hi < lo)
			hi = lo;
	}
	if (fa > DM_DIG_FA_TH2)
		igi += 4;
	else if (fa > DM_DIG_FA_TH1)
		igi += 2;
	else if (fa < DM_DIG_FA_TH0)
		igi -= 2;
	write_dig(hw, igi < lo ? lo : igi > hi ? hi : igi);
	dm_dig->rx_gain_min = lo;
	dm_dig->rx_gain_max = hi;
}

static inline void rtl_ap_dm_edcca(struct ieee80211_hw *hw, u8 igi)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	u8 l2h = igi + 8 > 0x30 ? igi + 8 : 0x30;
	u8 h2l = l2h - 8;

	if (rtl_read_byte(rtlpriv, ROFDM0_ECCATHRESHOLD) != l2h ||
	    rtl_read_byte(rtlpriv, ROFDM0_ECCATHRESHOLD + 2) != h2l) {
		rtl_write_byte(rtlpriv, ROFDM0_ECCATHRESHOLD, l2h);
		rtl_write_byte(rtlpriv, ROFDM0_ECCATHRESHOLD + 2, h2l);
		rtlpriv->rtlhal.pre_edcca_enable = true;
	}
}

#endif
