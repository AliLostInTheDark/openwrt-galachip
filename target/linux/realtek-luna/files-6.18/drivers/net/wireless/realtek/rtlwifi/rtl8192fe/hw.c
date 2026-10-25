// SPDX-License-Identifier: GPL-2.0
/* Clean-room rtl8192fe sub-driver for the Realtek RTL8192F (2T2R 802.11n). */

#include "../wifi.h"
#include "../efuse.h"
#include "../base.h"
#include "../regd.h"
#include "../cam.h"
#include "../ps.h"
#include "../pci.h"
#include "reg.h"
#include "def.h"
#include "phy.h"
#include "dm.h"
#include "fw.h"
#include "led.h"
#include <linux/of.h>
#include "hw.h"
#include "hwvar_width.h"
#include "../pwrseqcmd.h"
#include "pwrseq.h"

#define LLT_CONFIG	5

/* Crystal load-cap trim. This board's WiFi EFUSE is blank ... -- dev/MEASURED-hw.c.md sec 1. */
static int xtal_cap = -1;
module_param(xtal_cap, int, 0644);
MODULE_PARM_DESC(xtal_cap, "RTL8192F crystal load-cap trim 0..0x3f (<0 = use EFUSE/board-cal value)");

/* THIS unit's factory WiFi cal, from its own flash MIB (HW_WLAN0_*), as stock's
 * driver reads it: gpon_provision hands it over before the radio starts and
 * hw_init applies it over the board table. Byte arrays: the MIB's hex bytes. */
static u8 unit_cck_a[CHANNEL_MAX_NUMBER_2G], unit_cck_b[CHANNEL_MAX_NUMBER_2G];
static u8 unit_ht40_a[CHANNEL_MAX_NUMBER_2G], unit_ht40_b[CHANNEL_MAX_NUMBER_2G];
static unsigned int unit_cck_a_n, unit_cck_b_n, unit_ht40_a_n, unit_ht40_b_n;
static int unit_thermal = -1, unit_xcap = -1, unit_reg_domain = -1;
module_param_array(unit_cck_a, byte, &unit_cck_a_n, 0644);
module_param_array(unit_cck_b, byte, &unit_cck_b_n, 0644);
module_param_array(unit_ht40_a, byte, &unit_ht40_a_n, 0644);
module_param_array(unit_ht40_b, byte, &unit_ht40_b_n, 0644);
module_param(unit_thermal, int, 0644);
module_param(unit_xcap, int, 0644);
module_param(unit_reg_domain, int, 0644);
MODULE_PARM_DESC(unit_cck_a, "this unit's HW_WLAN0_TX_POWER_CCK_A, 14 bytes (all unit_* or none)");
MODULE_PARM_DESC(unit_thermal, "this unit's HW_WLAN0_11N_THER");
MODULE_PARM_DESC(unit_xcap, "this unit's HW_WLAN0_11N_XCAP");
MODULE_PARM_DESC(unit_reg_domain, "this unit's HW_WLAN0_REG_DOMAIN");

static void _rtl92fe_apply_unit_cal(struct ieee80211_hw *hw);

/* ★★ A BISECT KNOB, NOT A CONFIG KNOB (2026-09-28). The G24W's own stock differs from
 * ours in the beacon block (BCN_CTRL 0x5c vs 0x1f, BCN_CTRL_1, DRVERLYINT 5 vs 10 TU,
 * ATIMWND, BCN_MAX_ERR, RXTSF offsets, TBTT hold); the X111W runs ours clean. `pokes`
 * writes declared BYTES after hw init, after the beacon set-up and after every BCN_CTRL
 * update, so an arm can put any register at the vendor's value without a rebuild:
 *   rtl8192fe.pokes=0x550:5c,0x551:10,0x558:05  (hex address:hex byte, comma-separated) */
static char *pokes = "";
module_param(pokes, charp, 0444);
MODULE_PARM_DESC(pokes, "DIAGNOSTIC: byte writes 0xADDR:VAL,... applied after init and after "
		 "each beacon-control update (default none)");

/* The beacon preparation budget the guards in pci.c consume: the time after the early
 * interrupt within which a beacon write still lands before the chip's download at
 * BCNDMATIM. Zero means "no guard" to pci.c, so a window of one TU or less is not a
 * budget at all. */
static u32 _rtl92fe_bcn_budget_us(u8 early_tu, u8 dma_tu)
{
	return early_tu > dma_tu + 1 ? (u32)(early_tu - dma_tu - 1) * 1024 : 0;
}

static void _rtl92fe_set_beacon_window(struct ieee80211_hw *hw);

/* One token of `pokes`: 1 = (addr, val) parsed, 0 = the end, -1 = malformed (p stays on it). */
static int _rtl92fe_poke_next(const char **p, unsigned int *addr, unsigned int *val)
{
	int used;

	if (!**p)
		return 0;
	if (sscanf(*p, "%x:%x%n", addr, val, &used) != 2 || *addr > 0xffff || *val > 0xff)
		return -1;
	*p += used;
	if (**p == ',')
		(*p)++;
	return 1;
}

static void _rtl92fe_apply_pokes(struct ieee80211_hw *hw, const char *where)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	struct rtl_pci *rtlpci = rtl_pcidev(rtl_pcipriv(hw));
	const char *p = pokes;
	unsigned int addr, val, n = 0;
	bool timing = false;
	int r;

	if (!p || !*p)
		return;
	/* The whole list is validated before one byte is written: a malformed token after
	 * a timing poke once left the chip's window in force without the budget that
	 * follows it below (Codex, 2026-09-28). */
	while ((r = _rtl92fe_poke_next(&p, &addr, &val)) > 0)
		;
	if (r < 0) {
		pr_warn("pokes: cannot parse %.24s -- the whole list is REFUSED, nothing written\n", p);
		return;
	}
	for (p = pokes; _rtl92fe_poke_next(&p, &addr, &val) > 0; n++) {
		rtl_write_byte(rtlpriv, addr, (u8)val);
		/* The driver's own view follows the poke: the BCN_CTRL cache carries the
		 * poked base so a later set/clear transition works on it (a re-apply on
		 * every update would override the transition -- Codex, 2026-09-28). */
		if (addr == REG_BCN_CTRL)
			rtlpci->reg_bcn_ctrl_val = (u8)val;
		if (addr == REG_DRVERLYINT || addr == REG_BCNDMATIM)
			timing = true;
	}
	if (timing) {
		u8 early = rtl_read_byte(rtlpriv, REG_DRVERLYINT);
		u8 dma = rtl_read_byte(rtlpriv, REG_BCNDMATIM);
		u32 budget = _rtl92fe_bcn_budget_us(early, dma);

		if (!budget) {
			pr_warn("pokes: early=%u dma=%u leaves no preparation window -- REFUSED, "
				"the driver's window is restored\n", early, dma);
			_rtl92fe_set_beacon_window(hw);
		} else {
			rtlpci->bcn_prep_budget_us = budget;
			pr_info("pokes: beacon window early=%u dma=%u -> budget %u us\n",
				early, dma, budget);
		}
	}
	pr_info("pokes: %u byte(s) written (after %s)\n", n, where);
}

/* ★ THE SAME BISECT KNOB FOR THE 32-BIT REGISTERS (2026-09-28). The X111W's stock leaves the
 * front-end pinmux words 0x920-0x944 at the PHY table defaults while this port applies the
 * vendor's RFE7 branch unconditionally (blank efuse -> rfe_type forced to 7), and its AP
 * receives a 1 m client at -100 dBm. Restoring stock's words on ONE image, with the IQK and
 * the width untouched, is the discriminator. A MASK restores one field of a word other code
 * also owns (MAC 0x4c). */
static char *bbpokes = "";
module_param(bbpokes, charp, 0444);
MODULE_PARM_DESC(bbpokes, "DIAGNOSTIC: 32-bit register writes 0xADDR:VAL[/MASK],... applied once "
		 "after the front-end init; VAL holds the bits IN POSITION (a captured word), "
		 "only the MASK bits are written (default none)");

static int _rtl92fe_bbpoke_next(const char **p, unsigned int *addr, unsigned int *val,
				unsigned int *mask)
{
	int used, used2;

	if (!**p)
		return 0;
	if (sscanf(*p, "%x:%x%n", addr, val, &used) != 2 || *addr > 0xfffc || (*addr & 3))
		return -1;
	*mask = 0xffffffff;
	if ((*p)[used] == '/') {
		if (sscanf(*p + used, "/%x%n", mask, &used2) != 1 || !*mask)
			return -1;
		used += used2;
	}
	*p += used;
	if (**p == ',')
		(*p)++;
	return 1;
}

static void _rtl92fe_apply_bbpokes(struct ieee80211_hw *hw)
{
	const char *p = bbpokes;
	unsigned int addr, val, mask, n = 0;
	int r;

	if (!p || !*p)
		return;
	while ((r = _rtl92fe_bbpoke_next(&p, &addr, &val, &mask)) > 0)
		;
	if (r < 0) {
		pr_warn("bbpokes: cannot parse %.24s -- the whole list is REFUSED, nothing written\n", p);
		return;
	}
	for (p = bbpokes; _rtl92fe_bbpoke_next(&p, &addr, &val, &mask) > 0; n++) {
		/* VAL carries the bits IN POSITION (a captured word, masked), so this is a
		 * raw read-modify-write -- never rtl_set_bbreg(addr, mask, val), whose value
		 * is right-aligned and shifted by the mask's trailing zeros (Codex, 2026-09-28) */
		u32 word = rtl_get_bbreg(hw, addr, MASKDWORD);

		rtl_set_bbreg(hw, addr, MASKDWORD, (word & ~mask) | (val & mask));
	}
	pr_info("bbpokes: %u dword(s) written (after front-end init)\n", n);
}

static void _rtl92fe_set_bcn_ctrl_reg(struct ieee80211_hw *hw,
				      u8 set_bits, u8 clear_bits)
{
	struct rtl_pci *rtlpci = rtl_pcidev(rtl_pcipriv(hw));
	struct rtl_priv *rtlpriv = rtl_priv(hw);

	rtlpci->reg_bcn_ctrl_val |= set_bits;
	rtlpci->reg_bcn_ctrl_val &= ~clear_bits;

	rtl_write_byte(rtlpriv, REG_BCN_CTRL, (u8)rtlpci->reg_bcn_ctrl_val);
}

static void _rtl92fe_stop_tx_beacon(struct ieee80211_hw *hw)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	u8 tmp;

	tmp = rtl_read_byte(rtlpriv, REG_FWHW_TXQ_CTRL + 2);
	rtl_write_byte(rtlpriv, REG_FWHW_TXQ_CTRL + 2, tmp & (~BIT(6)));
	rtl_write_byte(rtlpriv, REG_TBTT_PROHIBIT + 1, 0x64);
	tmp = rtl_read_byte(rtlpriv, REG_TBTT_PROHIBIT + 2);
	tmp &= ~(BIT(0));
	rtl_write_byte(rtlpriv, REG_TBTT_PROHIBIT + 2, tmp);
}

/* ★★★ THE PAIR THE BISECT NAMED, AND THE VALUE THE VENDOR USES -- dev/MEASURED-hw.c.md sec 4. */
static int tbtt_prohibit_field = -1;
module_param(tbtt_prohibit_field, int, 0644);
MODULE_PARM_DESC(tbtt_prohibit_field,
		 "TBTT_PROHIBIT field [19:8] value (-1 = the legacy 0xff byte "
		 "poke; 0x138 is what the vendor driver programs)");

/* MEASURED 2026-09-28 on the G24W (RTL9603CVD): stock leaves the endpoint's PCIe
 * DevCtl at its power-on 0x2810 (MRRS 512 B, Enable No Snoop set); the mainline
 * MRRS backdoor below wiped bits 8-15 and left 0x2010. On a CPS host with a
 * coherence manager a snooped read takes the coherent path. 1 = keep the chip's
 * bits and set only the MRRS field; 0 = the mainline clearing (the A/B). */
static int nosnoop = 1;
module_param(nosnoop, int, 0644);
MODULE_PARM_DESC(nosnoop,
		 "keep the endpoint's Enable-No-Snoop DevCtl bit as the chip powers "
		 "up with it (default 1); 0 clears DevCtl[15:8] as mainline does");

static void _rtl92fe_resume_tx_beacon(struct ieee80211_hw *hw)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	u8 tmp;

	tmp = rtl_read_byte(rtlpriv, REG_FWHW_TXQ_CTRL + 2);
	rtl_write_byte(rtlpriv, REG_FWHW_TXQ_CTRL + 2, tmp | BIT(6));
	if (tbtt_prohibit_field >= 0) {
		u32 v = rtl_read_dword(rtlpriv, REG_TBTT_PROHIBIT);

		v = (v & ~0x000fff00u) |
		    (((u32)tbtt_prohibit_field << 8) & 0x000fff00u);
		rtl_write_dword(rtlpriv, REG_TBTT_PROHIBIT, v);
		rtl_dbg(rtlpriv, COMP_INIT, DBG_LOUD,
			"beacon arm: TBTT field 0x%08x ([19:8]=0x%03x)\n",
			v, tbtt_prohibit_field);
	}
	tmp = rtl_read_byte(rtlpriv, REG_TBTT_PROHIBIT + 2);
	rtl_write_byte(rtlpriv, REG_TBTT_PROHIBIT + 2, tmp | BIT(0));
	if (tbtt_prohibit_field < 0) {
		/* last, whole: the vendor's twenty bits (step2's bit 16 is hold bit 8 on this chip) */
		u32 v = rtl_read_dword(rtlpriv, REG_TBTT_PROHIBIT);

		v = (v & ~0x000fffffu) | TBTT_PROHIBIT_AP_92F;
		rtl_write_dword(rtlpriv, REG_TBTT_PROHIBIT, v);
		rtl_dbg(rtlpriv, COMP_INIT, DBG_LOUD,
			"beacon arm: TBTT prohibit 0x%08x (vendor)\n", v);
	}
}

static void _rtl92fe_enable_bcn_sub_func(struct ieee80211_hw *hw)
{
	_rtl92fe_set_bcn_ctrl_reg(hw, 0, BIT(1));
}

static void _rtl92fe_disable_bcn_sub_func(struct ieee80211_hw *hw)
{
	_rtl92fe_set_bcn_ctrl_reg(hw, BIT(1), 0);
}

static void _rtl92fe_set_fw_clock_on(struct ieee80211_hw *hw,
				     u8 rpwm_val, bool b_need_turn_off_ckk)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	struct rtl_hal *rtlhal = rtl_hal(rtl_priv(hw));
	bool b_support_remote_wake_up;
	u32 count = 0, isr_regaddr, content;
	bool b_schedule_timer = b_need_turn_off_ckk;

	rtlpriv->cfg->ops->get_hw_reg(hw, HAL_DEF_WOWLAN,
				      (u8 *)(&b_support_remote_wake_up));

	if (!rtlhal->fw_ready)
		return;
	if (!rtlpriv->psc.fw_current_inpsmode)
		return;

	while (1) {
		spin_lock_bh(&rtlpriv->locks.fw_ps_lock);
		if (rtlhal->fw_clk_change_in_progress) {
			while (rtlhal->fw_clk_change_in_progress) {
				spin_unlock_bh(&rtlpriv->locks.fw_ps_lock);
				count++;
				udelay(100);
				if (count > 1000)
					return;
				spin_lock_bh(&rtlpriv->locks.fw_ps_lock);
			}
			spin_unlock_bh(&rtlpriv->locks.fw_ps_lock);
		} else {
			rtlhal->fw_clk_change_in_progress = false;
			spin_unlock_bh(&rtlpriv->locks.fw_ps_lock);
			break;
		}
	}

	if (IS_IN_LOW_POWER_STATE_92F(rtlhal->fw_ps_state)) {
		rtlpriv->cfg->ops->get_hw_reg(hw, HW_VAR_SET_RPWM,
					      (u8 *)(&rpwm_val));
		if (FW_PS_IS_ACK(rpwm_val)) {
			isr_regaddr = REG_HISR;
			content = rtl_read_dword(rtlpriv, isr_regaddr);
			while (!(content & IMR_CPWM) && (count < 500)) {
				udelay(50);
				count++;
				content = rtl_read_dword(rtlpriv, isr_regaddr);
			}

			if (content & IMR_CPWM) {
				rtl_write_word(rtlpriv, isr_regaddr, 0x0100);
				rtlhal->fw_ps_state = FW_PS_STATE_RF_ON_92F;
				rtl_dbg(rtlpriv, COMP_POWER, DBG_LOUD,
					"Receive CPWM INT!!! PSState = %X\n",
					rtlhal->fw_ps_state);
			}
		}

		spin_lock_bh(&rtlpriv->locks.fw_ps_lock);
		rtlhal->fw_clk_change_in_progress = false;
		spin_unlock_bh(&rtlpriv->locks.fw_ps_lock);
		if (b_schedule_timer) {
			mod_timer(&rtlpriv->works.fw_clockoff_timer,
				  jiffies + MSECS(10));
		}
	} else  {
		spin_lock_bh(&rtlpriv->locks.fw_ps_lock);
		rtlhal->fw_clk_change_in_progress = false;
		spin_unlock_bh(&rtlpriv->locks.fw_ps_lock);
	}
}

static void _rtl92fe_set_fw_clock_off(struct ieee80211_hw *hw, u8 rpwm_val)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	struct rtl_hal *rtlhal = rtl_hal(rtl_priv(hw));
	struct rtl_pci *rtlpci = rtl_pcidev(rtl_pcipriv(hw));
	struct rtl8192_tx_ring *ring;
	enum rf_pwrstate rtstate;
	bool b_schedule_timer = false;
	u8 queue;

	if (!rtlhal->fw_ready)
		return;
	if (!rtlpriv->psc.fw_current_inpsmode)
		return;
	if (!rtlhal->allow_sw_to_change_hwclc)
		return;

	rtlpriv->cfg->ops->get_hw_reg(hw, HW_VAR_RF_STATE, (u8 *)(&rtstate));
	if (rtstate == ERFOFF || rtlpriv->psc.inactive_pwrstate == ERFOFF)
		return;

	for (queue = 0; queue < RTL_PCI_MAX_TX_QUEUE_COUNT; queue++) {
		ring = &rtlpci->tx_ring[queue];
		if (skb_queue_len(&ring->queue)) {
			b_schedule_timer = true;
			break;
		}
	}

	if (b_schedule_timer) {
		mod_timer(&rtlpriv->works.fw_clockoff_timer,
			  jiffies + MSECS(10));
		return;
	}

	if (FW_PS_STATE(rtlhal->fw_ps_state) != FW_PS_STATE_RF_OFF_LOW_PWR) {
		spin_lock_bh(&rtlpriv->locks.fw_ps_lock);
		if (!rtlhal->fw_clk_change_in_progress) {
			rtlhal->fw_clk_change_in_progress = true;
			spin_unlock_bh(&rtlpriv->locks.fw_ps_lock);
			rtlhal->fw_ps_state = FW_PS_STATE(rpwm_val);
			rtl_write_word(rtlpriv, REG_HISR, 0x0100);
			rtlpriv->cfg->ops->set_hw_reg(hw, HW_VAR_SET_RPWM,
						      (u8 *)(&rpwm_val));
			spin_lock_bh(&rtlpriv->locks.fw_ps_lock);
			rtlhal->fw_clk_change_in_progress = false;
			spin_unlock_bh(&rtlpriv->locks.fw_ps_lock);
		} else {
			spin_unlock_bh(&rtlpriv->locks.fw_ps_lock);
			mod_timer(&rtlpriv->works.fw_clockoff_timer,
				  jiffies + MSECS(10));
		}
	}
}

