// SPDX-License-Identifier: GPL-2.0
/* Copyright(c) 2026  Realtek RTL8192FE clean-room driver authors. */
/* RF6052 radio bring-up and per-rate TX-power programming for ... -- dev/MEASURED-rf.c.md sec 1. */

#include "../wifi.h"
#include "reg.h"
#include "def.h"
#include "phy.h"
#include "rf.h"
#include "dm.h"

static bool _rtl92fe_phy_rf6052_config_parafile(struct ieee80211_hw *hw);

/* Program the radio channel-bandwidth bits (RF reg 0x18, ... -- dev/MEASURED-rf.c.md sec 2. */
void rtl92fe_phy_rf6052_set_bandwidth(struct ieee80211_hw *hw, u8 bandwidth)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	struct rtl_phy *rtlphy = &rtlpriv->phy;
	u32 chnlval;

	switch (bandwidth) {
	case HT_CHANNEL_WIDTH_20:
		chnlval = (rtlphy->rfreg_chnlval[0] & 0xfffff3ff) |
			  BIT(10) | BIT(11);
		break;
	case HT_CHANNEL_WIDTH_20_40:
		chnlval = (rtlphy->rfreg_chnlval[0] & 0xfffff3ff) | BIT(10);
		break;
	default:
		pr_err("rtl8192fe: unknown bandwidth: %#X\n", bandwidth);
		return;
	}

	rtlphy->rfreg_chnlval[0] = chnlval;
	rtl_set_rfreg(hw, RF90_PATH_A, RF_CHNLBW, RFREG_OFFSET_MASK, chnlval);
	rtl_set_rfreg(hw, RF90_PATH_B, RF_CHNLBW, RFREG_OFFSET_MASK, chnlval);
}

/* Surface the thermal TX-power-tracking trim that dm.c ... -- dev/MEASURED-rf.c.md sec 3. */
static void _rtl92fe_txpower_track_trim(struct ieee80211_hw *hw,
					  u8 *direction, u32 *value)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	s8 remnant;

	*direction = 0;
	*value = 0;

	if (!rtlpriv->dm.txpower_tracking || !rtlpriv->dm.txpower_track_control)
		return;

	/* remnant_ofdm_swing_idx[] holds the leftover swing (in ...
	 * dev/MEASURED-rf.c.md sec 4. */
	remnant = rtlpriv->dm.remnant_ofdm_swing_idx[RF90_PATH_A];
	if (remnant == 0)
		return;

	if (remnant > 0) {
		*direction = 1;
		*value = (u32)remnant * 0x01010101;
	} else {
		*direction = 2;
		*value = (u32)(-remnant) * 0x01010101;
	}
}

/* Clamp every byte lane of a packed 4-rate AGC word to the 6-bit max. */
static u32 _rtl92fe_clamp_txagc(u32 val)
{
	u8 lane[4];
	int i;

	for (i = 0; i < 4; i++) {
		lane[i] = (u8)((val >> (i * 8)) & 0xff);
		if (lane[i] > RF6052_MAX_TX_PWR)
			lane[i] = RF6052_MAX_TX_PWR;
	}

	return ((u32)lane[3] << 24) | ((u32)lane[2] << 16) |
	       ((u32)lane[1] << 8)  |  (u32)lane[0];
}

