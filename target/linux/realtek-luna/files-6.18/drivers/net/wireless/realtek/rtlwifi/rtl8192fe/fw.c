// SPDX-License-Identifier: GPL-2.0
/* Clean-room RTL8192F PCIe 802.11n driver: firmware download + H2C/C2H. */

#include "../wifi.h"
#include "../pci.h"
#include "../base.h"
#include "../core.h"
#include "../efuse.h"
#include "reg.h"
#include "def.h"
#include "fw.h"
#include "trx.h"
#include "dm.h"

/* Arm/disarm the 8051 firmware-download path.  Enabling sets MCUFWDL_EN and
 * clears the RAM-boot-checksum-fail latch so a fresh image can be pushed;
 * disabling drops MCUFWDL_EN once the push is complete.
 */
static void _rtl92fe_enable_fw_download(struct ieee80211_hw *hw, bool enable)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	u8 tmp;

	if (enable) {
		/* Arm SRAM-download mode (MCUFWDLEN = REG_MCUFWDL BIT0) so the
		 * 8051 code-RAM window accepts the FW-FIFO writes that follow. */
		rtl_write_byte(rtlpriv, REG_MCUFWDL,
			       rtl_read_byte(rtlpriv, REG_MCUFWDL) | BIT(0));

		/* ★★ DRAIN THE ARM BEFORE THE FIFO IS WRITTEN -- and read this
		 * dev/MEASURED-fw.c.md sec 1. */
		tmp = rtl_read_byte(rtlpriv, REG_MCUFWDL + 2);
		rtl_dbg(rtlpriv, COMP_FW, DBG_LOUD,
			"8051 run bit before download: MCUFWDL+2=0x%02x (bit3=%u)\n",
			tmp, !!(tmp & BIT(3)));
		rtl_write_byte(rtlpriv, REG_MCUFWDL + 2, tmp & ~BIT(3));

		mdelay(1);
	} else {
		tmp = rtl_read_byte(rtlpriv, REG_MCUFWDL);
		rtl_write_byte(rtlpriv, REG_MCUFWDL, tmp & 0xfe);
	}
}

/* Push the firmware payload to the 8051 code RAM, one 4 KiB ... -- dev/MEASURED-fw.c.md sec 2. */
#define FW_DL_FIFO_ADDR		0x4000	/* RTL8192F FW-RAM window (8192EE uses 0x1000) */

/* The RTL8192F FW-download FIFO on this PCIe host is dword-access only: the
 * generic core byte-loop (rtl_fw_block_write) locks the SoC host bus partway
 * through the first page. Select the page, then push the page as little-endian
 * dwords (page sizes are 4-byte aligned, so the tail byte path is unused). */
static void _rtl92fe_fw_page_write_dw(struct ieee80211_hw *hw, u32 page,
				      const u8 *buffer, u32 size)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	u8 value8;
	u32 i;

	value8 = (rtl_read_byte(rtlpriv, REG_MCUFWDL + 2) & 0xF8) |
		 (u8)(page & 0x07);
	rtl_write_byte(rtlpriv, REG_MCUFWDL + 2, value8);

	rtl_dbg(rtlpriv, COMP_FW, DBG_LOUD,
		"FW page %u: %u byte(s) -> FIFO 0x%04x\n",
		page, size, (unsigned int)FW_DL_FIFO_ADDR);

	/* ★★ ONE READBACK PER PAGE, NOT ONE PER DWORD -- and the ...
	 * dev/MEASURED-fw.c.md sec 3. */
	for (i = 0; i + 4 <= size; i += 4)
		rtl_write_dword(rtlpriv, FW_DL_FIFO_ADDR + i,
				(u32)buffer[i] | ((u32)buffer[i + 1] << 8) |
				((u32)buffer[i + 2] << 16) |
				((u32)buffer[i + 3] << 24));
	for (; i < size; i++)
		rtl_write_byte(rtlpriv, FW_DL_FIFO_ADDR + i, buffer[i]);

	(void)rtl_read_byte(rtlpriv, REG_MCUFWDL);	/* drain, once */

	rtl_dbg(rtlpriv, COMP_FW, DBG_LOUD, "FW page %u written\n", page);
}