static void _rtl92fe_set_fw_ps_rf_on(struct ieee80211_hw *hw)
{
	u8 rpwm_val = 0;

	rpwm_val |= (FW_PS_STATE_RF_OFF_92F | FW_PS_ACK);
	_rtl92fe_set_fw_clock_on(hw, rpwm_val, true);
}

static void _rtl92fe_set_fw_ps_rf_off_low_power(struct ieee80211_hw *hw)
{
	u8 rpwm_val = 0;

	rpwm_val |= FW_PS_STATE_RF_OFF_LOW_PWR;
	_rtl92fe_set_fw_clock_off(hw, rpwm_val);
}

void rtl92fe_fw_clk_off_timer_callback(unsigned long data)
{
	struct ieee80211_hw *hw = (struct ieee80211_hw *)data;

	_rtl92fe_set_fw_ps_rf_off_low_power(hw);
}

static void _rtl92fe_fwlps_leave(struct ieee80211_hw *hw)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	struct rtl_ps_ctl *ppsc = rtl_psc(rtl_priv(hw));
	struct rtl_hal *rtlhal = rtl_hal(rtl_priv(hw));
	bool fw_current_inps = false;
	u8 rpwm_val = 0, fw_pwrmode = FW_PS_ACTIVE_MODE;

	if (ppsc->low_power_enable) {
		rpwm_val = (FW_PS_STATE_ALL_ON_92F | FW_PS_ACK);/* RF on */
		_rtl92fe_set_fw_clock_on(hw, rpwm_val, false);
		rtlhal->allow_sw_to_change_hwclc = false;
		rtlpriv->cfg->ops->set_hw_reg(hw, HW_VAR_H2C_FW_PWRMODE,
					      (u8 *)(&fw_pwrmode));
		rtlpriv->cfg->ops->set_hw_reg(hw, HW_VAR_FW_PSMODE_STATUS,
					      (u8 *)(&fw_current_inps));
	} else {
		rpwm_val = FW_PS_STATE_ALL_ON_92F;	/* RF on */
		rtlpriv->cfg->ops->set_hw_reg(hw, HW_VAR_SET_RPWM,
					      (u8 *)(&rpwm_val));
		rtlpriv->cfg->ops->set_hw_reg(hw, HW_VAR_H2C_FW_PWRMODE,
					      (u8 *)(&fw_pwrmode));
		rtlpriv->cfg->ops->set_hw_reg(hw, HW_VAR_FW_PSMODE_STATUS,
					      (u8 *)(&fw_current_inps));
	}
}

static void _rtl92fe_fwlps_enter(struct ieee80211_hw *hw)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	struct rtl_ps_ctl *ppsc = rtl_psc(rtl_priv(hw));
	struct rtl_hal *rtlhal = rtl_hal(rtl_priv(hw));
	bool fw_current_inps = true;
	u8 rpwm_val;

	if (ppsc->low_power_enable) {
		rpwm_val = FW_PS_STATE_RF_OFF_LOW_PWR;	/* RF off */
		rtlpriv->cfg->ops->set_hw_reg(hw, HW_VAR_FW_PSMODE_STATUS,
					      (u8 *)(&fw_current_inps));
		rtlpriv->cfg->ops->set_hw_reg(hw, HW_VAR_H2C_FW_PWRMODE,
					      (u8 *)(&ppsc->fwctrl_psmode));
		rtlhal->allow_sw_to_change_hwclc = true;
		_rtl92fe_set_fw_clock_off(hw, rpwm_val);
	} else {
		rpwm_val = FW_PS_STATE_RF_OFF_92F;	/* RF off */
		rtlpriv->cfg->ops->set_hw_reg(hw, HW_VAR_FW_PSMODE_STATUS,
					      (u8 *)(&fw_current_inps));
		rtlpriv->cfg->ops->set_hw_reg(hw, HW_VAR_H2C_FW_PWRMODE,
					      (u8 *)(&ppsc->fwctrl_psmode));
		rtlpriv->cfg->ops->set_hw_reg(hw, HW_VAR_SET_RPWM,
					      (u8 *)(&rpwm_val));
	}
}

void rtl92fe_get_hw_reg(struct ieee80211_hw *hw, u8 variable, u8 *val)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	struct rtl_ps_ctl *ppsc = rtl_psc(rtl_priv(hw));
	struct rtl_pci *rtlpci = rtl_pcidev(rtl_pcipriv(hw));

	switch (variable) {
	case HW_VAR_RCR:
		*((u32 *)(val)) = rtlpci->receive_config;
		break;
	case HW_VAR_RF_STATE:
		*((enum rf_pwrstate *)(val)) = ppsc->rfpwr_state;
		break;
	case HW_VAR_FWLPS_RF_ON:{
			enum rf_pwrstate rfstate;
			u32 val_rcr;

			rtlpriv->cfg->ops->get_hw_reg(hw, HW_VAR_RF_STATE,
						      (u8 *)(&rfstate));
			if (rfstate == ERFOFF) {
				*((bool *)(val)) = true;
			} else {
				val_rcr = rtl_read_dword(rtlpriv, REG_RCR);
				val_rcr &= 0x00070000;
				if (val_rcr)
					*((bool *)(val)) = false;
				else
					*((bool *)(val)) = true;
			}
		}
		break;
	case HW_VAR_FW_PSMODE_STATUS:
		*((bool *)(val)) = ppsc->fw_current_inpsmode;
		break;
	case HW_VAR_CORRECT_TSF:{
		/* composed by value: splitting a u64 through two u32 pointers puts the
		 * halves in little-endian order, swapped on this big-endian host */
		u32 tsf_high = rtl_read_dword(rtlpriv, REG_TSFTR + 4);
		u32 tsf_low = rtl_read_dword(rtlpriv, REG_TSFTR);

		*((u64 *)(val)) = rtl92fe_tsf_compose(tsf_high, tsf_low);
		}
		break;
	case HAL_DEF_WOWLAN:
		break;
	default:
		rtl_dbg(rtlpriv, COMP_ERR, DBG_DMESG,
			"switch case %#x not processed\n", variable);
		break;
	}
}

static void _rtl92fe_download_rsvd_page(struct ieee80211_hw *hw)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	u8 tmp_regcr, tmp_reg422;
	u8 bcnvalid_reg, txbc_reg;
	u8 count = 0, dlbcn_count = 0;
	bool b_recover = false;

	/* Set REG_CR bit 8. DMA beacon by SW. */
	tmp_regcr = rtl_read_byte(rtlpriv, REG_CR + 1);
	rtl_write_byte(rtlpriv, REG_CR + 1, tmp_regcr | BIT(0));

	/* Disable Hw protection for the window reserved for Hw beacon TX so
	 * that the reserved-page download does not collide with it.
	 */
	_rtl92fe_set_bcn_ctrl_reg(hw, 0, BIT(3));
	_rtl92fe_set_bcn_ctrl_reg(hw, BIT(4), 0);

	/* FWHW_TXQ_CTRL 0x422[6]=0: tell HW this is not a real beacon. */
	tmp_reg422 = rtl_read_byte(rtlpriv, REG_FWHW_TXQ_CTRL + 2);
	rtl_write_byte(rtlpriv, REG_FWHW_TXQ_CTRL + 2, tmp_reg422 & (~BIT(6)));

	if (tmp_reg422 & BIT(6))
		b_recover = true;

	do {
		/* Clear beacon-valid check bit */
		bcnvalid_reg = rtl_read_byte(rtlpriv, REG_DWBCN0_CTRL + 2);
		rtl_write_byte(rtlpriv, REG_DWBCN0_CTRL + 2,
			       bcnvalid_reg | BIT(0));

		/* download rsvd page */
		rtl92fe_set_fw_rsvdpagepkt(hw, false);

		txbc_reg = rtl_read_byte(rtlpriv, REG_MGQ_TXBD_NUM + 3);
		count = 0;
		while ((txbc_reg & BIT(4)) && count < 20) {
			count++;
			udelay(10);
			txbc_reg = rtl_read_byte(rtlpriv, REG_MGQ_TXBD_NUM + 3);
		}
		rtl_write_byte(rtlpriv, REG_MGQ_TXBD_NUM + 3,
			       txbc_reg | BIT(4));

		/* check rsvd page download OK. */
		bcnvalid_reg = rtl_read_byte(rtlpriv, REG_DWBCN0_CTRL + 2);
		count = 0;
		while (!(bcnvalid_reg & BIT(0)) && count < 20) {
			count++;
			udelay(50);
			bcnvalid_reg = rtl_read_byte(rtlpriv,
						     REG_DWBCN0_CTRL + 2);
		}

		if (bcnvalid_reg & BIT(0))
			rtl_write_byte(rtlpriv, REG_DWBCN0_CTRL + 2, BIT(0));

		dlbcn_count++;
	} while (!(bcnvalid_reg & BIT(0)) && dlbcn_count < 5);

	if (!(bcnvalid_reg & BIT(0)))
		pr_warn("rtl8192fe: reserved pages NOT downloaded after %u attempt(s): the firmware's "
			"page locations are stale (a command the chip never took is not an upload)\n",
			dlbcn_count);

	/* Enable Bcn */
	_rtl92fe_set_bcn_ctrl_reg(hw, BIT(3), 0);
	_rtl92fe_set_bcn_ctrl_reg(hw, 0, BIT(4));

	if (b_recover)
		rtl_write_byte(rtlpriv, REG_FWHW_TXQ_CTRL + 2, tmp_reg422);

	tmp_regcr = rtl_read_byte(rtlpriv, REG_CR + 1);
	rtl_write_byte(rtlpriv, REG_CR + 1, tmp_regcr & (~BIT(0)));
}

void rtl92fe_set_hw_reg(struct ieee80211_hw *hw, u8 variable, u8 *val)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	struct rtl_pci *rtlpci = rtl_pcidev(rtl_pcipriv(hw));
	struct rtl_mac *mac = rtl_mac(rtl_priv(hw));
	struct rtl_efuse *efuse = rtl_efuse(rtl_priv(hw));
	struct rtl_ps_ctl *ppsc = rtl_psc(rtl_priv(hw));
	u8 idx;

	switch (variable) {
	case HW_VAR_ETHER_ADDR:
		for (idx = 0; idx < ETH_ALEN; idx++)
			rtl_write_byte(rtlpriv, (REG_MACID + idx), val[idx]);
		break;
	case HW_VAR_BASIC_RATE:{
		/* every caller passes a u32 (mac->basic_rates): read it as one. `((u16 *)val)[0]`
		 * is its HIGH half on this big-endian host -- 0 -- so RRSR became 0x0d (CCK
		 * only) and every OFDM/HT frame was acknowledged at a CCK rate its sender
		 * cannot wait for: the Luna APs' deaf uplink (stock writes 0x15d). */
		u16 b_rate_cfg = rtl92fe_rrsr_from_basic(val);

		rtl_write_byte(rtlpriv, REG_RRSR, b_rate_cfg & 0xff);
		rtl_write_byte(rtlpriv, REG_RRSR + 1, (b_rate_cfg >> 8) & 0xff);
		break; }
	case HW_VAR_BSSID:
		for (idx = 0; idx < ETH_ALEN; idx++)
			rtl_write_byte(rtlpriv, (REG_BSSID + idx), val[idx]);
		break;
	case HW_VAR_SIFS:
		rtl_write_byte(rtlpriv, REG_SIFS_CTX + 1, val[0]);
		rtl_write_byte(rtlpriv, REG_SIFS_TRX + 1, val[1]);

		rtl_write_byte(rtlpriv, REG_SPEC_SIFS + 1, val[0]);
		rtl_write_byte(rtlpriv, REG_MAC_SPEC_SIFS + 1, val[0]);

		if (!mac->ht_enable)
			rtl_write_word(rtlpriv, REG_RESP_SIFS_OFDM, 0x0e0e);
		else
			rtl_write_word(rtlpriv, REG_RESP_SIFS_OFDM,
				       *((u16 *)val));
		break;
	case HW_VAR_SLOT_TIME:{
		u8 e_aci;

		rtl_dbg(rtlpriv, COMP_MLME, DBG_TRACE,
			"HW_VAR_SLOT_TIME %x\n", val[0]);

		rtl_write_byte(rtlpriv, REG_SLOT, val[0]);

		for (e_aci = 0; e_aci < AC_MAX; e_aci++) {
			rtlpriv->cfg->ops->set_hw_reg(hw, HW_VAR_AC_PARAM,
						      (u8 *)(&e_aci));
		}
		break; }
	case HW_VAR_ACK_PREAMBLE:{
		u8 reg_tmp;
		u8 short_preamble = (bool)(*(u8 *)val);

		/* RRSR_RSC stays 0 on the 8192F, see rtl92fe_phy_set_bw_mode_callback */
		reg_tmp = 0;
		if (short_preamble)
			reg_tmp |= 0x80;
		rtl_write_byte(rtlpriv, REG_RRSR + 2, reg_tmp);
		rtlpriv->mac80211.short_preamble = short_preamble;
		}
		break;
	case HW_VAR_WPA_CONFIG:
		rtl_write_byte(rtlpriv, REG_SECCFG, *((u8 *)val));
		break;
	case HW_VAR_AMPDU_FACTOR:{
		u8 regtoset_normal[4] = { 0x41, 0xa8, 0x72, 0xb9 };
		u8 fac;
		u8 *reg = NULL;
		u8 i = 0;

		reg = regtoset_normal;

		fac = *((u8 *)val);
		if (fac <= 3) {
			fac = (1 << (fac + 2));
			if (fac > 0xf)
				fac = 0xf;
			for (i = 0; i < 4; i++) {
				if ((reg[i] & 0xf0) > (fac << 4))
					reg[i] = (reg[i] & 0x0f) |
						(fac << 4);
				if ((reg[i] & 0x0f) > fac)
					reg[i] = (reg[i] & 0xf0) | fac;
				rtl_write_byte(rtlpriv,
					       (REG_AGGLEN_LMT + i),
					       reg[i]);
			}
			rtl_dbg(rtlpriv, COMP_MLME, DBG_LOUD,
				"Set HW_VAR_AMPDU_FACTOR:%#x\n", fac);
		}
		}
		break;
	case HW_VAR_AC_PARAM:{
		u8 e_aci = *((u8 *)val);

		if (rtlpci->acm_method != EACMWAY2_SW)
			rtlpriv->cfg->ops->set_hw_reg(hw, HW_VAR_ACM_CTRL,
						      (u8 *)(&e_aci));
		}
		break;
	case HW_VAR_ACM_CTRL:{
		u8 e_aci = *((u8 *)val);
		union aci_aifsn *aifs = (union aci_aifsn *)(&mac->ac[0].aifs);

		u8 acm = aifs->f.acm;
		u8 acm_ctrl = rtl_read_byte(rtlpriv, REG_ACMHWCTRL);

		acm_ctrl = acm_ctrl | ((rtlpci->acm_method == 2) ? 0x0 : 0x1);

		if (acm) {
			switch (e_aci) {
			case AC0_BE:
				acm_ctrl |= ACMHW_BEQEN;
				break;
			case AC2_VI:
				acm_ctrl |= ACMHW_VIQEN;
				break;
			case AC3_VO:
				acm_ctrl |= ACMHW_VOQEN;
				break;
			default:
				rtl_dbg(rtlpriv, COMP_ERR, DBG_WARNING,
					"HW_VAR_ACM_CTRL acm set failed: eACI is %d\n",
					acm);
				break;
			}
		} else {
			switch (e_aci) {
			case AC0_BE:
				acm_ctrl &= (~ACMHW_BEQEN);
				break;
			case AC2_VI:
				acm_ctrl &= (~ACMHW_VIQEN);
				break;
			case AC3_VO:
				acm_ctrl &= (~ACMHW_VOQEN);
				break;
			default:
				rtl_dbg(rtlpriv, COMP_ERR, DBG_DMESG,
					"switch case %#x not processed\n",
					e_aci);
				break;
			}
		}

		rtl_dbg(rtlpriv, COMP_QOS, DBG_TRACE,
			"SetHwReg8192pci(): [HW_VAR_ACM_CTRL] Write 0x%X\n",
			acm_ctrl);
		rtl_write_byte(rtlpriv, REG_ACMHWCTRL, acm_ctrl);
		}
		break;
	case HW_VAR_RCR:{
		rtl_write_dword(rtlpriv, REG_RCR, ((u32 *)(val))[0]);
		rtlpci->receive_config = ((u32 *)(val))[0];
		}
		break;
	case HW_VAR_RETRY_LIMIT:{
		u8 retry_limit = ((u8 *)(val))[0];

		rtl_write_word(rtlpriv, REG_RETRY_LIMIT,
			       retry_limit << RETRY_LIMIT_SHORT_SHIFT |
			       retry_limit << RETRY_LIMIT_LONG_SHIFT);
		}
		break;
	case HW_VAR_DUAL_TSF_RST:
		rtl_write_byte(rtlpriv, REG_DUAL_TSF_RST, (BIT(0) | BIT(1)));
		break;
	case HW_VAR_EFUSE_BYTES:
		efuse->efuse_usedbytes = *((u16 *)val);
		break;
	case HW_VAR_EFUSE_USAGE:
		efuse->efuse_usedpercentage = *((u8 *)val);
		break;
	case HW_VAR_IO_CMD:
		rtl92fe_phy_set_io_cmd(hw, (*(enum io_type *)val));
		break;
	case HW_VAR_SET_RPWM:{
		u8 rpwm_val;

		rpwm_val = rtl_read_byte(rtlpriv, REG_PCIE_HRPWM);
		udelay(1);

		if (rpwm_val & BIT(7)) {
			rtl_write_byte(rtlpriv, REG_PCIE_HRPWM, (*(u8 *)val));
		} else {
			rtl_write_byte(rtlpriv, REG_PCIE_HRPWM,
				       ((*(u8 *)val) | BIT(7)));
		}
		}
		break;
	case HW_VAR_H2C_FW_PWRMODE:
		rtl92fe_set_fw_pwrmode_cmd(hw, (*(u8 *)val));
		break;
	case HW_VAR_FW_PSMODE_STATUS:
		ppsc->fw_current_inpsmode = *((bool *)val);
		break;
	case HW_VAR_RESUME_CLK_ON:
		_rtl92fe_set_fw_ps_rf_on(hw);
		break;
	case HW_VAR_FW_LPS_ACTION:{
		bool b_enter_fwlps = *((bool *)val);

		if (b_enter_fwlps)
			_rtl92fe_fwlps_enter(hw);
		else
			_rtl92fe_fwlps_leave(hw);
		}
		break;
	case HW_VAR_H2C_FW_JOINBSSRPT:{
		u8 mstatus = (*(u8 *)val);

		if (mstatus == RT_MEDIA_CONNECT) {
			rtlpriv->cfg->ops->set_hw_reg(hw, HW_VAR_AID, NULL);
			_rtl92fe_download_rsvd_page(hw);
		}
		rtl92fe_set_fw_media_status_rpt_cmd(hw, mstatus, 0);
		}
		break;
	case HW_VAR_H2C_FW_P2P_PS_OFFLOAD:
		rtl92fe_set_p2p_ps_offload_cmd(hw, (*(u8 *)val));
		break;
	case HW_VAR_AID:{
		u16 u2btmp;

		u2btmp = rtl_read_word(rtlpriv, REG_BCN_PSR_RPT);
		u2btmp &= 0xC000;
		rtl_write_word(rtlpriv, REG_BCN_PSR_RPT,
			       (u2btmp | mac->assoc_id));
		}
		break;
	case HW_VAR_CORRECT_TSF:{
		u8 btype_ibss = ((u8 *)(val))[0];

		if (btype_ibss)
			_rtl92fe_stop_tx_beacon(hw);

		_rtl92fe_set_bcn_ctrl_reg(hw, 0, BIT(3));

		rtl_write_dword(rtlpriv, REG_TSFTR,
				(u32)(mac->tsf & 0xffffffff));
		rtl_write_dword(rtlpriv, REG_TSFTR + 4,
				(u32)((mac->tsf >> 32) & 0xffffffff));

		_rtl92fe_set_bcn_ctrl_reg(hw, BIT(3), 0);

		if (btype_ibss)
			_rtl92fe_resume_tx_beacon(hw);
		}
		break;
	case HW_VAR_KEEP_ALIVE: {
		u8 array[2];

		array[0] = 0xff;
		array[1] = *((u8 *)val);
		rtl92fe_fill_h2c_cmd(hw, H2C_92F_KEEP_ALIVE_CTRL, 2, array);
		}
		break;
	default:
		rtl_dbg(rtlpriv, COMP_ERR, DBG_DMESG,
			"switch case %#x not processed\n", variable);
		break;
	}
}

