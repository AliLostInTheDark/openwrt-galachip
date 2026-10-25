// SPDX-License-Identifier: GPL-2.0
/* Copyright(c) 2026  Realtek RTL8192FE clean-room PCIe WiFi driver authors */

#include "../wifi.h"
#include "../pci.h"
#include "../ps.h"
#include "reg.h"
#include "def.h"
#include "phy.h"
#include "rf.h"
#include "dm.h"
#include "table.h"

/* RTL8192F-specific BB anchors used by the IQK/LCK flows. ... -- dev/MEASURED-phy.c.md sec 1. */
#define R8192F_FPGA0_ANALOG4		0x088c
#define R8192F_ANAPWR1			0x0d94
#define R8192F_RX_WAIT_CCA		0x0e70
#define R8192F_FPGA0_XCD_RF_SW_CTRL	0x0874
#define R8192F_FPGA0_XA_HSSI_PARM1	0x0820
#define R8192F_FPGA0_XCD_RF_PARM	0x087c
#define R8192F_IQK_RPT_TXA		0x0e98
#define R8192F_IQK_RPT_RXA		0x0ea8
#define R8192F_IQK_RPT_TXB		0x0eb8
#define R8192F_IQK_RPT_RXB		0x0ec8
#define R8192F_NP_ANTA			0x0e20
#define R8192F_TAP_UPD_97F		0x0e24
/* The 8192F's 40 MHz words beyond the RF-mode bit: the MAC's data
 * sub-channel select and the BB's ADC-buffer clock, down-sampling factor
 * and RX DFIR mode.  Named as mainline rtl8xxxu (the same die on USB) names
 * them in rtl8192fu_config_channel(); the values are the ones it writes.
 */
#define R8192F_DATA_SUBCHANNEL		0x0483
#define R8192F_ANTDIV_PARA1		0x0ca4
#define R8192F_DOWNSAM_FACTOR		0x0c10
#define R8192F_RX_DFIR_MOD_97F		0x0948

/* CCK TX power-shaping filter (PSF) registers revised per channel. */
#define R8192F_CCK0_TX_FILTER1		0x0a20
#define R8192F_CCK0_TX_FILTER2		0x0a24
#define R8192F_CCK0_DEBUG_PORT		0x0a28
#define R8192F_CCK0_TX_FILTER3		0x0aac

/* RF-serial registers reused from the 8192F radio map.  The PA/PAD-by-0x56
 * gate, the WE_LUT write-enable, the TXPA-G LUT port (0x33) and the gain/CCA
 * register (0xdf) drive the LOK->TX-PA-LUT feedback specific to the 8192F.
 */
#define RF8192F_GAIN_CCA		0xdf
#define RF8192F_PAD_TXG			0x56
#define RF8192F_TXMOD			0x58
#define RF8192F_GAIN_P1			0x35
#define RF8192F_WE_LUT			0xef
#define RF8192F_TXPA_G3			0x33
#define RF8192F_AC			0x00

static u32 _rtl92fe_phy_rf_serial_read(struct ieee80211_hw *hw,
					 enum radio_path rfpath, u32 offset);
static void _rtl92fe_phy_rf_serial_write(struct ieee80211_hw *hw,
					   enum radio_path rfpath, u32 offset,
					   u32 data);
static bool _rtl92fe_phy_bb_config_parafile(struct ieee80211_hw *hw);
static bool _rtl92fe_phy_config_mac_with_headerfile(struct ieee80211_hw *hw);
static bool phy_config_bb_with_hdr_file(struct ieee80211_hw *hw,
					u8 configtype);
static bool phy_config_bb_with_pghdrfile(struct ieee80211_hw *hw,
					 u8 configtype);
static void phy_init_bb_rf_register_def(struct ieee80211_hw *hw);
static bool _rtl92fe_phy_set_sw_chnl_cmdarray(struct swchnlcmd *cmdtable,
						u32 cmdtableidx, u32 cmdtablesz,
						enum swchnlcmd_id cmdid,
						u32 para1, u32 para2,
						u32 msdelay);
static bool _rtl92fe_phy_sw_chnl_step_by_step(struct ieee80211_hw *hw,
						u8 channel, u8 *stage,
						u8 *step, u32 *delay);
static long _rtl92fe_phy_txpwr_idx_to_dbm(struct ieee80211_hw *hw,
					    enum wireless_mode wirelessmode,
					    u8 txpwridx);
static void rtl92fe_phy_set_rf_on(struct ieee80211_hw *hw);
static void rtl92fe_phy_set_io(struct ieee80211_hw *hw);
static void _rtl92fe_phy_revise_cck_tx_psf(struct ieee80211_hw *hw,
					     u8 channel);

u32 rtl92fe_phy_query_bb_reg(struct ieee80211_hw *hw, u32 regaddr,
			       u32 bitmask)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	u32 returnvalue, originalvalue, bitshift;

	originalvalue = rtl_read_dword(rtlpriv, regaddr);
	bitshift = calculate_bit_shift(bitmask);
	returnvalue = (originalvalue & bitmask) >> bitshift;

	rtl_dbg(rtlpriv, COMP_RF, DBG_TRACE,
		"BBR MASK=0x%x Addr[0x%x]=0x%x\n",
		bitmask, regaddr, originalvalue);

	return returnvalue;
}

void rtl92fe_phy_set_bb_reg(struct ieee80211_hw *hw, u32 regaddr,
			      u32 bitmask, u32 data)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	u32 originalvalue, bitshift;

	if (bitmask != MASKDWORD) {
		originalvalue = rtl_read_dword(rtlpriv, regaddr);
		bitshift = calculate_bit_shift(bitmask);
		data = ((originalvalue & (~bitmask)) | (data << bitshift));
	}

	rtl_write_dword(rtlpriv, regaddr, data);

	rtl_dbg(rtlpriv, COMP_RF, DBG_TRACE,
		"regaddr(%#x), bitmask(%#x), data(%#x)\n",
		regaddr, bitmask, data);
}

u32 rtl92fe_phy_query_rf_reg(struct ieee80211_hw *hw,
			       enum radio_path rfpath, u32 regaddr,
			       u32 bitmask)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	u32 original_value, readback_value, bitshift;

	spin_lock(&rtlpriv->locks.rf_lock);

	original_value = _rtl92fe_phy_rf_serial_read(hw, rfpath, regaddr);
	bitshift = calculate_bit_shift(bitmask);
	readback_value = (original_value & bitmask) >> bitshift;

	spin_unlock(&rtlpriv->locks.rf_lock);

	rtl_dbg(rtlpriv, COMP_RF, DBG_TRACE,
		"regaddr(%#x),rfpath(%#x),bitmask(%#x),original_value(%#x)\n",
		regaddr, rfpath, bitmask, original_value);

	return readback_value;
}

void rtl92fe_phy_set_rf_reg(struct ieee80211_hw *hw,
			      enum radio_path rfpath,
			      u32 addr, u32 bitmask, u32 data)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	u32 original_value, bitshift;

	spin_lock(&rtlpriv->locks.rf_lock);

	if (bitmask != RFREG_OFFSET_MASK) {
		original_value = _rtl92fe_phy_rf_serial_read(hw, rfpath, addr);
		bitshift = calculate_bit_shift(bitmask);
		data = (original_value & (~bitmask)) | (data << bitshift);
	}

	_rtl92fe_phy_rf_serial_write(hw, rfpath, addr, data);

	spin_unlock(&rtlpriv->locks.rf_lock);

	rtl_dbg(rtlpriv, COMP_RF, DBG_TRACE,
		"regaddr(%#x), bitmask(%#x), data(%#x), rfpath(%#x)\n",
		addr, bitmask, data, rfpath);
}

static u32 _rtl92fe_phy_rf_serial_read(struct ieee80211_hw *hw,
					 enum radio_path rfpath, u32 offset)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	struct rtl_phy *rtlphy = &rtlpriv->phy;
	struct bb_reg_def *pphyreg = &rtlphy->phyreg_def[rfpath];
	u32 newoffset;
	u32 tmplong, tmplong2;
	u8 rfpi_enable = 0;
	u32 retvalue;

	offset &= 0xff;
	newoffset = offset;
	if (RT_CANNOT_IO(hw)) {
		pr_err("return all one\n");
		return 0xFFFFFFFF;
	}
	tmplong2 = rtl_get_bbreg(hw, pphyreg->rfhssi_para2, MASKDWORD);
	tmplong2 &= ~(BLSSIREADADDRESS | BLSSIREADEDGE);
	tmplong2 |= newoffset << 23;
	rtl_set_bbreg(hw, pphyreg->rfhssi_para2, MASKDWORD, tmplong2);
	tmplong = rtl_get_bbreg(hw, RFPGA0_XA_HSSIPARAMETER2, MASKDWORD);
	rtl_set_bbreg(hw, RFPGA0_XA_HSSIPARAMETER2, MASKDWORD,
		      tmplong & (~BLSSIREADEDGE));
	rtl_set_bbreg(hw, RFPGA0_XA_HSSIPARAMETER2, MASKDWORD,
		      tmplong | BLSSIREADEDGE);
	udelay(20);
	if (rfpath == RF90_PATH_A)
		rfpi_enable = (u8)rtl_get_bbreg(hw, RFPGA0_XA_HSSIPARAMETER1,
						BIT(8));
	else if (rfpath == RF90_PATH_B)
		rfpi_enable = (u8)rtl_get_bbreg(hw, RFPGA0_XB_HSSIPARAMETER1,
						BIT(8));
	if (rfpi_enable)
		retvalue = rtl_get_bbreg(hw, pphyreg->rf_rbpi,
					 BLSSIREADBACKDATA);
	else
		retvalue = rtl_get_bbreg(hw, pphyreg->rf_rb,
					 BLSSIREADBACKDATA);
	rtl_dbg(rtlpriv, COMP_RF, DBG_TRACE,
		"RFR-%d Addr[0x%x]=0x%x\n",
		rfpath, pphyreg->rf_rb, retvalue);
	return retvalue;
}

static void _rtl92fe_phy_rf_serial_write(struct ieee80211_hw *hw,
					   enum radio_path rfpath, u32 offset,
					   u32 data)
{
	u32 data_and_addr;
	u32 newoffset;
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	struct rtl_phy *rtlphy = &rtlpriv->phy;
	struct bb_reg_def *pphyreg = &rtlphy->phyreg_def[rfpath];

	if (RT_CANNOT_IO(hw)) {
		pr_err("stop\n");
		return;
	}
	offset &= 0xff;
	newoffset = offset;
	data_and_addr = ((newoffset << 20) | (data & 0x000fffff)) & 0x0fffffff;
	rtl_set_bbreg(hw, pphyreg->rf3wire_offset, MASKDWORD, data_and_addr);
	rtl_dbg(rtlpriv, COMP_RF, DBG_TRACE,
		"RFW-%d Addr[0x%x]=0x%x\n", rfpath,
		pphyreg->rf3wire_offset, data_and_addr);
}

bool rtl92fe_phy_mac_config(struct ieee80211_hw *hw)
{
	return _rtl92fe_phy_config_mac_with_headerfile(hw);
}

bool rtl92fe_phy_bb_config(struct ieee80211_hw *hw)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	bool rtstatus;
	u16 regval;
	u8 crystal_cap;

	phy_init_bb_rf_register_def(hw);

	/* Enable BB and RF clock domains. */
	regval = rtl_read_word(rtlpriv, REG_SYS_FUNC_EN);
	rtl_write_word(rtlpriv, REG_SYS_FUNC_EN, regval | BIT(0) | BIT(1));

	rtl_write_byte(rtlpriv, REG_RF_CTRL, RF_EN | RF_RSTB | RF_SDMRSTB);
	/* Release RF path S0 (MAC reg 0x7B, distinct from radio RF_OPTION3 0x7B):
	 * vendor pairs S1@0x1F with S0@0x7B (8192cd_hw.c:18163-18164). Without
	 * this the second RF path stays in reset and on-air TX is dark.
	 */
	rtl_write_byte(rtlpriv, 0x7B, 0x00);
	rtl_write_byte(rtlpriv, 0x7B, RF_EN | RF_RSTB | RF_SDMRSTB);

	/* 8192F MAC-loopback workaround: LDOHCI12 + SWR_CTRL2 nudge before
	 * the BB tables are pushed.
	 */
	rtl_write_byte(rtlpriv, REG_LDOHCI12_CTRL, 0x0f);
	rtl_write_byte(rtlpriv, REG_SYS_SWR_CTRL2 + 1, 0xe9);

	rtstatus = _rtl92fe_phy_bb_config_parafile(hw);

	crystal_cap = rtlpriv->efuse.eeprom_crystalcap & 0x3F;
	rtl_set_bbreg(hw, REG_MAC_PHY_CTRL, 0x7FF80000,
		      (crystal_cap | (crystal_cap << 6)));

	return rtstatus;
}

bool rtl92fe_phy_rf_config(struct ieee80211_hw *hw)
{
	return rtl92fe_phy_rf6052_config(hw);
}

static bool _check_condition(struct ieee80211_hw *hw, const u32 condition)
{
	struct rtl_hal *rtlhal = rtl_hal(rtl_priv(hw));
	struct rtl_efuse *rtlefuse = rtl_efuse(rtl_priv(hw));
	u32 _board = rtlefuse->board_type;
	u32 _interface = rtlhal->interface;
	u32 _platform = 0x08; /* SupportPlatform */
	u32 cond = condition;

	if (condition == 0xCDCDCDCD)
		return true;

	cond = condition & 0xFF;
	if ((_board != cond) && (cond != 0xFF))
		return false;

	cond = condition & 0xFF00;
	cond = cond >> 8;
	if ((_interface & cond) == 0 && cond != 0x07)
		return false;

	cond = condition & 0xFF0000;
	cond = cond >> 16;
	if ((_platform & cond) == 0 && cond != 0x0F)
		return false;

	return true;
}

static void _rtl92fe_config_rf_reg(struct ieee80211_hw *hw, u32 addr,
				     u32 data, enum radio_path rfpath,
				     u32 regaddr)
{
	if (addr == 0xfe || addr == 0xffe) {
		mdelay(50);
	} else {
		rtl_set_rfreg(hw, rfpath, regaddr, RFREG_OFFSET_MASK, data);
		udelay(1);
	}
}

static void _rtl92fe_config_rf_radio_a(struct ieee80211_hw *hw,
					 u32 addr, u32 data)
{
	u32 content = 0x1000; /* RF Content: radio_a */
	u32 maskforphyset = (u32)(content & 0xE000);

	_rtl92fe_config_rf_reg(hw, addr, data, RF90_PATH_A,
				 addr | maskforphyset);
}

static void _rtl92fe_config_rf_radio_b(struct ieee80211_hw *hw,
					 u32 addr, u32 data)
{
	u32 content = 0x1001; /* RF Content: radio_b */
	u32 maskforphyset = (u32)(content & 0xE000);

	_rtl92fe_config_rf_reg(hw, addr, data, RF90_PATH_B,
				 addr | maskforphyset);
}

static void _rtl92fe_config_bb_reg(struct ieee80211_hw *hw,
				     u32 addr, u32 data)
{
	if (addr == 0xfe)
		mdelay(50);
	else if (addr == 0xfd)
		mdelay(5);
	else if (addr == 0xfc)
		mdelay(1);
	else if (addr == 0xfb)
		udelay(50);
	else if (addr == 0xfa)
		udelay(5);
	else if (addr == 0xf9)
		udelay(1);
	else
		rtl_set_bbreg(hw, addr, MASKDWORD, data);

	udelay(1);
}

static void _rtl92fe_phy_init_tx_power_by_rate(struct ieee80211_hw *hw)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	struct rtl_phy *rtlphy = &rtlpriv->phy;
	u8 band, rf, txnum, sec;

	for (band = BAND_ON_2_4G; band <= BAND_ON_5G; ++band)
		for (rf = 0; rf < TX_PWR_BY_RATE_NUM_RF; ++rf)
			for (txnum = 0; txnum < TX_PWR_BY_RATE_NUM_RF; ++txnum)
				for (sec = 0;
				     sec < TX_PWR_BY_RATE_NUM_SECTION; ++sec)
					rtlphy->tx_power_by_rate_offset
					     [band][rf][txnum][sec] = 0;
}

static void _rtl92fe_phy_set_txpower_by_rate_base(struct ieee80211_hw *hw,
						    u8 band, u8 path,
						    u8 rate_section, u8 txnum,
						    u8 value)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	struct rtl_phy *rtlphy = &rtlpriv->phy;

	if (path > RF90_PATH_D) {
		rtl_dbg(rtlpriv, COMP_INIT, DBG_LOUD,
			"Invalid Rf Path %d\n", path);
		return;
	}

	if (band == BAND_ON_2_4G) {
		switch (rate_section) {
		case CCK:
			rtlphy->txpwr_by_rate_base_24g[path][txnum][0] = value;
			break;
		case OFDM:
			rtlphy->txpwr_by_rate_base_24g[path][txnum][1] = value;
			break;
		case HT_MCS0_MCS7:
			rtlphy->txpwr_by_rate_base_24g[path][txnum][2] = value;
			break;
		case HT_MCS8_MCS15:
			rtlphy->txpwr_by_rate_base_24g[path][txnum][3] = value;
			break;
		default:
			rtl_dbg(rtlpriv, COMP_INIT, DBG_LOUD,
				"Invalid RateSection %d in 2.4G,Rf %d,%dTx\n",
				rate_section, path, txnum);
			break;
		}
	} else {
		rtl_dbg(rtlpriv, COMP_INIT, DBG_LOUD,
			"Invalid Band %d\n", band);
	}
}