static void _rtl92fe_write_fw(struct ieee80211_hw *hw,
				enum version_8192f version,
				u8 *buffer, u32 size)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	u8 *bufferptr = buffer;
	u32 pagenums, remainsize;
	u32 page, offset;

	rtl_dbg(rtlpriv, COMP_FW, DBG_LOUD, "FW size is %d bytes,\n", size);

	rtl_fill_dummy(bufferptr, &size);

	pagenums = size / FW_8192F_PAGE_SIZE;
	remainsize = size % FW_8192F_PAGE_SIZE;

	if (pagenums > 8)
		pr_err("Page numbers should not be greater than 8\n");

	for (page = 0; page < pagenums; page++) {
		offset = page * FW_8192F_PAGE_SIZE;
		_rtl92fe_fw_page_write_dw(hw, page, (bufferptr + offset),
					  FW_8192F_PAGE_SIZE);
		udelay(2);
	}

	if (remainsize) {
		offset = pagenums * FW_8192F_PAGE_SIZE;
		page = pagenums;
		_rtl92fe_fw_page_write_dw(hw, page, (bufferptr + offset),
					  remainsize);
	}
}

/* Wait for the RAM-code checksum report, latch the FW-ready handshake, then
 * self-reset the 8051 and poll for WINTINI_RDY (firmware init complete).
 * Every poll loop is capped so a stuck part fails cleanly (timeout != ready).
 */
static int _rtl92fe_fw_free_to_go(struct ieee80211_hw *hw)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	int err = -EIO;
	u32 counter = 0;
	u32 value32;

	do {
		value32 = rtl_read_dword(rtlpriv, REG_MCUFWDL);
	} while ((counter++ < FW_8192F_POLLING_TIMEOUT_COUNT) &&
		 (!(value32 & FWDL_CHKSUM_RPT)));

	if (counter >= FW_8192F_POLLING_TIMEOUT_COUNT) {
		pr_err("chksum report fail! REG_MCUFWDL:0x%08x\n", value32);
		goto exit;
	}

	value32 = rtl_read_dword(rtlpriv, REG_MCUFWDL);
	value32 |= MCUFWDL_RDY;
	value32 &= ~WINTINI_RDY;
	rtl_write_dword(rtlpriv, REG_MCUFWDL, value32);

	rtl92fe_firmware_selfreset(hw);
	counter = 0;

	do {
		value32 = rtl_read_dword(rtlpriv, REG_MCUFWDL);
		if (value32 & WINTINI_RDY)
			return 0;

		udelay(FW_8192F_POLLING_DELAY * 10);
	} while (counter++ < FW_8192F_POLLING_TIMEOUT_COUNT);

	pr_err("Polling FW ready fail!! REG_MCUFWDL:0x%08x. count = %d\n",
	       value32, counter);

exit:
	return err;
}

int rtl92fe_download_fw(struct ieee80211_hw *hw, bool buse_wake_on_wlan_fw)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	struct rtl_hal *rtlhal = rtl_hal(rtl_priv(hw));
	struct rtlwifi_firmware_header *pfwheader;
	u8 *pfwdata;
	u32 fwsize;
	enum version_8192f version = rtlhal->version;

	if (!rtlhal->pfirmware)
		return 1;

	/* The header's multi-byte fields are little-endian on the wire; read
	 * them through the typed __le accessors so the big-endian host gets the
	 * right values for the version/size bookkeeping below.
	 */
	pfwheader = (struct rtlwifi_firmware_header *)rtlhal->pfirmware;
	rtlhal->fw_version = le16_to_cpu(pfwheader->version);
	rtlhal->fw_subversion = pfwheader->subversion;
	pfwdata = (u8 *)rtlhal->pfirmware;
	fwsize = rtlhal->fwsize;
	rtl_dbg(rtlpriv, COMP_FW, DBG_DMESG,
		"normal Firmware SIZE %d\n", fwsize);

	if (IS_FW_HEADER_EXIST(pfwheader)) {
		rtl_dbg(rtlpriv, COMP_FW, DBG_DMESG,
			"Firmware Version(%d), Signature(%#x),Size(%d)\n",
			le16_to_cpu(pfwheader->version),
			le16_to_cpu(pfwheader->signature),
			(int)sizeof(struct rtlwifi_firmware_header));

		/* Strip the 32-byte header before pushing to the 8051. */
		pfwdata = pfwdata + sizeof(struct rtlwifi_firmware_header);
		fwsize = fwsize - sizeof(struct rtlwifi_firmware_header);
	} else {
		rtl_dbg(rtlpriv, COMP_FW, DBG_DMESG,
			"Firmware no Header, Signature(%#x)\n",
			le16_to_cpu(pfwheader->signature));
	}

	/* If the MAC is already up from a prior boot and the FW-download latch
	 * is still set, drop it and bounce the 8051 before re-downloading.
	 */
	if (rtlhal->mac_func_enable) {
		if (rtl_read_byte(rtlpriv, REG_MCUFWDL) & BIT(7)) {
			rtl_write_byte(rtlpriv, REG_MCUFWDL, 0);
			rtl92fe_firmware_selfreset(hw);
		}
	}

	_rtl92fe_enable_fw_download(hw, true);
	_rtl92fe_write_fw(hw, version, pfwdata, fwsize);
	_rtl92fe_enable_fw_download(hw, false);

	return _rtl92fe_fw_free_to_go(hw);
}