static bool _rtl92fe_llt_table_init(struct ieee80211_hw *hw)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	u8 txpktbuf_bndy;
	u8 u8tmp, testcnt = 0;

	txpktbuf_bndy = TX_PAGE_BOUNDARY;

	/* Vendor: the normal/extra quotas are latched by LD_RQPN, so before it. */
	rtl_write_byte(rtlpriv, REG_RQPN_NPQ, TX_PAGE_NUM_NPQ_92F);
	rtl_write_byte(rtlpriv, REG_RQPN_NPQ + 2, TX_PAGE_NUM_EPQ_92F);
	rtl_write_dword(rtlpriv, REG_RQPN, RQPN_INIT_VALUE);

	rtl_write_byte(rtlpriv, REG_TRXFF_BNDY, txpktbuf_bndy);
	rtl_write_word(rtlpriv, REG_TRXFF_BNDY + 2, RXFF_BNDY_92F);

	rtl_write_byte(rtlpriv, REG_DWBCN0_CTRL + 1, txpktbuf_bndy);
	rtl_write_byte(rtlpriv, REG_DWBCN1_CTRL + 1, txpktbuf_bndy);

	rtl_write_byte(rtlpriv, REG_BCNQ_BDNY, txpktbuf_bndy);
	rtl_write_byte(rtlpriv, REG_BCNQ1_BDNY, txpktbuf_bndy);

	rtl_write_byte(rtlpriv, REG_MGQ_BDNY, txpktbuf_bndy);
	rtl_write_byte(rtlpriv, REG_WMAC_LBK_BF_HD, txpktbuf_bndy);

	/* 256-byte page boundary, 8 bytes of RX driver-info. */
	rtl_write_byte(rtlpriv, REG_PBP, 0x31);
	rtl_write_byte(rtlpriv, REG_RX_DRVINFO_SZ, 0x4);

	u8tmp = rtl_read_byte(rtlpriv, REG_AUTO_LLT + 2);
	rtl_write_byte(rtlpriv, REG_AUTO_LLT + 2, u8tmp | BIT(0));

	for (testcnt = 0; testcnt < 100; testcnt++) {
		u8tmp = rtl_read_byte(rtlpriv, REG_AUTO_LLT + 2);
		if (!(u8tmp & BIT(0)))
			return true;
		udelay(10);
	}

	pr_err("rtl8192fe: LLT initialization timed out\n");
	return false;
}

static void _rtl92fe_gen_refresh_led_state(struct ieee80211_hw *hw)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	struct rtl_ps_ctl *ppsc = rtl_psc(rtl_priv(hw));
	enum rtl_led_pin pin0 = rtlpriv->ledctl.sw_led0;

	if (rtlpriv->rtlhal.up_first_time)
		return;

	if (ppsc->rfoff_reason == RF_CHANGE_BY_IPS)
		rtl92fe_sw_led_on(hw, pin0);
	else if (ppsc->rfoff_reason == RF_CHANGE_BY_INIT)
		rtl92fe_sw_led_on(hw, pin0);
	else
		rtl92fe_sw_led_off(hw, pin0);
}

static bool _rtl92fe_init_mac(struct ieee80211_hw *hw)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	struct rtl_pci *rtlpci = rtl_pcidev(rtl_pcipriv(hw));
	struct rtl_hal *rtlhal = rtl_hal(rtl_priv(hw));

	u8 bytetmp;
	u32 dwordtmp;

	rtl_write_byte(rtlpriv, REG_RSV_CTRL, 0x0);

	dwordtmp = rtl_read_dword(rtlpriv, REG_SYS_CFG1);
	if (dwordtmp & BIT(24)) {
		rtl_write_byte(rtlpriv, REG_LDO_SW_CTRL, 0xc3);
	} else {
		bytetmp = rtl_read_byte(rtlpriv, REG_AFE_PLL_CTRL + 2);
		rtl_write_byte(rtlpriv, REG_AFE_PLL_CTRL + 2,
			       bytetmp | BIT(4) | BIT(6));
		rtl_write_byte(rtlpriv, REG_LDO_SW_CTRL, 0x83);
	}

	/* 1. 40 MHz crystal source */
	bytetmp = rtl_read_byte(rtlpriv, REG_AFE_XTAL_CTRL);
	bytetmp &= 0xfb;
	rtl_write_byte(rtlpriv, REG_AFE_XTAL_CTRL, bytetmp);

	dwordtmp = rtl_read_dword(rtlpriv, REG_AFE_PLL_CTRL);
	dwordtmp &= 0xfffffc7f;
	rtl_write_dword(rtlpriv, REG_AFE_PLL_CTRL, dwordtmp);

	/* 2. AFE parameter trim */
	bytetmp = rtl_read_byte(rtlpriv, REG_AFE_XTAL_CTRL);
	bytetmp &= 0xbf;
	rtl_write_byte(rtlpriv, REG_AFE_XTAL_CTRL, bytetmp);

	dwordtmp = rtl_read_dword(rtlpriv, REG_AFE_PLL_CTRL);
	dwordtmp &= 0xffdfffff;
	rtl_write_dword(rtlpriv, REG_AFE_PLL_CTRL, dwordtmp);

	/* HW power-on sequence (parsed pwrseq command list). */
	if (!rtl_hal_pwrseqcmdparsing(rtlpriv, PWR_CUT_ALL_MSK, PWR_FAB_ALL_MSK,
				      PWR_INTF_PCI_MSK,
				      RTL8192F_NIC_ENABLE_FLOW)) {
		/* pr_err, not rtl_dbg: this is a HARD FAILURE PATH and the ...
		 * dev/MEASURED-hw.c.md sec 6. */
		pr_err("rtl8192fe: Init MAC failed at the power-on sequence "
		       "(rtl_hal_pwrseqcmdparsing, RTL8192F_NIC_ENABLE_FLOW)\n");
		return false;
	}

	/* Release MAC IO register reset */
	bytetmp = rtl_read_byte(rtlpriv, REG_CR);
	bytetmp = 0xff;
	rtl_write_byte(rtlpriv, REG_CR, bytetmp);
	mdelay(2);
	bytetmp = 0x7f;
	rtl_write_byte(rtlpriv, REG_HWSEQ_CTRL, bytetmp);
	mdelay(2);

	/* Add for wakeup online */
	bytetmp = rtl_read_byte(rtlpriv, REG_SYS_CLKR);
	rtl_write_byte(rtlpriv, REG_SYS_CLKR, bytetmp | BIT(3));
	bytetmp = rtl_read_byte(rtlpriv, REG_GPIO_MUXCFG + 1);
	rtl_write_byte(rtlpriv, REG_GPIO_MUXCFG + 1, bytetmp & (~BIT(4)));
	/* Release MAC IO register reset */
	rtl_write_word(rtlpriv, REG_CR, 0x2ff);

	if (!rtlhal->mac_func_enable) {
		if (!_rtl92fe_llt_table_init(hw)) {
			/* pr_err: see the power-on sequence note above. */
			pr_err("rtl8192fe: Init MAC failed at the LLT table "
			       "(_rtl92fe_llt_table_init)\n");
			return false;
		}
	}

	rtl_write_dword(rtlpriv, REG_HISR, 0xffffffff);
	rtl_write_dword(rtlpriv, REG_HISRE, 0xffffffff);

	/* A 32-bit register on the 8192F (the map reaches bit 21); the RX-DMA nibble is kept. */
	dwordtmp = rtl_read_dword(rtlpriv, REG_TRXDMA_CTRL);
	dwordtmp &= 0xf;
	dwordtmp |= TRXDMA_CTRL_QMAP_VALUE;
	rtl_write_dword(rtlpriv, REG_TRXDMA_CTRL, dwordtmp);
	/* Reported Tx status from HW for rate adaptive. */
	rtl_write_byte(rtlpriv, REG_FWHW_TXQ_CTRL + 1, 0x1F);

	/* Set RCR register */
	rtl_write_dword(rtlpriv, REG_RCR, rtlpci->receive_config);
	/* Per-frame-type RX subtype filter maps. Only RXFLTMAP2 ...
	 * dev/MEASURED-hw.c.md sec 8. */
	rtl_write_word(rtlpriv, REG_RXFLTMAP0, 0xffff);	/* data: accept all subtypes (incl. EAPOL) */
	rtl_write_word(rtlpriv, REG_RXFLTMAP1, 0x0400);	/* control: admit PS-Poll (subtype 10) */
	rtl_write_word(rtlpriv, REG_RXFLTMAP2, 0xffff);	/* management: accept all subtypes */

	/* Set TCR register */
	rtl_write_dword(rtlpriv, REG_TCR, rtlpci->transmit_config);

	/* Set TX/RX descriptor physical address -- HI part */
	if (!rtlpriv->cfg->mod_params->dma64)
		goto dma64_end;

	rtl_write_dword(rtlpriv, REG_BCNQ_DESA + 4,
			((u64)rtlpci->tx_ring[BEACON_QUEUE].buffer_desc_dma) >>
				32);
	rtl_write_dword(rtlpriv, REG_MGQ_DESA + 4,
			(u64)rtlpci->tx_ring[MGNT_QUEUE].buffer_desc_dma >> 32);
	rtl_write_dword(rtlpriv, REG_VOQ_DESA + 4,
			(u64)rtlpci->tx_ring[VO_QUEUE].buffer_desc_dma >> 32);
	rtl_write_dword(rtlpriv, REG_VIQ_DESA + 4,
			(u64)rtlpci->tx_ring[VI_QUEUE].buffer_desc_dma >> 32);
	rtl_write_dword(rtlpriv, REG_BEQ_DESA + 4,
			(u64)rtlpci->tx_ring[BE_QUEUE].buffer_desc_dma >> 32);
	rtl_write_dword(rtlpriv, REG_BKQ_DESA + 4,
			(u64)rtlpci->tx_ring[BK_QUEUE].buffer_desc_dma >> 32);
	rtl_write_dword(rtlpriv, REG_HQ0_DESA + 4,
			(u64)rtlpci->tx_ring[HIGH_QUEUE].buffer_desc_dma >> 32);

	rtl_write_dword(rtlpriv, REG_RX_DESA + 4,
			(u64)rtlpci->rx_ring[RX_MPDU_QUEUE].dma >> 32);

dma64_end:

	/* Set TX/RX descriptor physical address (lo part). The ...
	 * dev/MEASURED-hw.c.md sec 33. */
	rtl_write_dword(rtlpriv, REG_BCNQ_DESA,
			((u64)rtlpci->tx_ring[BEACON_QUEUE].buffer_desc_dma) &
			DMA_BIT_MASK(32));
	rtl_write_dword(rtlpriv, REG_MGQ_DESA,
			(u64)rtlpci->tx_ring[MGNT_QUEUE].buffer_desc_dma &
			DMA_BIT_MASK(32));
	rtl_write_dword(rtlpriv, REG_VOQ_DESA,
			(u64)rtlpci->tx_ring[VO_QUEUE].buffer_desc_dma &
			DMA_BIT_MASK(32));
	rtl_write_dword(rtlpriv, REG_VIQ_DESA,
			(u64)rtlpci->tx_ring[VI_QUEUE].buffer_desc_dma &
			DMA_BIT_MASK(32));

	rtl_write_dword(rtlpriv, REG_BEQ_DESA,
			(u64)rtlpci->tx_ring[BE_QUEUE].buffer_desc_dma &
			DMA_BIT_MASK(32));

	dwordtmp = rtl_read_dword(rtlpriv, REG_BEQ_DESA);

	rtl_write_dword(rtlpriv, REG_BKQ_DESA,
			(u64)rtlpci->tx_ring[BK_QUEUE].buffer_desc_dma &
			DMA_BIT_MASK(32));
	rtl_write_dword(rtlpriv, REG_HQ0_DESA,
			(u64)rtlpci->tx_ring[HIGH_QUEUE].buffer_desc_dma &
			DMA_BIT_MASK(32));

	rtl_write_dword(rtlpriv, REG_RX_DESA,
			(u64)rtlpci->rx_ring[RX_MPDU_QUEUE].dma &
			DMA_BIT_MASK(32));

	rtl_write_dword(rtlpriv, REG_TSFTIMER_HCI, 0x3fffffff);

	bytetmp = rtl_read_byte(rtlpriv, REG_PCIE_CTRL_REG + 3);
	rtl_write_byte(rtlpriv, REG_PCIE_CTRL_REG + 3, bytetmp | 0xF7);

	rtl_write_dword(rtlpriv, REG_INT_MIG, 0);

	rtl_write_dword(rtlpriv, REG_MCUTST_1, 0x0);

	/* Ring SW/HW-depth invariants (build-time, chip-agnostic). ...
	 * dev/MEASURED-hw.c.md sec 9. */
	BUILD_BUG_ON(TX_DESC_NUM_92F != RT_TXDESC_NUM);
	BUILD_BUG_ON(RX_DESC_NUM_92F != RTL_PCI_MAX_RX_COUNT);
	/* and each depth must fit its register's depth field without
	 * dev/MEASURED-hw.c.md sec 10. */
	BUILD_BUG_ON(TX_DESC_NUM_92F & ~0xFFFU);
	BUILD_BUG_ON(RX_DESC_NUM_92F & ~0x1FFFU);

	/* Program the per-queue TXBD ring depth + segment count. */
	rtl_write_word(rtlpriv, REG_MGQ_TXBD_NUM,
		       TX_DESC_NUM_92F | ((RTL8192FE_SEG_NUM << 12) & 0x3000));
	rtl_write_word(rtlpriv, REG_VOQ_TXBD_NUM,
		       TX_DESC_NUM_92F | ((RTL8192FE_SEG_NUM << 12) & 0x3000));
	rtl_write_word(rtlpriv, REG_VIQ_TXBD_NUM,
		       TX_DESC_NUM_92F | ((RTL8192FE_SEG_NUM << 12) & 0x3000));
	rtl_write_word(rtlpriv, REG_BEQ_TXBD_NUM,
		       TX_DESC_NUM_92F | ((RTL8192FE_SEG_NUM << 12) & 0x3000));
	rtl_write_word(rtlpriv, REG_BKQ_TXBD_NUM,
		       TX_DESC_NUM_92F | ((RTL8192FE_SEG_NUM << 12) & 0x3000));
	rtl_write_word(rtlpriv, REG_HI0Q_TXBD_NUM,
		       TX_DESC_NUM_92F | ((RTL8192FE_SEG_NUM << 12) & 0x3000));
	rtl_write_word(rtlpriv, REG_HI1Q_TXBD_NUM,
		       TX_DESC_NUM_92F | ((RTL8192FE_SEG_NUM << 12) & 0x3000));
	rtl_write_word(rtlpriv, REG_HI2Q_TXBD_NUM,
		       TX_DESC_NUM_92F | ((RTL8192FE_SEG_NUM << 12) & 0x3000));
	rtl_write_word(rtlpriv, REG_HI3Q_TXBD_NUM,
		       TX_DESC_NUM_92F | ((RTL8192FE_SEG_NUM << 12) & 0x3000));
	rtl_write_word(rtlpriv, REG_HI4Q_TXBD_NUM,
		       TX_DESC_NUM_92F | ((RTL8192FE_SEG_NUM << 12) & 0x3000));
	rtl_write_word(rtlpriv, REG_HI5Q_TXBD_NUM,
		       TX_DESC_NUM_92F | ((RTL8192FE_SEG_NUM << 12) & 0x3000));
	rtl_write_word(rtlpriv, REG_HI6Q_TXBD_NUM,
		       TX_DESC_NUM_92F | ((RTL8192FE_SEG_NUM << 12) & 0x3000));
	rtl_write_word(rtlpriv, REG_HI7Q_TXBD_NUM,
		       TX_DESC_NUM_92F | ((RTL8192FE_SEG_NUM << 12) & 0x3000));
	/* RX ring depth, the segment count, and bit 15 = BIT_SYS_32_64 (vendor HalComBit.h):
	 * SET, the 16-byte (4-dword) segment layout pci.h/trx.h lay out. It MUST agree with
	 * `struct rtl_rx_buffer_desc` -- a chip told 8-byte segments while the host lays out
	 * 16 kills RX and TX (2026-09-28, both Luna APs off the air; bd32_layout_guard). */
	rtl_write_word(rtlpriv, REG_RX_RXBD_NUM,
		       RX_DESC_NUM_92F |
		       ((RTL8192FE_SEG_NUM << 13) & 0x6000) | 0x8000);

	rtl_write_dword(rtlpriv, REG_TSFTIMER_HCI, 0xFFFFFFFF);

	_rtl92fe_gen_refresh_led_state(hw);
	return true;
}