static u8 _rtl92fe_phy_get_txpower_by_rate_base(struct ieee80211_hw *hw,
						  u8 band, u8 path, u8 txnum,
						  u8 rate_section)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	struct rtl_phy *rtlphy = &rtlpriv->phy;
	u8 value = 0;

	if (path > RF90_PATH_D) {
		rtl_dbg(rtlpriv, COMP_INIT, DBG_LOUD,
			"Invalid Rf Path %d\n", path);
		return 0;
	}

	if (band == BAND_ON_2_4G) {
		switch (rate_section) {
		case CCK:
			value = rtlphy->txpwr_by_rate_base_24g[path][txnum][0];
			break;
		case OFDM:
			value = rtlphy->txpwr_by_rate_base_24g[path][txnum][1];
			break;
		case HT_MCS0_MCS7:
			value = rtlphy->txpwr_by_rate_base_24g[path][txnum][2];
			break;
		case HT_MCS8_MCS15:
			value = rtlphy->txpwr_by_rate_base_24g[path][txnum][3];
			break;
		default:
			rtl_dbg(rtlpriv, COMP_INIT, DBG_LOUD,
				"Invalid RateSection %d in 2.4G,Rf %d,%dTx\n",
				rate_section, path, txnum);
			break;
		}
	} else {
		rtl_dbg(rtlpriv, COMP_INIT, DBG_LOUD,
			"Invalid Band %d()\n", band);
	}
	return value;
}

static void _rtl92fe_phy_store_txpower_by_rate_base(struct ieee80211_hw *hw)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	struct rtl_phy *rtlphy = &rtlpriv->phy;
	u16 raw = 0;
	u8 base = 0, path = 0;

	for (path = RF90_PATH_A; path <= RF90_PATH_B; ++path) {
		if (path == RF90_PATH_A) {
			raw = (u16)(rtlphy->tx_power_by_rate_offset
				    [BAND_ON_2_4G][path][RF_1TX][3] >> 24) &
				    0xFF;
			base = (raw >> 4) * 10 + (raw & 0xF);
			_rtl92fe_phy_set_txpower_by_rate_base(hw, BAND_ON_2_4G,
								path, CCK,
								RF_1TX, base);
		} else if (path == RF90_PATH_B) {
			raw = (u16)(rtlphy->tx_power_by_rate_offset
				    [BAND_ON_2_4G][path][RF_1TX][3] >> 0) &
				    0xFF;
			base = (raw >> 4) * 10 + (raw & 0xF);
			_rtl92fe_phy_set_txpower_by_rate_base(hw, BAND_ON_2_4G,
								path, CCK,
								RF_1TX, base);
		}
		raw = (u16)(rtlphy->tx_power_by_rate_offset
			    [BAND_ON_2_4G][path][RF_1TX][1] >> 24) & 0xFF;
		base = (raw >> 4) * 10 + (raw & 0xF);
		_rtl92fe_phy_set_txpower_by_rate_base(hw, BAND_ON_2_4G, path,
							OFDM, RF_1TX, base);

		raw = (u16)(rtlphy->tx_power_by_rate_offset
			    [BAND_ON_2_4G][path][RF_1TX][5] >> 24) & 0xFF;
		base = (raw >> 4) * 10 + (raw & 0xF);
		_rtl92fe_phy_set_txpower_by_rate_base(hw, BAND_ON_2_4G, path,
							HT_MCS0_MCS7, RF_1TX,
							base);

		raw = (u16)(rtlphy->tx_power_by_rate_offset
			    [BAND_ON_2_4G][path][RF_2TX][7] >> 24) & 0xFF;
		base = (raw >> 4) * 10 + (raw & 0xF);
		_rtl92fe_phy_set_txpower_by_rate_base(hw, BAND_ON_2_4G, path,
							HT_MCS8_MCS15, RF_2TX,
							base);
	}
}

static void _phy_convert_txpower_dbm_to_relative_value(u32 *data, u8 start,
						       u8 end, u8 base)
{
	s8 i = 0;
	u8 tmp = 0;
	u32 temp_data = 0;

	for (i = 3; i >= 0; --i) {
		if (i >= start && i <= end) {
			tmp = (u8)(*data >> (i * 8)) & 0xF;
			tmp += ((u8)((*data >> (i * 8 + 4)) & 0xF)) * 10;

			tmp = (tmp > base) ? tmp - base : base - tmp;
		} else {
			tmp = (u8)(*data >> (i * 8)) & 0xFF;
		}
		temp_data <<= 8;
		temp_data |= tmp;
	}
	*data = temp_data;
}

static void phy_convert_txpwr_dbm_to_rel_val(struct ieee80211_hw *hw)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	struct rtl_phy *rtlphy = &rtlpriv->phy;
	u8 base = 0, rf = 0, band = BAND_ON_2_4G;

	for (rf = RF90_PATH_A; rf <= RF90_PATH_B; ++rf) {
		if (rf == RF90_PATH_A) {
			base = _rtl92fe_phy_get_txpower_by_rate_base(hw, band,
								       rf,
								       RF_1TX,
								       CCK);
			_phy_convert_txpower_dbm_to_relative_value(
				&rtlphy->tx_power_by_rate_offset
				[band][rf][RF_1TX][2],
				1, 1, base);
			_phy_convert_txpower_dbm_to_relative_value(
				&rtlphy->tx_power_by_rate_offset
				[band][rf][RF_1TX][3],
				1, 3, base);
		} else if (rf == RF90_PATH_B) {
			base = _rtl92fe_phy_get_txpower_by_rate_base(hw, band,
								       rf,
								       RF_1TX,
								       CCK);
			_phy_convert_txpower_dbm_to_relative_value(
				&rtlphy->tx_power_by_rate_offset
				[band][rf][RF_1TX][3],
				0, 0, base);
			_phy_convert_txpower_dbm_to_relative_value(
				&rtlphy->tx_power_by_rate_offset
				[band][rf][RF_1TX][2],
				1, 3, base);
		}
		base = _rtl92fe_phy_get_txpower_by_rate_base(hw, band, rf,
							       RF_1TX, OFDM);
		_phy_convert_txpower_dbm_to_relative_value(
			&rtlphy->tx_power_by_rate_offset[band][rf][RF_1TX][0],
			0, 3, base);
		_phy_convert_txpower_dbm_to_relative_value(
			&rtlphy->tx_power_by_rate_offset[band][rf][RF_1TX][1],
			0, 3, base);

		base = _rtl92fe_phy_get_txpower_by_rate_base(hw, band, rf,
							       RF_1TX,
							       HT_MCS0_MCS7);
		_phy_convert_txpower_dbm_to_relative_value(
			&rtlphy->tx_power_by_rate_offset[band][rf][RF_1TX][4],
			0, 3, base);
		_phy_convert_txpower_dbm_to_relative_value(
			&rtlphy->tx_power_by_rate_offset[band][rf][RF_1TX][5],
			0, 3, base);

		base = _rtl92fe_phy_get_txpower_by_rate_base(hw, band, rf,
							       RF_2TX,
							       HT_MCS8_MCS15);
		_phy_convert_txpower_dbm_to_relative_value(
			&rtlphy->tx_power_by_rate_offset[band][rf][RF_2TX][6],
			0, 3, base);

		_phy_convert_txpower_dbm_to_relative_value(
			&rtlphy->tx_power_by_rate_offset[band][rf][RF_2TX][7],
			0, 3, base);
	}

	rtl_dbg(rtlpriv, COMP_POWER, DBG_TRACE, "<==%s\n", __func__);
}

static void _rtl92fe_phy_txpower_by_rate_configuration(struct ieee80211_hw *hw)
{
	_rtl92fe_phy_store_txpower_by_rate_base(hw);
	phy_convert_txpwr_dbm_to_rel_val(hw);
}

static bool _rtl92fe_phy_bb_config_parafile(struct ieee80211_hw *hw)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	struct rtl_phy *rtlphy = &rtlpriv->phy;
	struct rtl_efuse *rtlefuse = rtl_efuse(rtl_priv(hw));
	bool rtstatus;

	rtstatus = phy_config_bb_with_hdr_file(hw, BASEBAND_CONFIG_PHY_REG);
	if (!rtstatus) {
		pr_err("Write BB Reg Fail!!\n");
		return false;
	}

	_rtl92fe_phy_init_tx_power_by_rate(hw);
	if (!rtlefuse->autoload_failflag) {
		rtlphy->pwrgroup_cnt = 0;
		rtstatus =
		  phy_config_bb_with_pghdrfile(hw, BASEBAND_CONFIG_PHY_REG);
	}
	_rtl92fe_phy_txpower_by_rate_configuration(hw);
	if (!rtstatus) {
		pr_err("BB_PG Reg Fail!!\n");
		return false;
	}
	rtstatus = phy_config_bb_with_hdr_file(hw, BASEBAND_CONFIG_AGC_TAB);
	if (!rtstatus) {
		pr_err("AGC Table Fail\n");
		return false;
	}
	rtlphy->cck_high_power = (bool)(rtl_get_bbreg(hw,
						      RFPGA0_XA_HSSIPARAMETER2,
						      0x200));

	return true;
}

static bool _rtl92fe_phy_config_mac_with_headerfile(struct ieee80211_hw *hw)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	u32 i;
	u32 arraylength;
	u32 *ptrarray;

	arraylength = RTL8192FE_MAC_ARRAY_LEN;
	ptrarray = RTL8192FE_MAC_ARRAY;
	rtl_dbg(rtlpriv, COMP_INIT, DBG_LOUD,
		"Img:RTL8192FE_MAC_ARRAY LEN %d\n", arraylength);
	for (i = 0; i < arraylength; i = i + 2)
		rtl_write_byte(rtlpriv, ptrarray[i], (u8)ptrarray[i + 1]);
	return true;
}

#define READ_NEXT_PAIR(v1, v2, i) \
	do { \
		i += 2; \
		v1 = array[i]; \
		v2 = array[i + 1]; \
	} while (0)

static bool phy_config_bb_with_hdr_file(struct ieee80211_hw *hw,
					u8 configtype)
{
	int i;
	u32 *array;
	u16 len;
	u32 v1 = 0, v2 = 0;

	if (configtype == BASEBAND_CONFIG_PHY_REG) {
		len = RTL8192FE_PHY_REG_ARRAY_LEN;
		array = RTL8192FE_PHY_REG_ARRAY;

		for (i = 0; i < len; i = i + 2) {
			v1 = array[i];
			v2 = array[i + 1];
			if (v1 < 0xcdcdcdcd) {
				_rtl92fe_config_bb_reg(hw, v1, v2);
			} else {
				if (i >= len - 2)
					break;

				if (!_check_condition(hw, array[i])) {
					READ_NEXT_PAIR(v1, v2, i);
					while (v2 != 0xDEAD &&
					       v2 != 0xCDEF &&
					       v2 != 0xCDCD && i < len - 2) {
						READ_NEXT_PAIR(v1, v2, i);
					}
					i -= 2;
				} else {
					READ_NEXT_PAIR(v1, v2, i);
					while (v2 != 0xDEAD &&
					       v2 != 0xCDEF &&
					       v2 != 0xCDCD && i < len - 2) {
						_rtl92fe_config_bb_reg(hw, v1,
									 v2);
						READ_NEXT_PAIR(v1, v2, i);
					}

					while (v2 != 0xDEAD && i < len - 2)
						READ_NEXT_PAIR(v1, v2, i);
				}
			}
		}
	} else if (configtype == BASEBAND_CONFIG_AGC_TAB) {
		len = RTL8192FE_AGC_TAB_ARRAY_LEN;
		array = RTL8192FE_AGC_TAB_ARRAY;

		for (i = 0; i < len; i = i + 2) {
			v1 = array[i];
			v2 = array[i + 1];
			if (v1 < 0xCDCDCDCD) {
				rtl_set_bbreg(hw, array[i], MASKDWORD,
					      array[i + 1]);
				udelay(1);
				continue;
			} else {
				if (i >= len - 2)
					break;

				if (!_check_condition(hw, array[i])) {
					READ_NEXT_PAIR(v1, v2, i);
					while (v2 != 0xDEAD &&
					       v2 != 0xCDEF &&
					       v2 != 0xCDCD &&
					       i < len - 2) {
						READ_NEXT_PAIR(v1, v2, i);
					}
					i -= 2;
				} else {
					READ_NEXT_PAIR(v1, v2, i);
					while (v2 != 0xDEAD &&
					       v2 != 0xCDEF &&
					       v2 != 0xCDCD &&
					       i < len - 2) {
						rtl_set_bbreg(hw, array[i],
							      MASKDWORD,
							      array[i + 1]);
						udelay(1);
						READ_NEXT_PAIR(v1, v2, i);
					}

					while (v2 != 0xDEAD && i < len - 2)
						READ_NEXT_PAIR(v1, v2, i);
				}
			}
		}
	}
	return true;
}

static u8 _rtl92fe_get_rate_section_index(u32 regaddr)
{
	u8 index = 0;

	switch (regaddr) {
	case RTXAGC_A_RATE18_06:
	case RTXAGC_B_RATE18_06:
		index = 0;
		break;
	case RTXAGC_A_RATE54_24:
	case RTXAGC_B_RATE54_24:
		index = 1;
		break;
	case RTXAGC_A_CCK1_MCS32:
	case RTXAGC_B_CCK1_55_MCS32:
		index = 2;
		break;
	case RTXAGC_B_CCK11_A_CCK2_11:
		index = 3;
		break;
	case RTXAGC_A_MCS03_MCS00:
	case RTXAGC_B_MCS03_MCS00:
		index = 4;
		break;
	case RTXAGC_A_MCS07_MCS04:
	case RTXAGC_B_MCS07_MCS04:
		index = 5;
		break;
	case RTXAGC_A_MCS11_MCS08:
	case RTXAGC_B_MCS11_MCS08:
		index = 6;
		break;
	case RTXAGC_A_MCS15_MCS12:
	case RTXAGC_B_MCS15_MCS12:
		index = 7;
		break;
	default:
		regaddr &= 0xFFF;
		if (regaddr >= 0xC20 && regaddr <= 0xC4C)
			index = (u8)((regaddr - 0xC20) / 4);
		else if (regaddr >= 0xE20 && regaddr <= 0xE4C)
			index = (u8)((regaddr - 0xE20) / 4);
		break;
	}
	return index;
}

static void _rtl92fe_store_tx_power_by_rate(struct ieee80211_hw *hw,
					      enum band_type band,
					      enum radio_path rfpath,
					      u32 txnum, u32 regaddr,
					      u32 bitmask, u32 data)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	struct rtl_phy *rtlphy = &rtlpriv->phy;
	u8 section = _rtl92fe_get_rate_section_index(regaddr);

	if (band != BAND_ON_2_4G && band != BAND_ON_5G) {
		rtl_dbg(rtlpriv, FPHY, PHY_TXPWR, "Invalid Band %d\n", band);
		return;
	}

	if (rfpath > MAX_RF_PATH - 1) {
		rtl_dbg(rtlpriv, FPHY, PHY_TXPWR,
			"Invalid RfPath %d\n", rfpath);
		return;
	}
	if (txnum > MAX_RF_PATH - 1) {
		rtl_dbg(rtlpriv, FPHY, PHY_TXPWR, "Invalid TxNum %d\n", txnum);
		return;
	}

	rtlphy->tx_power_by_rate_offset[band][rfpath][txnum][section] = data;
}

static bool phy_config_bb_with_pghdrfile(struct ieee80211_hw *hw,
					 u8 configtype)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	int i;
	u32 *phy_regarray_table_pg;
	u16 phy_regarray_pg_len;
	u32 v1, v2, v3, v4, v5, v6;

	phy_regarray_pg_len = RTL8192FE_PHY_REG_ARRAY_PG_LEN;
	phy_regarray_table_pg = RTL8192FE_PHY_REG_ARRAY_PG;
	if (rtlpriv->rtlhal.rfe_type == 3) {
		phy_regarray_pg_len = RTL8192FE_PHY_REG_ARRAY_PG_RFE3_LEN;
		phy_regarray_table_pg = RTL8192FE_PHY_REG_ARRAY_PG_RFE3;
	}

	if (configtype == BASEBAND_CONFIG_PHY_REG) {
		for (i = 0; i < phy_regarray_pg_len; i = i + 6) {
			v1 = phy_regarray_table_pg[i];
			v2 = phy_regarray_table_pg[i + 1];
			v3 = phy_regarray_table_pg[i + 2];
			v4 = phy_regarray_table_pg[i + 3];
			v5 = phy_regarray_table_pg[i + 4];
			v6 = phy_regarray_table_pg[i + 5];

			if (v1 < 0xcdcdcdcd) {
				_rtl92fe_store_tx_power_by_rate(hw, v1, v2, v3,
								  v4, v5, v6);
				continue;
			}
		}
	} else {
		rtl_dbg(rtlpriv, COMP_SEND, DBG_TRACE,
			"configtype != BaseBand_Config_PHY_REG\n");
	}
	return true;
}

#define READ_NEXT_RF_PAIR(v1, v2, i) \
	do { \
		i += 2; \
		v1 = array[i]; \
		v2 = array[i + 1]; \
	} while (0)

