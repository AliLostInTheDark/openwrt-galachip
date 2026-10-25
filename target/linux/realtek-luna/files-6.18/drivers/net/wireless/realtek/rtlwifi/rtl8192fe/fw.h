/* SPDX-License-Identifier: GPL-2.0 */
/* Clean-room RTL8192F PCIe 802.11n driver firmware interface. */

#ifndef __RTL8192FE_FW_H__
#define __RTL8192FE_FW_H__

/* Firmware download region inside the 8051 code RAM.  The FW image is pushed
 * in 4 KiB pages and lands in the 0x1000..0x5FFF window; at most eight pages
 * are valid for this part.
 */
#define FW_8192F_SIZE				0x8000
#define FW_8192F_START_ADDRESS			0x1000
#define FW_8192F_END_ADDRESS			0x5FFF
#define FW_8192F_PAGE_SIZE			4096
#define FW_8192F_POLLING_DELAY			5
#define FW_8192F_POLLING_TIMEOUT_COUNT		3000

/* The RTL8192F firmware blob carries an optional 32-byte ... -- dev/MEASURED-fw.h.md sec 1. */
#define IS_FW_HEADER_EXIST(_pfwhdr)	\
	((le16_to_cpu(_pfwhdr->signature) & 0xFFF0) == 0x92F0)

#define USE_OLD_WOWLAN_DEBUG_FW			0

/* H2C command payload lengths (bytes). */
#define H2C_92F_RSVDPAGE_LOC_LEN		5
#define H2C_92F_PWEMODE_LENGTH			7
#define H2C_92F_JOINBSSRPT_LENGTH		1
#define H2C_92F_AP_OFFLOAD_LENGTH		3
#define H2C_92F_WOWLAN_LENGTH			3
#define H2C_92F_KEEP_ALIVE_CTRL_LENGTH		3
#if (USE_OLD_WOWLAN_DEBUG_FW == 0)
#define H2C_92F_REMOTE_WAKE_CTRL_LEN		1
#else
#define H2C_92F_REMOTE_WAKE_CTRL_LEN		3
#endif
#define H2C_92F_AOAC_GLOBAL_INFO_LEN		2
#define H2C_92F_AOAC_RSVDPAGE_LOC_LEN		7

/* Firmware power-state bits used in the RPWM handshake.
 * BIT[2:0] = HW state, BIT[3] = protocol PS state (1 active / 0 sleep),
 * BIT[4]   = sub-state.
 */
#define FW_PS_RF_ON				BIT(2)
#define FW_PS_REGISTER_ACTIVE			BIT(3)

#define FW_PS_ACK				BIT(6)
#define FW_PS_TOGGLE				BIT(7)

/* RPWM clock select: BIT[0] = 1 -> 32 kHz, 0 -> 40 MHz. */
#define FW_PS_CLOCK_OFF				BIT(0)	/* 32 kHz */
#define FW_PS_CLOCK_ON				0	/* 40 MHz */

#define FW_PS_STATE_MASK			(0x0F)
#define FW_PS_STATE_HW_MASK			(0x07)
#define FW_PS_STATE_INT_MASK			(0x3F)

#define FW_PS_STATE(x)				(FW_PS_STATE_MASK & (x))

#define FW_PS_STATE_ALL_ON_92F			(FW_PS_CLOCK_ON)
#define FW_PS_STATE_RF_ON_92F			(FW_PS_CLOCK_ON)
#define FW_PS_STATE_RF_OFF_92F			(FW_PS_CLOCK_ON)
#define FW_PS_STATE_RF_OFF_LOW_PWR		(FW_PS_CLOCK_OFF)

/* PwrMode (H2C id 0x20) power-state encoding. */
#define FW_PWR_STATE_ACTIVE	((FW_PS_RF_ON) | (FW_PS_REGISTER_ACTIVE))
#define FW_PWR_STATE_RF_OFF	0

#define FW_PS_IS_ACK(x)		((x) & FW_PS_ACK)

#define IS_IN_LOW_POWER_STATE_92F(__state)		\
	(FW_PS_STATE(__state) == FW_PS_CLOCK_OFF)

/* H2C element ids for the RTL8192F 8051 mailbox.  The id set matches the
 * 8192-series MCU interface used by the mainline rtl8192ee driver.
 */
enum rtl8192f_h2c_cmd {
	H2C_92F_RSVDPAGE = 0,
	H2C_92F_MSRRPT = 1,
	H2C_92F_SCAN = 2,
	H2C_92F_KEEP_ALIVE_CTRL = 3,
	H2C_92F_DISCONNECT_DECISION = 4,
#if (USE_OLD_WOWLAN_DEBUG_FW == 1)
	H2C_92F_WO_WLAN = 5,
#endif
	H2C_92F_INIT_OFFLOAD = 6,
#if (USE_OLD_WOWLAN_DEBUG_FW == 1)
	H2C_92F_REMOTE_WAKE_CTRL = 7,
#endif
	H2C_92F_AP_OFFLOAD = 8,
	H2C_92F_BCN_RSVDPAGE = 9,
	H2C_92F_PROBERSP_RSVDPAGE = 10,

