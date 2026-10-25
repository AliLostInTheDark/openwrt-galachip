/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright(c) 2026  Realtek RTL8192FE clean-room PCIe WiFi driver authors */

#ifndef __RTL8192FE_PHY_H__
#define __RTL8192FE_PHY_H__

/* MAX_TX_COUNT must stay 4 so the efuse tx-power read loop walks the table
 * in the order the parser expects.
 */
#define MAX_TX_COUNT				4
#define TX_1S					0
#define TX_2S					1
#define TX_3S					2
#define TX_4S					3

#define MAX_POWER_INDEX				0x3f

#define MAX_PRECMD_CNT				16
#define MAX_RFDEPENDCMD_CNT			16
#define MAX_POSTCMD_CNT				16

#define MAX_DOZE_WAITING_TIMES_9x		64

#define RT_CANNOT_IO(hw)			false

/* IQK / LCK working-set sizes. The RTL8192F keeps a small ... -- dev/MEASURED-phy.h.md sec 1. */
#undef IQK_ADDA_REG_NUM
#undef IQK_MAC_REG_NUM
#define IQK_ADDA_REG_NUM			2
#define IQK_MAC_REG_NUM				4
#define IQK_BB_REG_NUM				9
#define MAX_TOLERANCE				5
#define IQK_DELAY_TIME				15
#define IQK_RPT_POLL_MS				5
#define IQK_RPT_POLL_LIMIT_MS			21

#define RF6052_MAX_PATH				2

enum swchnlcmd_id {
	CMDID_END,
	CMDID_SET_TXPOWEROWER_LEVEL,
	CMDID_BBREGWRITE10,
	CMDID_WRITEPORT_ULONG,
	CMDID_WRITEPORT_USHORT,
	CMDID_WRITEPORT_UCHAR,
	CMDID_RF_WRITEREG,
};

struct swchnlcmd {
	enum swchnlcmd_id cmdid;
	u32 para1;
	u32 para2;
	u32 msdelay;
};

enum baseband_config_type {
	BASEBAND_CONFIG_PHY_REG = 0,
	BASEBAND_CONFIG_AGC_TAB = 1,
};

enum ant_div_type {
	NO_ANTDIV = 0xFF,
	CG_TRX_HW_ANTDIV = 0x01,
	CGCS_RX_HW_ANTDIV = 0x02,
	FIXED_HW_ANTDIV = 0x03,
	CG_TRX_SMART_ANTDIV = 0x04,
	CGCS_RX_SW_ANTDIV = 0x05,
};

u32 rtl92fe_phy_query_bb_reg(struct ieee80211_hw *hw,
			       u32 regaddr, u32 bitmask);
void rtl92fe_phy_set_bb_reg(struct ieee80211_hw *hw,
			      u32 regaddr, u32 bitmask, u32 data);
u32 rtl92fe_phy_query_rf_reg(struct ieee80211_hw *hw,
			       enum radio_path rfpath, u32 regaddr,
			       u32 bitmask);
void rtl92fe_phy_set_rf_reg(struct ieee80211_hw *hw,
			      enum radio_path rfpath, u32 regaddr,
			      u32 bitmask, u32 data);
bool rtl92fe_phy_mac_config(struct ieee80211_hw *hw);
bool rtl92fe_phy_bb_config(struct ieee80211_hw *hw);
bool rtl92fe_phy_rf_config(struct ieee80211_hw *hw);
void rtl92fe_phy_get_hw_reg_originalvalue(struct ieee80211_hw *hw);
void rtl92fe_phy_get_txpower_level(struct ieee80211_hw *hw,
				     long *powerlevel);
void rtl92fe_phy_set_txpower_level(struct ieee80211_hw *hw, u8 channel);
void rtl92fe_phy_scan_operation_backup(struct ieee80211_hw *hw,
					 u8 operation);
void rtl92fe_phy_set_bw_mode_callback(struct ieee80211_hw *hw);
void rtl92fe_phy_set_bw_mode(struct ieee80211_hw *hw,
			       enum nl80211_channel_type ch_type);
void rtl92fe_phy_sw_chnl_callback(struct ieee80211_hw *hw);
u8 rtl92fe_phy_sw_chnl(struct ieee80211_hw *hw);
void rtl92fe_phy_iq_calibrate(struct ieee80211_hw *hw, bool b_recovery);
void rtl92fe_phy_lc_calibrate(struct ieee80211_hw *hw);
void rtl92fe_phy_set_rfpath_switch(struct ieee80211_hw *hw, bool bmain);
bool rtl92fe_phy_config_rf_with_headerfile(struct ieee80211_hw *hw,
					     enum radio_path rfpath);
bool rtl92fe_phy_set_io_cmd(struct ieee80211_hw *hw, enum io_type iotype);
bool rtl92fe_phy_set_rf_power_state(struct ieee80211_hw *hw,
				      enum rf_pwrstate rfpwr_state);

#endif