bool rtl92fe_phy_config_rf_with_headerfile(struct ieee80211_hw *hw,
					     enum radio_path rfpath)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	int i;
	u32 *array;
	u16 len;
	u32 v1 = 0, v2 = 0;
	u32 sys_cfg = rtl_read_dword(rtlpriv, REG_SYS_CFG1);
	bool rfe3_cut_b = rtlpriv->rtlhal.rfe_type == 3 &&
		!(sys_cfg & TRP_VAUX_EN) &&
		((sys_cfg & CHIP_VER_RTL_MASK) >> CHIP_VER_RTL_SHIFT) == 1;

	switch (rfpath) {
	case RF90_PATH_A:
		len = rfe3_cut_b ? RTL8192FE_RADIOA_RFE3_CUT_B_ARRAY_LEN :
			RTL8192FE_RADIOA_ARRAY_LEN;
		array = rfe3_cut_b ? RTL8192FE_RADIOA_RFE3_CUT_B_ARRAY :
			RTL8192FE_RADIOA_ARRAY;
		rtl_dbg(rtlpriv, COMP_INIT, DBG_LOUD,
			"Radio_A:RTL8192FE_RADIOA_ARRAY %d\n", len);
		for (i = 0; i < len; i = i + 2) {
			v1 = array[i];
			v2 = array[i + 1];
			if (v1 < 0xcdcdcdcd) {
				_rtl92fe_config_rf_radio_a(hw, v1, v2);
				continue;
			} else {
				if (i >= len - 2)
					break;

				if (!_check_condition(hw, array[i])) {
					READ_NEXT_RF_PAIR(v1, v2, i);
					while (v2 != 0xDEAD &&
					       v2 != 0xCDEF &&
					       v2 != 0xCDCD && i < len - 2) {
						READ_NEXT_RF_PAIR(v1, v2, i);
					}
					i -= 2;
				} else {
					READ_NEXT_RF_PAIR(v1, v2, i);
					while (v2 != 0xDEAD &&
					       v2 != 0xCDEF &&
					       v2 != 0xCDCD && i < len - 2) {
						_rtl92fe_config_rf_radio_a(hw,
									     v1,
									     v2);
						READ_NEXT_RF_PAIR(v1, v2, i);
					}

					while (v2 != 0xDEAD && i < len - 2)
						READ_NEXT_RF_PAIR(v1, v2, i);
				}
			}
		}
		break;

	case RF90_PATH_B:
		len = rfe3_cut_b ? RTL8192FE_RADIOB_RFE3_CUT_B_ARRAY_LEN :
			RTL8192FE_RADIOB_ARRAY_LEN;
		array = rfe3_cut_b ? RTL8192FE_RADIOB_RFE3_CUT_B_ARRAY :
			RTL8192FE_RADIOB_ARRAY;
		rtl_dbg(rtlpriv, COMP_INIT, DBG_LOUD,
			"Radio_B:RTL8192FE_RADIOB_ARRAY %d\n", len);
		for (i = 0; i < len; i = i + 2) {
			v1 = array[i];
			v2 = array[i + 1];
			if (v1 < 0xcdcdcdcd) {
				_rtl92fe_config_rf_radio_b(hw, v1, v2);
				continue;
			} else {
				if (i >= len - 2)
					break;

				if (!_check_condition(hw, array[i])) {
					READ_NEXT_RF_PAIR(v1, v2, i);
					while (v2 != 0xDEAD &&
					       v2 != 0xCDEF &&
					       v2 != 0xCDCD && i < len - 2) {
						READ_NEXT_RF_PAIR(v1, v2, i);
					}
					i -= 2;
				} else {
					READ_NEXT_RF_PAIR(v1, v2, i);
					while (v2 != 0xDEAD &&
					       v2 != 0xCDEF &&
					       v2 != 0xCDCD && i < len - 2) {
						_rtl92fe_config_rf_radio_b(hw,
									     v1,
									     v2);
						READ_NEXT_RF_PAIR(v1, v2, i);
					}

					while (v2 != 0xDEAD && i < len - 2)
						READ_NEXT_RF_PAIR(v1, v2, i);
				}
			}
		}
		break;
	case RF90_PATH_C:
	case RF90_PATH_D:
		break;
	}
	return true;
}

void rtl92fe_phy_get_hw_reg_originalvalue(struct ieee80211_hw *hw)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	struct rtl_phy *rtlphy = &rtlpriv->phy;

	rtlphy->default_initialgain[0] =
		(u8)rtl_get_bbreg(hw, ROFDM0_XAAGCCORE1, MASKBYTE0);
	rtlphy->default_initialgain[1] =
		(u8)rtl_get_bbreg(hw, ROFDM0_XBAGCCORE1, MASKBYTE0);
	rtlphy->default_initialgain[2] =
		(u8)rtl_get_bbreg(hw, ROFDM0_XCAGCCORE1, MASKBYTE0);
	rtlphy->default_initialgain[3] =
		(u8)rtl_get_bbreg(hw, ROFDM0_XDAGCCORE1, MASKBYTE0);

	rtl_dbg(rtlpriv, COMP_INIT, DBG_TRACE,
		"Default initial gain (c50=0x%x, c58=0x%x, c60=0x%x, c68=0x%x\n",
		rtlphy->default_initialgain[0],
		rtlphy->default_initialgain[1],
		rtlphy->default_initialgain[2],
		rtlphy->default_initialgain[3]);

	rtlphy->framesync = (u8)rtl_get_bbreg(hw, ROFDM0_RXDETECTOR3,
					      MASKBYTE0);
	rtlphy->framesync_c34 = rtl_get_bbreg(hw, ROFDM0_RXDETECTOR2,
					      MASKDWORD);

	rtl_dbg(rtlpriv, COMP_INIT, DBG_TRACE,
		"Default framesync (0x%x) = 0x%x\n",
		ROFDM0_RXDETECTOR3, rtlphy->framesync);
}

static void phy_init_bb_rf_register_def(struct ieee80211_hw *hw)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	struct rtl_phy *rtlphy = &rtlpriv->phy;

	rtlphy->phyreg_def[RF90_PATH_A].rfintfs = RFPGA0_XAB_RFINTERFACESW;
	rtlphy->phyreg_def[RF90_PATH_B].rfintfs = RFPGA0_XAB_RFINTERFACESW;

	rtlphy->phyreg_def[RF90_PATH_A].rfintfo = RFPGA0_XA_RFINTERFACEOE;
	rtlphy->phyreg_def[RF90_PATH_B].rfintfo = RFPGA0_XB_RFINTERFACEOE;

	rtlphy->phyreg_def[RF90_PATH_A].rfintfe = RFPGA0_XA_RFINTERFACEOE;
	rtlphy->phyreg_def[RF90_PATH_B].rfintfe = RFPGA0_XB_RFINTERFACEOE;

	rtlphy->phyreg_def[RF90_PATH_A].rf3wire_offset =
						RFPGA0_XA_LSSIPARAMETER;
	rtlphy->phyreg_def[RF90_PATH_B].rf3wire_offset =
						RFPGA0_XB_LSSIPARAMETER;

	rtlphy->phyreg_def[RF90_PATH_A].rfhssi_para2 = RFPGA0_XA_HSSIPARAMETER2;
	rtlphy->phyreg_def[RF90_PATH_B].rfhssi_para2 = RFPGA0_XB_HSSIPARAMETER2;

	rtlphy->phyreg_def[RF90_PATH_A].rf_rb = RFPGA0_XA_LSSIREADBACK;
	rtlphy->phyreg_def[RF90_PATH_B].rf_rb = RFPGA0_XB_LSSIREADBACK;

	rtlphy->phyreg_def[RF90_PATH_A].rf_rbpi = TRANSCEIVEA_HSPI_READBACK;
	rtlphy->phyreg_def[RF90_PATH_B].rf_rbpi = TRANSCEIVEB_HSPI_READBACK;
}

void rtl92fe_phy_get_txpower_level(struct ieee80211_hw *hw, long *powerlevel)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	struct rtl_phy *rtlphy = &rtlpriv->phy;
	u8 txpwr_level;
	long txpwr_dbm;

	txpwr_level = rtlphy->cur_cck_txpwridx;
	txpwr_dbm = _rtl92fe_phy_txpwr_idx_to_dbm(hw, WIRELESS_MODE_B,
						    txpwr_level);
	txpwr_level = rtlphy->cur_ofdm24g_txpwridx;
	if (_rtl92fe_phy_txpwr_idx_to_dbm(hw, WIRELESS_MODE_G, txpwr_level) >
	    txpwr_dbm)
		txpwr_dbm = _rtl92fe_phy_txpwr_idx_to_dbm(hw, WIRELESS_MODE_G,
							    txpwr_level);
	txpwr_level = rtlphy->cur_ofdm24g_txpwridx;
	if (_rtl92fe_phy_txpwr_idx_to_dbm(hw, WIRELESS_MODE_N_24G,
					    txpwr_level) > txpwr_dbm)
		txpwr_dbm = _rtl92fe_phy_txpwr_idx_to_dbm(hw,
							    WIRELESS_MODE_N_24G,
							    txpwr_level);
	*powerlevel = txpwr_dbm;
}

static u8 _rtl92fe_phy_get_ratesection_intxpower_byrate(enum radio_path path,
							  u8 rate)
{
	u8 rate_section = 0;

	switch (rate) {
	case DESC_RATE1M:
		rate_section = 2;
		break;
	case DESC_RATE2M:
	case DESC_RATE5_5M:
		if (path == RF90_PATH_A)
			rate_section = 3;
		else if (path == RF90_PATH_B)
			rate_section = 2;
		break;
	case DESC_RATE11M:
		rate_section = 3;
		break;
	case DESC_RATE6M:
	case DESC_RATE9M:
	case DESC_RATE12M:
	case DESC_RATE18M:
		rate_section = 0;
		break;
	case DESC_RATE24M:
	case DESC_RATE36M:
	case DESC_RATE48M:
	case DESC_RATE54M:
		rate_section = 1;
		break;
	case DESC_RATEMCS0:
	case DESC_RATEMCS1:
	case DESC_RATEMCS2:
	case DESC_RATEMCS3:
		rate_section = 4;
		break;
	case DESC_RATEMCS4:
	case DESC_RATEMCS5:
	case DESC_RATEMCS6:
	case DESC_RATEMCS7:
		rate_section = 5;
		break;
	case DESC_RATEMCS8:
	case DESC_RATEMCS9:
	case DESC_RATEMCS10:
	case DESC_RATEMCS11:
		rate_section = 6;
		break;
	case DESC_RATEMCS12:
	case DESC_RATEMCS13:
	case DESC_RATEMCS14:
	case DESC_RATEMCS15:
		rate_section = 7;
		break;
	default:
		WARN_ONCE(true, "rtl8192fe: Rate_Section is Illegal\n");
		break;
	}
	return rate_section;
}

static u8 _rtl92fe_get_txpower_by_rate(struct ieee80211_hw *hw,
					 enum band_type band,
					 enum radio_path rf, u8 rate)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	struct rtl_phy *rtlphy = &rtlpriv->phy;
	u8 shift = 0, sec, tx_num;
	s8 diff = 0;

	sec = _rtl92fe_phy_get_ratesection_intxpower_byrate(rf, rate);
	tx_num = RF_TX_NUM_NONIMPLEMENT;

	if (tx_num == RF_TX_NUM_NONIMPLEMENT) {
		if (rate >= DESC_RATEMCS8 && rate <= DESC_RATEMCS15)
			tx_num = RF_2TX;
		else
			tx_num = RF_1TX;
	}

	switch (rate) {
	case DESC_RATE1M:
	case DESC_RATE6M:
	case DESC_RATE24M:
	case DESC_RATEMCS0:
	case DESC_RATEMCS4:
	case DESC_RATEMCS8:
	case DESC_RATEMCS12:
		shift = 0;
		break;
	case DESC_RATE2M:
	case DESC_RATE9M:
	case DESC_RATE36M:
	case DESC_RATEMCS1:
	case DESC_RATEMCS5:
	case DESC_RATEMCS9:
	case DESC_RATEMCS13:
		shift = 8;
		break;
	case DESC_RATE5_5M:
	case DESC_RATE12M:
	case DESC_RATE48M:
	case DESC_RATEMCS2:
	case DESC_RATEMCS6:
	case DESC_RATEMCS10:
	case DESC_RATEMCS14:
		shift = 16;
		break;
	case DESC_RATE11M:
	case DESC_RATE18M:
	case DESC_RATE54M:
	case DESC_RATEMCS3:
	case DESC_RATEMCS7:
	case DESC_RATEMCS11:
	case DESC_RATEMCS15:
		shift = 24;
		break;
	default:
		WARN_ONCE(true, "rtl8192fe: Rate_Section is Illegal\n");
		break;
	}

	diff = (u8)(rtlphy->tx_power_by_rate_offset[band][rf][tx_num][sec] >>
		    shift) & 0xff;

	return diff;
}

static u8 _rtl92fe_get_txpower_index(struct ieee80211_hw *hw,
				       enum radio_path rfpath, u8 rate,
				       u8 bw, u8 channel)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	struct rtl_efuse *rtlefuse = rtl_efuse(rtlpriv);
	u8 index = (channel - 1);
	u8 tx_power = 0;
	u8 diff = 0;

	if (channel < 1 || channel > 14) {
		index = 0;
		rtl_dbg(rtlpriv, COMP_POWER_TRACKING, DBG_DMESG,
			"Illegal channel!!\n");
	}

	if (IS_CCK_RATE((s8)rate))
		tx_power = rtlefuse->txpwrlevel_cck[rfpath][index];
	else if (DESC_RATE6M <= rate)
		tx_power = rtlefuse->txpwrlevel_ht40_1s[rfpath][index];

	/* OFDM-1T */
	if (DESC_RATE6M <= rate && rate <= DESC_RATE54M &&
	    !IS_CCK_RATE((s8)rate))
		tx_power += rtlefuse->txpwr_legacyhtdiff[rfpath][TX_1S];

	/* BW20-1S, BW20-2S */
	if (bw == HT_CHANNEL_WIDTH_20) {
		if (DESC_RATEMCS0 <= rate && rate <= DESC_RATEMCS15)
			tx_power += rtlefuse->txpwr_ht20diff[rfpath][TX_1S];
		if (DESC_RATEMCS8 <= rate && rate <= DESC_RATEMCS15)
			tx_power += rtlefuse->txpwr_ht20diff[rfpath][TX_2S];
	} else if (bw == HT_CHANNEL_WIDTH_20_40) {
		if (DESC_RATEMCS0 <= rate && rate <= DESC_RATEMCS15)
			tx_power += rtlefuse->txpwr_ht40diff[rfpath][TX_1S];
		if (DESC_RATEMCS8 <= rate && rate <= DESC_RATEMCS15)
			tx_power += rtlefuse->txpwr_ht40diff[rfpath][TX_2S];
	}

	if (rtlefuse->eeprom_regulatory != 2)
		diff = _rtl92fe_get_txpower_by_rate(hw, BAND_ON_2_4G,
						      rfpath, rate);

	tx_power += diff;

	if (tx_power > MAX_POWER_INDEX)
		tx_power = MAX_POWER_INDEX;

	return tx_power;
}