/* The HMETFR free-flag register reports which mailbox the 8051 has already
 * drained.  A clear bit for our box means the previous H2C was consumed.
 */
static bool _rtl92fe_check_fw_read_last_h2c(struct ieee80211_hw *hw,
					      u8 boxnum)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	u8 val_hmetfr;
	bool result = false;

	val_hmetfr = rtl_read_byte(rtlpriv, REG_HMETFR);
	if (((val_hmetfr >> boxnum) & BIT(0)) == 0)
		result = true;

	return result;
}

static void _rtl92fe_fill_h2c_command(struct ieee80211_hw *hw, u8 element_id,
					u32 cmd_len, u8 *cmdbuffer)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	struct rtl_hal *rtlhal = rtl_hal(rtl_priv(hw));
	struct rtl_ps_ctl *ppsc = rtl_psc(rtl_priv(hw));
	u8 boxnum;
	u16 box_reg = 0, box_extreg = 0;
	u8 u1b_tmp;
	bool isfw_read = false;
	u8 buf_index = 0;
	bool bwrite_sucess = false;
	u8 wait_h2c_limmit = 100;
	u8 boxcontent[4], boxextcontent[4];
	u32 h2c_waitcounter = 0;
	unsigned long flag;
	u8 idx;

	if (ppsc->dot11_psmode != EACTIVE ||
	    ppsc->inactive_pwrstate == ERFOFF) {
		rtl_dbg(rtlpriv, COMP_CMD, DBG_LOUD,
			"FillH2CCommand8192F(): Return because RF is off!!!\n");
		return;
	}

	rtl_dbg(rtlpriv, COMP_CMD, DBG_LOUD, "come in\n");

	/* 1. Serialise H2C submission against concurrent setters. */
	while (true) {
		spin_lock_irqsave(&rtlpriv->locks.h2c_lock, flag);
		if (rtlhal->h2c_setinprogress) {
			rtl_dbg(rtlpriv, COMP_CMD, DBG_LOUD,
				"H2C set in progress! Wait to set..element_id(%d).\n",
				element_id);

			while (rtlhal->h2c_setinprogress) {
				spin_unlock_irqrestore(&rtlpriv->locks.h2c_lock,
						       flag);
				h2c_waitcounter++;
				rtl_dbg(rtlpriv, COMP_CMD, DBG_LOUD,
					"Wait 100 us (%d times)...\n",
					h2c_waitcounter);
				udelay(100);

				if (h2c_waitcounter > 1000)
					return;
				spin_lock_irqsave(&rtlpriv->locks.h2c_lock,
						  flag);
			}
			spin_unlock_irqrestore(&rtlpriv->locks.h2c_lock, flag);
		} else {
			rtlhal->h2c_setinprogress = true;
			spin_unlock_irqrestore(&rtlpriv->locks.h2c_lock, flag);
			break;
		}
	}

	while (!bwrite_sucess) {
		/* 2. Pick up the next round-robin mailbox. */
		boxnum = rtlhal->last_hmeboxnum;
		switch (boxnum) {
		case 0:
			box_reg = REG_HMEBOX_0;
			box_extreg = REG_HMEBOX_EXT_0;
			break;
		case 1:
			box_reg = REG_HMEBOX_1;
			box_extreg = REG_HMEBOX_EXT_1;
			break;
		case 2:
			box_reg = REG_HMEBOX_2;
			box_extreg = REG_HMEBOX_EXT_2;
			break;
		case 3:
			box_reg = REG_HMEBOX_3;
			box_extreg = REG_HMEBOX_EXT_3;
			break;
		default:
			rtl_dbg(rtlpriv, COMP_ERR, DBG_LOUD,
				"switch case %#x not processed\n", boxnum);
			break;
		}

		/* 3. Confirm the mailbox is empty / drained by the 8051. */
		isfw_read = false;
		u1b_tmp = rtl_read_byte(rtlpriv, REG_CR);

		if (u1b_tmp != 0xea) {
			isfw_read = true;
		} else {
			if (rtl_read_byte(rtlpriv, REG_TXDMA_STATUS) == 0xea ||
			    rtl_read_byte(rtlpriv, REG_TXPKT_EMPTY) == 0xea)
				rtl_write_byte(rtlpriv, REG_SYS_CFG1 + 3, 0xff);
		}

		if (isfw_read) {
			wait_h2c_limmit = 100;
			isfw_read =
				_rtl92fe_check_fw_read_last_h2c(hw, boxnum);
			while (!isfw_read) {
				wait_h2c_limmit--;
				if (wait_h2c_limmit == 0) {
					rtl_dbg(rtlpriv, COMP_CMD, DBG_LOUD,
						"Waiting too long for FW read clear HMEBox(%d)!!!\n",
						boxnum);
					break;
				}
				udelay(10);
				isfw_read =
					_rtl92fe_check_fw_read_last_h2c(hw,
									  boxnum);
				u1b_tmp = rtl_read_byte(rtlpriv, 0x130);
				rtl_dbg(rtlpriv, COMP_CMD, DBG_LOUD,
					"Waiting for FW read clear HMEBox(%d)!!! 0x130 = %2x\n",
					boxnum, u1b_tmp);
			}
		}

		/* If the FW never drained the prior H2C, drop this one. */
		if (!isfw_read) {
			rtl_dbg(rtlpriv, COMP_CMD, DBG_LOUD,
				"Write H2C reg BOX[%d] fail,Fw don't read.\n",
				boxnum);
			break;
		}

		/* 4. Pack the H2C into the mailbox. Short commands (<=3 bytes)
		 * dev/MEASURED-fw.c.md sec 4. */
		memset(boxcontent, 0, sizeof(boxcontent));
		memset(boxextcontent, 0, sizeof(boxextcontent));
		boxcontent[0] = element_id;
		rtl_dbg(rtlpriv, COMP_CMD, DBG_LOUD,
			"Write element_id box_reg(%4x) = %2x\n",
			box_reg, element_id);

		switch (cmd_len) {
		case 1:
		case 2:
		case 3:
			memcpy((u8 *)(boxcontent) + 1,
			       cmdbuffer + buf_index, cmd_len);

			for (idx = 0; idx < 4; idx++) {
				rtl_write_byte(rtlpriv, box_reg + idx,
					       boxcontent[idx]);
			}
			break;
		case 4:
		case 5:
		case 6:
		case 7:
			memcpy((u8 *)(boxextcontent),
			       cmdbuffer + buf_index + 3, cmd_len - 3);
			memcpy((u8 *)(boxcontent) + 1,
			       cmdbuffer + buf_index, 3);

			for (idx = 0; idx < 4; idx++) {
				rtl_write_byte(rtlpriv, box_extreg + idx,
					       boxextcontent[idx]);
			}

			for (idx = 0; idx < 4; idx++) {
				rtl_write_byte(rtlpriv, box_reg + idx,
					       boxcontent[idx]);
			}
			break;
		default:
			rtl_dbg(rtlpriv, COMP_ERR, DBG_LOUD,
				"switch case %#x not processed\n", cmd_len);
			break;
		}

		bwrite_sucess = true;

		rtlhal->last_hmeboxnum = boxnum + 1;
		if (rtlhal->last_hmeboxnum == 4)
			rtlhal->last_hmeboxnum = 0;

		rtl_dbg(rtlpriv, COMP_CMD, DBG_LOUD,
			"pHalData->last_hmeboxnum  = %d\n",
			rtlhal->last_hmeboxnum);
	}

	spin_lock_irqsave(&rtlpriv->locks.h2c_lock, flag);
	rtlhal->h2c_setinprogress = false;
	spin_unlock_irqrestore(&rtlpriv->locks.h2c_lock, flag);

	rtl_dbg(rtlpriv, COMP_CMD, DBG_LOUD, "go out\n");
}