	H2C_92F_SETPWRMODE = 0x20,
	H2C_92F_PS_TUNING_PARA = 0x21,
	H2C_92F_PS_TUNING_PARA2 = 0x22,
	H2C_92F_PS_LPS_PARA = 0x23,
	H2C_92F_P2P_PS_OFFLOAD = 0x24,

#if (USE_OLD_WOWLAN_DEBUG_FW == 0)
	H2C_92F_WO_WLAN = 0x80,
	H2C_92F_REMOTE_WAKE_CTRL = 0x81,
	H2C_92F_AOAC_GLOBAL_INFO = 0x82,
	H2C_92F_AOAC_RSVDPAGE = 0x83,
#endif
	H2C_92F_RA_MASK = 0x40,
	H2C_92F_RSSI_REPORT = 0x42,
	H2C_92F_SELECTIVE_SUSPEND_ROF_CMD,
	H2C_92F_P2P_PS_MODE,
	H2C_92F_PSD_RESULT,
	/* CTW window command for P2P. */
	H2C_92F_P2P_PS_CTW_CMD,
	MAX_92F_H2CCMD
};

#define pagenum_128(_len)	\
	(u32)(((_len) >> 7) + ((_len) & 0x7F ? 1 : 0))

/* PwrMode (id 0x20) parameter packing. */
#define SET_H2CCMD_PWRMODE_PARM_MODE(__ph2ccmd, __val)			\
	*(u8 *)__ph2ccmd = __val;
#define SET_H2CCMD_PWRMODE_PARM_RLBM(__cmd, __val)			\
	u8p_replace_bits(__cmd + 1, __val, GENMASK(3, 0))
#define SET_H2CCMD_PWRMODE_PARM_SMART_PS(__cmd, __val)			\
	u8p_replace_bits(__cmd + 1, __val, GENMASK(7, 4))
#define SET_H2CCMD_PWRMODE_PARM_AWAKE_INTERVAL(__cmd, __val)		\
	*(u8 *)(__cmd + 2) = __val;
#define SET_H2CCMD_PWRMODE_PARM_ALL_QUEUE_UAPSD(__cmd, __val)		\
	*(u8 *)(__cmd + 3) = __val;
#define SET_H2CCMD_PWRMODE_PARM_PWR_STATE(__cmd, __val)			\
	*(u8 *)(__cmd + 4) = __val;
#define SET_H2CCMD_PWRMODE_PARM_BYTE5(__cmd, __val)			\
	*(u8 *)(__cmd + 5) = __val;

/* RSVD-page location (id 0) parameter packing. */
#define SET_H2CCMD_RSVDPAGE_LOC_PROBE_RSP(__ph2ccmd, __val)		\
	*(u8 *)__ph2ccmd = __val;
#define SET_H2CCMD_RSVDPAGE_LOC_PSPOLL(__ph2ccmd, __val)		\
	*(u8 *)(__ph2ccmd + 1) = __val;
#define SET_H2CCMD_RSVDPAGE_LOC_NULL_DATA(__ph2ccmd, __val)		\
	*(u8 *)(__ph2ccmd + 2) = __val;
#define SET_H2CCMD_RSVDPAGE_LOC_QOS_NULL_DATA(__ph2ccmd, __val)		\
	*(u8 *)(__ph2ccmd + 3) = __val;
#define SET_H2CCMD_RSVDPAGE_LOC_BT_QOS_NULL_DATA(__ph2ccmd, __val)	\
	*(u8 *)(__ph2ccmd + 4) = __val;

/* Media-status report (id 1) parameter packing. */
#define SET_H2CCMD_MSRRPT_PARM_OPMODE(__cmd, __val)			\
	u8p_replace_bits(__cmd, __val, BIT(0))
#define SET_H2CCMD_MSRRPT_PARM_MACID_IND(__cmd, __val)			\
	u8p_replace_bits(__cmd, __val, BIT(1))
#define SET_H2CCMD_MSRRPT_PARM_MACID(__cmd, __val)			\
	*(u8 *)(__cmd + 1) = __val;
#define SET_H2CCMD_MSRRPT_PARM_MACID_END(__cmd, __val)			\
	*(u8 *)(__cmd + 2) = __val;

int rtl92fe_download_fw(struct ieee80211_hw *hw, bool buse_wake_on_wlan_fw);
void rtl92fe_fill_h2c_cmd(struct ieee80211_hw *hw, u8 element_id,
			    u32 cmd_len, u8 *cmdbuffer);
void rtl92fe_firmware_selfreset(struct ieee80211_hw *hw);
void rtl92fe_set_fw_pwrmode_cmd(struct ieee80211_hw *hw, u8 mode);
void rtl92fe_set_fw_media_status_rpt_cmd(struct ieee80211_hw *hw, u8 mstatus,
					 u8 macid);
void rtl92fe_set_fw_rsvdpagepkt(struct ieee80211_hw *hw, bool b_dl_finished);
void rtl92fe_set_p2p_ps_offload_cmd(struct ieee80211_hw *hw,
				      u8 p2p_ps_state);
void rtl92fe_c2h_ra_report_handler(struct ieee80211_hw *hw,
				     u8 *cmd_buf, u8 cmd_len);

#endif