static void _rtl92fe_set_txpower_index(struct ieee80211_hw *hw, u8 pwr_idx,
					 enum radio_path rfpath, u8 rate)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);

	if (rfpath == RF90_PATH_A) {
		switch (rate) {
		case DESC_RATE1M:
			rtl_set_bbreg(hw, RTXAGC_A_CCK1_MCS32, MASKBYTE1,
				      pwr_idx);
			break;
		case DESC_RATE2M:
			rtl_set_bbreg(hw, RTXAGC_B_CCK11_A_CCK2_11, MASKBYTE1,
				      pwr_idx);
			break;
		case DESC_RATE5_5M:
			rtl_set_bbreg(hw, RTXAGC_B_CCK11_A_CCK2_11, MASKBYTE2,
				      pwr_idx);
			break;
		case DESC_RATE11M:
			rtl_set_bbreg(hw, RTXAGC_B_CCK11_A_CCK2_11, MASKBYTE3,
				      pwr_idx);
			break;
		case DESC_RATE6M:
			rtl_set_bbreg(hw, RTXAGC_A_RATE18_06, MASKBYTE0,
				      pwr_idx);
			break;
		case DESC_RATE9M:
			rtl_set_bbreg(hw, RTXAGC_A_RATE18_06, MASKBYTE1,
				      pwr_idx);
			break;
		case DESC_RATE12M:
			rtl_set_bbreg(hw, RTXAGC_A_RATE18_06, MASKBYTE2,
				      pwr_idx);
			break;
		case DESC_RATE18M:
			rtl_set_bbreg(hw, RTXAGC_A_RATE18_06, MASKBYTE3,
				      pwr_idx);
			break;
		case DESC_RATE24M:
			rtl_set_bbreg(hw, RTXAGC_A_RATE54_24, MASKBYTE0,
				      pwr_idx);
			break;
		case DESC_RATE36M:
			rtl_set_bbreg(hw, RTXAGC_A_RATE54_24, MASKBYTE1,
				      pwr_idx);
			break;
		case DESC_RATE48M:
			rtl_set_bbreg(hw, RTXAGC_A_RATE54_24, MASKBYTE2,
				      pwr_idx);
			break;
		case DESC_RATE54M:
			rtl_set_bbreg(hw, RTXAGC_A_RATE54_24, MASKBYTE3,
				      pwr_idx);
			break;
		case DESC_RATEMCS0:
			rtl_set_bbreg(hw, RTXAGC_A_MCS03_MCS00, MASKBYTE0,
				      pwr_idx);
			break;
		case DESC_RATEMCS1:
			rtl_set_bbreg(hw, RTXAGC_A_MCS03_MCS00, MASKBYTE1,
				      pwr_idx);
			break;
		case DESC_RATEMCS2:
			rtl_set_bbreg(hw, RTXAGC_A_MCS03_MCS00, MASKBYTE2,
				      pwr_idx);
			break;
		case DESC_RATEMCS3:
			rtl_set_bbreg(hw, RTXAGC_A_MCS03_MCS00, MASKBYTE3,
				      pwr_idx);
			break;
		case DESC_RATEMCS4:
			rtl_set_bbreg(hw, RTXAGC_A_MCS07_MCS04, MASKBYTE0,
				      pwr_idx);
			break;
		case DESC_RATEMCS5:
			rtl_set_bbreg(hw, RTXAGC_A_MCS07_MCS04, MASKBYTE1,
				      pwr_idx);
			break;
		case DESC_RATEMCS6:
			rtl_set_bbreg(hw, RTXAGC_A_MCS07_MCS04, MASKBYTE2,
				      pwr_idx);
			break;
		case DESC_RATEMCS7:
			rtl_set_bbreg(hw, RTXAGC_A_MCS07_MCS04, MASKBYTE3,
				      pwr_idx);
			break;
		case DESC_RATEMCS8:
			rtl_set_bbreg(hw, RTXAGC_A_MCS11_MCS08, MASKBYTE0,
				      pwr_idx);
			break;
		case DESC_RATEMCS9:
			rtl_set_bbreg(hw, RTXAGC_A_MCS11_MCS08, MASKBYTE1,
				      pwr_idx);
			break;
		case DESC_RATEMCS10:
			rtl_set_bbreg(hw, RTXAGC_A_MCS11_MCS08, MASKBYTE2,
				      pwr_idx);
			break;
		case DESC_RATEMCS11:
			rtl_set_bbreg(hw, RTXAGC_A_MCS11_MCS08, MASKBYTE3,
				      pwr_idx);
			break;
		case DESC_RATEMCS12:
			rtl_set_bbreg(hw, RTXAGC_A_MCS15_MCS12, MASKBYTE0,
				      pwr_idx);
			break;
		case DESC_RATEMCS13:
			rtl_set_bbreg(hw, RTXAGC_A_MCS15_MCS12, MASKBYTE1,
				      pwr_idx);
			break;
		case DESC_RATEMCS14:
			rtl_set_bbreg(hw, RTXAGC_A_MCS15_MCS12, MASKBYTE2,
				      pwr_idx);
			break;
		case DESC_RATEMCS15:
			rtl_set_bbreg(hw, RTXAGC_A_MCS15_MCS12, MASKBYTE3,
				      pwr_idx);
			break;
		default:
			rtl_dbg(rtlpriv, COMP_POWER, DBG_LOUD,
				"Invalid Rate!!\n");
			break;
		}
	} else if (rfpath == RF90_PATH_B) {
		switch (rate) {
		case DESC_RATE1M:
			rtl_set_bbreg(hw, RTXAGC_B_CCK1_55_MCS32, MASKBYTE1,
				      pwr_idx);
			break;
		case DESC_RATE2M:
			rtl_set_bbreg(hw, RTXAGC_B_CCK1_55_MCS32, MASKBYTE2,
				      pwr_idx);
			break;
		case DESC_RATE5_5M:
			rtl_set_bbreg(hw, RTXAGC_B_CCK1_55_MCS32, MASKBYTE3,
				      pwr_idx);
			break;
		case DESC_RATE11M:
			rtl_set_bbreg(hw, RTXAGC_B_CCK11_A_CCK2_11, MASKBYTE0,
				      pwr_idx);
			break;
		case DESC_RATE6M:
			rtl_set_bbreg(hw, RTXAGC_B_RATE18_06, MASKBYTE0,
				      pwr_idx);
			break;
		case DESC_RATE9M:
			rtl_set_bbreg(hw, RTXAGC_B_RATE18_06, MASKBYTE1,
				      pwr_idx);
			break;
		case DESC_RATE12M:
			rtl_set_bbreg(hw, RTXAGC_B_RATE18_06, MASKBYTE2,
				      pwr_idx);
			break;
		case DESC_RATE18M:
			rtl_set_bbreg(hw, RTXAGC_B_RATE18_06, MASKBYTE3,
				      pwr_idx);
			break;
		case DESC_RATE24M:
			rtl_set_bbreg(hw, RTXAGC_B_RATE54_24, MASKBYTE0,
				      pwr_idx);
			break;
		case DESC_RATE36M:
			rtl_set_bbreg(hw, RTXAGC_B_RATE54_24, MASKBYTE1,
				      pwr_idx);
			break;
		case DESC_RATE48M:
			rtl_set_bbreg(hw, RTXAGC_B_RATE54_24, MASKBYTE2,
				      pwr_idx);
			break;
		case DESC_RATE54M:
			rtl_set_bbreg(hw, RTXAGC_B_RATE54_24, MASKBYTE3,
				      pwr_idx);
			break;
		case DESC_RATEMCS0:
			rtl_set_bbreg(hw, RTXAGC_B_MCS03_MCS00, MASKBYTE0,
				      pwr_idx);
			break;
		case DESC_RATEMCS1:
			rtl_set_bbreg(hw, RTXAGC_B_MCS03_MCS00, MASKBYTE1,
				      pwr_idx);
			break;
		case DESC_RATEMCS2:
			rtl_set_bbreg(hw, RTXAGC_B_MCS03_MCS00, MASKBYTE2,
				      pwr_idx);
			break;
		case DESC_RATEMCS3:
			rtl_set_bbreg(hw, RTXAGC_B_MCS03_MCS00, MASKBYTE3,
				      pwr_idx);
			break;
		case DESC_RATEMCS4:
			rtl_set_bbreg(hw, RTXAGC_B_MCS07_MCS04, MASKBYTE0,
				      pwr_idx);
			break;
		case DESC_RATEMCS5:
			rtl_set_bbreg(hw, RTXAGC_B_MCS07_MCS04, MASKBYTE1,
				      pwr_idx);
			break;
		case DESC_RATEMCS6:
			rtl_set_bbreg(hw, RTXAGC_B_MCS07_MCS04, MASKBYTE2,
				      pwr_idx);
			break;
		case DESC_RATEMCS7:
			rtl_set_bbreg(hw, RTXAGC_B_MCS07_MCS04, MASKBYTE3,
				      pwr_idx);
			break;
		case DESC_RATEMCS8:
			rtl_set_bbreg(hw, RTXAGC_B_MCS11_MCS08, MASKBYTE0,
				      pwr_idx);
			break;
		case DESC_RATEMCS9:
			rtl_set_bbreg(hw, RTXAGC_B_MCS11_MCS08, MASKBYTE1,
				      pwr_idx);
			break;
		case DESC_RATEMCS10:
			rtl_set_bbreg(hw, RTXAGC_B_MCS11_MCS08, MASKBYTE2,
				      pwr_idx);
			break;
		case DESC_RATEMCS11:
			rtl_set_bbreg(hw, RTXAGC_B_MCS11_MCS08, MASKBYTE3,
				      pwr_idx);
			break;
		case DESC_RATEMCS12:
			rtl_set_bbreg(hw, RTXAGC_B_MCS15_MCS12, MASKBYTE0,
				      pwr_idx);
			break;
		case DESC_RATEMCS13:
			rtl_set_bbreg(hw, RTXAGC_B_MCS15_MCS12, MASKBYTE1,
				      pwr_idx);
			break;
		case DESC_RATEMCS14:
			rtl_set_bbreg(hw, RTXAGC_B_MCS15_MCS12, MASKBYTE2,
				      pwr_idx);
			break;
		case DESC_RATEMCS15:
			rtl_set_bbreg(hw, RTXAGC_B_MCS15_MCS12, MASKBYTE3,
				      pwr_idx);
			break;
		default:
			rtl_dbg(rtlpriv, COMP_POWER, DBG_LOUD,
				"Invalid Rate!!\n");
			break;
		}
	} else {
		rtl_dbg(rtlpriv, COMP_POWER, DBG_LOUD, "Invalid RFPath!!\n");
	}
}

static void phy_set_txpower_index_by_rate_array(struct ieee80211_hw *hw,
						enum radio_path rfpath, u8 bw,
						u8 channel, u8 *rates, u8 size)
{
	u8 i;
	u8 power_index;

	for (i = 0; i < size; i++) {
		power_index = _rtl92fe_get_txpower_index(hw, rfpath, rates[i],
							   bw, channel);
		_rtl92fe_set_txpower_index(hw, power_index, rfpath, rates[i]);
	}
}

static void phy_set_txpower_index_by_rate_section(struct ieee80211_hw *hw,
						  enum radio_path rfpath,
						  u8 channel,
						  enum rate_section section)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	struct rtl_hal *rtlhal = rtl_hal(rtlpriv);
	struct rtl_phy *rtlphy = &rtlpriv->phy;

	if (section == CCK) {
		u8 cck_rates[] = {DESC_RATE1M, DESC_RATE2M,
				  DESC_RATE5_5M, DESC_RATE11M};
		if (rtlhal->current_bandtype == BAND_ON_2_4G)
			phy_set_txpower_index_by_rate_array(hw, rfpath,
						rtlphy->current_chan_bw,
						channel, cck_rates, 4);
	} else if (section == OFDM) {
		u8 ofdm_rates[] = {DESC_RATE6M, DESC_RATE9M,
				   DESC_RATE12M, DESC_RATE18M,
				   DESC_RATE24M, DESC_RATE36M,
				   DESC_RATE48M, DESC_RATE54M};
		phy_set_txpower_index_by_rate_array(hw, rfpath,
						    rtlphy->current_chan_bw,
						    channel, ofdm_rates, 8);
	} else if (section == HT_MCS0_MCS7) {
		u8 ht_rates1t[] = {DESC_RATEMCS0, DESC_RATEMCS1,
				   DESC_RATEMCS2, DESC_RATEMCS3,
				   DESC_RATEMCS4, DESC_RATEMCS5,
				   DESC_RATEMCS6, DESC_RATEMCS7};
		phy_set_txpower_index_by_rate_array(hw, rfpath,
						    rtlphy->current_chan_bw,
						    channel, ht_rates1t, 8);
	} else if (section == HT_MCS8_MCS15) {
		u8 ht_rates2t[] = {DESC_RATEMCS8, DESC_RATEMCS9,
				   DESC_RATEMCS10, DESC_RATEMCS11,
				   DESC_RATEMCS12, DESC_RATEMCS13,
				   DESC_RATEMCS14, DESC_RATEMCS15};
		phy_set_txpower_index_by_rate_array(hw, rfpath,
						    rtlphy->current_chan_bw,
						    channel, ht_rates2t, 8);
	} else {
		rtl_dbg(rtlpriv, FPHY, PHY_TXPWR,
			"Invalid RateSection %d\n", section);
	}
}

void rtl92fe_phy_set_txpower_level(struct ieee80211_hw *hw, u8 channel)
{
	struct rtl_efuse *rtlefuse = rtl_efuse(rtl_priv(hw));
	struct rtl_phy *rtlphy = &rtl_priv(hw)->phy;
	enum radio_path rfpath;

	if (!rtlefuse->txpwr_fromeprom)
		return;
	for (rfpath = RF90_PATH_A; rfpath < rtlphy->num_total_rfpath;
	     rfpath++) {
		phy_set_txpower_index_by_rate_section(hw, rfpath,
						      channel, CCK);
		phy_set_txpower_index_by_rate_section(hw, rfpath,
						      channel, OFDM);
		phy_set_txpower_index_by_rate_section(hw, rfpath,
						      channel,
						      HT_MCS0_MCS7);

		if (rtlphy->num_total_rfpath >= 2)
			phy_set_txpower_index_by_rate_section(hw, rfpath,
							      channel,
							      HT_MCS8_MCS15);
	}
}

static long _rtl92fe_phy_txpwr_idx_to_dbm(struct ieee80211_hw *hw,
					    enum wireless_mode wirelessmode,
					    u8 txpwridx)
{
	long offset;
	long pwrout_dbm;

	switch (wirelessmode) {
	case WIRELESS_MODE_B:
		offset = -7;
		break;
	case WIRELESS_MODE_G:
	case WIRELESS_MODE_N_24G:
	default:
		offset = -8;
		break;
	}
	pwrout_dbm = txpwridx / 2 + offset;
	return pwrout_dbm;
}

void rtl92fe_phy_scan_operation_backup(struct ieee80211_hw *hw, u8 operation)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	struct rtl_hal *rtlhal = rtl_hal(rtl_priv(hw));
	enum io_type iotype;

	if (!is_hal_stop(rtlhal)) {
		switch (operation) {
		case SCAN_OPT_BACKUP_BAND0:
			iotype = IO_CMD_PAUSE_BAND0_DM_BY_SCAN;
			rtlpriv->cfg->ops->set_hw_reg(hw, HW_VAR_IO_CMD,
						      (u8 *)&iotype);
			break;
		case SCAN_OPT_RESTORE:
			iotype = IO_CMD_RESUME_DM_BY_SCAN;
			rtlpriv->cfg->ops->set_hw_reg(hw, HW_VAR_IO_CMD,
						      (u8 *)&iotype);
			break;
		default:
			pr_err("Unknown Scan Backup operation.\n");
			break;
		}
	}
}

/* The 8192F revises the CCK TX power-shaping filter for channels 13 and 14
 * so the spectral mask still passes at the band edge.  These four register
 * writes must track the values in the PHY init table.
 */
static void _rtl92fe_phy_revise_cck_tx_psf(struct ieee80211_hw *hw,
					     u8 channel)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);

	if (channel == 13) {
		rtl_write_dword(rtlpriv, R8192F_CCK0_TX_FILTER1, 0xf8fe0001);
		rtl_write_dword(rtlpriv, R8192F_CCK0_TX_FILTER2, 0x64b80c1c);
		rtl_write_word(rtlpriv, R8192F_CCK0_DEBUG_PORT, 0x8810);
		rtl_write_dword(rtlpriv, R8192F_CCK0_TX_FILTER3, 0x01235667);
	} else if (channel == 14) {
		rtl_write_dword(rtlpriv, R8192F_CCK0_TX_FILTER1, 0xe82c0001);
		rtl_write_dword(rtlpriv, R8192F_CCK0_TX_FILTER2, 0x0000b81c);
		rtl_write_word(rtlpriv, R8192F_CCK0_DEBUG_PORT, 0x0000);
		rtl_write_dword(rtlpriv, R8192F_CCK0_TX_FILTER3, 0x00003667);
	} else {
		rtl_write_dword(rtlpriv, R8192F_CCK0_TX_FILTER1, 0xe82c0001);
		rtl_write_dword(rtlpriv, R8192F_CCK0_TX_FILTER2, 0x64b80c1c);
		rtl_write_word(rtlpriv, R8192F_CCK0_DEBUG_PORT, 0x8810);
		rtl_write_dword(rtlpriv, R8192F_CCK0_TX_FILTER3, 0x01235667);
	}
}

void rtl92fe_phy_set_bw_mode_callback(struct ieee80211_hw *hw)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	struct rtl_hal *rtlhal = rtl_hal(rtl_priv(hw));
	struct rtl_phy *rtlphy = &rtlpriv->phy;
	struct rtl_mac *mac = rtl_mac(rtl_priv(hw));
	u8 reg_bw_opmode;
	u8 reg_prsr_rsc;
	u8 ht40 = (rtlphy->current_chan_bw == HT_CHANNEL_WIDTH_20_40) ? 1 : 0;

	rtl_dbg(rtlpriv, COMP_SCAN, DBG_TRACE,
		"Switch to %s bandwidth\n",
		rtlphy->current_chan_bw == HT_CHANNEL_WIDTH_20 ?
		"20MHz" : "40MHz");

	if (is_hal_stop(rtlhal)) {
		rtlphy->set_bwmode_inprogress = false;
		return;
	}

	reg_bw_opmode = rtl_read_byte(rtlpriv, REG_BWOPMODE);
	reg_prsr_rsc = rtl_read_byte(rtlpriv, REG_RRSR + 2);

	switch (rtlphy->current_chan_bw) {
	case HT_CHANNEL_WIDTH_20:
		reg_bw_opmode |= BW_OPMODE_20MHZ;
		rtl_write_byte(rtlpriv, REG_BWOPMODE, reg_bw_opmode);
		break;
	case HT_CHANNEL_WIDTH_20_40:
		reg_bw_opmode &= ~BW_OPMODE_20MHZ;
		rtl_write_byte(rtlpriv, REG_BWOPMODE, reg_bw_opmode);
		/* RRSR_RSC (RRSR[22:21]) stays 0 on the 8192F: the vendor's 8192FE
		 * bandwidth handler (SwBWMode88XX_N) clears it at 40 MHz and
		 * rtl8xxxu's 8192F never sets it.  The 92EE value (prime_sc) left
		 * the client's frames unacknowledged at HT40 (X111W 2026-09-25:
		 * uplink 0.1-2.6 Mbps, client TX rate down to 6 Mbps).
		 */
		reg_prsr_rsc &= ~(BIT(6) | BIT(5));
		rtl_write_byte(rtlpriv, REG_RRSR + 2, reg_prsr_rsc);
		break;
	default:
		pr_err("unknown bandwidth: %#X\n", rtlphy->current_chan_bw);
		break;
	}

	/* MAC data sub-channel: 2 = primary is the lower 20 MHz (secondary
	 * above), 1 = upper, 0 = no secondary.  Without it the MAC keeps 20 MHz
	 * data framing while the BB is at 40 MHz: association completes on the
	 * duplicate-legacy management frames and no HT40 data frame is
	 * delivered (measured on the G24W 2026-09-17: 0 of 1000 pings back).
	 */
	rtl_write_byte(rtlpriv, R8192F_DATA_SUBCHANNEL,
		       !ht40 ? 0 :
		       (mac->cur_40_prime_sc == PRIME_CHNL_OFFSET_LOWER ? 2 : 1));

	/* BB-side bandwidth select.  The 8192F sets the FPGA RF-mode bit on
	 * both BB sub-banks and re-programs the ADC/DAC clock dividers for
	 * 40 MHz; the small-BW pseudo-noise weight is cleared first.
	 */
	rtl_set_bbreg(hw, ROFDM0_TXPSEUDONOISEWGT, (BIT(31) | BIT(30)), 0x0);

	rtl_set_bbreg(hw, RFPGA0_RFMOD, BRFMOD, ht40);
	rtl_set_bbreg(hw, RFPGA1_RFMOD, BRFMOD, ht40);

	/* ADC clock = 160 MHz, DAC clock = 80 MHz, ADC buffer clk.  The buffer
	 * clock is 0xca4[27:26]; this used to write 0xe24[27:26], a field
	 * mainline never touches, and left the RX DFIR words below unset.
	 */
	rtl_set_bbreg(hw, RFPGA0_RFMOD, (BIT(10) | BIT(9) | BIT(8)), 0x4);
	rtl_set_bbreg(hw, RFPGA0_RFMOD, (BIT(13) | BIT(12)), 0x2);
	rtl_set_bbreg(hw, R8192F_ANTDIV_PARA1, (BIT(27) | BIT(26)), 0x2);

	switch (rtlphy->current_chan_bw) {
	case HT_CHANNEL_WIDTH_20:
		break;
	case HT_CHANNEL_WIDTH_20_40:
		rtl_set_bbreg(hw, RCCK0_SYSTEM, BCCK_SIDEBAND,
			      (mac->cur_40_prime_sc >> 1));
		rtl_set_bbreg(hw, ROFDM1_LSTF, 0xC00, mac->cur_40_prime_sc);
		break;
	default:
		pr_err("unknown bandwidth: %#X\n", rtlphy->current_chan_bw);
		break;
	}

	rtl92fe_phy_rf6052_set_bandwidth(hw, rtlphy->current_chan_bw);

	/* RX DFIR for the new width: TAP_UPD 0xe24[21:20], the down-sampling
	 * factor 0xc10[29:28], and the DFIR mode word 0x948[8:0] (0x3 at
	 * 40 MHz, 0x1a3 at 20 MHz).
	 */
	rtl_set_bbreg(hw, R8192F_TAP_UPD_97F, (BIT(21) | BIT(20)), 0x2);
	rtl_set_bbreg(hw, R8192F_DOWNSAM_FACTOR, (BIT(29) | BIT(28)), 0x2);
	rtl_set_bbreg(hw, R8192F_RX_DFIR_MOD_97F, 0x1ff, ht40 ? 0x3 : 0x1a3);
	rtlphy->set_bwmode_inprogress = false;
	rtl_dbg(rtlpriv, COMP_SCAN, DBG_LOUD, "\n");
}