/* CCK TX-power: build a per-path word where all four rate ... -- dev/MEASURED-rf.c.md sec 5. */
void rtl92fe_phy_rf6052_set_cck_txpower(struct ieee80211_hw *hw,
					  u8 *ppowerlevel)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	struct rtl_phy *rtlphy = &rtlpriv->phy;
	struct rtl_mac *mac = rtl_mac(rtlpriv);
	struct rtl_efuse *rtlefuse = rtl_efuse(rtlpriv);
	u32 tx_agc[2] = { 0, 0 };
	bool turbo_scanoff = false;
	u8 direction;
	u32 pwrtrac_value;
	u32 tmpval;
	u8 idx;

	if (rtlefuse->eeprom_regulatory != 0)
		turbo_scanoff = true;

	if (mac->act_scanning) {
		tx_agc[RF90_PATH_A] = 0x3f3f3f3f;
		tx_agc[RF90_PATH_B] = 0x3f3f3f3f;

		if (turbo_scanoff) {
			for (idx = RF90_PATH_A; idx <= RF90_PATH_B; idx++)
				tx_agc[idx] = ppowerlevel[idx] * 0x01010101;
		}
	} else {
		for (idx = RF90_PATH_A; idx <= RF90_PATH_B; idx++)
			tx_agc[idx] = ppowerlevel[idx] * 0x01010101;

		/* Regulatory mode 0 (the device's default "better ...
		 * dev/MEASURED-rf.c.md sec 9. */
		if (rtlefuse->eeprom_regulatory == 0) {
			tmpval = rtlphy->mcs_txpwrlevel_origoffset[0][6] +
				 (rtlphy->mcs_txpwrlevel_origoffset[0][7] << 8);
			tx_agc[RF90_PATH_A] += tmpval;

			tmpval = rtlphy->mcs_txpwrlevel_origoffset[0][14] +
				 (rtlphy->mcs_txpwrlevel_origoffset[0][15] << 24);
			tx_agc[RF90_PATH_B] += tmpval;
		}
	}

	for (idx = RF90_PATH_A; idx <= RF90_PATH_B; idx++)
		tx_agc[idx] = _rtl92fe_clamp_txagc(tx_agc[idx]);

	/* Apply the thermal TX-power-tracking trim. On the 8192F the ...
	 * dev/MEASURED-rf.c.md sec 10. */
	_rtl92fe_txpower_track_trim(hw, &direction, &pwrtrac_value);
	if (direction == 1) {
		tx_agc[0] += pwrtrac_value;
		tx_agc[1] += pwrtrac_value;
	} else if (direction == 2) {
		tx_agc[0] -= pwrtrac_value;
		tx_agc[1] -= pwrtrac_value;
	}

	/* Path A 1M lives in byte1 of the CCK1/MCS32 register. */
	tmpval = tx_agc[RF90_PATH_A] & 0xff;
	rtl_set_bbreg(hw, RTXAGC_A_CCK1_MCS32, MASKBYTE1, tmpval);

	/* Path A 2/5.5/11M occupy the top three bytes of the shared reg. */
	tmpval = tx_agc[RF90_PATH_A] >> 8;
	rtl_set_bbreg(hw, RTXAGC_B_CCK11_A_CCK2_11, 0xffffff00, tmpval);

	/* Path B 11M sits in byte0 of the same shared register. */
	tmpval = tx_agc[RF90_PATH_B] >> 24;
	rtl_set_bbreg(hw, RTXAGC_B_CCK11_A_CCK2_11, MASKBYTE0, tmpval);

	/* Path B 1/2/5.5M occupy the top three bytes of CCK1_55/MCS32. */
	tmpval = tx_agc[RF90_PATH_B] & 0x00ffffff;
	rtl_set_bbreg(hw, RTXAGC_B_CCK1_55_MCS32, 0xffffff00, tmpval);
}

/* Replicate the per-path OFDM and HT base indices into a ... -- dev/MEASURED-rf.c.md sec 11. */
static void _rtl92fe_get_power_base(struct ieee80211_hw *hw,
				      u8 *ppowerlevel_ofdm,
				      u8 *ppowerlevel_bw20,
				      u8 *ppowerlevel_bw40,
				      u8 channel, u32 *ofdmbase, u32 *mcsbase)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	struct rtl_phy *rtlphy = &rtlpriv->phy;
	u8 i, level;

	for (i = 0; i < 2; i++)
		ofdmbase[i] = ppowerlevel_ofdm[i] * 0x01010101;

	for (i = 0; i < 2; i++) {
		if (rtlphy->current_chan_bw == HT_CHANNEL_WIDTH_20)
			level = ppowerlevel_bw20[i];
		else
			level = ppowerlevel_bw40[i];

		mcsbase[i] = level * 0x01010101;
	}
}