/* Vendor 88XX MAC init (Hal88XXGen.c): the beacon-early interrupt 10 TU
 * before TBTT, the beacon DMA 1 TU before it.  At the chip's own 2/2 both
 * fire together and the tasklet rewrites the descriptor the DMA is reading
 * (TXDMA_STATUS payload OVF/UDN, the whole TXDMA halts).  The tasklet gets
 * the gap minus one TU of margin; a later run keeps the previous beacon.
 */
static void _rtl92fe_set_beacon_window(struct ieee80211_hw *hw)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	struct rtl_pci *rtlpci = rtl_pcidev(rtl_pcipriv(hw));
	const u8 early_tu = 10, dma_tu = 1;

	rtl_write_byte(rtlpriv, REG_DRVERLYINT, early_tu);
	rtl_write_byte(rtlpriv, REG_BCNDMATIM, dma_tu);
	rtlpci->bcn_prep_budget_us = _rtl92fe_bcn_budget_us(early_tu, dma_tu);
}

static void _rtl92fe_hw_configure(struct ieee80211_hw *hw)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	struct rtl_pci *rtlpci = rtl_pcidev(rtl_pcipriv(hw));
	u32 reg_rrsr;

	reg_rrsr = RATE_ALL_CCK | RATE_ALL_OFDM_AG;
	/* Init value for RRSR. */
	rtl_write_dword(rtlpriv, REG_RRSR, reg_rrsr);

	/* ARFB table 8 for 11n 2SS */
	rtl_write_dword(rtlpriv, REG_ARFR0, 0x00000010);
	rtl_write_dword(rtlpriv, REG_ARFR0 + 4, 0x3e0ff000);

	/* ARFB table 9 for 11n 1SS */
	rtl_write_dword(rtlpriv, REG_ARFR1, 0x00000010);
	rtl_write_dword(rtlpriv, REG_ARFR1 + 4, 0x000ff000);

	/* Set SLOT time */
	rtl_write_byte(rtlpriv, REG_SLOT, 0x09);

	/* CF-End setting. */
	rtl_write_word(rtlpriv, REG_FWHW_TXQ_CTRL, 0x1F80);

	/* Set retry limit */
	rtl_write_word(rtlpriv, REG_RETRY_LIMIT, 0x0707);

	/* BAR settings */
	rtl_write_dword(rtlpriv, REG_BAR_MODE_CTRL, 0x0201ffff);

	/* Set Data / Response auto rate fallback retry count */
	rtl_write_dword(rtlpriv, REG_DARFRC, 0x01000000);
	rtl_write_dword(rtlpriv, REG_DARFRC + 4, 0x07060504);
	rtl_write_dword(rtlpriv, REG_RARFRC, 0x01000000);
	rtl_write_dword(rtlpriv, REG_RARFRC + 4, 0x07060504);

	/* Beacon related, for rate adaptive */
	rtl_write_byte(rtlpriv, REG_ATIMWND, 0x2);
	rtl_write_byte(rtlpriv, REG_BCN_MAX_ERR, 0xff);

	rtlpci->reg_bcn_ctrl_val = 0x1d;
	rtl_write_byte(rtlpriv, REG_BCN_CTRL, rtlpci->reg_bcn_ctrl_val);

	/* Second-beacon (multi-BSSID) control register; disabled for the
	 * single-BSSID case.
	 */
	rtl_write_byte(rtlpriv, REG_BCN_CTRL_1, 0);

	/* TBTT prohibit hold time. */
	rtl_write_byte(rtlpriv, REG_TBTT_PROHIBIT + 1, 0xff); /* 8 ms */
	_rtl92fe_set_beacon_window(hw);

	rtl_write_byte(rtlpriv, REG_PIFS, 0);
	rtl_write_byte(rtlpriv, REG_AGGR_BREAK_TIME, 0x16);

	rtl_write_word(rtlpriv, REG_NAV_PROT_LEN, 0x0040);
	rtl_write_word(rtlpriv, REG_PROT_MODE_CTRL, 0x08ff);

	/* For Rx TP. */
	rtl_write_dword(rtlpriv, REG_FAST_EDCA_CTRL, 0x03086666);

	/* ACKTO for IOT issue. */
	rtl_write_byte(rtlpriv, REG_ACKTO, 0x40);

	/* Set Spec SIFS (used in NAV) */
	rtl_write_word(rtlpriv, REG_SPEC_SIFS, 0x100a);
	rtl_write_word(rtlpriv, REG_MAC_SPEC_SIFS, 0x100a);

	/* Set SIFS for CCK */
	rtl_write_word(rtlpriv, REG_SIFS_CTX, 0x100a);

	/* Set SIFS for OFDM */
	rtl_write_word(rtlpriv, REG_SIFS_TRX, 0x100a);

	rtl_write_byte(rtlpriv, REG_RX_PKT_LIMIT, 0x20);

	rtl_write_word(rtlpriv, REG_MAX_AGGR_NUM, 0x1f1f);

	/* Set Multicast Address. */
	rtl_write_dword(rtlpriv, REG_MAR, 0xffffffff);
	rtl_write_dword(rtlpriv, REG_MAR + 4, 0xffffffff);
}

static void _rtl92fe_enable_aspm_back_door(struct ieee80211_hw *hw)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	struct rtl_ps_ctl *ppsc = rtl_psc(rtl_priv(hw));
	u32 tmp32 = 0, count = 0;
	u8 tmp8 = 0;

	rtl_write_word(rtlpriv, REG_BACKDOOR_DBI_DATA, 0x78);
	rtl_write_byte(rtlpriv, REG_BACKDOOR_DBI_DATA + 2, 0x2);
	tmp8 = rtl_read_byte(rtlpriv, REG_BACKDOOR_DBI_DATA + 2);
	count = 0;
	while (tmp8 && count < 20) {
		udelay(10);
		tmp8 = rtl_read_byte(rtlpriv, REG_BACKDOOR_DBI_DATA + 2);
		count++;
	}

	if (tmp8 == 0) {
		tmp32 = rtl_read_dword(rtlpriv, REG_BACKDOOR_DBI_RDATA);
		if ((tmp32 & (nosnoop ? 0x7000 : 0xff00)) != 0x2000) {
			tmp32 &= nosnoop ? 0xffff8fff : 0xffff00ff;
			rtl_write_dword(rtlpriv, REG_BACKDOOR_DBI_WDATA,
					tmp32 | BIT(13));
			rtl_write_word(rtlpriv, REG_BACKDOOR_DBI_DATA, 0xf078);
			rtl_write_byte(rtlpriv, REG_BACKDOOR_DBI_DATA + 2, 0x1);

			tmp8 = rtl_read_byte(rtlpriv,
					     REG_BACKDOOR_DBI_DATA + 2);
			count = 0;
			while (tmp8 && count < 20) {
				udelay(10);
				tmp8 = rtl_read_byte(rtlpriv,
						     REG_BACKDOOR_DBI_DATA + 2);
				count++;
			}
		}
	}

	rtl_write_word(rtlpriv, REG_BACKDOOR_DBI_DATA, 0x70c);
	rtl_write_byte(rtlpriv, REG_BACKDOOR_DBI_DATA + 2, 0x2);
	tmp8 = rtl_read_byte(rtlpriv, REG_BACKDOOR_DBI_DATA + 2);
	count = 0;
	while (tmp8 && count < 20) {
		udelay(10);
		tmp8 = rtl_read_byte(rtlpriv, REG_BACKDOOR_DBI_DATA + 2);
		count++;
	}
	/* ASPM off (the default) CLEARS the L1/CLKREQ backdoor bits rather than
	 * skipping them: a warm re-init must not keep what an earlier init set.
	 */
	if (tmp8 == 0) {
		tmp32 = rtl_read_dword(rtlpriv, REG_BACKDOOR_DBI_RDATA);
		tmp32 = ppsc->support_backdoor ? tmp32 | BIT(31) : tmp32 & ~BIT(31);
		rtl_write_dword(rtlpriv, REG_BACKDOOR_DBI_WDATA, tmp32);
		rtl_write_word(rtlpriv, REG_BACKDOOR_DBI_DATA, 0xf70c);
		rtl_write_byte(rtlpriv, REG_BACKDOOR_DBI_DATA + 2, 0x1);
	}

	tmp8 = rtl_read_byte(rtlpriv, REG_BACKDOOR_DBI_DATA + 2);
	count = 0;
	while (tmp8 && count < 20) {
		udelay(10);
		tmp8 = rtl_read_byte(rtlpriv, REG_BACKDOOR_DBI_DATA + 2);
		count++;
	}

	rtl_write_word(rtlpriv, REG_BACKDOOR_DBI_DATA, 0x718);
	rtl_write_byte(rtlpriv, REG_BACKDOOR_DBI_DATA + 2, 0x2);
	tmp8 = rtl_read_byte(rtlpriv, REG_BACKDOOR_DBI_DATA + 2);
	count = 0;
	while (tmp8 && count < 20) {
		udelay(10);
		tmp8 = rtl_read_byte(rtlpriv, REG_BACKDOOR_DBI_DATA + 2);
		count++;
	}
	if (tmp8 == 0) {
		tmp32 = rtl_read_dword(rtlpriv, REG_BACKDOOR_DBI_RDATA);
		tmp32 = ppsc->support_backdoor ? tmp32 | BIT(11) | BIT(12) :
						 tmp32 & ~(BIT(11) | BIT(12));
		rtl_write_dword(rtlpriv, REG_BACKDOOR_DBI_WDATA, tmp32);
		rtl_write_word(rtlpriv, REG_BACKDOOR_DBI_DATA, 0xf718);
		rtl_write_byte(rtlpriv, REG_BACKDOOR_DBI_DATA + 2, 0x1);
	}
	tmp8 = rtl_read_byte(rtlpriv, REG_BACKDOOR_DBI_DATA + 2);
	count = 0;
	while (tmp8 && count < 20) {
		udelay(10);
		tmp8 = rtl_read_byte(rtlpriv, REG_BACKDOOR_DBI_DATA + 2);
		count++;
	}
}

void rtl92fe_enable_hw_security_config(struct ieee80211_hw *hw)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	u8 sec_reg_value;
	u8 tmp;

	rtl_dbg(rtlpriv, COMP_INIT, DBG_DMESG,
		"PairwiseEncAlgorithm = %d GroupEncAlgorithm = %d\n",
		rtlpriv->sec.pairwise_enc_algorithm,
		rtlpriv->sec.group_enc_algorithm);

	if (rtlpriv->cfg->mod_params->sw_crypto || rtlpriv->sec.use_sw_sec) {
		rtl_dbg(rtlpriv, COMP_SEC, DBG_DMESG,
			"not open hw encryption\n");
		return;
	}

	sec_reg_value = SCR_TXENCENABLE | SCR_RXDECENABLE;

	if (rtlpriv->sec.use_defaultkey) {
		sec_reg_value |= SCR_TXUSEDK;
		sec_reg_value |= SCR_RXUSEDK;
	}

	sec_reg_value |= (SCR_RXBCUSEDK | SCR_TXBCUSEDK);

	tmp = rtl_read_byte(rtlpriv, REG_CR + 1);
	rtl_write_byte(rtlpriv, REG_CR + 1, tmp | BIT(1));

	rtl_dbg(rtlpriv, COMP_SEC, DBG_DMESG,
		"The SECR-value %x\n", sec_reg_value);

	rtlpriv->cfg->ops->set_hw_reg(hw, HW_VAR_WPA_CONFIG, &sec_reg_value);
}

static bool _rtl92fe_check_pcie_dma_hang(struct rtl_priv *rtlpriv)
{
	u8 tmp;

	/* Enable the PCIe debug port (reg 0x350 bit[26]). */
	tmp = rtl_read_byte(rtlpriv, REG_BACKDOOR_DBI_DATA + 3);
	if (!(tmp & BIT(2))) {
		rtl_write_byte(rtlpriv, REG_BACKDOOR_DBI_DATA + 3,
			       tmp | BIT(2));
		mdelay(100);
	}

	/* reg 0x350 bit[25] = RX hang, bit[24] = TX hang. */
	tmp = rtl_read_byte(rtlpriv, REG_BACKDOOR_DBI_DATA + 3);
	if ((tmp & BIT(0)) || (tmp & BIT(1))) {
		rtl_dbg(rtlpriv, COMP_INIT, DBG_LOUD,
			"CheckPcieDMAHang8192FE(): true!!\n");
		return true;
	}
	return false;
}

static void _rtl92fe_reset_pcie_interface_dma(struct rtl_priv *rtlpriv,
					      bool mac_power_on)
{
	u8 tmp;
	bool release_mac_rx_pause;
	u8 backup_pcie_dma_pause;

	rtl_dbg(rtlpriv, COMP_INIT, DBG_LOUD,
		"ResetPcieInterfaceDMA8192FE()\n");

	/* PCIe RX-DMA-hang reset flow. 1. disable register write lock ...
	 * dev/MEASURED-hw.c.md sec 11. */
	tmp = rtl_read_byte(rtlpriv, REG_RSV_CTRL);
	tmp &= ~(BIT(1) | BIT(0));
	rtl_write_byte(rtlpriv, REG_RSV_CTRL, tmp);
	tmp = rtl_read_byte(rtlpriv, REG_PMC_DBG_CTRL2);
	tmp |= BIT(2);
	rtl_write_byte(rtlpriv, REG_PMC_DBG_CTRL2, tmp);

	/* 2. Check and pause TRX DMA
	 *	write 0x284 bit[18] = 1'b1
	 *	write 0x301 = 0xFF
	 */
	tmp = rtl_read_byte(rtlpriv, REG_RXDMA_CONTROL);
	if (tmp & BIT(2)) {
		/* Already paused for another reason. */
		release_mac_rx_pause = false;
	} else {
		rtl_write_byte(rtlpriv, REG_RXDMA_CONTROL, (tmp | BIT(2)));
		release_mac_rx_pause = true;
	}

	backup_pcie_dma_pause = rtl_read_byte(rtlpriv, REG_PCIE_CTRL_REG + 1);
	if (backup_pcie_dma_pause != 0xFF)
		rtl_write_byte(rtlpriv, REG_PCIE_CTRL_REG + 1, 0xFF);

	if (mac_power_on) {
		/* 3. reset TRX function: write 0x100 = 0x00 */
		rtl_write_byte(rtlpriv, REG_CR, 0);
	}

	/* 4. Reset PCIe DMA: write 0x003 bit[0] = 0 */
	tmp = rtl_read_byte(rtlpriv, REG_SYS_FUNC_EN + 1);
	tmp &= ~(BIT(0));
	rtl_write_byte(rtlpriv, REG_SYS_FUNC_EN + 1, tmp);

	/* 5. Enable PCIe DMA: write 0x003 bit[0] = 1 */
	tmp = rtl_read_byte(rtlpriv, REG_SYS_FUNC_EN + 1);
	tmp |= BIT(0);
	rtl_write_byte(rtlpriv, REG_SYS_FUNC_EN + 1, tmp);

	if (mac_power_on) {
		/* 6. enable TRX function: write 0x100 = 0xFF.
		 * LLT/RQPN and ring base addresses are re-initialised later
		 * because the MAC function was reset.
		 */
		rtl_write_byte(rtlpriv, REG_CR, 0xFF);
	}

	/* 7. Restore PCIe autoload-down bit: write 0xF8 bit[17] = 1'b1 */
	tmp = rtl_read_byte(rtlpriv, REG_MAC_PHY_CTRL_NORMAL + 2);
	tmp |= BIT(1);
	rtl_write_byte(rtlpriv, REG_MAC_PHY_CTRL_NORMAL + 2, tmp);

	/* In MAC-power-on state BB/RF may be ON; releasing TRX DMA here
	 * would start traffic, so defer it until after re-init.
	 */
	if (!mac_power_on) {
		/* 8. release TRX DMA */
		if (release_mac_rx_pause) {
			tmp = rtl_read_byte(rtlpriv, REG_RXDMA_CONTROL);
			rtl_write_byte(rtlpriv, REG_RXDMA_CONTROL,
				       (tmp & (~BIT(2))));
		}
		rtl_write_byte(rtlpriv, REG_PCIE_CTRL_REG + 1,
			       backup_pcie_dma_pause);
	}

	/* 9. lock system register: write 0xCC bit[2] = 1'b0 */
	tmp = rtl_read_byte(rtlpriv, REG_PMC_DBG_CTRL2);
	tmp &= ~(BIT(2));
	rtl_write_byte(rtlpriv, REG_PMC_DBG_CTRL2, tmp);
}

/* RFE 7 AB/AB overrides; X111W stock RFE 3 keeps the BB table defaults. */
static void _rtl92fe_config_rfe(struct ieee80211_hw *hw)
{
	if (rtl_hal(rtl_priv(hw))->rfe_type != 7)
		return;

	rtl_set_bbreg(hw, 0x103c, 0x70000, 0x7);
	rtl_set_bbreg(hw, 0x04c, 0x6c00000, 0x0);
	rtl_set_bbreg(hw, 0x064, BIT(29) | BIT(28), 0x3);
	rtl_set_bbreg(hw, 0x1038, 0x600010, 0x0);
	rtl_set_bbreg(hw, 0x944, 0xfff, 0x081f);
	rtl_set_bbreg(hw, 0x930, 0xfffff, 0x23200);
	rtl_set_bbreg(hw, 0x938, 0xfffff, 0x23200);
	rtl_set_bbreg(hw, 0x934, 0xf000, 0x3);
	rtl_set_bbreg(hw, 0x93c, 0xf000, 0x3);
	rtl_set_bbreg(hw, 0x968, BIT(2), 0x0);
	rtl_set_bbreg(hw, 0x920, 0xffffffff, 0x03000003);
	rtl_set_bbreg(hw, 0x940, 0xffffffff, 0x004007ae);
}