void rtl92fe_fill_h2c_cmd(struct ieee80211_hw *hw,
			    u8 element_id, u32 cmd_len, u8 *cmdbuffer)
{
	struct rtl_hal *rtlhal = rtl_hal(rtl_priv(hw));
	u32 tmp_cmdbuf[2];

	if (!rtlhal->fw_ready) {
		WARN_ONCE(true,
			  "rtl8192fe: error H2C cmd because of Fw download fail!!!\n");
		return;
	}

	memset(tmp_cmdbuf, 0, 8);
	memcpy(tmp_cmdbuf, cmdbuffer, cmd_len);
	_rtl92fe_fill_h2c_command(hw, element_id, cmd_len, (u8 *)&tmp_cmdbuf);
}

/* Bounce the 8051 micro-controller: gate the reserved-control write lock,
 * drop CPU_ENABLE, settle, then re-enable.  Used after the FW-ready latch and
 * by the download path's stale-image recovery.
 */
void rtl92fe_firmware_selfreset(struct ieee80211_hw *hw)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	u8 u1b_tmp;

	u1b_tmp = rtl_read_byte(rtlpriv, REG_RSV_CTRL + 1);
	rtl_write_byte(rtlpriv, REG_RSV_CTRL + 1, (u1b_tmp & (~BIT(0))));

	u1b_tmp = rtl_read_byte(rtlpriv, REG_SYS_FUNC_EN + 1);
	rtl_write_byte(rtlpriv, REG_SYS_FUNC_EN + 1, (u1b_tmp & (~BIT(2))));

	udelay(50);

	u1b_tmp = rtl_read_byte(rtlpriv, REG_RSV_CTRL + 1);
	rtl_write_byte(rtlpriv, REG_RSV_CTRL + 1, (u1b_tmp | BIT(0)));

	u1b_tmp = rtl_read_byte(rtlpriv, REG_SYS_FUNC_EN + 1);
	rtl_write_byte(rtlpriv, REG_SYS_FUNC_EN + 1, (u1b_tmp | BIT(2)));

	rtl_dbg(rtlpriv, COMP_INIT, DBG_LOUD,
		"  _8051Reset8192F(): 8051 reset success .\n");
}