/* Resolve the final 4-lane write value for an OFDM/HT ... -- dev/MEASURED-rf.c.md sec 6. */
static void _rtl92fe_get_txpower_writeval_by_regulatory(struct ieee80211_hw *hw,
							  u8 channel, u8 index,
							  u32 *powerbase0,
							  u32 *powerbase1,
							  u32 *p_outwriteval)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	struct rtl_phy *rtlphy = &rtlpriv->phy;
	struct rtl_efuse *rtlefuse = rtl_efuse(rtlpriv);
	u8 i, chnlgroup = 0, pwr_diff_limit[4], pwr_diff, customer_pwr_diff;
	u32 writeval, customer_limit, rf;
	u32 base;

	for (rf = 0; rf < 2; rf++) {
		base = (index < 2) ? powerbase0[rf] : powerbase1[rf];

		switch (rtlefuse->eeprom_regulatory) {
		case 1:
			if (rtlphy->pwrgroup_cnt == 1) {
				chnlgroup = 0;
			} else if (channel < 3) {
				chnlgroup = 0;
			} else if (channel < 6) {
				chnlgroup = 1;
			} else if (channel < 9) {
				chnlgroup = 2;
			} else if (channel < 12) {
				chnlgroup = 3;
			} else if (channel < 14) {
				chnlgroup = 4;
			} else {
				chnlgroup = 5;
			}
			writeval = rtlphy->mcs_txpwrlevel_origoffset[chnlgroup]
					[index + (rf ? 8 : 0)] + base;
			break;
		case 2:
			writeval = base;
			break;
		case 3:
			chnlgroup = 0;

			if (index < 2)
				pwr_diff =
				  rtlefuse->txpwr_legacyhtdiff[rf][channel - 1];
			else if (rtlphy->current_chan_bw == HT_CHANNEL_WIDTH_20)
				pwr_diff =
				  rtlefuse->txpwr_ht20diff[rf][channel - 1];
			else
				pwr_diff = 0;

			if (rtlphy->current_chan_bw == HT_CHANNEL_WIDTH_20_40)
				customer_pwr_diff =
				  rtlefuse->pwrgroup_ht40[rf][channel - 1];
			else
				customer_pwr_diff =
				  rtlefuse->pwrgroup_ht20[rf][channel - 1];

			if (pwr_diff > customer_pwr_diff)
				pwr_diff = 0;
			else
				pwr_diff = customer_pwr_diff - pwr_diff;

			for (i = 0; i < 4; i++) {
				pwr_diff_limit[i] = (u8)
				  ((rtlphy->mcs_txpwrlevel_origoffset[chnlgroup]
					[index + (rf ? 8 : 0)] &
				    (0x7f << (i * 8))) >> (i * 8));

				if (pwr_diff_limit[i] > pwr_diff)
					pwr_diff_limit[i] = pwr_diff;
			}

			customer_limit = ((u32)pwr_diff_limit[3] << 24) |
					 ((u32)pwr_diff_limit[2] << 16) |
					 ((u32)pwr_diff_limit[1] << 8)  |
					  (u32)pwr_diff_limit[0];

			writeval = customer_limit + base;
			break;
		case 0:
		default:
			chnlgroup = 0;
			writeval = rtlphy->mcs_txpwrlevel_origoffset[chnlgroup]
					[index + (rf ? 8 : 0)] + base;
			break;
		}

		if (rtlpriv->dm.dynamic_txhighpower_lvl == TXHIGHPWRLEVEL_BT1)
			writeval -= 0x06060606;
		else if (rtlpriv->dm.dynamic_txhighpower_lvl ==
			 TXHIGHPWRLEVEL_BT2)
			writeval -= 0x0c0c0c0c;

		p_outwriteval[rf] = writeval;
	}
}

/* Commit one OFDM/HT register index for both RF paths. The ... -- dev/MEASURED-rf.c.md sec 7. */
static void _rtl92fe_write_ofdm_power_reg(struct ieee80211_hw *hw,
					    u8 index, u32 *pvalue)
{
	static const u16 regoffset_a[6] = {
		RTXAGC_A_RATE18_06, RTXAGC_A_RATE54_24,
		RTXAGC_A_MCS03_MCS00, RTXAGC_A_MCS07_MCS04,
		RTXAGC_A_MCS11_MCS08, RTXAGC_A_MCS15_MCS12,
	};
	static const u16 regoffset_b[6] = {
		RTXAGC_B_RATE18_06, RTXAGC_B_RATE54_24,
		RTXAGC_B_MCS03_MCS00, RTXAGC_B_MCS07_MCS04,
		RTXAGC_B_MCS11_MCS08, RTXAGC_B_MCS15_MCS12,
	};
	u8 i, rf, pwr_val[4];
	u32 writeval;
	u16 regoffset;

	for (rf = 0; rf < 2; rf++) {
		writeval = pvalue[rf];

		for (i = 0; i < 4; i++) {
			pwr_val[i] = (u8)((writeval >> (i * 8)) & 0x7f);
			if (pwr_val[i] > RF6052_MAX_TX_PWR)
				pwr_val[i] = RF6052_MAX_TX_PWR;
		}

		writeval = ((u32)pwr_val[3] << 24) | ((u32)pwr_val[2] << 16) |
			   ((u32)pwr_val[1] << 8)  |  (u32)pwr_val[0];

		regoffset = (rf == 0) ? regoffset_a[index] : regoffset_b[index];
		rtl_set_bbreg(hw, regoffset, MASKDWORD, writeval);
	}
}