static void _rtl92fe_config_trx_mode_ab(struct ieee80211_hw *hw)
{
	/* ==== [RF Mode Table] (tx_path_en == BB_PATH_AB) ==== */
	rtl_set_bbreg(hw, 0x824, 0xe, 2);
	rtl_set_bbreg(hw, 0x82c, 0xe, 2);

	rtl_set_bbreg(hw, 0x804, 0xf, 0x3);
	/* CCK TX path control by REG */
	rtl_set_bbreg(hw, 0x80c, BIT(31), 0x0);

	/* ==== [RX Path] configure RX paths for BB_PATH_AB ==== */
	/* OFDM Rx path (val = 3 for AB) */
	rtl_set_bbreg(hw, 0xc04, 0xff, 0x33);
	rtl_set_bbreg(hw, 0xd04, 0xf, 3);
	/* CCK Rx path: generic 2R block (num_enable_path > 1) */
	rtl_set_bbreg(hw, 0xa04, (BIT(27) | BIT(26)), 0);
	rtl_set_bbreg(hw, 0xa04, (BIT(25) | BIT(24)), 1);
	rtl_set_bbreg(hw, 0xa74, BIT(8), 1);
	rtl_set_bbreg(hw, 0xa2c, BIT(18), 1);
	rtl_set_bbreg(hw, 0xa2c, BIT(22), 1);
	/* CCK Rx path: 8192F-specific AB override */
	rtl_set_bbreg(hw, 0xa04, (BIT(27) | BIT(26)), 0);
	rtl_set_bbreg(hw, 0xa04, (BIT(25) | BIT(24)), 1);
	rtl_set_bbreg(hw, 0xa74, BIT(8), 1);
	rtl_set_bbreg(hw, 0xa2c, (BIT(18) | BIT(17)), 1);
	rtl_set_bbreg(hw, 0xa2c, (BIT(22) | BIT(21)), 1);

	/* ==== [TX Path] configure TX paths for AB, AB, AB ==== */
	/* CCK TX antenna mapping (BB_PATH_AB) */
	rtl_set_bbreg(hw, 0xa04, 0xf0000000, 0xc);
	/* OFDM TX path (tx_path_en == AB -> ofdm_tx_path(BB_PATH_AB)) */
	rtl_set_bbreg(hw, 0x90c, 0xffffffff, 0x83321333);

	/* External-PA/FEM front-end pinmux (T/R switch + PA-enable + RX-LNA). */
	_rtl92fe_config_rfe(hw);
}

int rtl92fe_hw_init(struct ieee80211_hw *hw)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	struct rtl_hal *rtlhal = rtl_hal(rtl_priv(hw));
	struct rtl_mac *mac = rtl_mac(rtl_priv(hw));
	struct rtl_phy *rtlphy = &rtlpriv->phy;
	struct rtl_ps_ctl *ppsc = rtl_psc(rtl_priv(hw));
	bool rtstatus = true;
	int err = 0;
	u8 tmp_u1b, u1byte;

	rtl_dbg(rtlpriv, COMP_INIT, DBG_LOUD, " Rtl8192FE hw init\n");
	_rtl92fe_apply_unit_cal(hw);
	rtlpriv->rtlhal.being_init_adapter = true;
	rtlpriv->intf_ops->disable_aspm(hw);

	/* Decide whether the MAC sub-system is already enabled (warm boot). */
	tmp_u1b = rtl_read_byte(rtlpriv, REG_SYS_CLKR + 1);
	u1byte = rtl_read_byte(rtlpriv, REG_CR);
	if ((tmp_u1b & BIT(3)) && (u1byte != 0 && u1byte != 0xEA)) {
		rtlhal->mac_func_enable = true;
	} else {
		rtlhal->mac_func_enable = false;
		rtlhal->fw_ps_state = FW_PS_STATE_ALL_ON_92F;
	}

	if (_rtl92fe_check_pcie_dma_hang(rtlpriv)) {
		rtl_dbg(rtlpriv, COMP_INIT, DBG_DMESG, "92fe dma hang!\n");
		_rtl92fe_reset_pcie_interface_dma(rtlpriv,
						  rtlhal->mac_func_enable);
		rtlhal->mac_func_enable = false;
	}

	/* RTL8192F crystal-frequency select: 25 MHz crystal -> REG_AFE_CTRL5
	 * (0x0094) BIT_REF_SEL [28:25] = 1 (0 selects 40 MHz). Set before pwron. */
	rtl_write_dword(rtlpriv, 0x0094,
			(rtl_read_dword(rtlpriv, 0x0094) & ~(0xfu << 25)) |
			(1u << 25));

	/* Init order: pwron -> MAC init/LLT/queue-page alloc. */
	rtstatus = _rtl92fe_init_mac(hw);
	{
		struct rtl_pci *rp = rtl_pcidev(rtl_pcipriv(hw));
		u16 pw;
		int round = 0;

		pcie_capability_clear_word(rp->pdev, PCI_EXP_LNKCTL,
					   PCI_EXP_LNKCTL_ASPMC);
		if (rp->pdev->bus->self)
			pcie_capability_clear_word(rp->pdev->bus->self,
						   PCI_EXP_LNKCTL,
						   PCI_EXP_LNKCTL_ASPMC);

		/* Power-on tail the pwrseq-only init_mac does not cover (stock
		 * dev/MEASURED-hw.c.md sec 34. */
		rtl_write_byte(rtlpriv, 0x24, rtl_read_byte(rtlpriv, 0x24) | BIT(0));
		pw = (rtl_read_word(rtlpriv, 0x04) & 0xe7ff) | 0x0800;
		rtl_write_word(rtlpriv, 0x04, pw);
		while (!(rtl_read_dword(rtlpriv, 0x04) & 0x00020000) &&
		       ++round < 10000)
			;
		rtl_write_word(rtlpriv, 0x04, rtl_read_word(rtlpriv, 0x04) & 0x7fff);
		rtl_write_word(rtlpriv, 0x04, rtl_read_word(rtlpriv, 0x04) & 0xe7ff);
		mdelay(1);
		rtl_write_byte(rtlpriv, 0x05, rtl_read_byte(rtlpriv, 0x05) | BIT(0));
		round = 0;
		while ((rtl_read_byte(rtlpriv, 0x05) & BIT(0)) && ++round < 1000)
			udelay(100);
		rtl_write_byte(rtlpriv, 0x1d, rtl_read_byte(rtlpriv, 0x1d) & ~BIT(0));
		rtl_write_word(rtlpriv, 0x02, rtl_read_word(rtlpriv, 0x02) & ~BIT(10));
		udelay(2);
		if (rtl_read_dword(rtlpriv, 0xf0) & BIT(24))
			rtl_write_byte(rtlpriv, 0x7c, 0xc3);
		else
			rtl_write_byte(rtlpriv, 0x7c, 0x83);
	}

	/* AFE PLL/XTAL are set by the crystal-select (AFE_CTRL5 BIT_REF_SEL=25M)
	 * in the power-on tail above + the pwrseq MAC table; the old hardcoded 40 MHz
	 * block here re-mistuned the PLL on this 25 MHz board, re-breaking the
	 * config-core clock and hanging high-offset writes in phy_mac_config. */

	if (!rtstatus) {
		pr_err("Init MAC failed\n");
		err = 1;
		return err;
	}

	rtl_write_word(rtlpriv, REG_PCIE_CTRL_REG, 0x8000);

	/* Download the 8051 firmware. With the 25 MHz crystal-select + the
	 * power-on tail in place, the high-offset FW-FIFO writes (0x4000) complete,
	 * so the real download path runs. Non-fatal: even on failure the radio
	 * still brings up for scan/monitor. */
	err = rtl92fe_download_fw(hw, false);
	rtlhal->fw_ready = !err;
	if (err)
		pr_warn("rtl8192fe: FW download failed (err=%d)\n", err);
	err = 0;
	/* FW-related variable init. */
	ppsc->fw_current_inpsmode = false;
	rtlhal->fw_ps_state = FW_PS_STATE_ALL_ON_92F;
	rtlhal->fw_clk_change_in_progress = false;
	rtlhal->allow_sw_to_change_hwclc = false;
	rtlhal->last_hmeboxnum = 0;

	/* BB/RF bring-up via phy.c (MAC table, BB, RF, then channel). */
	rtl92fe_phy_mac_config(hw);
	rtl92fe_phy_bb_config(hw);
	rtl92fe_phy_rf_config(hw);

	rtlphy->rfreg_chnlval[0] = rtl_get_rfreg(hw, RF90_PATH_A,
						 RF_CHNLBW, RFREG_OFFSET_MASK);
	rtlphy->rfreg_chnlval[1] = rtl_get_rfreg(hw, RF90_PATH_B,
						 RF_CHNLBW, RFREG_OFFSET_MASK);
	rtlphy->backup_rf_0x1a = (u32)rtl_get_rfreg(hw, RF90_PATH_A, RF_RX_G1,
						    RFREG_OFFSET_MASK);
	rtlphy->rfreg_chnlval[0] = (rtlphy->rfreg_chnlval[0] & 0xfffff3ff) |
				   BIT(10) | BIT(11);
	/* Seed path B's tracked channel word from path A's known-good value
	 * (band + BW20 bits) so sw_chnl never re-keys RF_B[0x18] from a stale
	 * power-on read.  Belt-and-suspenders with the sw_chnl full-word fix. */
	rtlphy->rfreg_chnlval[1] = rtlphy->rfreg_chnlval[0];

	rtl_set_rfreg(hw, RF90_PATH_A, RF_CHNLBW, RFREG_OFFSET_MASK,
		      rtlphy->rfreg_chnlval[0]);
	rtl_set_rfreg(hw, RF90_PATH_B, RF_CHNLBW, RFREG_OFFSET_MASK,
		      rtlphy->rfreg_chnlval[0]);

	/* ---- Set CCK and OFDM block "ON" ---- */
	rtl_set_bbreg(hw, RFPGA0_RFMOD, BCCKEN, 0x1);
	rtl_set_bbreg(hw, RFPGA0_RFMOD, BOFDMEN, 0x1);

	/* RX-sensitivity LNA bias trim required by the RTL8192F radio. */
	rtl_set_rfreg(hw, RF90_PATH_A, 0xB1, RFREG_OFFSET_MASK, 0x33B8F);

	/* Set hardware (MAC default setting). */
	_rtl92fe_hw_configure(hw);

	rtlhal->mac_func_enable = true;

	rtl_cam_reset_all_entry(hw);
	rtl92fe_enable_hw_security_config(hw);

	ppsc->rfpwr_state = ERFON;

	rtlpriv->cfg->ops->set_hw_reg(hw, HW_VAR_ETHER_ADDR, mac->mac_addr);
	_rtl92fe_enable_aspm_back_door(hw);
	rtlpriv->intf_ops->enable_aspm(hw);

	rtl92fe_bt_hw_init(hw);

	rtlpriv->rtlhal.being_init_adapter = false;

	/* Trim the crystal load cap into the AFE registers (8192F: ...
	 * dev/MEASURED-hw.c.md sec 14. */
	{
		u8 cc = (xtal_cap >= 0) ? (xtal_cap & 0x3f)
				       : (rtlpriv->efuse.crystalcap & 0x3f);
		u32 x1 = rtl_read_dword(rtlpriv, REG_AFE_PLL_CTRL);
		u32 x0 = rtl_read_dword(rtlpriv, REG_AFE_XTAL_CTRL);

		x1 = (x1 & ~0x7eu) | (cc << 1);
		x0 = (x0 & ~0x7e000000u) | (cc << 25);
		rtl_write_dword(rtlpriv, REG_AFE_PLL_CTRL, x1);
		rtl_write_dword(rtlpriv, REG_AFE_XTAL_CTRL, x0);
	}

	/* Run the 8192F TX/RX-path ("TRX mode") init that must follow BB config
	 * (AB/AB paths, rfe_type 3). Skipping it leaves the path registers
	 * mis-configured (0x804[3:0], 0xc04, 0x90c, ...).
	 */
	_rtl92fe_config_trx_mode_ab(hw);
	_rtl92fe_apply_bbpokes(hw);

	/* RX/TX engine is configured (RCR/CR) via the MAC init + hw_configure
	 * above; finish with calibration.
	 */
	if (ppsc->rfpwr_state == ERFON) {
		/* The RTL8192F runs LCK then a 3-run IQK (TX-LOK+IQK then
		 * RX-IQK, path A then B); the heavy lifting lives in phy.c.
		 */
		rtl92fe_phy_lc_calibrate(hw);
		if (rtlphy->iqk_initialized) {
			rtl92fe_phy_iq_calibrate(hw, true);
		} else {
			rtl92fe_phy_iq_calibrate(hw, false);
			rtlphy->iqk_initialized = true;
		}
	}

	rtlphy->rfpath_rx_enable[0] = true;
	if (rtlphy->rf_type == RF_2T2R)
		rtlphy->rfpath_rx_enable[1] = true;

	/* PA-bias compensation per efuse flag at raw byte 0x1FA. */
	efuse_one_byte_read(hw, 0x1FA, &tmp_u1b);
	if (!(tmp_u1b & BIT(0))) {
		rtl_set_rfreg(hw, RF90_PATH_A, 0x15, 0x0F, 0x05);
		rtl_dbg(rtlpriv, COMP_INIT, DBG_LOUD, "PA BIAS path A\n");
	}

	if ((!(tmp_u1b & BIT(1))) && (rtlphy->rf_type == RF_2T2R)) {
		rtl_set_rfreg(hw, RF90_PATH_B, 0x15, 0x0F, 0x05);
		rtl_dbg(rtlpriv, COMP_INIT, DBG_LOUD, "PA BIAS path B\n");
	}

	rtl_write_byte(rtlpriv, REG_NAV_UPPER, ((30000 + 127) / 128));

	rtl92fe_dm_init(hw);

	/* One-line RF bring-up summary (info level so it shows in ...
	 * dev/MEASURED-hw.c.md sec 35. */
	pr_info("rtl8192fe: RF up: chipver=0x%x rf_type=%s rfe_type=0x%x xtal_cap=0x%x RF_A[0x00]=0x%05x RF_A[0x18]=0x%05x RF_B[0x18]=0x%05x\n",
		rtlhal->version,
		(rtlphy->rf_type == RF_2T2R) ? "2T2R" : "1T1R",
		rtlhal->rfe_type,
		(xtal_cap >= 0) ? (xtal_cap & 0x3f)
				: (rtlpriv->efuse.crystalcap & 0x3f),
		rtl_get_rfreg(hw, RF90_PATH_A, RF_AC, RFREG_OFFSET_MASK),
		rtl_get_rfreg(hw, RF90_PATH_A, RF_CHNLBW, RFREG_OFFSET_MASK),
		rtl_get_rfreg(hw, RF90_PATH_B, RF_CHNLBW, RFREG_OFFSET_MASK));

	/* Post-IQK operating dump: TX-IQ correction result ... -- dev/MEASURED-hw.c.md sec 15. */
	pr_info("rtl8192fe: PHY op: 0xc80=0x%08x 0xc94=0x%08x 0xc88=0x%08x 0xc9c=0x%08x 0x90c=0x%08x 0xe00=0x%08x RF_A[0x00]=0x%05x RF_B[0x00]=0x%05x iqk{e94=0x%x e9c=0x%x eb4=0x%x ebc=0x%x}\n",
		rtl_get_bbreg(hw, ROFDM0_XATXIQIMBALANCE, MASKDWORD),
		rtl_get_bbreg(hw, ROFDM0_XCTXAFE, MASKDWORD),
		rtl_get_bbreg(hw, ROFDM0_XBTXIQIMBALANCE, MASKDWORD),
		rtl_get_bbreg(hw, ROFDM0_XDTXAFE, MASKDWORD),
		rtl_get_bbreg(hw, 0x90c, MASKDWORD),
		rtl_get_bbreg(hw, RTXAGC_A_RATE18_06, MASKDWORD),
		rtl_get_rfreg(hw, RF90_PATH_A, RF_AC, RFREG_OFFSET_MASK),
		rtl_get_rfreg(hw, RF90_PATH_B, RF_AC, RFREG_OFFSET_MASK),
		(u32)rtlphy->reg_e94, (u32)rtlphy->reg_e9c,
		(u32)rtlphy->reg_eb4, (u32)rtlphy->reg_ebc);

	_rtl92fe_apply_pokes(hw, "hw_init");
	rtl_dbg(rtlpriv, COMP_INIT, DBG_LOUD,
		"end of Rtl8192FE hw init %x\n", err);
	return 0;
}

static enum version_8192f _rtl92fe_read_chip_version(struct ieee80211_hw *hw)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	struct rtl_phy *rtlphy = &rtlpriv->phy;
	enum version_8192f version;
	u32 value32;

	/* The RTL8192F silicon is a 2-chain (2T2R) RECEIVER, and ...
	 * dev/MEASURED-hw.c.md sec 16. */
	rtlphy->rf_type = RF_2T2R;

	value32 = rtl_read_dword(rtlpriv, REG_SYS_CFG1);
	if (value32 & TRP_VAUX_EN)
		version = (enum version_8192f)VERSION_TEST_CHIP_2T2R_8192F;
	else
		version = (enum version_8192f)VERSION_NORMAL_CHIP_2T2R_8192F;

	rtl_dbg(rtlpriv, COMP_INIT, DBG_LOUD,
		"Chip RF Type: %s\n", (rtlphy->rf_type == RF_2T2R) ?
		"RF_2T2R" : "RF_1T1R");

	return version;
}

static int _rtl92fe_set_media_status(struct ieee80211_hw *hw,
				     enum nl80211_iftype type)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	u8 bt_msr = rtl_read_byte(rtlpriv, MSR) & 0xfc;
	enum led_ctl_mode ledaction = LED_CTL_NO_LINK;
	u8 mode = MSR_NOLINK;

	switch (type) {
	case NL80211_IFTYPE_UNSPECIFIED:
		mode = MSR_NOLINK;
		rtl_dbg(rtlpriv, COMP_INIT, DBG_TRACE,
			"Set Network type to NO LINK!\n");
		break;
	case NL80211_IFTYPE_ADHOC:
	case NL80211_IFTYPE_MESH_POINT:
		mode = MSR_ADHOC;
		rtl_dbg(rtlpriv, COMP_INIT, DBG_TRACE,
			"Set Network type to Ad Hoc!\n");
		break;
	case NL80211_IFTYPE_STATION:
		mode = MSR_INFRA;
		ledaction = LED_CTL_LINK;
		rtl_dbg(rtlpriv, COMP_INIT, DBG_TRACE,
			"Set Network type to STA!\n");
		break;
	case NL80211_IFTYPE_AP:
		mode = MSR_AP;
		ledaction = LED_CTL_LINK;
		rtl_dbg(rtlpriv, COMP_INIT, DBG_TRACE,
			"Set Network type to AP!\n");
		break;
	default:
		pr_err("Network type %d not support!\n", type);
		return 1;
	}

	if (mode != MSR_AP && rtlpriv->mac80211.link_state < MAC80211_LINKED) {
		mode = MSR_NOLINK;
		ledaction = LED_CTL_NO_LINK;
	}

	if (mode == MSR_NOLINK || mode == MSR_INFRA) {
		_rtl92fe_stop_tx_beacon(hw);
		_rtl92fe_enable_bcn_sub_func(hw);
	} else if (mode == MSR_ADHOC || mode == MSR_AP) {
		/* ⚠ THE ARM STAYS HERE FOR NOW, AND THE REASON IS A REFUTATION
		 * dev/MEASURED-hw.c.md sec 17. */
		_rtl92fe_resume_tx_beacon(hw);
		_rtl92fe_disable_bcn_sub_func(hw);
	} else {
		rtl_dbg(rtlpriv, COMP_ERR, DBG_WARNING,
			"Set HW_VAR_MEDIA_STATUS: No such media status(%x).\n",
			mode);
	}

	rtl_write_byte(rtlpriv, MSR, bt_msr | mode);
	rtlpriv->cfg->ops->led_control(hw, ledaction);
	if (mode == MSR_AP)
		rtl_write_byte(rtlpriv, REG_BCNTCFG + 1, 0x00);
	else
		rtl_write_byte(rtlpriv, REG_BCNTCFG + 1, 0x66);
	return 0;
}