void rtl92fe_set_fw_pwrmode_cmd(struct ieee80211_hw *hw, u8 mode)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	u8 u1_h2c_set_pwrmode[H2C_92F_PWEMODE_LENGTH] = { 0 };
	struct rtl_ps_ctl *ppsc = rtl_psc(rtl_priv(hw));
	u8 rlbm, power_state = 0, byte5 = 0;
	u8 awake_intvl;	/* DTIM = (awake_intvl - 1) */
	struct rtl_btc_ops *btc_ops = rtlpriv->btcoexist.btc_ops;
	bool bt_ctrl_lps = (rtlpriv->cfg->ops->get_btc_status() ?
			    btc_ops->btc_is_bt_ctrl_lps(rtlpriv) : false);
	bool bt_lps_on = (rtlpriv->cfg->ops->get_btc_status() ?
			  btc_ops->btc_is_bt_lps_on(rtlpriv) : false);

	if (bt_ctrl_lps)
		mode = (bt_lps_on ? FW_PS_MIN_MODE : FW_PS_ACTIVE_MODE);

	rtl_dbg(rtlpriv, COMP_POWER, DBG_DMESG, "FW LPS mode = %d (coex:%d)\n",
		mode, bt_ctrl_lps);

	switch (mode) {
	case FW_PS_MIN_MODE:
		rlbm = 0;
		awake_intvl = 2;
		break;
	case FW_PS_MAX_MODE:
		rlbm = 1;
		awake_intvl = 2;
		break;
	case FW_PS_DTIM_MODE:
		rlbm = 2;
		awake_intvl = ppsc->reg_max_lps_awakeintvl;
		/* hw->conf.ps_dtim_period or mac->vif->bss_conf.dtim_period is
		 * only used in swlps.
		 */
		break;
	default:
		rlbm = 2;
		awake_intvl = 4;
		break;
	}

	if (rtlpriv->mac80211.p2p) {
		awake_intvl = 2;
		rlbm = 1;
	}

	if (mode == FW_PS_ACTIVE_MODE) {
		byte5 = 0x40;
		power_state = FW_PWR_STATE_ACTIVE;
	} else {
		if (bt_ctrl_lps) {
			byte5 = btc_ops->btc_get_lps_val(rtlpriv);
			power_state = btc_ops->btc_get_rpwm_val(rtlpriv);

			if ((rlbm == 2) && (byte5 & BIT(4))) {
				/* Hold awake interval at 1 to avoid hurting
				 * coexistence performance.
				 */
				awake_intvl = 2;
				rlbm = 2;
			}
		} else {
			byte5 = 0x40;
			power_state = FW_PWR_STATE_RF_OFF;
		}
	}

	SET_H2CCMD_PWRMODE_PARM_MODE(u1_h2c_set_pwrmode, ((mode) ? 1 : 0));
	SET_H2CCMD_PWRMODE_PARM_RLBM(u1_h2c_set_pwrmode, rlbm);
	SET_H2CCMD_PWRMODE_PARM_SMART_PS(u1_h2c_set_pwrmode,
					 bt_ctrl_lps ? 0 :
					 ((rtlpriv->mac80211.p2p) ?
					  ppsc->smart_ps : 1));
	SET_H2CCMD_PWRMODE_PARM_AWAKE_INTERVAL(u1_h2c_set_pwrmode,
					       awake_intvl);
	SET_H2CCMD_PWRMODE_PARM_ALL_QUEUE_UAPSD(u1_h2c_set_pwrmode, 0);
	SET_H2CCMD_PWRMODE_PARM_PWR_STATE(u1_h2c_set_pwrmode, power_state);
	SET_H2CCMD_PWRMODE_PARM_BYTE5(u1_h2c_set_pwrmode, byte5);

	RT_PRINT_DATA(rtlpriv, COMP_CMD, DBG_DMESG,
		      "rtl92fe_set_fw_pwrmode(): u1_h2c_set_pwrmode\n",
		      u1_h2c_set_pwrmode, H2C_92F_PWEMODE_LENGTH);
	if (rtlpriv->cfg->ops->get_btc_status())
		btc_ops->btc_record_pwr_mode(rtlpriv, u1_h2c_set_pwrmode,
					     H2C_92F_PWEMODE_LENGTH);
	rtl92fe_fill_h2c_cmd(hw, H2C_92F_SETPWRMODE, H2C_92F_PWEMODE_LENGTH,
			       u1_h2c_set_pwrmode);
}