void rtl92fe_phy_set_bw_mode(struct ieee80211_hw *hw,
			       enum nl80211_channel_type ch_type)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	struct rtl_phy *rtlphy = &rtlpriv->phy;
	struct rtl_hal *rtlhal = rtl_hal(rtl_priv(hw));
	u8 tmp_bw = rtlphy->current_chan_bw;

	if (rtlphy->set_bwmode_inprogress)
		return;
	rtlphy->set_bwmode_inprogress = true;
	if ((!is_hal_stop(rtlhal)) && !(RT_CANNOT_IO(hw))) {
		rtl92fe_phy_set_bw_mode_callback(hw);
	} else {
		rtl_dbg(rtlpriv, COMP_ERR, DBG_WARNING,
			"false driver sleep or unload\n");
		rtlphy->set_bwmode_inprogress = false;
		rtlphy->current_chan_bw = tmp_bw;
	}
}

void rtl92fe_phy_sw_chnl_callback(struct ieee80211_hw *hw)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	struct rtl_hal *rtlhal = rtl_hal(rtl_priv(hw));
	struct rtl_phy *rtlphy = &rtlpriv->phy;
	u32 delay;

	rtl_dbg(rtlpriv, COMP_SCAN, DBG_TRACE,
		"switch to channel%d\n", rtlphy->current_channel);
	if (is_hal_stop(rtlhal))
		return;
	do {
		if (!rtlphy->sw_chnl_inprogress)
			break;
		if (!_rtl92fe_phy_sw_chnl_step_by_step
		    (hw, rtlphy->current_channel, &rtlphy->sw_chnl_stage,
		     &rtlphy->sw_chnl_step, &delay)) {
			if (delay > 0)
				mdelay(delay);
			else
				continue;
		} else {
			rtlphy->sw_chnl_inprogress = false;
		}
		break;
	} while (true);
	rtl_dbg(rtlpriv, COMP_SCAN, DBG_TRACE, "\n");
}

u8 rtl92fe_phy_sw_chnl(struct ieee80211_hw *hw)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	struct rtl_phy *rtlphy = &rtlpriv->phy;
	struct rtl_hal *rtlhal = rtl_hal(rtl_priv(hw));

	if (rtlphy->sw_chnl_inprogress)
		return 0;
	if (rtlphy->set_bwmode_inprogress)
		return 0;
	WARN_ONCE((rtlphy->current_channel > 14),
		  "rtl8192fe: WIRELESS_MODE_G but channel>14");
	rtlphy->sw_chnl_inprogress = true;
	rtlphy->sw_chnl_stage = 0;
	rtlphy->sw_chnl_step = 0;
	if (!(is_hal_stop(rtlhal)) && !(RT_CANNOT_IO(hw))) {
		rtl92fe_phy_sw_chnl_callback(hw);
		rtl_dbg(rtlpriv, COMP_CHAN, DBG_LOUD,
			"sw_chnl_inprogress false schedule workitem current channel %d\n",
			rtlphy->current_channel);
		rtlphy->sw_chnl_inprogress = false;
	} else {
		rtl_dbg(rtlpriv, COMP_CHAN, DBG_LOUD,
			"sw_chnl_inprogress false driver sleep or unload\n");
		rtlphy->sw_chnl_inprogress = false;
	}
	return 1;
}

static bool _rtl92fe_phy_sw_chnl_step_by_step(struct ieee80211_hw *hw,
						u8 channel, u8 *stage,
						u8 *step, u32 *delay)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	struct rtl_phy *rtlphy = &rtlpriv->phy;
	struct swchnlcmd precommoncmd[MAX_PRECMD_CNT];
	u32 precommoncmdcnt;
	struct swchnlcmd postcommoncmd[MAX_POSTCMD_CNT];
	u32 postcommoncmdcnt;
	struct swchnlcmd rfdependcmd[MAX_RFDEPENDCMD_CNT];
	u32 rfdependcmdcnt;
	struct swchnlcmd *currentcmd = NULL;
	u8 rfpath;
	u8 num_total_rfpath = rtlphy->num_total_rfpath;

	/* The 8192F shapes the CCK TX filter per channel before the RF
	 * channel word is written, so band-edge channels stay in mask.
	 */
	_rtl92fe_phy_revise_cck_tx_psf(hw, channel);

	precommoncmdcnt = 0;
	_rtl92fe_phy_set_sw_chnl_cmdarray(precommoncmd, precommoncmdcnt++,
					    MAX_PRECMD_CNT,
					    CMDID_SET_TXPOWEROWER_LEVEL,
					    0, 0, 0);
	_rtl92fe_phy_set_sw_chnl_cmdarray(precommoncmd, precommoncmdcnt++,
					    MAX_PRECMD_CNT, CMDID_END, 0, 0, 0);

	postcommoncmdcnt = 0;
	_rtl92fe_phy_set_sw_chnl_cmdarray(postcommoncmd, postcommoncmdcnt++,
					    MAX_POSTCMD_CNT, CMDID_END,
					    0, 0, 0);

	rfdependcmdcnt = 0;

	WARN_ONCE((channel < 1 || channel > 14),
		  "rtl8192fe: illegal channel for Zebra: %d\n", channel);

	_rtl92fe_phy_set_sw_chnl_cmdarray(rfdependcmd, rfdependcmdcnt++,
					    MAX_RFDEPENDCMD_CNT,
					    CMDID_RF_WRITEREG,
					    RF_CHNLBW, channel, 10);

	_rtl92fe_phy_set_sw_chnl_cmdarray(rfdependcmd, rfdependcmdcnt++,
					    MAX_RFDEPENDCMD_CNT, CMDID_END,
					    0, 0, 0);

	do {
		switch (*stage) {
		case 0:
			currentcmd = &precommoncmd[*step];
			break;
		case 1:
			currentcmd = &rfdependcmd[*step];
			break;
		case 2:
			currentcmd = &postcommoncmd[*step];
			break;
		default:
			pr_err("Invalid 'stage' = %d, Check it!\n", *stage);
			return true;
		}

		if (currentcmd->cmdid == CMDID_END) {
			if ((*stage) == 2)
				return true;
			(*stage)++;
			(*step) = 0;
			continue;
		}

		switch (currentcmd->cmdid) {
		case CMDID_SET_TXPOWEROWER_LEVEL:
			rtl92fe_phy_set_txpower_level(hw, channel);
			break;
		case CMDID_WRITEPORT_ULONG:
			rtl_write_dword(rtlpriv, currentcmd->para1,
					currentcmd->para2);
			break;
		case CMDID_WRITEPORT_USHORT:
			rtl_write_word(rtlpriv, currentcmd->para1,
				       (u16)currentcmd->para2);
			break;
		case CMDID_WRITEPORT_UCHAR:
			rtl_write_byte(rtlpriv, currentcmd->para1,
				       (u8)currentcmd->para2);
			break;
		case CMDID_RF_WRITEREG: {
			/* Match vendor config_phydm_switch_channel_8192f: key the ...
			 * dev/MEASURED-phy.c.md sec 3. */
			u32 chnlval = (rtlphy->rfreg_chnlval[0] & 0xfffff00) |
				      currentcmd->para2;

			for (rfpath = 0; rfpath < num_total_rfpath; rfpath++) {
				rtlphy->rfreg_chnlval[rfpath] = chnlval;
				rtl_set_rfreg(hw, (enum radio_path)rfpath,
					      currentcmd->para1,
					      RFREG_OFFSET_MASK,
					      chnlval);
			}
			break;
		}
		default:
			rtl_dbg(rtlpriv, COMP_ERR, DBG_LOUD,
				"switch case %#x not processed\n",
				currentcmd->cmdid);
			break;
		}

		break;
	} while (true);

	(*delay) = currentcmd->msdelay;
	(*step)++;
	return false;
}

static bool _rtl92fe_phy_set_sw_chnl_cmdarray(struct swchnlcmd *cmdtable,
						u32 cmdtableidx, u32 cmdtablesz,
						enum swchnlcmd_id cmdid,
						u32 para1, u32 para2,
						u32 msdelay)
{
	struct swchnlcmd *pcmd;

	if (!cmdtable) {
		WARN_ONCE(true, "rtl8192fe: cmdtable cannot be NULL.\n");
		return false;
	}

	if (cmdtableidx >= cmdtablesz)
		return false;

	pcmd = cmdtable + cmdtableidx;
	pcmd->cmdid = cmdid;
	pcmd->para1 = para1;
	pcmd->para2 = para2;
	pcmd->msdelay = msdelay;
	return true;
}

/* IQ / LO / LC calibration. The RTL8192F runs a TX-LOK+IQK ... -- dev/MEASURED-phy.c.md sec 4. */

static void _rtl92fe_phy_save_regs(struct ieee80211_hw *hw, const u32 *reg,
				     u32 *backup, u32 num)
{
	u32 i;

	for (i = 0; i < num; i++)
		backup[i] = rtl_get_bbreg(hw, reg[i], MASKDWORD);
}

static void _rtl92fe_phy_reload_regs(struct ieee80211_hw *hw, const u32 *reg,
				       u32 *backup, u32 num)
{
	u32 i;

	for (i = 0; i < num; i++)
		rtl_set_bbreg(hw, reg[i], MASKDWORD, backup[i]);
}

static void _rtl92fe_phy_save_mac_registers(struct ieee80211_hw *hw,
					      const u32 *macreg, u32 *macbackup)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	u32 i;

	for (i = 0; i < (IQK_MAC_REG_NUM - 1); i++)
		macbackup[i] = rtl_read_byte(rtlpriv, macreg[i]);

	macbackup[i] = rtl_read_dword(rtlpriv, macreg[i]);
}

static void _rtl92fe_phy_reload_mac_registers(struct ieee80211_hw *hw,
						const u32 *macreg,
						u32 *macbackup)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	u32 i;

	for (i = 0; i < (IQK_MAC_REG_NUM - 1); i++)
		rtl_write_byte(rtlpriv, macreg[i], (u8)macbackup[i]);
	rtl_write_dword(rtlpriv, macreg[i], macbackup[i]);
}

/* Poll a one-shot IQK report register with a bounded wait. */
static void _rtl92fe_phy_wait_iqk_done(struct ieee80211_hw *hw, u32 rpt_reg)
{
	u32 elapsed = 0;

	mdelay(IQK_DELAY_TIME);
	while (rtl_get_bbreg(hw, rpt_reg, MASKDWORD) == 0 &&
	       elapsed < IQK_RPT_POLL_LIMIT_MS) {
		mdelay(IQK_RPT_POLL_MS);
		elapsed += IQK_RPT_POLL_MS;
	}
}

/* Fold the LOK result back into the TX-PA LUT (RF 0x33) for the given path,
 * then disable the PA/PAD overrides used during calibration.
 */
static void _rtl92fe_phy_patch_txpa_lut(struct ieee80211_hw *hw,
					  enum radio_path path)
{
	u32 rf_0x58, lut_i, lut_q;
	int i;

	rtl_set_bbreg(hw, RFPGA0_IQK, 0xffffff00, 0);
	rtl_set_rfreg(hw, path, RF8192F_WE_LUT, BIT(4), 1);

	rf_0x58 = rtl_get_rfreg(hw, path, RF8192F_TXMOD, RFREG_OFFSET_MASK);
	lut_i = (rf_0x58 & 0xfc000) >> 14;
	lut_q = (rf_0x58 & 0x003f0) >> 4;

	for (i = 0; i < 8; i++) {
		rtl_set_rfreg(hw, path, RF8192F_TXPA_G3, 0x1c000, i);
		rtl_set_rfreg(hw, path, RF8192F_TXPA_G3, 0x00fc0, lut_i);
		rtl_set_rfreg(hw, path, RF8192F_TXPA_G3, 0x0003f, lut_q);
	}

	rtl_set_rfreg(hw, path, RF8192F_AC, BIT(14), 0);
	rtl_set_rfreg(hw, path, RF8192F_WE_LUT, BIT(4), 0);
	rtl_set_rfreg(hw, path, RF8192F_GAIN_CCA, 0x00810, 0);
}

static u8 _rtl92fe_phy_path_a_iqk(struct ieee80211_hw *hw)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	struct rtl_hal *rtlhal = rtl_hal(rtlpriv);
	u32 reg_eac, reg_e94, reg_e9c, val32;
	u8 rfe = rtlhal->rfe_type;
	u8 result = 0x00;

	rtl_set_bbreg(hw, RFPGA0_IQK, 0xffffff00, 0);

	rtl_set_bbreg(hw, R8192F_FPGA0_ANALOG4, MASKDWORD, 0xccf000c0);
	rtl_set_bbreg(hw, R8192F_ANAPWR1, MASKDWORD, 0x44ffbb44);
	rtl_set_bbreg(hw, R8192F_RX_WAIT_CCA, MASKDWORD, 0x00400040);
	rtl_set_bbreg(hw, ROFDM0_TRXPATHENABLE, MASKDWORD, 0x6f005403);
	rtl_set_bbreg(hw, ROFDM0_TRMUXPAR, MASKDWORD, 0x000804e4);
	rtl_set_bbreg(hw, R8192F_FPGA0_XCD_RF_SW_CTRL, MASKDWORD, 0x04203400);
	rtl_set_bbreg(hw, R8192F_FPGA0_XA_HSSI_PARM1, MASKDWORD, 0x01000100);

	rtl_set_rfreg(hw, RF90_PATH_A, RF8192F_GAIN_CCA, BIT(4), 1);
	rtl_set_rfreg(hw, RF90_PATH_A, RF8192F_GAIN_CCA, BIT(11), 1);
	if (rfe == 7 || rfe == 8 || rfe == 9 || rfe == 12)
		val32 = 0x30;
	else
		val32 = 0xe9;
	rtl_set_rfreg(hw, RF90_PATH_A, RF8192F_PAD_TXG, 0x003ff, val32);

	rtl_set_bbreg(hw, RFPGA0_IQK, 0xffffff00, 0x808000);

	rtl_set_bbreg(hw, RTX_IQK_TONE_A, MASKDWORD, 0x18008c1c);
	rtl_set_bbreg(hw, RRX_IQK_TONE_A, MASKDWORD, 0x38008c1c);
	rtl_set_bbreg(hw, RTX_IQK_TONE_B, MASKDWORD, 0x38008c1c);
	rtl_set_bbreg(hw, RRX_IQK_TONE_B, MASKDWORD, 0x38008c1c);

	rtl_set_bbreg(hw, RTX_IQK_PI_A, MASKDWORD, 0x8214000f);
	rtl_set_bbreg(hw, RRX_IQK_PI_A, MASKDWORD, 0x28140000);

	rtl_set_bbreg(hw, RTX_IQK, MASKDWORD, 0x01007c00);
	rtl_set_bbreg(hw, RRX_IQK, MASKDWORD, 0x01004800);

	/* LO calibration setting */
	rtl_set_bbreg(hw, RIQK_AGC_RSP, MASKDWORD, 0x00e62911);

	/* One shot, path A LOK & IQK */
	rtl_set_bbreg(hw, RIQK_AGC_PTS, MASKDWORD, 0xfa005800);
	rtl_set_bbreg(hw, RIQK_AGC_PTS, MASKDWORD, 0xf8005800);

	_rtl92fe_phy_wait_iqk_done(hw, R8192F_IQK_RPT_TXA);

	reg_eac = rtl_get_bbreg(hw, RRX_POWER_AFTER_IQK_A_2, MASKDWORD);
	reg_e94 = rtl_get_bbreg(hw, RTX_POWER_BEFORE_IQK_A, MASKDWORD);
	reg_e9c = rtl_get_bbreg(hw, RTX_POWER_AFTER_IQK_A, MASKDWORD);

	/* Fold LOK result into TX-PA LUT, then drop calibration overrides. */
	_rtl92fe_phy_patch_txpa_lut(hw, RF90_PATH_A);

	if (!(reg_eac & BIT(28)) &&
	    ((reg_e94 & 0x03ff0000) != 0x01420000) &&
	    ((reg_e9c & 0x03ff0000) != 0x00420000))
		result |= 0x01;
	else
		rtl_dbg(rtlpriv, COMP_RF, DBG_LOUD, "Path A Tx IQK fail!\n");

	return result;
}