void rtl92fe_set_check_bssid(struct ieee80211_hw *hw, bool check_bssid)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	struct rtl_pci *rtlpci = rtl_pcidev(rtl_pcipriv(hw));
	u32 reg_rcr = rtlpci->receive_config;

	if (rtlpriv->psc.rfpwr_state != ERFON)
		return;

	if (check_bssid) {
		reg_rcr |= (RCR_CBSSID_DATA | RCR_CBSSID_BCN);
		rtlpriv->cfg->ops->set_hw_reg(hw, HW_VAR_RCR,
					      (u8 *)(&reg_rcr));
		_rtl92fe_set_bcn_ctrl_reg(hw, 0, BIT(4));
	} else {
		reg_rcr &= (~(RCR_CBSSID_DATA | RCR_CBSSID_BCN));
		_rtl92fe_set_bcn_ctrl_reg(hw, BIT(4), 0);
		rtlpriv->cfg->ops->set_hw_reg(hw, HW_VAR_RCR,
					      (u8 *)(&reg_rcr));
	}
}

int rtl92fe_set_network_type(struct ieee80211_hw *hw, enum nl80211_iftype type)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);

	if (_rtl92fe_set_media_status(hw, type))
		return -EOPNOTSUPP;

	/* check-BSSID (drop RX not matching our BSSID) is wanted ONLY ...
	 * dev/MEASURED-hw.c.md sec 18. */
	if (rtlpriv->mac80211.link_state == MAC80211_LINKED &&
	    type != NL80211_IFTYPE_AP &&
	    type != NL80211_IFTYPE_MESH_POINT)
		rtl92fe_set_check_bssid(hw, true);
	else
		rtl92fe_set_check_bssid(hw, false);

	return 0;
}

/* Don't set REG_EDCA_BE_PARAM here because mac80211 sends pkt when scanning. */
void rtl92fe_set_qos(struct ieee80211_hw *hw, int aci)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);

	rtl92fe_dm_init_edca_turbo(hw);
	switch (aci) {
	case AC1_BK:
		rtl_write_dword(rtlpriv, REG_EDCA_BK_PARAM, 0xa44f);
		break;
	case AC0_BE:
		/* handled by EDCA-turbo */
		break;
	case AC2_VI:
		rtl_write_dword(rtlpriv, REG_EDCA_VI_PARAM, 0x5ea324);
		break;
	case AC3_VO:
		rtl_write_dword(rtlpriv, REG_EDCA_VO_PARAM, 0x2fa226);
		break;
	default:
		WARN_ONCE(true, "rtl8192fe: invalid aci: %d !\n", aci);
		break;
	}
}

void rtl92fe_enable_interrupt(struct ieee80211_hw *hw)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	struct rtl_pci *rtlpci = rtl_pcidev(rtl_pcipriv(hw));

	/* ★★ THE FLAG FIRST, THEN THE HARDWARE -- THIS ORDER IS THE ...
	 * dev/MEASURED-hw.c.md sec 19. */
	rtlpci->irq_enabled = true;
	rtl_write_dword(rtlpriv, REG_HIMR, rtlpci->irq_mask[0] & 0xFFFFFFFF);
	rtl_write_dword(rtlpriv, REG_HIMRE, rtlpci->irq_mask[1] & 0xFFFFFFFF);
}

void rtl92fe_disable_interrupt(struct ieee80211_hw *hw)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	struct rtl_pci *rtlpci = rtl_pcidev(rtl_pcipriv(hw));

	/* ★ AND THE MIRROR ON THE WAY DOWN. The io accessors ... -- dev/MEASURED-hw.c.md sec 20. */
	rtl_write_dword(rtlpriv, REG_HIMR, IMR_DISABLED);
	rtl_write_dword(rtlpriv, REG_HIMRE, IMR_DISABLED);
	rtl_read_dword(rtlpriv, REG_HIMR);	/* flush the posted writes */
	rtlpci->irq_enabled = false;
}

static void _rtl92fe_poweroff_adapter(struct ieee80211_hw *hw)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	struct rtl_hal *rtlhal = rtl_hal(rtl_priv(hw));
	u8 u1b_tmp;

	rtlhal->mac_func_enable = false;

	rtl_dbg(rtlpriv, COMP_INIT, DBG_LOUD, "POWER OFF adapter\n");

	/* Run LPS WL RFOFF flow */
	rtl_hal_pwrseqcmdparsing(rtlpriv, PWR_CUT_ALL_MSK, PWR_FAB_ALL_MSK,
				 PWR_INTF_PCI_MSK, RTL8192F_NIC_LPS_ENTER_FLOW);
	/* turn off RF */
	rtl_write_byte(rtlpriv, REG_RF_CTRL, 0x00);

	/* ==== Reset digital sequence ==== */
	if ((rtl_read_byte(rtlpriv, REG_MCUFWDL) & BIT(7)) && rtlhal->fw_ready)
		rtl92fe_firmware_selfreset(hw);

	/* Reset MCU */
	u1b_tmp = rtl_read_byte(rtlpriv, REG_SYS_FUNC_EN + 1);
	rtl_write_byte(rtlpriv, REG_SYS_FUNC_EN + 1, (u1b_tmp & (~BIT(2))));

	/* reset MCU ready status */
	rtl_write_byte(rtlpriv, REG_MCUFWDL, 0x00);

	/* HW card disable configuration. */
	rtl_hal_pwrseqcmdparsing(rtlpriv, PWR_CUT_ALL_MSK, PWR_FAB_ALL_MSK,
				 PWR_INTF_PCI_MSK, RTL8192F_NIC_DISABLE_FLOW);

	/* Reset MCU IO Wrapper */
	u1b_tmp = rtl_read_byte(rtlpriv, REG_RSV_CTRL + 1);
	rtl_write_byte(rtlpriv, REG_RSV_CTRL + 1, (u1b_tmp & (~BIT(0))));
	u1b_tmp = rtl_read_byte(rtlpriv, REG_RSV_CTRL + 1);
	rtl_write_byte(rtlpriv, REG_RSV_CTRL + 1, (u1b_tmp | BIT(0)));

	/* lock ISO/CLK/Power control register */
	rtl_write_byte(rtlpriv, REG_RSV_CTRL, 0x0E);
}

void rtl92fe_card_disable(struct ieee80211_hw *hw)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	struct rtl_ps_ctl *ppsc = rtl_psc(rtl_priv(hw));
	struct rtl_mac *mac = rtl_mac(rtl_priv(hw));
	enum nl80211_iftype opmode;

	rtl_dbg(rtlpriv, COMP_INIT, DBG_LOUD, "RTL8192fe card disable\n");

	RT_SET_PS_LEVEL(ppsc, RT_RF_OFF_LEVL_HALT_NIC);

	mac->link_state = MAC80211_NOLINK;
	opmode = NL80211_IFTYPE_UNSPECIFIED;

	_rtl92fe_set_media_status(hw, opmode);

	if (rtlpriv->rtlhal.driver_is_goingto_unload ||
	    ppsc->rfoff_reason > RF_CHANGE_BY_PS)
		rtlpriv->cfg->ops->led_control(hw, LED_CTL_POWER_OFF);

	_rtl92fe_poweroff_adapter(hw);

	/* P3: after power off we must redo IQK. Clear iqk_initialized ...
	 * dev/MEASURED-hw.c.md sec 21. */
	rtlpriv->phy.iqk_initialized = false;
}

/* Vendor 8192cd on TXERR: read TXDMA_STATUS, log, count, write it back.
 * Also polled once per watchdog tick: 0x8006 raised no interrupt (measured).
 */
void rtl92fe_txdma_error(struct ieee80211_hw *hw)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	struct rtl_pci *rtlpci = rtl_pcidev(rtl_pcipriv(hw));
	u32 status = rtl_read_dword(rtlpriv, REG_TXDMA_STATUS);

	if (!status)
		return;
	rtlpci->txdma_err++;
	rtlpci->txdma_status |= status;
	rtl_write_dword(rtlpriv, REG_TXDMA_STATUS, status);
	/* The beacon's side of the moment: under an Ethernet load the WiFi TX
	 * path carries almost only beacons, so the context that discriminates
	 * a late beacon rewrite from a starved DMA is printed with the error.
	 */
	pr_warn_ratelimited("TXDMA error 0x%08x (%u so far) bcn: %uus after irq, tasklet done at %uus, tasklet %u late %u, DWBCN0 0x%08x free_tail 0x%02x mgq_idx 0x%08x\n",
			    status, rtlpci->txdma_err,
			    (u32)ktime_to_us(ktime_get()) - rtlpci->bcn_irq_us,
			    rtlpci->bcn_done_us - rtlpci->bcn_irq_us,
			    rtlpci->bcn_tasklet, rtlpci->bcn_late,
			    rtl_read_dword(rtlpriv, REG_DWBCN0_CTRL),
			    rtl_read_byte(rtlpriv, REG_MULTI_BCNQ_OFFSET),
			    rtl_read_dword(rtlpriv, REG_MGQ_TXBD_IDX));
}

void rtl92fe_interrupt_recognized(struct ieee80211_hw *hw,
				  struct rtl_int *intvec)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	struct rtl_pci *rtlpci = rtl_pcidev(rtl_pcipriv(hw));

	intvec->inta = rtl_read_dword(rtlpriv, ISR) & rtlpci->irq_mask[0];
	rtl_write_dword(rtlpriv, ISR, intvec->inta);

	intvec->intb = rtl_read_dword(rtlpriv, REG_HISRE) & rtlpci->irq_mask[1];
	rtl_write_dword(rtlpriv, REG_HISRE, intvec->intb);
	if (intvec->intb & IMR_TXERR)
		rtl92fe_txdma_error(hw);
}

void rtl92fe_set_beacon_related_registers(struct ieee80211_hw *hw)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	struct rtl_mac *mac = rtl_mac(rtl_priv(hw));
	struct rtl_pci *rtlpci = rtl_pcidev(rtl_pcipriv(hw));
	u16 bcn_interval, atim_window;

	bcn_interval = mac->beacon_interval;
	atim_window = 2;
	rtl92fe_disable_interrupt(hw);
	rtl_write_word(rtlpriv, REG_ATIMWND, atim_window);
	rtl_write_word(rtlpriv, REG_BCN_INTERVAL, bcn_interval);
	rtl_write_word(rtlpriv, REG_BCNTCFG, 0x660f);
	/* DRVERLYINT/BCNDMATIM: set once at MAC init, _rtl92fe_set_beacon_window. */
	rtl_write_byte(rtlpriv, REG_RXTSF_OFFSET_CCK, 0x18);
	rtl_write_byte(rtlpriv, REG_RXTSF_OFFSET_OFDM, 0x18);
	rtl_write_byte(rtlpriv, REG_RXTSF_OFFSET_OFDM - 2, 0x30);
	rtlpci->reg_bcn_ctrl_val |= BIT(3);
	rtl_write_byte(rtlpriv, REG_BCN_CTRL, (u8)rtlpci->reg_bcn_ctrl_val);
	_rtl92fe_apply_pokes(hw, "beacon_related_registers");

	/* NB: do NOT set ENSWBCN (REG_CR bit8) here to force a ...
	 * dev/MEASURED-hw.c.md sec 22. */
	rtl92fe_enable_interrupt(hw);
}

void rtl92fe_set_beacon_interval(struct ieee80211_hw *hw)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	struct rtl_mac *mac = rtl_mac(rtl_priv(hw));
	u16 bcn_interval = mac->beacon_interval;

	rtl_dbg(rtlpriv, COMP_BEACON, DBG_DMESG,
		"beacon_interval:%d\n", bcn_interval);
	rtl_write_word(rtlpriv, REG_BCN_INTERVAL, bcn_interval);
}

void rtl92fe_update_interrupt_mask(struct ieee80211_hw *hw,
				   u32 add_msr, u32 rm_msr)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	struct rtl_pci *rtlpci = rtl_pcidev(rtl_pcipriv(hw));

	rtl_dbg(rtlpriv, COMP_INTR, DBG_LOUD,
		"add_msr:%x, rm_msr:%x\n", add_msr, rm_msr);

	if (add_msr)
		rtlpci->irq_mask[0] |= add_msr;
	if (rm_msr)
		rtlpci->irq_mask[0] &= (~rm_msr);
	rtl92fe_disable_interrupt(hw);
	rtl92fe_enable_interrupt(hw);
}

static __always_inline u8 _rtl92fe_get_chnl_group(u8 chnl)
{
	u8 group = 0;

	/* The RTL8192F is a 2.4 GHz-only part: only the 14-channel
	 * 5-group mapping applies.
	 */
	if (chnl >= 1 && chnl <= 2)
		group = 0;
	else if (chnl >= 3 && chnl <= 5)
		group = 1;
	else if (chnl >= 6 && chnl <= 8)
		group = 2;
	else if (chnl >= 9 && chnl <= 11)
		group = 3;
	else if (chnl >= 12 && chnl <= 14)
		group = 4;

	return group;
}

static void _rtl92fe_read_power_value_fromprom(struct ieee80211_hw *hw,
					       struct txpower_info_2g *pwr2g,
					       bool autoload_fail, u8 *hwinfo)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	u32 rf, addr = EEPROM_TX_PWR_INX, group, i = 0;

	rtl_dbg(rtlpriv, COMP_INIT, DBG_LOUD,
		"hal_ReadPowerValueFromPROM92F(): PROMContent[0x%x]=0x%x\n",
		(addr + 1), hwinfo[addr + 1]);
	if (hwinfo[addr + 1] == 0xFF)	/* signature byte unprogrammed */
		autoload_fail = true;

	if (autoload_fail) {
		rtl_dbg(rtlpriv, COMP_INIT, DBG_LOUD,
			"auto load fail : Use Default value!\n");
		for (rf = 0; rf < MAX_RF_PATH; rf++) {
			/* 2.4 GHz default value */
			for (group = 0; group < MAX_CHNL_GROUP_24G; group++) {
				pwr2g->index_cck_base[rf][group] = 0x2D;
				pwr2g->index_bw40_base[rf][group] = 0x2D;
			}
			for (i = 0; i < MAX_TX_COUNT; i++) {
				if (i == 0) {
					pwr2g->bw20_diff[rf][0] = 0x02;
					pwr2g->ofdm_diff[rf][0] = 0x04;
				} else {
					pwr2g->bw20_diff[rf][i] = 0xFE;
					pwr2g->bw40_diff[rf][i] = 0xFE;
					pwr2g->cck_diff[rf][i] = 0xFE;
					pwr2g->ofdm_diff[rf][i] = 0xFE;
				}
			}
		}
		return;
	}

	rtl_priv(hw)->efuse.txpwr_fromeprom = true;

	for (rf = 0; rf < MAX_RF_PATH; rf++) {
		/* 2.4 GHz CCK base */
		for (group = 0; group < MAX_CHNL_GROUP_24G; group++) {
			pwr2g->index_cck_base[rf][group] = hwinfo[addr++];
			if (pwr2g->index_cck_base[rf][group] == 0xFF)
				pwr2g->index_cck_base[rf][group] = 0x2D;
		}
		/* 2.4 GHz BW40 base (5 group bytes; last reused for ch14) */
		for (group = 0; group < MAX_CHNL_GROUP_24G - 1; group++) {
			pwr2g->index_bw40_base[rf][group] = hwinfo[addr++];
			if (pwr2g->index_bw40_base[rf][group] == 0xFF)
				pwr2g->index_bw40_base[rf][group] = 0x2D;
		}
		for (i = 0; i < MAX_TX_COUNT; i++) {
			if (i == 0) {
				pwr2g->bw40_diff[rf][i] = 0;
				if (hwinfo[addr] == 0xFF) {
					pwr2g->bw20_diff[rf][i] = 0x02;
				} else {
					pwr2g->bw20_diff[rf][i] = (hwinfo[addr]
								   & 0xf0) >> 4;
					if (pwr2g->bw20_diff[rf][i] & BIT(3))
						pwr2g->bw20_diff[rf][i] |= 0xF0;
				}

				if (hwinfo[addr] == 0xFF) {
					pwr2g->ofdm_diff[rf][i] = 0x04;
				} else {
					pwr2g->ofdm_diff[rf][i] = (hwinfo[addr]
								   & 0x0f);
					if (pwr2g->ofdm_diff[rf][i] & BIT(3))
						pwr2g->ofdm_diff[rf][i] |= 0xF0;
				}
				pwr2g->cck_diff[rf][i] = 0;
				addr++;
			} else {
				if (hwinfo[addr] == 0xFF) {
					pwr2g->bw40_diff[rf][i] = 0xFE;
				} else {
					pwr2g->bw40_diff[rf][i] = (hwinfo[addr]
								   & 0xf0) >> 4;
					if (pwr2g->bw40_diff[rf][i] & BIT(3))
						pwr2g->bw40_diff[rf][i] |= 0xF0;
				}

				if (hwinfo[addr] == 0xFF) {
					pwr2g->bw20_diff[rf][i] = 0xFE;
				} else {
					pwr2g->bw20_diff[rf][i] = (hwinfo[addr]
								   & 0x0f);
					if (pwr2g->bw20_diff[rf][i] & BIT(3))
						pwr2g->bw20_diff[rf][i] |= 0xF0;
				}
				addr++;

				if (hwinfo[addr] == 0xFF) {
					pwr2g->ofdm_diff[rf][i] = 0xFE;
				} else {
					pwr2g->ofdm_diff[rf][i] = (hwinfo[addr]
								   & 0xf0) >> 4;
					if (pwr2g->ofdm_diff[rf][i] & BIT(3))
						pwr2g->ofdm_diff[rf][i] |= 0xF0;
				}

				if (hwinfo[addr] == 0xFF) {
					pwr2g->cck_diff[rf][i] = 0xFE;
				} else {
					pwr2g->cck_diff[rf][i] = (hwinfo[addr]
								  & 0x0f);
					if (pwr2g->cck_diff[rf][i] & BIT(3))
						pwr2g->cck_diff[rf][i] |= 0xF0;
				}
				addr++;
			}
		}

		/* The 2.4 GHz-only RTL8192F has no 5 GHz tx-power block, but
		 * the path-B sub-table still begins at the 0x3A offset; the
		 * EEPROM_TX_PWR_INX base + per-rf stride lands path B there.
		 */
	}
}