void rtl92fe_set_fw_media_status_rpt_cmd(struct ieee80211_hw *hw, u8 mstatus,
					 u8 macid)
{
	u8 parm[3] = { 0, 0, 0 };
	/* parm[0]: bit0=0 disconnect / 1 connect bit1=0 update one ...
	 * dev/MEASURED-fw.c.md sec 5. */
	SET_H2CCMD_MSRRPT_PARM_OPMODE(parm, mstatus);
	SET_H2CCMD_MSRRPT_PARM_MACID_IND(parm, 0);
	SET_H2CCMD_MSRRPT_PARM_MACID(parm, macid);

	rtl92fe_fill_h2c_cmd(hw, H2C_92F_MSRRPT, 3, parm);
}

/* Reserved pages hold the frames the firmware sends by itself while the
 * station sleeps. They go down the TX path, which prepends ONE descriptor to
 * the whole buffer, so page p >= 1 holds [its descriptor][its frame]: in the
 * buffer the descriptor fills the last RSVD_DESC_LEN bytes of page p-1 and the
 * frame starts on the page boundary (rtw88's rtw_build_rsvd_page, same rule).
 * Pages are computed from the frame lengths; the fixed template this replaces
 * put both QoS nulls 32 bytes early, so the firmware's pages 6 and 7 held a
 * shifted packet -- dev/FINDING-rtl8192fe-reserved-pages-2026-09-27.md. */
#define RSVD_PAGE_LEN		128
#define RSVD_DESC_LEN		40
#define RSVD_PAGES		8

struct rtl92fe_rsvd_pkt {
	const u8 *data;
	u32 len;
	bool pspoll;	/* the duration field carries the AID, keep it */
	bool bt_null;
	u8 page;	/* out */
};

static void rtl92fe_rsvd_desc(u8 *desc8, const struct rtl92fe_rsvd_pkt *pkt)
{
	__le32 *desc = (__le32 *)desc8;

	memset(desc8, 0, RSVD_DESC_LEN);
	set_tx_desc_pkt_size(desc, pkt->len);
	set_tx_desc_offset(desc, RSVD_DESC_LEN);
	set_tx_desc_first_seg(desc, 1);
	set_tx_desc_last_seg(desc, 1);
	set_tx_desc_own(desc, 1);
	set_tx_desc_queue_sel(desc, QSLT_MGNT);
	set_tx_desc_use_rate(desc, 1);		/* tx_rate 0 = 1 Mb/s CCK */
	set_tx_desc_nav_use_hdr(desc, pkt->pspoll);
	set_tx_desc_en_hwseq(desc, !pkt->pspoll);
	set_tx_desc_bt_null(desc, pkt->bt_null);
}

/* Lay @n packets out page by page into @buf -> the bytes used, 0 if they do
 * not fit in @size. The first packet's descriptor is the TX path's own. */
static u32 rtl92fe_rsvd_layout(u8 *buf, u32 size, struct rtl92fe_rsvd_pkt *pkt, int n)
{
	u32 at, end = 0;
	u8 page = 0;
	int i;

	memset(buf, 0, size);
	for (i = 0; i < n; i++) {
		at = page * RSVD_PAGE_LEN;
		if (at + pkt[i].len > size)
			return 0;
		if (i)
			rtl92fe_rsvd_desc(buf + at - RSVD_DESC_LEN, &pkt[i]);
		if (pkt[i].len)			/* RSVD_FIRST is {NULL, 0} */
			memcpy(buf + at, pkt[i].data, pkt[i].len);
		pkt[i].page = page;
		end = at + pkt[i].len;
		page += DIV_ROUND_UP(RSVD_DESC_LEN + pkt[i].len, RSVD_PAGE_LEN);
	}
	return end;
}