static u8 _rtl92fe_phy_path_a_rx_iqk(struct ieee80211_hw *hw)
{
	u32 reg_eac, reg_e94, reg_e9c, reg_ea4, val32;
	u8 result = 0x00;

	rtl_set_bbreg(hw, RFPGA0_IQK, 0xffffff00, 0);

	/* PA/PAD control by 0x56 */
	rtl_set_rfreg(hw, RF90_PATH_A, RF8192F_GAIN_CCA, BIT(1), 1);
	rtl_set_rfreg(hw, RF90_PATH_A, RF8192F_GAIN_P1, RFREG_OFFSET_MASK, 0);
	rtl_set_rfreg(hw, RF90_PATH_A, RF8192F_GAIN_CCA, BIT(11), 1);
	rtl_set_rfreg(hw, RF90_PATH_A, RF8192F_PAD_TXG, 0x003ff, 0x27);

	rtl_set_bbreg(hw, RFPGA0_IQK, 0xffffff00, 0x808000);

	rtl_set_bbreg(hw, RTX_IQK_TONE_A, MASKDWORD, 0x18008c1c);
	rtl_set_bbreg(hw, RRX_IQK_TONE_A, MASKDWORD, 0x38008c1c);
	rtl_set_bbreg(hw, RTX_IQK_TONE_B, MASKDWORD, 0x38008c1c);
	rtl_set_bbreg(hw, RRX_IQK_TONE_B, MASKDWORD, 0x38008c1c);

	rtl_set_bbreg(hw, RTX_IQK_PI_A, MASKDWORD, 0x82160027);
	rtl_set_bbreg(hw, RRX_IQK_PI_A, MASKDWORD, 0x28160000);

	rtl_set_bbreg(hw, RTX_IQK, MASKDWORD, 0x01007c00);
	rtl_set_bbreg(hw, RRX_IQK, MASKDWORD, 0x01004800);

	rtl_set_bbreg(hw, RIQK_AGC_RSP, MASKDWORD, 0x0086a911);

	rtl_set_bbreg(hw, RIQK_AGC_PTS, MASKDWORD, 0xfa005800);
	rtl_set_bbreg(hw, RIQK_AGC_PTS, MASKDWORD, 0xf8005800);

	_rtl92fe_phy_wait_iqk_done(hw, R8192F_IQK_RPT_TXA);

	reg_eac = rtl_get_bbreg(hw, RRX_POWER_AFTER_IQK_A_2, MASKDWORD);
	reg_e94 = rtl_get_bbreg(hw, RTX_POWER_BEFORE_IQK_A, MASKDWORD);
	reg_e9c = rtl_get_bbreg(hw, RTX_POWER_AFTER_IQK_A, MASKDWORD);

	if (!(reg_eac & BIT(28)) &&
	    ((reg_e94 & 0x03ff0000) != 0x01420000) &&
	    ((reg_e9c & 0x03ff0000) != 0x00420000)) {
		result |= 0x01;
	} else {
		rtl_set_bbreg(hw, RFPGA0_IQK, 0xffffff00, 0);
		rtl_set_rfreg(hw, RF90_PATH_A, RF8192F_GAIN_CCA, BIT(11), 0);
		return result;
	}

	val32 = 0x80007c00 | (reg_e94 & 0x3ff0000) |
		((reg_e9c & 0x3ff0000) >> 16);
	rtl_set_bbreg(hw, RTX_IQK, MASKDWORD, val32);

	/* RX IQK */
	rtl_set_bbreg(hw, RFPGA0_IQK, 0xffffff00, 0);

	rtl_set_rfreg(hw, RF90_PATH_A, RF8192F_GAIN_CCA, BIT(1), 1);
	rtl_set_rfreg(hw, RF90_PATH_A, RF8192F_GAIN_P1, RFREG_OFFSET_MASK, 0);
	rtl_set_rfreg(hw, RF90_PATH_A, RF8192F_GAIN_CCA, BIT(11), 1);
	rtl_set_rfreg(hw, RF90_PATH_A, RF8192F_PAD_TXG, 0x003ff, 0x1e0);

	rtl_set_bbreg(hw, R8192F_FPGA0_ANALOG4, MASKDWORD, 0xccf000c0);
	rtl_set_bbreg(hw, R8192F_ANAPWR1, MASKDWORD, 0x44ffbb44);
	rtl_set_bbreg(hw, R8192F_RX_WAIT_CCA, MASKDWORD, 0x00400040);
	rtl_set_bbreg(hw, ROFDM0_TRXPATHENABLE, MASKDWORD, 0x6f005403);
	rtl_set_bbreg(hw, ROFDM0_TRMUXPAR, MASKDWORD, 0x000804e4);
	rtl_set_bbreg(hw, R8192F_FPGA0_XCD_RF_SW_CTRL, MASKDWORD, 0x04203400);
	rtl_set_bbreg(hw, R8192F_FPGA0_XA_HSSI_PARM1, MASKDWORD, 0x01000100);

	rtl_set_bbreg(hw, RFPGA0_IQK, 0xffffff00, 0x808000);

	rtl_set_bbreg(hw, RTX_IQK_TONE_A, MASKDWORD, 0x38008c1c);
	rtl_set_bbreg(hw, RRX_IQK_TONE_A, MASKDWORD, 0x18008c1c);
	rtl_set_bbreg(hw, RTX_IQK_TONE_B, MASKDWORD, 0x38008c1c);
	rtl_set_bbreg(hw, RRX_IQK_TONE_B, MASKDWORD, 0x38008c1c);

	rtl_set_bbreg(hw, RTX_IQK_PI_A, MASKDWORD, 0x82170000);
	rtl_set_bbreg(hw, RRX_IQK_PI_A, MASKDWORD, 0x28170000);

	rtl_set_bbreg(hw, RRX_IQK, MASKDWORD, 0x01004800);

	rtl_set_bbreg(hw, RIQK_AGC_RSP, MASKDWORD, 0x0046a8d1);

	rtl_set_bbreg(hw, RIQK_AGC_PTS, MASKDWORD, 0xfa005800);
	rtl_set_bbreg(hw, RIQK_AGC_PTS, MASKDWORD, 0xf8005800);

	_rtl92fe_phy_wait_iqk_done(hw, R8192F_IQK_RPT_RXA);

	reg_eac = rtl_get_bbreg(hw, RRX_POWER_AFTER_IQK_A_2, MASKDWORD);
	reg_ea4 = rtl_get_bbreg(hw, RRX_POWER_BEFORE_IQK_A_2, MASKDWORD);

	rtl_set_bbreg(hw, RFPGA0_IQK, 0xffffff00, 0);
	rtl_set_rfreg(hw, RF90_PATH_A, RF8192F_GAIN_CCA, BIT(11), 0);
	rtl_set_rfreg(hw, RF90_PATH_A, RF8192F_GAIN_P1, RFREG_OFFSET_MASK,
		      0x02000);

	if (!(reg_eac & BIT(27)) &&
	    ((reg_ea4 & 0x03ff0000) != 0x01320000) &&
	    ((reg_eac & 0x03ff0000) != 0x00360000))
		result |= 0x02;

	return result;
}

static u8 _rtl92fe_phy_path_b_iqk(struct ieee80211_hw *hw)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	struct rtl_hal *rtlhal = rtl_hal(rtlpriv);
	u32 reg_eac, reg_eb4, reg_ebc;
	u8 rfe = rtlhal->rfe_type;
	u8 result = 0x00;

	rtl_set_bbreg(hw, RFPGA0_IQK, 0xffffff00, 0);

	rtl_set_bbreg(hw, R8192F_FPGA0_ANALOG4, MASKDWORD, 0xccf000c0);
	rtl_set_bbreg(hw, R8192F_ANAPWR1, MASKDWORD, 0x44ffbb44);
	rtl_set_bbreg(hw, R8192F_RX_WAIT_CCA, MASKDWORD, 0x00400040);
	rtl_set_bbreg(hw, ROFDM0_TRXPATHENABLE, MASKDWORD, 0x6f005403);
	rtl_set_bbreg(hw, ROFDM0_TRMUXPAR, MASKDWORD, 0x000804e4);
	rtl_set_bbreg(hw, R8192F_FPGA0_XCD_RF_SW_CTRL, MASKDWORD, 0x04203400);
	rtl_set_bbreg(hw, R8192F_FPGA0_XA_HSSI_PARM1, MASKDWORD, 0x01000000);

	rtl_set_rfreg(hw, RF90_PATH_B, RF8192F_GAIN_CCA, BIT(4), 1);
	rtl_set_rfreg(hw, RF90_PATH_B, RF8192F_GAIN_CCA, BIT(11), 1);
	if (rfe == 7 || rfe == 8 || rfe == 9 || rfe == 12)
		rtl_set_rfreg(hw, RF90_PATH_B, RF8192F_PAD_TXG, 0x003ff, 0x30);
	else
		rtl_set_rfreg(hw, RF90_PATH_B, RF8192F_PAD_TXG, 0x00fff, 0xe9);

	rtl_set_bbreg(hw, RFPGA0_IQK, 0xffffff00, 0x808000);

	rtl_set_bbreg(hw, RTX_IQK_TONE_A, MASKDWORD, 0x38008c1c);
	rtl_set_bbreg(hw, RRX_IQK_TONE_A, MASKDWORD, 0x38008c1c);
	rtl_set_bbreg(hw, RTX_IQK_TONE_B, MASKDWORD, 0x18008c1c);
	rtl_set_bbreg(hw, RRX_IQK_TONE_B, MASKDWORD, 0x38008c1c);

	rtl_set_bbreg(hw, RTX_IQK_PI_B, MASKDWORD, 0x8214000f);
	rtl_set_bbreg(hw, RRX_IQK_PI_B, MASKDWORD, 0x28140000);

	rtl_set_bbreg(hw, RTX_IQK, MASKDWORD, 0x01007c00);
	rtl_set_bbreg(hw, RRX_IQK, MASKDWORD, 0x01004800);

	rtl_set_bbreg(hw, RIQK_AGC_RSP, MASKDWORD, 0x00e62911);

	rtl_set_bbreg(hw, RIQK_AGC_PTS, MASKDWORD, 0xfa005800);
	rtl_set_bbreg(hw, RIQK_AGC_PTS, MASKDWORD, 0xf8005800);

	_rtl92fe_phy_wait_iqk_done(hw, R8192F_IQK_RPT_TXB);

	reg_eac = rtl_get_bbreg(hw, RRX_POWER_AFTER_IQK_A_2, MASKDWORD);
	reg_eb4 = rtl_get_bbreg(hw, RTX_POWER_BEFORE_IQK_B, MASKDWORD);
	reg_ebc = rtl_get_bbreg(hw, RTX_POWER_AFTER_IQK_B, MASKDWORD);

	_rtl92fe_phy_patch_txpa_lut(hw, RF90_PATH_B);

	if (!(reg_eac & BIT(31)) &&
	    ((reg_eb4 & 0x03ff0000) != 0x01420000) &&
	    ((reg_ebc & 0x03ff0000) != 0x00420000))
		result |= 0x01;
	else
		rtl_dbg(rtlpriv, COMP_RF, DBG_LOUD, "Path B Tx IQK fail!\n");

	return result;
}

static u8 _rtl92fe_phy_path_b_rx_iqk(struct ieee80211_hw *hw)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	u32 reg_eac, reg_eb4, reg_ebc, reg_ec4, reg_ecc, val32;
	u8 result = 0x00;

	rtl_set_bbreg(hw, RFPGA0_IQK, 0xffffff00, 0);

	rtl_set_rfreg(hw, RF90_PATH_B, RF8192F_GAIN_CCA, BIT(1), 1);
	rtl_set_rfreg(hw, RF90_PATH_B, RF8192F_GAIN_P1, RFREG_OFFSET_MASK, 0);
	rtl_set_rfreg(hw, RF90_PATH_B, RF8192F_GAIN_CCA, BIT(11), 1);
	rtl_set_rfreg(hw, RF90_PATH_B, RF8192F_PAD_TXG, 0x003ff, 0x67);

	rtl_set_bbreg(hw, R8192F_FPGA0_ANALOG4, MASKDWORD, 0xccf000c0);
	rtl_set_bbreg(hw, R8192F_ANAPWR1, MASKDWORD, 0x44ffbb44);
	rtl_set_bbreg(hw, R8192F_RX_WAIT_CCA, MASKDWORD, 0x00400040);
	rtl_set_bbreg(hw, ROFDM0_TRXPATHENABLE, MASKDWORD, 0x6f005403);
	rtl_set_bbreg(hw, ROFDM0_TRMUXPAR, MASKDWORD, 0x000804e4);
	rtl_set_bbreg(hw, R8192F_FPGA0_XCD_RF_SW_CTRL, MASKDWORD, 0x04203400);
	rtl_set_bbreg(hw, R8192F_FPGA0_XA_HSSI_PARM1, MASKDWORD, 0x01000000);

	rtl_set_bbreg(hw, RFPGA0_IQK, 0xffffff00, 0x808000);

	rtl_set_bbreg(hw, RTX_IQK_TONE_A, MASKDWORD, 0x38008c1c);
	rtl_set_bbreg(hw, RRX_IQK_TONE_A, MASKDWORD, 0x38008c1c);
	rtl_set_bbreg(hw, RTX_IQK_TONE_B, MASKDWORD, 0x18008c1c);
	rtl_set_bbreg(hw, RRX_IQK_TONE_B, MASKDWORD, 0x38008c1c);

	rtl_set_bbreg(hw, RTX_IQK_PI_B, MASKDWORD, 0x82160027);
	rtl_set_bbreg(hw, RRX_IQK_PI_B, MASKDWORD, 0x28160000);

	rtl_set_bbreg(hw, RIQK_AGC_RSP, MASKDWORD, 0x0086a911);

	rtl_set_bbreg(hw, RIQK_AGC_PTS, MASKDWORD, 0xfa005800);
	rtl_set_bbreg(hw, RIQK_AGC_PTS, MASKDWORD, 0xf8005800);

	_rtl92fe_phy_wait_iqk_done(hw, R8192F_IQK_RPT_TXB);

	reg_eac = rtl_get_bbreg(hw, RRX_POWER_AFTER_IQK_A_2, MASKDWORD);
	reg_eb4 = rtl_get_bbreg(hw, RTX_POWER_BEFORE_IQK_B, MASKDWORD);
	reg_ebc = rtl_get_bbreg(hw, RTX_POWER_AFTER_IQK_B, MASKDWORD);

	if (!(reg_eac & BIT(31)) &&
	    ((reg_eb4 & 0x03ff0000) != 0x01420000) &&
	    ((reg_ebc & 0x03ff0000) != 0x00420000)) {
		result |= 0x01;
	} else {
		rtl_set_bbreg(hw, RFPGA0_IQK, 0xffffff00, 0);
		rtl_set_rfreg(hw, RF90_PATH_B, RF8192F_GAIN_CCA, BIT(11), 0);
		return result;
	}

	val32 = 0x80007c00 | (reg_eb4 & 0x03ff0000) |
		((reg_ebc >> 16) & 0x03ff);
	rtl_set_bbreg(hw, RTX_IQK, MASKDWORD, val32);

	rtl_set_bbreg(hw, RFPGA0_IQK, 0xffffff00, 0);

	rtl_set_rfreg(hw, RF90_PATH_B, RF8192F_GAIN_CCA, BIT(1), 1);
	rtl_set_rfreg(hw, RF90_PATH_B, RF8192F_GAIN_P1, RFREG_OFFSET_MASK, 0);
	rtl_set_rfreg(hw, RF90_PATH_B, RF8192F_GAIN_CCA, BIT(11), 1);
	rtl_set_rfreg(hw, RF90_PATH_B, RF8192F_PAD_TXG, 0x003ff, 0x1e0);

	rtl_set_bbreg(hw, R8192F_FPGA0_ANALOG4, MASKDWORD, 0xccf000c0);
	rtl_set_bbreg(hw, R8192F_ANAPWR1, MASKDWORD, 0x44ffbb44);
	rtl_set_bbreg(hw, R8192F_RX_WAIT_CCA, MASKDWORD, 0x00400040);
	rtl_set_bbreg(hw, ROFDM0_TRXPATHENABLE, MASKDWORD, 0x6f005403);
	rtl_set_bbreg(hw, ROFDM0_TRMUXPAR, MASKDWORD, 0x000804e4);
	rtl_set_bbreg(hw, R8192F_FPGA0_XCD_RF_SW_CTRL, MASKDWORD, 0x04203400);
	rtl_set_bbreg(hw, R8192F_FPGA0_XA_HSSI_PARM1, MASKDWORD, 0x01000000);

	rtl_set_bbreg(hw, RFPGA0_IQK, 0xffffff00, 0x808000);

	rtl_set_bbreg(hw, RTX_IQK_TONE_A, MASKDWORD, 0x38008c1c);
	rtl_set_bbreg(hw, RRX_IQK_TONE_A, MASKDWORD, 0x38008c1c);
	rtl_set_bbreg(hw, RTX_IQK_TONE_B, MASKDWORD, 0x38008c1c);
	rtl_set_bbreg(hw, RRX_IQK_TONE_B, MASKDWORD, 0x18008c1c);

	rtl_set_bbreg(hw, RTX_IQK_PI_B, MASKDWORD, 0x82170000);
	rtl_set_bbreg(hw, RRX_IQK_PI_B, MASKDWORD, 0x28170000);

	rtl_set_bbreg(hw, RRX_IQK, MASKDWORD, 0x01004800);

	rtl_set_bbreg(hw, RIQK_AGC_RSP, MASKDWORD, 0x0046a911);

	rtl_set_bbreg(hw, RIQK_AGC_PTS, MASKDWORD, 0xfa005800);
	rtl_set_bbreg(hw, RIQK_AGC_PTS, MASKDWORD, 0xf8005800);

	_rtl92fe_phy_wait_iqk_done(hw, R8192F_IQK_RPT_RXB);

	reg_eac = rtl_get_bbreg(hw, RRX_POWER_AFTER_IQK_A_2, MASKDWORD);
	reg_ec4 = rtl_get_bbreg(hw, RRX_POWER_BEFORE_IQK_B_2, MASKDWORD);
	reg_ecc = rtl_get_bbreg(hw, RRX_POWER_AFTER_IQK_B_2, MASKDWORD);

	rtl_set_bbreg(hw, RFPGA0_IQK, 0xffffff00, 0);
	rtl_set_bbreg(hw, R8192F_FPGA0_XA_HSSI_PARM1, MASKDWORD, 0x01000100);

	rtl_set_rfreg(hw, RF90_PATH_B, RF8192F_GAIN_CCA, BIT(11), 0);
	rtl_set_rfreg(hw, RF90_PATH_B, RF8192F_GAIN_CCA, BIT(1), 0);
	rtl_set_rfreg(hw, RF90_PATH_B, RF8192F_GAIN_P1, RFREG_OFFSET_MASK,
		      0x02000);

	if (!(reg_eac & BIT(30)) &&
	    ((reg_ec4 & 0x03ff0000) != 0x01320000) &&
	    ((reg_ecc & 0x03ff0000) != 0x00360000))
		result |= 0x02;
	else
		rtl_dbg(rtlpriv, COMP_RF, DBG_LOUD, "Path B Rx IQK fail!\n");

	return result;
}