static noinline_for_stack void
_rtl92fe_read_txpower_info_from_hwpg(struct ieee80211_hw *hw,
				     bool autoload_fail, u8 *hwinfo)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	struct rtl_efuse *efu = rtl_efuse(rtl_priv(hw));
	struct txpower_info_2g pwr2g;
	u8 rf, idx;
	u8 i;

	_rtl92fe_read_power_value_fromprom(hw, &pwr2g, autoload_fail, hwinfo);

	for (rf = 0; rf < MAX_RF_PATH; rf++) {
		for (i = 0; i < 14; i++) {
			idx = _rtl92fe_get_chnl_group(i + 1);

			if (i == CHANNEL_MAX_NUMBER_2G - 1) {
				efu->txpwrlevel_cck[rf][i] =
						pwr2g.index_cck_base[rf][5];
				efu->txpwrlevel_ht40_1s[rf][i] =
						pwr2g.index_bw40_base[rf][idx];
			} else {
				efu->txpwrlevel_cck[rf][i] =
						pwr2g.index_cck_base[rf][idx];
				efu->txpwrlevel_ht40_1s[rf][i] =
						pwr2g.index_bw40_base[rf][idx];
			}
		}
		for (i = 0; i < MAX_TX_COUNT; i++) {
			efu->txpwr_cckdiff[rf][i] = pwr2g.cck_diff[rf][i];
			efu->txpwr_legacyhtdiff[rf][i] = pwr2g.ofdm_diff[rf][i];
			efu->txpwr_ht20diff[rf][i] = pwr2g.bw20_diff[rf][i];
			efu->txpwr_ht40diff[rf][i] = pwr2g.bw40_diff[rf][i];
		}
	}

	/* thermal meter @ 0xBA */
	if (!autoload_fail)
		efu->eeprom_thermalmeter = hwinfo[EEPROM_THERMAL_METER_92F];
	else
		efu->eeprom_thermalmeter = EEPROM_DEFAULT_THERMALMETER;

	if (efu->eeprom_thermalmeter == 0xff || autoload_fail) {
		efu->apk_thermalmeterignore = true;
		efu->eeprom_thermalmeter = EEPROM_DEFAULT_THERMALMETER;
	}

	efu->thermalmeter[0] = efu->eeprom_thermalmeter;
	RTPRINT(rtlpriv, FINIT, INIT_TXPOWER,
		"thermalmeter = 0x%x\n", efu->eeprom_thermalmeter);

	/* RFE/board option @ 0xCA on the RTL8192F (& 0x1F). */
	if (!autoload_fail) {
		efu->eeprom_regulatory = hwinfo[EEPROM_RFE_OPTION_92F] & 0x07;
		if (hwinfo[EEPROM_RFE_OPTION_92F] == 0xFF)
			efu->eeprom_regulatory = 0;
	} else {
		efu->eeprom_regulatory = 0;
	}
	RTPRINT(rtlpriv, FINIT, INIT_TXPOWER,
		"eeprom_regulatory = 0x%x\n", efu->eeprom_regulatory);

	/* The RFE type (efuse RFE option @0xCA, bits[4:0]) selects ...
	 * dev/MEASURED-hw.c.md sec 23. */
	if (!autoload_fail && hwinfo[EEPROM_RFE_OPTION_92F] != 0xFF)
		rtl_hal(rtlpriv)->rfe_type = hwinfo[EEPROM_RFE_OPTION_92F] & 0x1f;
	else
		rtl_hal(rtlpriv)->rfe_type = 0;
	RTPRINT(rtlpriv, FINIT, INIT_TXPOWER,
		"rfe_type = 0x%x\n", rtl_hal(rtlpriv)->rfe_type);
}

static void _rtl92fe_read_adapter_info(struct ieee80211_hw *hw)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	struct rtl_efuse *rtlefuse = rtl_efuse(rtl_priv(hw));
	struct rtl_hal *rtlhal = rtl_hal(rtl_priv(hw));
	/* The RTL8192F efuse signature (0x8129) lives at offset 0x00 ...
	 * dev/MEASURED-hw.c.md sec 24. */
	int params[] = {RTL8192F_EEPROM_ID, EEPROM_VID_92F, EEPROM_DID_92F,
			EEPROM_SVID_92F, EEPROM_SMID_92F, EEPROM_MAC_ADDR_92F,
			EEPROM_CHANNELPLAN_92F, EEPROM_VERSION_92F,
			EEPROM_CUSTOMER_ID_92F, COUNTRY_CODE_WORLD_WIDE_13};
	u8 *hwinfo;

	hwinfo = kzalloc(HWSET_MAX_SIZE, GFP_KERNEL);
	if (!hwinfo)
		return;

	if (rtl_get_hwinfo(hw, rtlpriv, HWSET_MAX_SIZE, hwinfo, params))
		goto exit;

	if (rtlefuse->eeprom_oemid == 0xFF)
		rtlefuse->eeprom_oemid = 0;

	rtl_dbg(rtlpriv, COMP_INIT, DBG_LOUD,
		"EEPROM Customer ID: 0x%2x\n", rtlefuse->eeprom_oemid);
	/* set channel plan from efuse (@ 0xB8) */
	rtlefuse->channel_plan = rtlefuse->eeprom_channelplan;
	/* tx power */
	_rtl92fe_read_txpower_info_from_hwpg(hw, rtlefuse->autoload_failflag,
					     hwinfo);

	rtl92fe_read_bt_coexist_info_from_hwpg(hw, rtlefuse->autoload_failflag,
					       hwinfo);

	/* board type (RFE option @ 0xCA, board-type nibble in bits[7:5]) */
	rtlefuse->board_type = (((*(u8 *)&hwinfo[EEPROM_RFE_OPTION_92F])
				& 0xE0) >> 5);
	if ((*(u8 *)&hwinfo[EEPROM_RFE_OPTION_92F]) == 0xFF)
		rtlefuse->board_type = 0;

	if (rtlpriv->btcoexist.btc_info.btcoexist == 1)
		rtlefuse->board_type |= BIT(2); /* ODM_BOARD_BT */

	rtlhal->board_type = rtlefuse->board_type;
	rtl_dbg(rtlpriv, COMP_INIT, DBG_LOUD,
		"board_type = 0x%x\n", rtlefuse->board_type);
	/* parse xtal (@ 0xB9, & 0x3F) */
	rtlefuse->crystalcap = hwinfo[EEPROM_XTAL_92F] & 0x3F;
	if (hwinfo[EEPROM_XTAL_92F] == 0xFF)
		rtlefuse->crystalcap = 0x20;

	/* antenna diversity */
	rtlefuse->antenna_div_type = NO_ANTDIV;
	rtlefuse->antenna_div_cfg = 0;

	if (rtlhal->oem_id == RT_CID_DEFAULT) {
		switch (rtlefuse->eeprom_oemid) {
		case EEPROM_CID_DEFAULT:
			if (rtlefuse->eeprom_did == 0x818C) {
				if ((rtlefuse->eeprom_svid == 0x10EC) &&
				    (rtlefuse->eeprom_smid == 0x001B))
					rtlhal->oem_id = RT_CID_819X_LENOVO;
			} else {
				rtlhal->oem_id = RT_CID_DEFAULT;
			}
			break;
		default:
			rtlhal->oem_id = RT_CID_DEFAULT;
			break;
		}
	}
exit:
	kfree(hwinfo);
}

static void _rtl92fe_hal_customized_behavior(struct ieee80211_hw *hw)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	struct rtl_hal *rtlhal = rtl_hal(rtl_priv(hw));

	rtlpriv->ledctl.led_opendrain = true;

	rtl_dbg(rtlpriv, COMP_INIT, DBG_DMESG,
		"RT Customized ID: 0x%02X\n", rtlhal->oem_id);
}

/* Board factory WiFi calibration (flash apmib HW_WLAN0_*) The ...
 * dev/MEASURED-hw.c.md sec 25. */
struct rtl92fe_board_cal {
	/* ★★★ WHICH BOARD THIS CAL BELONGS TO -- the DT ROOT compatible.
	 * Added 2026-08-27 after this table was MEASURED being applied to the
	 * wrong board: see the note at rtl92fe_board_cals[]. */
	const char *compat;
	const char *board;
	u8 mac[ETH_ALEN];
	/* per-channel (ch1..ch14) CCK base power, path A / path B */
	u8 cck_a[CHANNEL_MAX_NUMBER_2G];
	u8 cck_b[CHANNEL_MAX_NUMBER_2G];
	/* per-channel (ch1..ch14) HT40 1S base power, path A / path B */
	u8 ht40_1s_a[CHANNEL_MAX_NUMBER_2G];
	u8 ht40_1s_b[CHANNEL_MAX_NUMBER_2G];
	u8 thermalmeter;	/* HW_WLAN0_11N_THER  */
	u8 crystalcap;		/* HW_WLAN0_11N_XCAP  (xtal load cap) */
	u8 pa_type;		/* HW_WLAN0_11N_PA_TYPE (0 = internal PA) */
	u8 rfe_type;		/* Board RF front-end topology, independent of PA type. */
	u8 reg_domain;		/* HW_WLAN0_REG_DOMAIN */
};

static const struct rtl92fe_board_cal rtl92fe_x111w_cal = {
	.compat = "realtek,rtl9602c", .board = "X111W",
	/* NO MAC: every X111W wore the lab unit's 98:c7:a4:32:82:ae (2026-09-30).
	 * The BSSID is the unit's own (uci); the numbers below are the lab
	 * unit's, a model fallback until gpon_provision hands over unit_*. */
	.cck_a = { 0x27, 0x27, 0x27, 0x28, 0x28, 0x28, 0x28,
		   0x28, 0x28, 0x29, 0x29, 0x29, 0x29, 0x29 },
	.cck_b = { 0x26, 0x26, 0x26, 0x28, 0x28, 0x28, 0x28,
		   0x28, 0x28, 0x27, 0x27, 0x27, 0x27, 0x27 },
	.ht40_1s_a = { 0x2e, 0x2e, 0x2e, 0x2e, 0x2e, 0x2e, 0x2e,
		       0x2e, 0x2e, 0x2e, 0x2e, 0x2e, 0x2e, 0x2e },
	.ht40_1s_b = { 0x2c, 0x2c, 0x2c, 0x2c, 0x2c, 0x2c, 0x2c,
		       0x2c, 0x2c, 0x2c, 0x2c, 0x2c, 0x2c, 0x2c },
	.thermalmeter = 36,		/* MIB text "36", INT_T => base 10 */
	.crystalcap = 47,		/* MIB text "47", INT_T => base 10 */
	.pa_type = 0,
	.rfe_type = 3,		/* Own stock M225-260618 mib_rf and native RFE init. */
	.reg_domain = 1,
};

/* LANLY G24W (RTL9603CVD). READ FROM THIS UNIT'S OWN FLASH ... -- dev/MEASURED-hw.c.md sec 26. */
static const struct rtl92fe_board_cal rtl92fe_g24w_cal = {
	.compat = "realtek,rtl9603cvd", .board = "G24W",
	/* NO MAC, as the X111W: a unit MAC compiled in is every unit's MAC. */
	.cck_a = { 0x2c, 0x2c, 0x2c, 0x2c, 0x2c, 0x2c, 0x2c,
		   0x2c, 0x2c, 0x2c, 0x2c, 0x2c, 0x2c, 0x2c },
	.cck_b = { 0x2f, 0x2f, 0x2f, 0x2f, 0x2f, 0x2f, 0x2f,
		   0x2f, 0x2f, 0x2f, 0x2f, 0x2f, 0x2f, 0x2f },
	.ht40_1s_a = { 0x2c, 0x2c, 0x2c, 0x2c, 0x2c, 0x2c, 0x2c,
		       0x2c, 0x2c, 0x2d, 0x2d, 0x2d, 0x2d, 0x2d },
	.ht40_1s_b = { 0x2e, 0x2e, 0x2e, 0x2e, 0x2e, 0x2e, 0x2e,
		       0x2e, 0x2e, 0x2f, 0x2f, 0x2f, 0x2f, 0x2f },
	.thermalmeter = 34,		/* MIB text "34", INT_T => base 10 */
	.crystalcap = 21,		/* MIB text "21", INT_T => base 10 */
	.pa_type = 0,
	.rfe_type = 7,		/* Preserve existing profile pending own-stock RFE evidence. */
	.reg_domain = 14,		/* MIB text "14", INT_T => base 10 */
};

/* ★★★ A BOARD'S CAL IS APPLIED TO THAT BOARD, AND TO NO OTHER ...
 * dev/MEASURED-hw.c.md sec 27. */
static const struct rtl92fe_board_cal *const rtl92fe_board_cals[] = {
	&rtl92fe_x111w_cal,
	&rtl92fe_g24w_cal,
};

static const struct rtl92fe_board_cal *_rtl92fe_board_cal_for_this_board(void)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(rtl92fe_board_cals); i++)
		if (rtl92fe_board_cals[i]->compat &&
		    of_machine_is_compatible(rtl92fe_board_cals[i]->compat))
			return rtl92fe_board_cals[i];
	return NULL;
}

/* Populate rtlefuse from the board cal table when the efuse is blank, then
 * clear autoload_failflag so the regular RF / tx-power init runs with these
 * values instead of the generic defaults.
 */
static void _rtl92fe_apply_board_cal(struct ieee80211_hw *hw,
				     const struct rtl92fe_board_cal *cal)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	struct rtl_efuse *efu = rtl_efuse(rtl_priv(hw));
	u8 rf, ch;

	/* MAC: identity hint / fallback only. The operational MAC is ...
	 * dev/MEASURED-hw.c.md sec 36. */
	memcpy(efu->dev_addr, cal->mac, ETH_ALEN);

	/* Per-channel tx-power.  Paths A/B are the only populated RF paths on
	 * this 1T1R part; fill A and B (and mirror onto any extra MAX_RF_PATH
	 * slots from path A so nothing is left at 0).
	 */
	for (rf = 0; rf < MAX_RF_PATH; rf++) {
		const u8 *cck = (rf == RF90_PATH_B) ? cal->cck_b : cal->cck_a;
		const u8 *ht40 = (rf == RF90_PATH_B) ? cal->ht40_1s_b
						     : cal->ht40_1s_a;

		for (ch = 0; ch < CHANNEL_MAX_NUMBER_2G; ch++) {
			efu->txpwrlevel_cck[rf][ch] = cck[ch];
			efu->txpwrlevel_ht40_1s[rf][ch] = ht40[ch];
		}

		/* All power diffs are zero on this board (DIFF_OFDM /
		 * DIFF_HT20 / DIFF_HT40_2S == 0): HT20 == HT40, OFDM == CCK
		 * base, no 2S path.
		 */
		for (ch = 0; ch < MAX_TX_COUNT; ch++) {
			efu->txpwr_cckdiff[rf][ch] = 0;
			efu->txpwr_legacyhtdiff[rf][ch] = 0;
			efu->txpwr_ht20diff[rf][ch] = 0;
			efu->txpwr_ht40diff[rf][ch] = 0;
		}
	}
	efu->txpwr_fromeprom = true;

	/* Thermal meter: drives the thermal-tracking tx-power compensation.
	 * A real (non-0xff) value means we do NOT ignore thermal tracking.
	 */
	efu->eeprom_thermalmeter = cal->thermalmeter;
	efu->thermalmeter[0] = cal->thermalmeter;
	efu->thermalmeter[1] = cal->thermalmeter;
	efu->apk_thermalmeterignore = false;

	/* Crystal load cap (XCAP). Critical for an on-frequency ...
	 * dev/MEASURED-hw.c.md sec 28. */
	if (cal->crystalcap > 0x3f)
		pr_warn("rtl8192fe: board cal crystalcap 0x%02x exceeds the 6-bit AFE field and is being masked to 0x%02x -- the cal is wrong, not the radio\n",
			cal->crystalcap, cal->crystalcap & 0x3f);
	efu->crystalcap = cal->crystalcap & 0x3f;
	efu->eeprom_crystalcap = cal->crystalcap & 0x3f;

	/* Front-end / regulatory. The WiFi efuse is blank, so ...
	 * dev/MEASURED-hw.c.md sec 29. */
	rtl_hal(rtlpriv)->rfe_type = cal->rfe_type;
	efu->board_type = 0;
	rtl_hal(rtlpriv)->board_type = 0;
	efu->external_pa = !!cal->pa_type;
	efu->eeprom_regulatory = cal->reg_domain & 0x07;

	/* Channel plan: 2.4 GHz world-wide 13 (+ ch14 handled per-channel). */
	efu->channel_plan = COUNTRY_CODE_WORLD_WIDE_13;

	/* The cal is now in place: let the normal init treat the efuse as
	 * loaded so RF / tx-power bring-up uses these values.
	 */
	efu->autoload_failflag = false;

	pr_info("rtl8192fe: applied board WiFi cal (%s): MAC=%pM xtal=0x%02x thermal=0x%02x cck[A1]=0x%02x ht40[A1]=0x%02x reg=%u\n",
		cal->board ? cal->board : "?",
		efu->dev_addr, efu->crystalcap, efu->eeprom_thermalmeter,
		efu->txpwrlevel_cck[RF90_PATH_A][0],
		efu->txpwrlevel_ht40_1s[RF90_PATH_A][0],
		efu->eeprom_regulatory);
}

/* The unit's own cal, when gpon_provision handed over all of it. Runs at every
 * hw_init: the radio starts after provisioning, and a restart keeps it. */
static void _rtl92fe_apply_unit_cal(struct ieee80211_hw *hw)
{
	const struct rtl92fe_board_cal *model = _rtl92fe_board_cal_for_this_board();
	struct rtl92fe_board_cal cal = { .board = "this unit's flash MIB" };

	if (unit_cck_a_n != CHANNEL_MAX_NUMBER_2G ||
	    unit_cck_b_n != CHANNEL_MAX_NUMBER_2G ||
	    unit_ht40_a_n != CHANNEL_MAX_NUMBER_2G ||
	    unit_ht40_b_n != CHANNEL_MAX_NUMBER_2G ||
	    unit_thermal < 0 || unit_thermal > 0xff ||
	    unit_xcap < 0 || unit_xcap > 0xff ||
	    unit_reg_domain < 0 || unit_reg_domain > 0xff) {
		pr_warn_once("rtl8192fe: no complete per-unit cal (unit_* parameters) -- tx-power and crystal trim are the board table's, another unit's\n");
		return;
	}
	memcpy(cal.mac, rtl_efuse(rtl_priv(hw))->dev_addr, ETH_ALEN);
	memcpy(cal.cck_a, unit_cck_a, sizeof(cal.cck_a));
	memcpy(cal.cck_b, unit_cck_b, sizeof(cal.cck_b));
	memcpy(cal.ht40_1s_a, unit_ht40_a, sizeof(cal.ht40_1s_a));
	memcpy(cal.ht40_1s_b, unit_ht40_b, sizeof(cal.ht40_1s_b));
	cal.thermalmeter = unit_thermal;
	cal.crystalcap = unit_xcap;
	cal.reg_domain = unit_reg_domain;
	cal.pa_type = model ? model->pa_type : 0;
	cal.rfe_type = model ? model->rfe_type : rtl_hal(rtl_priv(hw))->rfe_type;
	_rtl92fe_apply_board_cal(hw, &cal);
}