enum { RSVD_FIRST, RSVD_PSPOLL, RSVD_NULL, RSVD_QOS_NULL, RSVD_BT_QOS_NULL, RSVD_NUM };

/* Only a station reaches this (BSS_CHANGED_ASSOC -> HW_VAR_H2C_FW_JOINBSSRPT),
 * so there is no beacon: page 0 is an empty placeholder, as in rtw88, and the
 * probe-response location stays 0. */
void rtl92fe_set_fw_rsvdpagepkt(struct ieee80211_hw *hw, bool b_dl_finished)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	struct ieee80211_vif *vif = rtl_mac(rtlpriv)->vif;
	struct rtl92fe_rsvd_pkt pkt[RSVD_NUM] = {
		[RSVD_PSPOLL] = { .pspoll = true },
		[RSVD_BT_QOS_NULL] = { .bt_null = true },
	};
	struct sk_buff *frame[RSVD_NUM] = { NULL };
	u8 u1rsvdpageloc[5] = { 0 };
	struct sk_buff *skb;
	bool templates;
	u32 len = 0;
	int i;

	if (vif) {
		frame[RSVD_PSPOLL] = ieee80211_pspoll_get(hw, vif);
		frame[RSVD_NULL] = ieee80211_nullfunc_get(hw, vif, -1, false);
		frame[RSVD_QOS_NULL] = ieee80211_nullfunc_get(hw, vif, -1, true);
		frame[RSVD_BT_QOS_NULL] = ieee80211_nullfunc_get(hw, vif, -1, true);
	}
	skb = dev_alloc_skb(RSVD_PAGES * RSVD_PAGE_LEN);
	for (i = RSVD_PSPOLL; i < RSVD_NUM; i++) {
		if (!frame[i])
			break;
		pkt[i].data = frame[i]->data;
		pkt[i].len = frame[i]->len;
	}
	templates = i == RSVD_NUM;
	if (skb && templates)
		len = rtl92fe_rsvd_layout(skb->data, RSVD_PAGES * RSVD_PAGE_LEN,
					  pkt, RSVD_NUM);
	for (i = 0; i < RSVD_NUM; i++)
		dev_kfree_skb(frame[i]);
	if (!len) {
		pr_warn("rtl8192fe: reserved pages not built (vif %d, templates %s, skb %d)\n",
			!!vif, templates ? "ok" : "missing", !!skb);
		dev_kfree_skb(skb);
		return;
	}
	skb_put(skb, len);

	SET_H2CCMD_RSVDPAGE_LOC_PSPOLL(u1rsvdpageloc, pkt[RSVD_PSPOLL].page);
	SET_H2CCMD_RSVDPAGE_LOC_NULL_DATA(u1rsvdpageloc, pkt[RSVD_NULL].page);
	SET_H2CCMD_RSVDPAGE_LOC_QOS_NULL_DATA(u1rsvdpageloc, pkt[RSVD_QOS_NULL].page);
	SET_H2CCMD_RSVDPAGE_LOC_BT_QOS_NULL_DATA(u1rsvdpageloc,
						 pkt[RSVD_BT_QOS_NULL].page);
	RT_PRINT_DATA(rtlpriv, COMP_CMD, DBG_LOUD, "rsvd pages\n", skb->data, len);

	if (!rtl_cmd_send_packet(hw, skb)) {
		rtl_dbg(rtlpriv, COMP_ERR, DBG_WARNING,
			"Set RSVD page location to Fw FAIL!!!!!!.\n");
		return;
	}
	RT_PRINT_DATA(rtlpriv, COMP_CMD, DBG_LOUD, "H2C_RSVDPAGE:\n", u1rsvdpageloc, 5);
	rtl92fe_fill_h2c_cmd(hw, H2C_92F_RSVDPAGE, sizeof(u1rsvdpageloc), u1rsvdpageloc);
}

/* P2P CTWindow period (sub-command of the P2P PS offload). */
static void rtl92fe_set_p2p_ctw_period_cmd(struct ieee80211_hw *hw,
					     u8 ctwindow)
{
	u8 u1_ctwindow_period[1] = { ctwindow };

	rtl92fe_fill_h2c_cmd(hw, H2C_92F_P2P_PS_CTW_CMD, 1,
			       u1_ctwindow_period);
}