static void _rtl92fe_phy_path_a_fill_iqk_matrix(struct ieee80211_hw *hw,
						  bool iqk_ok, long result[][8],
						  u8 final_candidate,
						  bool btxonly)
{
	u32 oldval_0, x, tx0_a, reg;
	long y, tx0_c;

	if (final_candidate == 0xFF) {
		return;
	} else if (iqk_ok) {
		oldval_0 = (rtl_get_bbreg(hw, ROFDM0_XATXIQIMBALANCE,
					  MASKDWORD) >> 22) & 0x3FF;
		x = result[final_candidate][0];
		if ((x & 0x00000200) != 0)
			x = x | 0xFFFFFC00;
		tx0_a = (x * oldval_0) >> 8;
		rtl_set_bbreg(hw, ROFDM0_XATXIQIMBALANCE, 0x3FF, tx0_a);
		rtl_set_bbreg(hw, ROFDM0_ECCATHRESHOLD, BIT(31),
			      ((x * oldval_0 >> 7) & 0x1));
		y = result[final_candidate][1];
		if ((y & 0x00000200) != 0)
			y = y | 0xFFFFFC00;
		tx0_c = (y * oldval_0) >> 8;
		rtl_set_bbreg(hw, ROFDM0_XCTXAFE, 0xF0000000,
			      ((tx0_c & 0x3C0) >> 6));
		rtl_set_bbreg(hw, ROFDM0_XATXIQIMBALANCE, 0x003F0000,
			      (tx0_c & 0x3F));
		rtl_set_bbreg(hw, ROFDM0_ECCATHRESHOLD, BIT(29),
			      ((y * oldval_0 >> 7) & 0x1));

		if (btxonly)
			return;

		reg = result[final_candidate][2];
		if (reg <= 0xf0 || reg >= 0x110)
			reg = 0x100;
		rtl_set_bbreg(hw, ROFDM0_XARXIQIMBALANCE, 0x3FF, reg);

		reg = result[final_candidate][3] & 0x3F;
		rtl_set_bbreg(hw, ROFDM0_XARXIQIMBALANCE, 0xFC00, reg);

		reg = (result[final_candidate][3] >> 6) & 0xF;
		rtl_set_bbreg(hw, ROFDM0_RXIQEXTANTA, 0xF0000000, reg);
	}
}

static void _rtl92fe_phy_path_b_fill_iqk_matrix(struct ieee80211_hw *hw,
						  bool iqk_ok, long result[][8],
						  u8 final_candidate,
						  bool btxonly)
{
	u32 oldval_1, x, tx1_a, reg;
	long y, tx1_c;

	if (final_candidate == 0xFF) {
		return;
	} else if (iqk_ok) {
		oldval_1 = (rtl_get_bbreg(hw, ROFDM0_XBTXIQIMBALANCE,
					  MASKDWORD) >> 22) & 0x3FF;
		x = result[final_candidate][4];
		if ((x & 0x00000200) != 0)
			x = x | 0xFFFFFC00;
		tx1_a = (x * oldval_1) >> 8;
		rtl_set_bbreg(hw, ROFDM0_XBTXIQIMBALANCE, 0x3FF, tx1_a);
		rtl_set_bbreg(hw, ROFDM0_ECCATHRESHOLD, BIT(27),
			      ((x * oldval_1 >> 7) & 0x1));
		y = result[final_candidate][5];
		if ((y & 0x00000200) != 0)
			y = y | 0xFFFFFC00;
		tx1_c = (y * oldval_1) >> 8;
		rtl_set_bbreg(hw, ROFDM0_XDTXAFE, 0xF0000000,
			      ((tx1_c & 0x3C0) >> 6));
		rtl_set_bbreg(hw, ROFDM0_XBTXIQIMBALANCE, 0x003F0000,
			      (tx1_c & 0x3F));
		rtl_set_bbreg(hw, ROFDM0_ECCATHRESHOLD, BIT(25),
			      ((y * oldval_1 >> 7) & 0x1));

		if (btxonly)
			return;

		reg = result[final_candidate][6];
		rtl_set_bbreg(hw, ROFDM0_XBRXIQIMBALANCE, 0x3FF, reg);

		reg = result[final_candidate][7] & 0x3F;
		rtl_set_bbreg(hw, ROFDM0_XBRXIQIMBALANCE, 0xFC00, reg);

		reg = (result[final_candidate][7] >> 6) & 0xF;
		rtl_set_bbreg(hw, ROFDM0_TXCOEFF2, 0x000000F0, reg);
	}
}

static bool _rtl92fe_phy_simularity_compare(struct ieee80211_hw *hw,
					      long result[][8], u8 c1, u8 c2)
{
	u32 i, j, diff, simularity_bitmap, bound;
	u8 final_candidate[2] = { 0xFF, 0xFF };
	bool bresult = true;
	s32 tmp1, tmp2;

	bound = 8;
	simularity_bitmap = 0;

	for (i = 0; i < bound; i++) {
		if ((i == 1) || (i == 3) || (i == 5) || (i == 7)) {
			if ((result[c1][i] & 0x00000200) != 0)
				tmp1 = result[c1][i] | 0xFFFFFC00;
			else
				tmp1 = result[c1][i];

			if ((result[c2][i] & 0x00000200) != 0)
				tmp2 = result[c2][i] | 0xFFFFFC00;
			else
				tmp2 = result[c2][i];
		} else {
			tmp1 = result[c1][i];
			tmp2 = result[c2][i];
		}

		diff = (tmp1 > tmp2) ? (tmp1 - tmp2) : (tmp2 - tmp1);

		if (diff > MAX_TOLERANCE) {
			if ((i == 2 || i == 6) && !simularity_bitmap) {
				if (result[c1][i] + result[c1][i + 1] == 0)
					final_candidate[(i / 4)] = c2;
				else if (result[c2][i] + result[c2][i + 1] == 0)
					final_candidate[(i / 4)] = c1;
				else
					simularity_bitmap |= (1 << i);
			} else {
				simularity_bitmap |= (1 << i);
			}
		}
	}

	if (simularity_bitmap == 0) {
		for (i = 0; i < (bound / 4); i++) {
			if (final_candidate[i] != 0xFF) {
				for (j = i * 4; j < (i + 1) * 4 - 2; j++)
					result[3][j] =
						result[final_candidate[i]][j];
				bresult = false;
			}
		}
		return bresult;
	}
	if (!(simularity_bitmap & 0x03)) {
		for (i = 0; i < 2; i++)
			result[3][i] = result[c1][i];
	}
	if (!(simularity_bitmap & 0x0c)) {
		for (i = 2; i < 4; i++)
			result[3][i] = result[c1][i];
	}
	if (!(simularity_bitmap & 0x30)) {
		for (i = 4; i < 6; i++)
			result[3][i] = result[c1][i];
	}
	if (!(simularity_bitmap & 0xc0)) {
		for (i = 6; i < 8; i++)
			result[3][i] = result[c1][i];
	}
	return false;
}

static void _rtl92fe_phy_iq_calibrate(struct ieee80211_hw *hw,
					long result[][8], u8 t, bool is2t)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	struct rtl_phy *rtlphy = &rtlpriv->phy;
	u8 patha_ok, pathb_ok;
	u32 rx_gain_a, rx_gain_b;
	u32 i;
	const u32 retrycount = 2;
	/* ADDA / MAC / BB backup windows for the 8192F IQK. */
	static const u32 adda_reg[IQK_ADDA_REG_NUM] = {
		R8192F_ANAPWR1, R8192F_RX_WAIT_CCA
	};
	static const u32 iqk_mac_reg[IQK_MAC_REG_NUM] = {
		REG_TXPAUSE, REG_BCN_CTRL, REG_BCN_CTRL + 1, REG_GPIO_MUXCFG
	};
	static const u32 iqk_bb_reg[IQK_BB_REG_NUM] = {
		ROFDM0_TRXPATHENABLE, ROFDM0_TRMUXPAR,
		R8192F_FPGA0_XCD_RF_SW_CTRL, RFPGA0_XA_RFINTERFACEOE,
		RFPGA0_XB_RFINTERFACEOE, RFPGA0_XAB_RFINTERFACESW,
		R8192F_FPGA0_ANALOG4, R8192F_RX_WAIT_CCA, RCCK0_SYSTEM
	};

	rtl_set_bbreg(hw, RFPGA0_IQK, 0xffffff00, 0);

	rx_gain_a = rtl_get_bbreg(hw, ROFDM0_XAAGCCORE1, MASKDWORD);
	rx_gain_b = rtl_get_bbreg(hw, ROFDM0_XBAGCCORE1, MASKDWORD);

	if (t == 0) {
		_rtl92fe_phy_save_regs(hw, adda_reg, rtlphy->adda_backup,
					 IQK_ADDA_REG_NUM);
		_rtl92fe_phy_save_mac_registers(hw, iqk_mac_reg,
						  rtlphy->iqk_mac_backup);
		_rtl92fe_phy_save_regs(hw, iqk_bb_reg, rtlphy->iqk_bb_backup,
					 IQK_BB_REG_NUM);
	}

	/* Turn the calibration data path on (path-A ADDA). */
	rtl_set_bbreg(hw, R8192F_FPGA0_XCD_RF_PARM, BIT(31), 0x1);

	/* MAC pause + clear BT IO-sel so the IQK tone is undisturbed. */
	rtl_write_byte(rtlpriv, REG_TXPAUSE, 0xff);

	if (is2t) {
		/* Park path B while path A calibrates. */
		rtl_set_bbreg(hw, RFPGA0_IQK, 0xffffff00, 0x000000);
		rtl_set_rfreg(hw, RF90_PATH_B, RF8192F_AC, RFREG_OFFSET_MASK,
			      0x10000);
		rtl_set_bbreg(hw, RFPGA0_IQK, 0xffffff00, 0x808000);
	}

	for (i = 0; i < retrycount; i++) {
		patha_ok = _rtl92fe_phy_path_a_iqk(hw);
		if (patha_ok == 0x01) {
			result[t][0] = (rtl_get_bbreg(hw,
						      RTX_POWER_BEFORE_IQK_A,
						      MASKDWORD) & 0x3ff0000)
						      >> 16;
			result[t][1] = (rtl_get_bbreg(hw, RTX_POWER_AFTER_IQK_A,
						      MASKDWORD) & 0x3ff0000)
						      >> 16;
			break;
		}
		result[t][0] = 0x100;
		result[t][1] = 0x0;
	}

	for (i = 0; i < retrycount; i++) {
		patha_ok = _rtl92fe_phy_path_a_rx_iqk(hw);
		if (patha_ok == 0x03) {
			result[t][2] = (rtl_get_bbreg(hw,
						      RRX_POWER_BEFORE_IQK_A_2,
						      MASKDWORD) & 0x3ff0000)
						      >> 16;
			result[t][3] = (rtl_get_bbreg(hw,
						      RRX_POWER_AFTER_IQK_A_2,
						      MASKDWORD) & 0x3ff0000)
						      >> 16;
			break;
		}
		result[t][2] = 0x100;
		result[t][3] = 0x0;
	}

	if (patha_ok == 0x00)
		rtl_dbg(rtlpriv, COMP_RF, DBG_LOUD, "Path A IQK failed!\n");

	if (is2t) {
		for (i = 0; i < retrycount; i++) {
			pathb_ok = _rtl92fe_phy_path_b_iqk(hw);
			if (pathb_ok == 0x01) {
				result[t][4] = (rtl_get_bbreg(hw,
						RTX_POWER_BEFORE_IQK_B,
						MASKDWORD) & 0x3ff0000) >> 16;
				result[t][5] = (rtl_get_bbreg(hw,
						RTX_POWER_AFTER_IQK_B,
						MASKDWORD) & 0x3ff0000) >> 16;
				break;
			}
			result[t][4] = 0x100;
			result[t][5] = 0x0;
		}

		for (i = 0; i < retrycount; i++) {
			pathb_ok = _rtl92fe_phy_path_b_rx_iqk(hw);
			if (pathb_ok == 0x03) {
				result[t][6] = (rtl_get_bbreg(hw,
						RRX_POWER_BEFORE_IQK_B_2,
						MASKDWORD) & 0x3ff0000) >> 16;
				result[t][7] = (rtl_get_bbreg(hw,
						RRX_POWER_AFTER_IQK_B_2,
						MASKDWORD) & 0x3ff0000) >> 16;
				break;
			}
			result[t][6] = 0x100;
			result[t][7] = 0x0;
		}

		if (pathb_ok == 0x00)
			rtl_dbg(rtlpriv, COMP_RF, DBG_LOUD,
				"Path B IQK failed!\n");
	}

	/* Back to BB mode, restore the analog calibration data path. */
	rtl_set_bbreg(hw, RFPGA0_IQK, 0xffffff00, 0);

	if (is2t) {
		/* Path B was parked into RF standby (RF reg 0x00 = 0x10000) at
		 * dev/MEASURED-phy.c.md sec 5. */
		rtl_set_rfreg(hw, RF90_PATH_B, RF8192F_AC, RFREG_OFFSET_MASK,
			      0x31DD5);
		rtl_set_rfreg(hw, RF90_PATH_B, RF_CHNLBW, RFREG_OFFSET_MASK,
			      rtlphy->rfreg_chnlval[0]);
	}

	rtl_set_bbreg(hw, R8192F_FPGA0_ANALOG4, MASKDWORD, 0xcc0000c0);
	rtl_set_bbreg(hw, R8192F_ANAPWR1, MASKDWORD, 0x44bbbb44);
	rtl_set_bbreg(hw, R8192F_RX_WAIT_CCA, MASKDWORD, 0x80408040);
	rtl_set_bbreg(hw, ROFDM0_TRXPATHENABLE, MASKDWORD, 0x6f005433);
	rtl_set_bbreg(hw, ROFDM0_TRMUXPAR, MASKDWORD, 0x000004e4);
	rtl_set_bbreg(hw, R8192F_FPGA0_XCD_RF_SW_CTRL, MASKDWORD, 0x04003400);
	rtl_set_bbreg(hw, R8192F_FPGA0_XA_HSSI_PARM1, MASKDWORD, 0x01000100);

	if (t != 0) {
		_rtl92fe_phy_reload_regs(hw, adda_reg, rtlphy->adda_backup,
					   IQK_ADDA_REG_NUM);
		_rtl92fe_phy_reload_mac_registers(hw, iqk_mac_reg,
						    rtlphy->iqk_mac_backup);
		_rtl92fe_phy_reload_regs(hw, iqk_bb_reg, rtlphy->iqk_bb_backup,
					   IQK_BB_REG_NUM);

		rtl_set_bbreg(hw, R8192F_FPGA0_XCD_RF_PARM, BIT(31), 0x0);

		/* Restore RX initial gain. */
		rtl_set_bbreg(hw, ROFDM0_XAAGCCORE1, 0xff, 0x50);
		rtl_set_bbreg(hw, ROFDM0_XAAGCCORE1, 0xff, rx_gain_a & 0xff);
		if (is2t) {
			rtl_set_bbreg(hw, ROFDM0_XBAGCCORE1, 0xff, 0x50);
			rtl_set_bbreg(hw, ROFDM0_XBAGCCORE1, 0xff,
				      rx_gain_b & 0xff);
		}
	}
}