void rtl92fe_read_eeprom_info(struct ieee80211_hw *hw)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	struct rtl_efuse *rtlefuse = rtl_efuse(rtl_priv(hw));
	struct rtl_phy *rtlphy = &rtlpriv->phy;
	struct rtl_hal *rtlhal = rtl_hal(rtl_priv(hw));
	u8 tmp_u1b;

	rtlhal->version = _rtl92fe_read_chip_version(hw);
	if (get_rf_type(rtlphy) == RF_1T1R) {
		rtlpriv->dm.rfpath_rxenable[0] = true;
	} else {
		rtlpriv->dm.rfpath_rxenable[0] = true;
		rtlpriv->dm.rfpath_rxenable[1] = true;
	}
	rtl_dbg(rtlpriv, COMP_INIT, DBG_LOUD, "VersionID = 0x%4x\n",
		rtlhal->version);
	tmp_u1b = rtl_read_byte(rtlpriv, REG_9346CR);
	if (tmp_u1b & BIT(4)) {
		rtl_dbg(rtlpriv, COMP_INIT, DBG_DMESG, "Boot from EEPROM\n");
		rtlefuse->epromtype = EEPROM_93C46;
	} else {
		rtl_dbg(rtlpriv, COMP_INIT, DBG_DMESG, "Boot from EFUSE\n");
		rtlefuse->epromtype = EEPROM_BOOT_EFUSE;
	}
	if (tmp_u1b & BIT(5)) {
		rtl_dbg(rtlpriv, COMP_INIT, DBG_LOUD, "Autoload OK\n");
		rtlefuse->autoload_failflag = false;
		_rtl92fe_read_adapter_info(hw);
	} else {
		pr_err("Autoload ERR!!\n");
		rtlefuse->autoload_failflag = true;
	}

	/* Blank efuse (no autoload, or bad signature so ... -- dev/MEASURED-hw.c.md sec 30. */
	if (rtlefuse->autoload_failflag) {
		const struct rtl92fe_board_cal *cal =
			_rtl92fe_board_cal_for_this_board();

		if (cal) {
			_rtl92fe_apply_board_cal(hw, cal);
		} else {
			/* ★ LOUD, and it leaves autoload_failflag SET so the core
			 * takes its own random-MAC / generic-power path. An
			 * uncalibrated radio that says so is recoverable; a radio
			 * silently wearing another board's identity is not. */
			pr_warn("rtl8192fe: blank efuse and NO factory cal declared for this board -- the radio stays UNCALIBRATED (generic tx-power, untrimmed crystal, core-assigned MAC). Read HW_WLAN0_* from this unit's own flash MIB and add an entry keyed on its DT root compatible; applying another board's cal would give two units one MAC.\n");
		}
	}

	_rtl92fe_hal_customized_behavior(hw);

	rtlphy->rfpath_rx_enable[0] = true;
	if (rtlphy->rf_type == RF_2T2R)
		rtlphy->rfpath_rx_enable[1] = true;
}

static u8 _rtl92fe_mrate_idx_to_arfr_id(struct ieee80211_hw *hw, u8 rate_index)
{
	u8 ret = 0;

	switch (rate_index) {
	case RATR_INX_WIRELESS_NGB:
		ret = 0;
		break;
	case RATR_INX_WIRELESS_N:
	case RATR_INX_WIRELESS_NG:
		ret = 4;
		break;
	case RATR_INX_WIRELESS_NB:
		ret = 2;
		break;
	case RATR_INX_WIRELESS_GB:
		ret = 6;
		break;
	case RATR_INX_WIRELESS_G:
		ret = 7;
		break;
	case RATR_INX_WIRELESS_B:
		ret = 8;
		break;
	default:
		ret = 0;
		break;
	}
	return ret;
}

static void rtl92fe_update_hal_rate_mask(struct ieee80211_hw *hw,
					 struct ieee80211_sta *sta,
					 u8 rssi_level, bool update_bw)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	struct rtl_phy *rtlphy = &rtlpriv->phy;
	struct rtl_mac *mac = rtl_mac(rtl_priv(hw));
	struct rtl_sta_info *sta_entry = NULL;
	u32 ratr_bitmap;
	u8 ratr_index;
	u8 curtxbw_40mhz = (sta->deflink.ht_cap.cap &
			    IEEE80211_HT_CAP_SUP_WIDTH_20_40) ? 1 : 0;
	u8 b_curshortgi_40mhz = (sta->deflink.ht_cap.cap &
				 IEEE80211_HT_CAP_SGI_40) ? 1 : 0;
	u8 b_curshortgi_20mhz = (sta->deflink.ht_cap.cap &
				 IEEE80211_HT_CAP_SGI_20) ? 1 : 0;
	enum wireless_mode wirelessmode = 0;
	bool b_shortgi = false;
	u8 rate_mask[7] = {0};
	u8 macid = 0;

	sta_entry = (struct rtl_sta_info *)sta->drv_priv;
	wirelessmode = sta_entry->wireless_mode;
	if (mac->opmode == NL80211_IFTYPE_STATION ||
	    mac->opmode == NL80211_IFTYPE_MESH_POINT)
		curtxbw_40mhz = mac->bw_40;
	else if (mac->opmode == NL80211_IFTYPE_AP ||
		 mac->opmode == NL80211_IFTYPE_ADHOC) {
		macid = sta->aid + 1;
		/* This board's 8192FR has a single usable TX chain, so ...
		 * dev/MEASURED-hw.c.md sec 31. */
		sta->deflink.ht_cap.mcs.rx_mask[1] = 0;
	}

	ratr_bitmap = sta->deflink.supp_rates[0];
	if (mac->opmode == NL80211_IFTYPE_ADHOC)
		ratr_bitmap = 0xfff;

	ratr_bitmap |= (sta->deflink.ht_cap.mcs.rx_mask[1] << 20 |
			sta->deflink.ht_cap.mcs.rx_mask[0] << 12);

	switch (wirelessmode) {
	case WIRELESS_MODE_B:
		ratr_index = RATR_INX_WIRELESS_B;
		if (ratr_bitmap & 0x0000000c)
			ratr_bitmap &= 0x0000000d;
		else
			ratr_bitmap &= 0x0000000f;
		break;
	case WIRELESS_MODE_G:
		ratr_index = RATR_INX_WIRELESS_GB;

		if (rssi_level == 1)
			ratr_bitmap &= 0x00000f00;
		else if (rssi_level == 2)
			ratr_bitmap &= 0x00000ff0;
		else
			ratr_bitmap &= 0x00000ff5;
		break;
	case WIRELESS_MODE_N_24G:
		if (curtxbw_40mhz)
			ratr_index = RATR_INX_WIRELESS_NGB;
		else
			ratr_index = RATR_INX_WIRELESS_NB;

		if (rtlphy->rf_type == RF_1T1R) {
			if (curtxbw_40mhz) {
				if (rssi_level == 1)
					ratr_bitmap &= 0x000f0000;
				else if (rssi_level == 2)
					ratr_bitmap &= 0x000ff000;
				else
					ratr_bitmap &= 0x000ff015;
			} else {
				if (rssi_level == 1)
					ratr_bitmap &= 0x000f0000;
				else if (rssi_level == 2)
					ratr_bitmap &= 0x000ff000;
				else
					ratr_bitmap &= 0x000ff005;
			}
		} else {
			if (curtxbw_40mhz) {
				if (rssi_level == 1)
					ratr_bitmap &= 0x0f8f0000;
				else if (rssi_level == 2)
					ratr_bitmap &= 0x0ffff000;
				else
					ratr_bitmap &= 0x0ffff015;
			} else {
				if (rssi_level == 1)
					ratr_bitmap &= 0x0f8f0000;
				else if (rssi_level == 2)
					ratr_bitmap &= 0x0ffff000;
				else
					ratr_bitmap &= 0x0ffff005;
			}
		}

		if ((curtxbw_40mhz && b_curshortgi_40mhz) ||
		    (!curtxbw_40mhz && b_curshortgi_20mhz)) {
			if (macid == 0)
				b_shortgi = true;
			else if (macid == 1)
				b_shortgi = false;
		}
		break;
	default:
		ratr_index = RATR_INX_WIRELESS_NGB;

		if (rtlphy->rf_type == RF_1T1R)
			ratr_bitmap &= 0x000ff0ff;
		else
			ratr_bitmap &= 0x0f8ff0ff;
		break;
	}
	ratr_index = _rtl92fe_mrate_idx_to_arfr_id(hw, ratr_index);
	sta_entry->ratr_index = ratr_index;

	rtl_dbg(rtlpriv, COMP_RATR, DBG_DMESG,
		"ratr_bitmap :%x\n", ratr_bitmap);
	*(u32 *)&rate_mask = (ratr_bitmap & 0x0fffffff) |
				       (ratr_index << 28);
	rate_mask[0] = macid;
	rate_mask[1] = ratr_index | (b_shortgi ? 0x80 : 0x00);
	rate_mask[2] = curtxbw_40mhz | ((!update_bw) << 3);
	rate_mask[3] = (u8)(ratr_bitmap & 0x000000ff);
	rate_mask[4] = (u8)((ratr_bitmap & 0x0000ff00) >> 8);
	rate_mask[5] = (u8)((ratr_bitmap & 0x00ff0000) >> 16);
	rate_mask[6] = (u8)((ratr_bitmap & 0xff000000) >> 24);
	rtl_dbg(rtlpriv, COMP_RATR, DBG_DMESG,
		"Rate_index:%x, ratr_val:%x, %x:%x:%x:%x:%x:%x:%x\n",
		ratr_index, ratr_bitmap, rate_mask[0], rate_mask[1],
		rate_mask[2], rate_mask[3], rate_mask[4],
		rate_mask[5], rate_mask[6]);
	/* AP/ADHOC peer: the FW rate/security context for this macid ...
	 * dev/MEASURED-hw.c.md sec 32. */
	if (macid)		/* != 0 => an AP/ADHOC peer, not the STA-self (macid 0) */
		rtl92fe_set_fw_media_status_rpt_cmd(hw, RT_MEDIA_CONNECT, macid);

	rtl92fe_fill_h2c_cmd(hw, H2C_92F_RA_MASK, 7, rate_mask);
	_rtl92fe_set_bcn_ctrl_reg(hw, BIT(3), 0);
}

void rtl92fe_update_hal_rate_tbl(struct ieee80211_hw *hw,
				 struct ieee80211_sta *sta, u8 rssi_level,
				 bool update_bw)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);

	if (rtlpriv->dm.useramask)
		rtl92fe_update_hal_rate_mask(hw, sta, rssi_level, update_bw);
}

void rtl92fe_update_channel_access_setting(struct ieee80211_hw *hw)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	struct rtl_mac *mac = rtl_mac(rtl_priv(hw));
	u16 sifs_timer;

	rtlpriv->cfg->ops->set_hw_reg(hw, HW_VAR_SLOT_TIME,
				      (u8 *)&mac->slot_time);
	if (!mac->ht_enable)
		sifs_timer = 0x0a0a;
	else
		sifs_timer = 0x0e0e;
	rtlpriv->cfg->ops->set_hw_reg(hw, HW_VAR_SIFS, (u8 *)&sifs_timer);
}

bool rtl92fe_gpio_radio_on_off_checking(struct ieee80211_hw *hw, u8 *valid)
{
	/* The RTL8192F has no hardware RF-kill GPIO wired on this ...
	 * dev/MEASURED-hw.c.md sec 37. */
	*valid = 1;
	return true;
}

void rtl92fe_set_key(struct ieee80211_hw *hw, u32 key_index,
		     u8 *p_macaddr, bool is_group, u8 enc_algo,
		     bool is_wepkey, bool clear_all)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	struct rtl_mac *mac = rtl_mac(rtl_priv(hw));
	struct rtl_efuse *rtlefuse = rtl_efuse(rtl_priv(hw));
	u8 *macaddr = p_macaddr;
	u32 entry_id = 0;
	bool is_pairwise = false;

	static u8 cam_const_addr[4][6] = {
		{0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
		{0x00, 0x00, 0x00, 0x00, 0x00, 0x01},
		{0x00, 0x00, 0x00, 0x00, 0x00, 0x02},
		{0x00, 0x00, 0x00, 0x00, 0x00, 0x03}
	};
	static u8 cam_const_broad[] = {
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff
	};

	if (clear_all) {
		u8 idx = 0;
		u8 cam_offset = 0;
		u8 clear_number = 5;

		rtl_dbg(rtlpriv, COMP_SEC, DBG_DMESG, "clear_all\n");

		for (idx = 0; idx < clear_number; idx++) {
			rtl_cam_mark_invalid(hw, cam_offset + idx);
			rtl_cam_empty_entry(hw, cam_offset + idx);

			if (idx < 5) {
				memset(rtlpriv->sec.key_buf[idx], 0,
				       MAX_KEY_LEN);
				rtlpriv->sec.key_len[idx] = 0;
			}
		}

	} else {
		switch (enc_algo) {
		case WEP40_ENCRYPTION:
			enc_algo = CAM_WEP40;
			break;
		case WEP104_ENCRYPTION:
			enc_algo = CAM_WEP104;
			break;
		case TKIP_ENCRYPTION:
			enc_algo = CAM_TKIP;
			break;
		case AESCCMP_ENCRYPTION:
			enc_algo = CAM_AES;
			break;
		default:
			rtl_dbg(rtlpriv, COMP_ERR, DBG_DMESG,
				"switch case %#x not processed\n", enc_algo);
			enc_algo = CAM_TKIP;
			break;
		}

		if (is_wepkey || rtlpriv->sec.use_defaultkey) {
			macaddr = cam_const_addr[key_index];
			entry_id = key_index;
		} else {
			if (is_group) {
				macaddr = cam_const_broad;
				entry_id = key_index;
			} else {
				if (mac->opmode == NL80211_IFTYPE_AP ||
				    mac->opmode == NL80211_IFTYPE_MESH_POINT) {
					entry_id = rtl_cam_get_free_entry(hw,
								     p_macaddr);
					if (entry_id >= TOTAL_CAM_ENTRY) {
						pr_err("Can not find free hw security cam entry\n");
						return;
					}
				} else {
					entry_id = CAM_PAIRWISE_KEY_POSITION;
				}

				key_index = PAIRWISE_KEYIDX;
				is_pairwise = true;
			}
		}

		if (rtlpriv->sec.key_len[key_index] == 0) {
			rtl_dbg(rtlpriv, COMP_SEC, DBG_DMESG,
				"delete one entry, entry_id is %d\n",
				entry_id);
			if (mac->opmode == NL80211_IFTYPE_AP ||
			    mac->opmode == NL80211_IFTYPE_MESH_POINT)
				rtl_cam_del_entry(hw, p_macaddr);
			rtl_cam_delete_one_entry(hw, p_macaddr, entry_id);
		} else {
			rtl_dbg(rtlpriv, COMP_SEC, DBG_DMESG,
				"add one entry\n");
			/* KEY spy: prove the PTK/GTK actually reach the HW CAM. A ...
			 * dev/MEASURED-hw.c.md sec 38. */
			pr_info("92f-spy KEY add entry=%u kidx=%u %s %pM alg=%u\n",
				entry_id, key_index,
				is_pairwise ? "PAIRWISE" : (is_group ? "GROUP" : "def"),
				macaddr, enc_algo);
			if (is_pairwise) {
				rtl_dbg(rtlpriv, COMP_SEC, DBG_DMESG,
					"set Pairwise key\n");

				rtl_cam_add_one_entry(hw, macaddr, key_index,
					       entry_id, enc_algo,
					       CAM_CONFIG_NO_USEDK,
					       rtlpriv->sec.key_buf[key_index]);
			} else {
				rtl_dbg(rtlpriv, COMP_SEC, DBG_DMESG,
					"set group key\n");

				if (mac->opmode == NL80211_IFTYPE_ADHOC) {
					rtl_cam_add_one_entry(hw,
						rtlefuse->dev_addr,
						PAIRWISE_KEYIDX,
						CAM_PAIRWISE_KEY_POSITION,
						enc_algo, CAM_CONFIG_NO_USEDK,
						rtlpriv->sec.key_buf[entry_id]);
				}

				rtl_cam_add_one_entry(hw, macaddr, key_index,
						entry_id, enc_algo,
						CAM_CONFIG_NO_USEDK,
						rtlpriv->sec.key_buf[entry_id]);
			}
		}
	}
}

void rtl92fe_read_bt_coexist_info_from_hwpg(struct ieee80211_hw *hw,
					    bool auto_load_fail, u8 *hwinfo)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	u8 value;

	if (!auto_load_fail) {
		value = hwinfo[EEPROM_RFE_OPTION_92F];
		if (((value & 0xe0) >> 5) == 0x1)
			rtlpriv->btcoexist.btc_info.btcoexist = 1;
		else
			rtlpriv->btcoexist.btc_info.btcoexist = 0;

		rtlpriv->btcoexist.btc_info.bt_type = BT_RTL8192E;
		rtlpriv->btcoexist.btc_info.ant_num = ANT_X2;
	} else {
		rtlpriv->btcoexist.btc_info.btcoexist = 1;
		rtlpriv->btcoexist.btc_info.bt_type = BT_RTL8192E;
		rtlpriv->btcoexist.btc_info.ant_num = ANT_X1;
	}
}

void rtl92fe_bt_reg_init(struct ieee80211_hw *hw)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);

	/* 0:Low, 1:High, 2:From Efuse. */
	rtlpriv->btcoexist.reg_bt_iso = 2;
	/* 0:Idle, 1:None-SCO, 2:SCO, 3:From Counter. */
	rtlpriv->btcoexist.reg_bt_sco = 3;
	/* 0:Disable BT control A-MPDU, 1:Enable BT control A-MPDU. */
	rtlpriv->btcoexist.reg_bt_sco = 0;
}

void rtl92fe_bt_hw_init(struct ieee80211_hw *hw)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);

	if (rtlpriv->cfg->ops->get_btc_status())
		rtlpriv->btcoexist.btc_ops->btc_init_hw_config(rtlpriv);
}

void rtl92fe_suspend(struct ieee80211_hw *hw)
{
}

void rtl92fe_resume(struct ieee80211_hw *hw)
{
}

/* Turn on AAP (RCR:bit 0) for promiscuous mode. */
void rtl92fe_allow_all_destaddr(struct ieee80211_hw *hw,
				bool allow_all_da, bool write_into_reg)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	struct rtl_pci *rtlpci = rtl_pcidev(rtl_pcipriv(hw));

	if (allow_all_da)	/* Set BIT0 */
		rtlpci->receive_config |= RCR_AAP;
	else			/* Clear BIT0 */
		rtlpci->receive_config &= ~RCR_AAP;

	if (write_into_reg)
		rtl_write_dword(rtlpriv, REG_RCR, rtlpci->receive_config);

	rtl_dbg(rtlpriv, COMP_TURBO | COMP_INIT, DBG_LOUD,
		"receive_config=0x%08X, write_into_reg=%d\n",
		rtlpci->receive_config, write_into_reg);
}