void rtl92fe_set_p2p_ps_offload_cmd(struct ieee80211_hw *hw, u8 p2p_ps_state)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	struct rtl_ps_ctl *rtlps = rtl_psc(rtl_priv(hw));
	struct rtl_hal *rtlhal = rtl_hal(rtl_priv(hw));
	struct rtl_p2p_ps_info *p2pinfo = &rtlps->p2p_ps_info;
	struct p2p_ps_offload_t *p2p_ps_offload = &rtlhal->p2p_ps_offload;
	u8 i;
	u16 ctwindow;
	u32 start_time, tsf_low;

	switch (p2p_ps_state) {
	case P2P_PS_DISABLE:
		rtl_dbg(rtlpriv, COMP_FW, DBG_LOUD, "P2P_PS_DISABLE\n");
		memset(p2p_ps_offload, 0, sizeof(*p2p_ps_offload));
		break;
	case P2P_PS_ENABLE:
		rtl_dbg(rtlpriv, COMP_FW, DBG_LOUD, "P2P_PS_ENABLE\n");
		/* update CTWindow value. */
		if (p2pinfo->ctwindow > 0) {
			p2p_ps_offload->ctwindow_en = 1;
			ctwindow = p2pinfo->ctwindow;
			rtl92fe_set_p2p_ctw_period_cmd(hw, ctwindow);
		}
		/* hw supports up to two NoA descriptors. */
		for (i = 0; i < p2pinfo->noa_num; i++) {
			/* select which NoA the register set targets. */
			rtl_write_byte(rtlpriv, 0x5cf, (i << 4));
			if (i == 0)
				p2p_ps_offload->noa0_en = 1;
			else
				p2p_ps_offload->noa1_en = 1;
			/* program the P2P NoA descriptor registers. */
			rtl_write_dword(rtlpriv, 0x5E0,
					p2pinfo->noa_duration[i]);
			rtl_write_dword(rtlpriv, 0x5E4,
					p2pinfo->noa_interval[i]);

			/* read the current TSF. */
			tsf_low = rtl_read_dword(rtlpriv, REG_TSFTR);

			start_time = p2pinfo->noa_start_time[i];
			if (p2pinfo->noa_count_type[i] != 1) {
				while (start_time <= (tsf_low + (50 * 1024))) {
					start_time += p2pinfo->noa_interval[i];
					if (p2pinfo->noa_count_type[i] != 255)
						p2pinfo->noa_count_type[i]--;
				}
			}
			rtl_write_dword(rtlpriv, 0x5E8, start_time);
			rtl_write_dword(rtlpriv, 0x5EC,
					p2pinfo->noa_count_type[i]);
		}
		if ((p2pinfo->opp_ps == 1) || (p2pinfo->noa_num > 0)) {
			/* reset the P2P circuit. */
			rtl_write_byte(rtlpriv, REG_DUAL_TSF_RST, BIT(4));
			p2p_ps_offload->offload_en = 1;

			if (P2P_ROLE_GO == rtlpriv->mac80211.p2p) {
				p2p_ps_offload->role = 1;
				p2p_ps_offload->allstasleep = 0;
			} else {
				p2p_ps_offload->role = 0;
			}
			p2p_ps_offload->discovery = 0;
		}
		break;
	case P2P_PS_SCAN:
		rtl_dbg(rtlpriv, COMP_FW, DBG_LOUD, "P2P_PS_SCAN\n");
		p2p_ps_offload->discovery = 1;
		break;
	case P2P_PS_SCAN_DONE:
		rtl_dbg(rtlpriv, COMP_FW, DBG_LOUD, "P2P_PS_SCAN_DONE\n");
		p2p_ps_offload->discovery = 0;
		p2pinfo->p2p_ps_state = P2P_PS_ENABLE;
		break;
	default:
		break;
	}
	rtl92fe_fill_h2c_cmd(hw, H2C_92F_P2P_PS_OFFLOAD, 1,
			       (u8 *)p2p_ps_offload);
}

/* C2H rate-adaptive report: byte0[5:0] = the FW-selected rate, byte3 bit0 =
 * the collision-state hint.  Hand both to the DM rate-fallback selector.
 */
void rtl92fe_c2h_ra_report_handler(struct ieee80211_hw *hw,
				     u8 *cmd_buf, u8 cmd_len)
{
	u8 rate = cmd_buf[0] & 0x3F;
	bool collision_state = cmd_buf[3] & BIT(0);

	rtl92fe_dm_dynamic_arfb_select(hw, rate, collision_state);
}