void rtl92fe_phy_iq_calibrate(struct ieee80211_hw *hw, bool b_recovery)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	struct rtl_phy *rtlphy = &rtlpriv->phy;
	long result[4][8];
	u8 i, final_candidate;
	bool b_patha_ok, b_pathb_ok;
	long reg_e94, reg_e9c, reg_ea4;
	long reg_eb4, reg_ebc, reg_ec4;
	bool is12simular, is13simular, is23simular;
	long reg_tmp = 0;
	u32 path_a_0xdf, path_a_0x35, path_b_0xdf, path_b_0x35;
	u32 iqk_bb_reg[IQK_BB_REG_NUM] = {
		ROFDM0_XARXIQIMBALANCE,
		ROFDM0_XBRXIQIMBALANCE,
		ROFDM0_ECCATHRESHOLD,
		ROFDM0_AGCRSSITABLE,
		ROFDM0_XATXIQIMBALANCE,
		ROFDM0_XBTXIQIMBALANCE,
		ROFDM0_XCTXAFE,
		ROFDM0_XDTXAFE,
		ROFDM0_RXIQEXTANTA
	};

	if (b_recovery) {
		_rtl92fe_phy_reload_regs(hw, iqk_bb_reg,
					   rtlphy->iqk_bb_backup, 9);
		return;
	}

	/* Snapshot the RF gain/CCA + gain-P1 ports so they can be restored
	 * after the calibration overrides them.
	 */
	path_a_0xdf = rtl_get_rfreg(hw, RF90_PATH_A, RF8192F_GAIN_CCA,
				    RFREG_OFFSET_MASK);
	path_a_0x35 = rtl_get_rfreg(hw, RF90_PATH_A, RF8192F_GAIN_P1,
				    RFREG_OFFSET_MASK);
	path_b_0xdf = rtl_get_rfreg(hw, RF90_PATH_B, RF8192F_GAIN_CCA,
				    RFREG_OFFSET_MASK);
	path_b_0x35 = rtl_get_rfreg(hw, RF90_PATH_B, RF8192F_GAIN_P1,
				    RFREG_OFFSET_MASK);

	for (i = 0; i < 8; i++) {
		result[0][i] = 0;
		result[1][i] = 0;
		result[2][i] = 0;

		if ((i == 0) || (i == 2) || (i == 4) || (i == 6))
			result[3][i] = 0x100;
		else
			result[3][i] = 0;
	}
	final_candidate = 0xff;
	b_patha_ok = false;
	b_pathb_ok = false;
	is12simular = false;
	is23simular = false;
	is13simular = false;

	for (i = 0; i < 3; i++) {
		_rtl92fe_phy_iq_calibrate(hw, result, i, true);
		if (i == 1) {
			is12simular = _rtl92fe_phy_simularity_compare(hw,
									result,
									0, 1);
			if (is12simular) {
				final_candidate = 0;
				break;
			}
		}

		if (i == 2) {
			is13simular = _rtl92fe_phy_simularity_compare(hw,
									result,
									0, 2);
			if (is13simular) {
				final_candidate = 0;
				break;
			}
			is23simular = _rtl92fe_phy_simularity_compare(hw,
									result,
									1, 2);
			if (is23simular) {
				final_candidate = 1;
			} else {
				for (i = 0; i < 8; i++)
					reg_tmp += result[3][i];

				if (reg_tmp != 0)
					final_candidate = 3;
				else
					final_candidate = 0xff;
			}
		}
	}

	reg_e94 = result[3][0];
	reg_e9c = result[3][1];
	reg_ea4 = result[3][2];
	reg_eb4 = result[3][4];
	reg_ebc = result[3][5];
	reg_ec4 = result[3][6];

	if (final_candidate != 0xff) {
		reg_e94 = result[final_candidate][0];
		rtlphy->reg_e94 = reg_e94;
		reg_e9c = result[final_candidate][1];
		rtlphy->reg_e9c = reg_e9c;
		reg_ea4 = result[final_candidate][2];
		reg_eb4 = result[final_candidate][4];
		rtlphy->reg_eb4 = reg_eb4;
		reg_ebc = result[final_candidate][5];
		rtlphy->reg_ebc = reg_ebc;
		reg_ec4 = result[final_candidate][6];
		b_patha_ok = true;
		b_pathb_ok = true;
	} else {
		rtlphy->reg_e94 = 0x100;
		rtlphy->reg_eb4 = 0x100;
		rtlphy->reg_e9c = 0x0;
		rtlphy->reg_ebc = 0x0;
	}

	/* Re-center the IQK tone before applying the matrix. */
	rtl_set_bbreg(hw, RTX_IQK_TONE_A, 0x3ff00000, 0x100);
	rtl_set_bbreg(hw, R8192F_NP_ANTA, 0x3ff, 0);
	rtl_set_bbreg(hw, RTX_IQK_TONE_B, 0x3ff00000, 0x100);
	rtl_set_bbreg(hw, R8192F_TAP_UPD_97F, 0x3ff, 0);

	if (reg_e94 != 0)
		_rtl92fe_phy_path_a_fill_iqk_matrix(hw, b_patha_ok, result,
						      final_candidate,
						      (reg_ea4 == 0));

	if (reg_eb4 != 0)
		_rtl92fe_phy_path_b_fill_iqk_matrix(hw, b_pathb_ok, result,
						      final_candidate,
						      (reg_ec4 == 0));

	/* Restore the snapshotted RF gain ports. */
	rtl_set_rfreg(hw, RF90_PATH_A, RF8192F_GAIN_CCA, RFREG_OFFSET_MASK,
		      path_a_0xdf);
	rtl_set_rfreg(hw, RF90_PATH_A, RF8192F_GAIN_P1, RFREG_OFFSET_MASK,
		      path_a_0x35);
	rtl_set_rfreg(hw, RF90_PATH_B, RF8192F_GAIN_CCA, RFREG_OFFSET_MASK,
		      path_b_0xdf);
	rtl_set_rfreg(hw, RF90_PATH_B, RF8192F_GAIN_P1, RFREG_OFFSET_MASK,
		      path_b_0x35);

	if (final_candidate < 4) {
		for (i = 0; i < IQK_MATRIX_REG_NUM; i++)
			rtlphy->iqk_matrix[0].value[0][i] =
				result[final_candidate][i];

		rtlphy->iqk_matrix[0].iqk_done = true;
	}
	_rtl92fe_phy_save_regs(hw, iqk_bb_reg, rtlphy->iqk_bb_backup, 9);
}

static void _rtl92fe_phy_lc_calibrate(struct ieee80211_hw *hw, bool is2t)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	u32 psf_backup, channel;
	u8 tx_mode;
	unsigned int poll;

	(void)is2t;
	psf_backup = rtl_get_bbreg(hw, ROFDM0_TXPSEUDONOISEWGT,
				   BIT(31) | BIT(30));
	rtl_set_bbreg(hw, ROFDM0_TXPSEUDONOISEWGT, BIT(31) | BIT(30), 0);
	tx_mode = rtl_read_byte(rtlpriv, 0xd03);
	if (tx_mode & 0x70)
		rtl_write_byte(rtlpriv, 0xd03, tx_mode & 0x8f);
	else
		rtl_write_byte(rtlpriv, REG_TXPAUSE, 0xff);

	/* Stock 8192F restores the entire channel word even after a timeout. */
	channel = rtl_get_rfreg(hw, RF90_PATH_A, 0x18, RFREG_OFFSET_MASK);
	rtl_set_rfreg(hw, RF90_PATH_A, 0x18, RFREG_OFFSET_MASK,
		      channel | BIT(15));
	for (poll = 0; poll < 100; poll++) {
		if (!rtl_get_rfreg(hw, RF90_PATH_A, 0x18, BIT(15)))
			break;
		mdelay(10);
	}
	if (poll == 100)
		pr_warn_ratelimited("LCK timed out; restoring the operating channel\n");
	rtl_set_rfreg(hw, RF90_PATH_A, 0x18, RFREG_OFFSET_MASK, channel);

	if (tx_mode & 0x70)
		rtl_write_byte(rtlpriv, 0xd03, tx_mode);
	else
		rtl_write_byte(rtlpriv, REG_TXPAUSE, 0);
	rtl_set_bbreg(hw, ROFDM0_TXPSEUDONOISEWGT, BIT(31) | BIT(30),
		      psf_backup);
	rtl_set_bbreg(hw, RFPGA0_RFMOD, BIT(25), 0);
	rtl_set_bbreg(hw, RFPGA0_RFMOD, BIT(25), 1);
}

void rtl92fe_phy_lc_calibrate(struct ieee80211_hw *hw)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	struct rtl_phy *rtlphy = &rtlpriv->phy;

	if (rtlpriv->mac80211.act_scanning || rtlphy->lck_inprogress)
		return;
	rtlphy->lck_inprogress = true;
	_rtl92fe_phy_lc_calibrate(hw, rtlphy->num_total_rfpath > 1);
	rtlphy->lck_inprogress = false;
}

static void _rtl92fe_phy_set_rfpath_switch(struct ieee80211_hw *hw,
					     bool bmain, bool is2t)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	struct rtl_hal *rtlhal = rtl_hal(rtl_priv(hw));

	rtl_dbg(rtlpriv, COMP_INIT, DBG_LOUD, "\n");

	if (is_hal_stop(rtlhal)) {
		u8 u1btmp;

		u1btmp = rtl_read_byte(rtlpriv, REG_LEDCFG0);
		rtl_write_byte(rtlpriv, REG_LEDCFG0, u1btmp | BIT(7));
		rtl_set_bbreg(hw, RFPGA0_XAB_RFPARAMETER, BIT(13), 0x01);
	}
	if (is2t) {
		if (bmain)
			rtl_set_bbreg(hw, RFPGA0_XB_RFINTERFACEOE,
				      BIT(5) | BIT(6), 0x1);
		else
			rtl_set_bbreg(hw, RFPGA0_XB_RFINTERFACEOE,
				      BIT(5) | BIT(6), 0x2);
	} else {
		rtl_set_bbreg(hw, RFPGA0_XAB_RFINTERFACESW, BIT(8) | BIT(9), 0);
		rtl_set_bbreg(hw, 0x914, MASKLWORD, 0x0201);

		if (bmain)
			rtl_set_bbreg(hw, RFPGA0_XA_RFINTERFACEOE,
				      BIT(14) | BIT(13) | BIT(12), 0);
		else
			rtl_set_bbreg(hw, RFPGA0_XA_RFINTERFACEOE,
				      BIT(14) | BIT(13) | BIT(12), 1);
	}
}

void rtl92fe_phy_set_rfpath_switch(struct ieee80211_hw *hw, bool bmain)
{
	_rtl92fe_phy_set_rfpath_switch(hw, bmain, false);
}

bool rtl92fe_phy_set_io_cmd(struct ieee80211_hw *hw, enum io_type iotype)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	struct rtl_phy *rtlphy = &rtlpriv->phy;
	bool postprocessing = false;

	rtl_dbg(rtlpriv, COMP_CMD, DBG_TRACE,
		"-->IO Cmd(%#x), set_io_inprogress(%d)\n",
		iotype, rtlphy->set_io_inprogress);
	do {
		switch (iotype) {
		case IO_CMD_RESUME_DM_BY_SCAN:
			rtl_dbg(rtlpriv, COMP_CMD, DBG_TRACE,
				"[IO CMD] Resume DM after scan.\n");
			postprocessing = true;
			break;
		case IO_CMD_PAUSE_BAND0_DM_BY_SCAN:
			rtl_dbg(rtlpriv, COMP_CMD, DBG_TRACE,
				"[IO CMD] Pause DM before scan.\n");
			postprocessing = true;
			break;
		default:
			rtl_dbg(rtlpriv, COMP_ERR, DBG_LOUD,
				"switch case %#x not processed\n", iotype);
			break;
		}
	} while (false);
	if (postprocessing && !rtlphy->set_io_inprogress) {
		rtlphy->set_io_inprogress = true;
		rtlphy->current_io_type = iotype;
	} else {
		return false;
	}
	rtl92fe_phy_set_io(hw);
	rtl_dbg(rtlpriv, COMP_CMD, DBG_TRACE, "IO Type(%#x)\n", iotype);
	return true;
}

static void rtl92fe_phy_set_io(struct ieee80211_hw *hw)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	struct rtl_phy *rtlphy = &rtlpriv->phy;
	struct dig_t *dm_dig = &rtlpriv->dm_digtable;

	rtl_dbg(rtlpriv, COMP_CMD, DBG_TRACE,
		"--->Cmd(%#x), set_io_inprogress(%d)\n",
		rtlphy->current_io_type, rtlphy->set_io_inprogress);
	switch (rtlphy->current_io_type) {
	case IO_CMD_RESUME_DM_BY_SCAN:
		rtl92fe_dm_write_dig(hw, rtlphy->initgain_backup.xaagccore1);
		rtl92fe_dm_write_cck_cca_thres(hw,
						 rtlphy->initgain_backup.cca);
		rtl92fe_phy_set_txpower_level(hw, rtlphy->current_channel);
		break;
	case IO_CMD_PAUSE_BAND0_DM_BY_SCAN:
		rtlphy->initgain_backup.xaagccore1 = dm_dig->cur_igvalue;
		rtl92fe_dm_write_dig(hw, 0x17);
		rtlphy->initgain_backup.cca = dm_dig->cur_cck_cca_thres;
		rtl92fe_dm_write_cck_cca_thres(hw, 0x40);
		break;
	default:
		rtl_dbg(rtlpriv, COMP_ERR, DBG_LOUD,
			"switch case %#x not processed\n",
			rtlphy->current_io_type);
		break;
	}
	rtlphy->set_io_inprogress = false;
	rtl_dbg(rtlpriv, COMP_CMD, DBG_TRACE,
		"(%#x)\n", rtlphy->current_io_type);
}

static void rtl92fe_phy_set_rf_on(struct ieee80211_hw *hw)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);

	rtl_write_byte(rtlpriv, REG_SPS0_CTRL, 0x2b);
	rtl_write_byte(rtlpriv, REG_SYS_FUNC_EN, 0xE3);
	rtl_write_byte(rtlpriv, REG_SYS_FUNC_EN, 0xE2);
	rtl_write_byte(rtlpriv, REG_SYS_FUNC_EN, 0xE3);
	rtl_write_byte(rtlpriv, REG_TXPAUSE, 0x00);
}

static void _rtl92fe_phy_set_rf_sleep(struct ieee80211_hw *hw)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);

	rtl_write_byte(rtlpriv, REG_TXPAUSE, 0xFF);
	rtl_set_rfreg(hw, RF90_PATH_A, 0x00, RFREG_OFFSET_MASK, 0x00);

	rtl_write_byte(rtlpriv, REG_SYS_FUNC_EN, 0xE2);
	rtl_write_byte(rtlpriv, REG_SPS0_CTRL, 0x22);
}

static bool _rtl92fe_phy_set_rf_power_state(struct ieee80211_hw *hw,
					      enum rf_pwrstate rfpwr_state)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	struct rtl_pci_priv *pcipriv = rtl_pcipriv(hw);
	struct rtl_mac *mac = rtl_mac(rtl_priv(hw));
	struct rtl_ps_ctl *ppsc = rtl_psc(rtl_priv(hw));
	bool bresult = true;
	u8 i, queue_id;
	struct rtl8192_tx_ring *ring = NULL;

	switch (rfpwr_state) {
	case ERFON:
		if ((ppsc->rfpwr_state == ERFOFF) &&
		    RT_IN_PS_LEVEL(ppsc, RT_RF_OFF_LEVL_HALT_NIC)) {
			bool rtstatus;
			u32 initializecount = 0;

			do {
				initializecount++;
				rtl_dbg(rtlpriv, COMP_RF, DBG_DMESG,
					"IPS Set eRf nic enable\n");
				rtstatus = rtl_ps_enable_nic(hw);
			} while (!rtstatus && (initializecount < 10));
			RT_CLEAR_PS_LEVEL(ppsc, RT_RF_OFF_LEVL_HALT_NIC);
		} else {
			rtl_dbg(rtlpriv, COMP_RF, DBG_DMESG,
				"Set ERFON sleeping:%d ms\n",
				jiffies_to_msecs(jiffies -
						 ppsc->last_sleep_jiffies));
			ppsc->last_awake_jiffies = jiffies;
			rtl92fe_phy_set_rf_on(hw);
		}
		if (mac->link_state == MAC80211_LINKED)
			rtlpriv->cfg->ops->led_control(hw, LED_CTL_LINK);
		else
			rtlpriv->cfg->ops->led_control(hw, LED_CTL_NO_LINK);
		break;
	case ERFOFF:
		for (queue_id = 0, i = 0;
		     queue_id < RTL_PCI_MAX_TX_QUEUE_COUNT;) {
			ring = &pcipriv->dev.tx_ring[queue_id];
			if (queue_id == BEACON_QUEUE ||
			    skb_queue_len(&ring->queue) == 0) {
				queue_id++;
				continue;
			} else {
				rtl_dbg(rtlpriv, COMP_ERR, DBG_WARNING,
					"eRf Off/Sleep: %d times TcbBusyQueue[%d] =%d before doze!\n",
					(i + 1), queue_id,
					skb_queue_len(&ring->queue));

				udelay(10);
				i++;
			}
			if (i >= MAX_DOZE_WAITING_TIMES_9x) {
				rtl_dbg(rtlpriv, COMP_ERR, DBG_WARNING,
					"\n ERFSLEEP: %d times TcbBusyQueue[%d] = %d !\n",
					MAX_DOZE_WAITING_TIMES_9x,
					queue_id,
					skb_queue_len(&ring->queue));
				break;
			}
		}

		if (ppsc->reg_rfps_level & RT_RF_OFF_LEVL_HALT_NIC) {
			rtl_dbg(rtlpriv, COMP_RF, DBG_DMESG,
				"IPS Set eRf nic disable\n");
			rtl_ps_disable_nic(hw);
			RT_SET_PS_LEVEL(ppsc, RT_RF_OFF_LEVL_HALT_NIC);
		} else {
			if (ppsc->rfoff_reason == RF_CHANGE_BY_IPS) {
				rtlpriv->cfg->ops->led_control(hw,
							LED_CTL_NO_LINK);
			} else {
				rtlpriv->cfg->ops->led_control(hw,
							LED_CTL_POWER_OFF);
			}
		}
		break;
	case ERFSLEEP:
		if (ppsc->rfpwr_state == ERFOFF)
			break;
		for (queue_id = 0, i = 0;
		     queue_id < RTL_PCI_MAX_TX_QUEUE_COUNT;) {
			ring = &pcipriv->dev.tx_ring[queue_id];
			if (skb_queue_len(&ring->queue) == 0) {
				queue_id++;
				continue;
			} else {
				rtl_dbg(rtlpriv, COMP_ERR, DBG_WARNING,
					"eRf Off/Sleep: %d times TcbBusyQueue[%d] =%d before doze!\n",
					(i + 1), queue_id,
					skb_queue_len(&ring->queue));
				udelay(10);
				i++;
			}
			if (i >= MAX_DOZE_WAITING_TIMES_9x) {
				rtl_dbg(rtlpriv, COMP_ERR, DBG_WARNING,
					"\n ERFSLEEP: %d times TcbBusyQueue[%d] = %d !\n",
					MAX_DOZE_WAITING_TIMES_9x,
					queue_id,
					skb_queue_len(&ring->queue));
				break;
			}
		}
		rtl_dbg(rtlpriv, COMP_RF, DBG_DMESG,
			"Set ERFSLEEP awaked:%d ms\n",
			jiffies_to_msecs(jiffies -
					 ppsc->last_awake_jiffies));
		ppsc->last_sleep_jiffies = jiffies;
		_rtl92fe_phy_set_rf_sleep(hw);
		break;
	default:
		rtl_dbg(rtlpriv, COMP_ERR, DBG_LOUD,
			"switch case %#x not processed\n", rfpwr_state);
		bresult = false;
		break;
	}
	if (bresult)
		ppsc->rfpwr_state = rfpwr_state;
	return bresult;
}

bool rtl92fe_phy_set_rf_power_state(struct ieee80211_hw *hw,
				      enum rf_pwrstate rfpwr_state)
{
	struct rtl_ps_ctl *ppsc = rtl_psc(rtl_priv(hw));
	bool bresult = false;

	if (rfpwr_state == ppsc->rfpwr_state)
		return bresult;
	bresult = _rtl92fe_phy_set_rf_power_state(hw, rfpwr_state);
	return bresult;
}