/* OFDM + HT TX-power: derive the per-path base words, then ... -- dev/MEASURED-rf.c.md sec 12. */
void rtl92fe_phy_rf6052_set_ofdm_txpower(struct ieee80211_hw *hw,
					   u8 *ppowerlevel_ofdm,
					   u8 *ppowerlevel_bw20,
					   u8 *ppowerlevel_bw40, u8 channel)
{
	u32 writeval[2], powerbase0[2], powerbase1[2];
	u8 index;
	u8 direction;
	u32 pwrtrac_value;

	_rtl92fe_get_power_base(hw, ppowerlevel_ofdm, ppowerlevel_bw20,
				  ppowerlevel_bw40, channel,
				  &powerbase0[0], &powerbase1[0]);

	_rtl92fe_txpower_track_trim(hw, &direction, &pwrtrac_value);

	for (index = 0; index < 6; index++) {
		_rtl92fe_get_txpower_writeval_by_regulatory(hw, channel, index,
							      &powerbase0[0],
							      &powerbase1[0],
							      &writeval[0]);
		if (direction == 1) {
			writeval[0] += pwrtrac_value;
			writeval[1] += pwrtrac_value;
		} else if (direction == 2) {
			writeval[0] -= pwrtrac_value;
			writeval[1] -= pwrtrac_value;
		}

		_rtl92fe_write_ofdm_power_reg(hw, index, &writeval[0]);
	}
}

/* Load the RadioA/RadioB register tables. The 8192F is 2T2R, ... -- dev/MEASURED-rf.c.md sec 13. */
bool rtl92fe_phy_rf6052_config(struct ieee80211_hw *hw)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	struct rtl_phy *rtlphy = &rtlpriv->phy;

	if (rtlphy->rf_type == RF_1T1R)
		rtlphy->num_total_rfpath = 1;
	else
		rtlphy->num_total_rfpath = 2;

	return _rtl92fe_phy_rf6052_config_parafile(hw);
}

/* Per-path RF6052 table apply. For each path the 3-wire RFENV ... -- dev/MEASURED-rf.c.md sec 8. */
static bool _rtl92fe_phy_rf6052_config_parafile(struct ieee80211_hw *hw)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	struct rtl_phy *rtlphy = &rtlpriv->phy;
	struct bb_reg_def *pphyreg;
	u32 u4_regvalue = 0;
	bool rtstatus = true;
	u8 rfpath;

	for (rfpath = 0; rfpath < rtlphy->num_total_rfpath; rfpath++) {
		pphyreg = &rtlphy->phyreg_def[rfpath];

		switch (rfpath) {
		case RF90_PATH_A:
			u4_regvalue = rtl_get_bbreg(hw, pphyreg->rfintfs,
						    BRFSI_RFENV);
			break;
		case RF90_PATH_B:
			u4_regvalue = rtl_get_bbreg(hw, pphyreg->rfintfs,
						    BRFSI_RFENV << 16);
			break;
		}

		rtl_set_bbreg(hw, pphyreg->rfintfe, BRFSI_RFENV << 16, 0x1);
		udelay(1);

		rtl_set_bbreg(hw, pphyreg->rfintfo, BRFSI_RFENV, 0x1);
		udelay(1);

		rtl_set_bbreg(hw, pphyreg->rfhssi_para2,
			      B3WIREADDREAALENGTH, 0x0);
		udelay(1);

		rtl_set_bbreg(hw, pphyreg->rfhssi_para2, B3WIREDATALENGTH, 0x0);
		udelay(1);

		switch (rfpath) {
		case RF90_PATH_A:
		case RF90_PATH_B:
			rtstatus = rtl92fe_phy_config_rf_with_headerfile(hw,
						(enum radio_path)rfpath);
			break;
		}

		switch (rfpath) {
		case RF90_PATH_A:
			rtl_set_bbreg(hw, pphyreg->rfintfs, BRFSI_RFENV,
				      u4_regvalue);
			break;
		case RF90_PATH_B:
			rtl_set_bbreg(hw, pphyreg->rfintfs, BRFSI_RFENV << 16,
				      u4_regvalue);
			break;
		}

		if (!rtstatus) {
			rtl_dbg(rtlpriv, COMP_INIT, DBG_TRACE,
				"Radio[%d] Fail!!\n", rfpath);
			return false;
		}
	}

	rtl_dbg(rtlpriv, COMP_INIT, DBG_TRACE, "\n");
	return rtstatus;
}
