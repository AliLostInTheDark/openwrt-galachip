// SPDX-License-Identifier: GPL-2.0
/* Clean-room mac80211 PCIe driver for the Realtek RTL8192F.
 *
 * ★★ WHY THIS DRIVER EXISTS AT ALL, MEASURED 2026-09-17 AND NOT ASSUMED. The
 * standing rule is not to port what a newer mainline kernel already carries, so
 * the question was asked of mainline directly rather than answered from memory:
 *   - rtlwifi has NO rtl8192fe. Its Makefile on torvalds/linux master (>= 7.2,
 *     Sept 2026) builds 8192ce, 8192cu, 8192se, 8192de, 8192du, 8723ae, 8723be,
 *     8188ee, 8821ae and 8192ee -- and no `fe`. The 6.18.31 tree we build
 *     against agrees.
 *   - rtw88 does not claim the die either (8822b/c, 8723x/8703b/8723d, 8821c,
 *     88xxa/8821a/8812a, 8814a), and nor does rtw89.
 *   - The ONLY mainline code for this silicon is rtl8xxxu's 8192f.c, and it is
 *     USB (rtl8192fu). Our part is PCIe, so it cannot serve us.
 * => there is nothing upstream to switch to, and carrying this driver is
 * justified rather than habitual. It is also ~24% of the whole port by line
 * count (`code_share.py`), so the justification is worth writing down where the
 * next person asks the question.
 *
 * ⚠ AND THE CONSEQUENCE FOR BUGS FOUND HERE: they are OURS. When the 40 MHz MAC
 * data sub-channel and RX DFIR words were found unwritten (an AP up at 40 MHz
 * delivering no HT40 data frame), the fix could not come from a mainline
 * backport -- only the REGISTER FACTS could, from rtl8xxxu's USB driver for the
 * same die, which programs all three correctly and still does on master.
 */

#include "../wifi.h"
#include "../core.h"
#include "../pci.h"
#include "reg.h"
#include "def.h"
#include "phy.h"
#include "dm.h"
#include "hw.h"
#include "fw.h"
#include "trx.h"
#include "led.h"
#include "table.h"

#include "../btcoexist/rtl_btc.h"

#include <linux/vmalloc.h>
#include <linux/module.h>

static void rtl92fe_init_aspm_vars(struct ieee80211_hw *hw)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	struct rtl_pci *rtlpci = rtl_pcidev(rtl_pcipriv(hw));

	/* ASPM PS mode. 0 - Disable ASPM, 1 - Enable ASPM without ...
	 * dev/MEASURED-sw.c.md sec 1. */
	rtlpci->const_pci_aspm = 3;

	/* Setting for PCI-E device */
	rtlpci->const_devicepci_aspm_setting = 0x03;

	/* Setting for PCI-E bridge */
	rtlpci->const_hostpci_aspm_setting = 0x02;

	/* In Hw/Sw Radio Off situation. 0 - Default, 1 - From ASPM ...
	 * dev/MEASURED-sw.c.md sec 2. */
	rtlpci->const_hwsw_rfoff_d3 = 0;

	/* This setting works for those device with backdoor ASPM ...
	 * dev/MEASURED-sw.c.md sec 3. */
	rtlpci->const_support_pciaspm = rtlpriv->cfg->mod_params->aspm_support;
}

static int rtl92fe_init_sw_vars(struct ieee80211_hw *hw)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	struct rtl_pci *rtlpci = rtl_pcidev(rtl_pcipriv(hw));
	int err = 0;
	char *fw_name;

	rtl92fe_bt_reg_init(hw);
	rtlpci->msi_support = rtlpriv->cfg->mod_params->msi_support;
	rtlpriv->btcoexist.btc_ops = rtl_btc_get_ops_pointer();

	/* RTL8192F uses the packet-buffer-descriptor ("new") TRX ring ...
	 * dev/MEASURED-sw.c.md sec 4. */
	rtlpriv->use_new_trx_flow = true;

	rtlpriv->dm.dm_initialgain_enable = true;
	rtlpriv->dm.dm_flag = 0;
	rtlpriv->dm.disable_framebursting = false;
	rtlpci->transmit_config = CFENDFORM | BIT(15);

	/* The RTL8192F is a 2.4 GHz-only 802.11n part. */
	rtlpriv->rtlhal.current_bandtype = BAND_ON_2_4G;
	rtlpriv->rtlhal.bandset = BAND_ON_2_4G;
	rtlpriv->rtlhal.macphymode = SINGLEMAC_SINGLEPHY;

	rtlpci->receive_config = (RCR_APPFCS			|
				  RCR_APP_MIC			|
				  RCR_APP_ICV			|
				  RCR_APP_PHYST_RXFF		|
				  RCR_HTC_LOC_CTRL		|
				  RCR_AMF			|
				  RCR_ACF			|
				  RCR_ACRC32			|
				  RCR_AB			|
				  RCR_AM			|
				  RCR_APM			|
				  0);

	rtlpci->irq_mask[0] = (u32)(IMR_PSTIMEOUT		|
				    IMR_C2HCMD			|
				    IMR_HIGHDOK			|
				    IMR_MGNTDOK			|
				    IMR_BKDOK			|
				    IMR_BEDOK			|
				    IMR_VIDOK			|
				    IMR_VODOK			|
				    IMR_RDU			|
				    IMR_ROK			|
				    0);
	rtlpci->irq_mask[1] = (u32)(IMR_RXFOVW | IMR_TXERR);

	/* for LPS & IPS */
	rtlpriv->psc.inactiveps = rtlpriv->cfg->mod_params->inactiveps;
	rtlpriv->psc.swctrl_lps = rtlpriv->cfg->mod_params->swctrl_lps;
	rtlpriv->psc.fwctrl_lps = rtlpriv->cfg->mod_params->fwctrl_lps;
	rtlpci->msi_support = rtlpriv->cfg->mod_params->msi_support;
	if (rtlpriv->cfg->mod_params->disable_watchdog)
		pr_info("watchdog disabled\n");
	rtlpriv->psc.reg_fwctrl_lps = 3;
	rtlpriv->psc.reg_max_lps_awakeintvl = 5;
	/* for ASPM, you can close aspm through
	 * set const_support_pciaspm = 0
	 */
	rtl92fe_init_aspm_vars(hw);

	if (rtlpriv->psc.reg_fwctrl_lps == 1)
		rtlpriv->psc.fwctrl_psmode = FW_PS_MIN_MODE;
	else if (rtlpriv->psc.reg_fwctrl_lps == 2)
		rtlpriv->psc.fwctrl_psmode = FW_PS_MAX_MODE;
	else if (rtlpriv->psc.reg_fwctrl_lps == 3)
		rtlpriv->psc.fwctrl_psmode = FW_PS_DTIM_MODE;

	/* for early mode */
	rtlpriv->rtlhal.earlymode_enable = false;

	/* low power */
	rtlpriv->psc.low_power_enable = false;

	/* for firmware buf */
	rtlpriv->rtlhal.pfirmware = vzalloc(0x8000);
	if (!rtlpriv->rtlhal.pfirmware) {
		pr_err("Can't alloc buffer for fw\n");
		return 1;
	}

	/* request fw */
	fw_name = "rtlwifi/rtl8192fefw.bin";

	rtlpriv->max_fw_size = 0x8000;
	pr_info("Using firmware %s\n", fw_name);
	err = request_firmware_nowait(THIS_MODULE, 1, fw_name,
				      rtlpriv->io.dev, GFP_KERNEL, hw,
				      rtl_fw_cb);
	if (err) {
		pr_err("Failed to request firmware!\n");
		vfree(rtlpriv->rtlhal.pfirmware);
		rtlpriv->rtlhal.pfirmware = NULL;
		return 1;
	}

	rtl_lamp_register(hw, "rtl8192fe", rtl92fe_lamp_set);
	return 0;
}

static void rtl92fe_deinit_sw_vars(struct ieee80211_hw *hw)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);

	rtl_lamp_unregister(hw);

	if (rtlpriv->rtlhal.pfirmware) {
		vfree(rtlpriv->rtlhal.pfirmware);
		rtlpriv->rtlhal.pfirmware = NULL;
	}
}

/* get bt coexist status */
static bool rtl92fe_get_btc_status(void)
{
	return true;
}

static const struct rtl_hal_ops rtl8192fe_hal_ops = {
	.init_sw_vars = rtl92fe_init_sw_vars,
	.deinit_sw_vars = rtl92fe_deinit_sw_vars,
	.read_eeprom_info = rtl92fe_read_eeprom_info,
	.interrupt_recognized = rtl92fe_interrupt_recognized,
	.hw_init = rtl92fe_hw_init,
	.hw_disable = rtl92fe_card_disable,
	.hw_suspend = rtl92fe_suspend,
	.hw_resume = rtl92fe_resume,
	.enable_interrupt = rtl92fe_enable_interrupt,
	.disable_interrupt = rtl92fe_disable_interrupt,
	.set_network_type = rtl92fe_set_network_type,
	.set_chk_bssid = rtl92fe_set_check_bssid,
	.set_qos = rtl92fe_set_qos,
	.set_bcn_reg = rtl92fe_set_beacon_related_registers,
	.set_bcn_intv = rtl92fe_set_beacon_interval,
	.update_interrupt_mask = rtl92fe_update_interrupt_mask,
	.get_hw_reg = rtl92fe_get_hw_reg,
	.set_hw_reg = rtl92fe_set_hw_reg,
	.update_rate_tbl = rtl92fe_update_hal_rate_tbl,
	.rx_desc_buff_remained_cnt = rtl92fe_rx_desc_buff_remained_cnt,
	.rx_check_dma_ok = rtl92fe_rx_check_dma_ok,
	.fill_tx_desc = rtl92fe_tx_fill_desc,
	.fill_tx_cmddesc = rtl92fe_tx_fill_cmddesc,
	.query_rx_desc = rtl92fe_rx_query_desc,
	.set_channel_access = rtl92fe_update_channel_access_setting,
	.radio_onoff_checking = rtl92fe_gpio_radio_on_off_checking,
	.set_bw_mode = rtl92fe_phy_set_bw_mode,
	.switch_channel = rtl92fe_phy_sw_chnl,
	.dm_watchdog = rtl92fe_dm_watchdog,
	.scan_operation_backup = rtl92fe_phy_scan_operation_backup,
	.set_rf_power_state = rtl92fe_phy_set_rf_power_state,
	.led_control = rtl92fe_led_control,
	.set_desc = rtl92fe_set_desc,
	.get_desc = rtl92fe_get_desc,
	.is_tx_desc_closed = rtl92fe_is_tx_desc_closed,
	.get_available_desc = rtl92fe_get_available_desc,
	.tx_polling = rtl92fe_tx_polling,
	.enable_hw_sec = rtl92fe_enable_hw_security_config,
	.set_key = rtl92fe_set_key,
	.get_bbreg = rtl92fe_phy_query_bb_reg,
	.set_bbreg = rtl92fe_phy_set_bb_reg,
	.get_rfreg = rtl92fe_phy_query_rf_reg,
	.set_rfreg = rtl92fe_phy_set_rf_reg,
	.fill_h2c_cmd = rtl92fe_fill_h2c_cmd,
	.get_btc_status = rtl92fe_get_btc_status,
	.c2h_ra_report_handler = rtl92fe_c2h_ra_report_handler,
};

static struct rtl_mod_params rtl92fe_mod_params = {
	/* HW CCMP: the 8192F HW TX-encrypt is fixed by the cam.c ...
	 * dev/MEASURED-sw.c.md sec 6. */
	.sw_crypto = false,
	/* This is an AP product: firmware/inactive power-save must be ...
	 * dev/MEASURED-sw.c.md sec 5. */
	.inactiveps = false,
	.swctrl_lps = false,
	.fwctrl_lps = false,
	.msi_support = true,
	.dma64 = false,
	.aspm_support = 0,
	.debug_level = 0,
	.debug_mask = 0,
};

static const struct rtl_hal_cfg rtl8192fe_hal_cfg = {
	.bar_id = 2,
	.write_readback = true,
	.name = "rtl8192fe_pci",
	.ops = &rtl8192fe_hal_ops,
	.mod_params = &rtl92fe_mod_params,

	.maps[SYS_ISO_CTRL] = REG_SYS_ISO_CTRL,
	.maps[SYS_FUNC_EN] = REG_SYS_FUNC_EN,
	.maps[SYS_CLK] = REG_SYS_CLKR,
	.maps[MAC_RCR_AM] = AM,
	.maps[MAC_RCR_AB] = AB,
	.maps[MAC_RCR_ACRC32] = ACRC32,
	.maps[MAC_RCR_ACF] = ACF,
	.maps[MAC_RCR_AAP] = AAP,
	.maps[MAC_HIMR] = REG_HIMR,
	.maps[MAC_HIMRE] = REG_HIMRE,

	.maps[EFUSE_ACCESS] = REG_EFUSE_ACCESS,

	.maps[EFUSE_TEST] = REG_EFUSE_TEST,
	.maps[EFUSE_CTRL] = REG_EFUSE_CTRL,
	.maps[EFUSE_CLK] = 0,
	.maps[EFUSE_CLK_CTRL] = REG_EFUSE_CTRL,
	.maps[EFUSE_PWC_EV12V] = PWC_EV12V,
	.maps[EFUSE_FEN_ELDR] = FEN_ELDR,
	.maps[EFUSE_LOADER_CLK_EN] = LOADER_CLK_EN,
	.maps[EFUSE_ANA8M] = ANA8M,
	.maps[EFUSE_HWSET_MAX_SIZE] = HWSET_MAX_SIZE,
	.maps[EFUSE_MAX_SECTION_MAP] = EFUSE_MAX_SECTION,
	.maps[EFUSE_REAL_CONTENT_SIZE] = EFUSE_REAL_CONTENT_LEN,
	.maps[EFUSE_OOB_PROTECT_BYTES_LEN] = EFUSE_OOB_PROTECT_BYTES,

	.maps[RWCAM] = REG_CAMCMD,
	.maps[WCAMI] = REG_CAMWRITE,
	.maps[RCAMO] = REG_CAMREAD,
	.maps[CAMDBG] = REG_CAMDBG,
	.maps[SECR] = REG_SECCFG,
	.maps[SEC_CAM_NONE] = CAM_NONE,
	.maps[SEC_CAM_WEP40] = CAM_WEP40,
	.maps[SEC_CAM_TKIP] = CAM_TKIP,
	.maps[SEC_CAM_AES] = CAM_AES,
	.maps[SEC_CAM_WEP104] = CAM_WEP104,

	.maps[RTL_IMR_BCNDMAINT6] = IMR_BCNDMAINT6,
	.maps[RTL_IMR_BCNDMAINT5] = IMR_BCNDMAINT5,
	.maps[RTL_IMR_BCNDMAINT4] = IMR_BCNDMAINT4,
	.maps[RTL_IMR_BCNDMAINT3] = IMR_BCNDMAINT3,
	.maps[RTL_IMR_BCNDMAINT2] = IMR_BCNDMAINT2,
	.maps[RTL_IMR_BCNDMAINT1] = IMR_BCNDMAINT1,
	.maps[RTL_IMR_BCNDOK7] = IMR_BCNDOK7,
	.maps[RTL_IMR_BCNDOK6] = IMR_BCNDOK6,
	.maps[RTL_IMR_BCNDOK5] = IMR_BCNDOK5,
	.maps[RTL_IMR_BCNDOK4] = IMR_BCNDOK4,
	.maps[RTL_IMR_BCNDOK3] = IMR_BCNDOK3,
	.maps[RTL_IMR_BCNDOK2] = IMR_BCNDOK2,
	.maps[RTL_IMR_BCNDOK1] = IMR_BCNDOK1,

	.maps[RTL_IMR_TXFOVW] = IMR_TXFOVW,
	.maps[RTL_IMR_PSTIMEOUT] = IMR_PSTIMEOUT,
	.maps[RTL_IMR_BCNINT] = IMR_BCNDMAINT0,
	.maps[RTL_IMR_RXFOVW] = IMR_RXFOVW,
	.maps[RTL_IMR_RDU] = IMR_RDU,
	.maps[RTL_IMR_ATIMEND] = IMR_ATIMEND,
	.maps[RTL_IMR_BDOK] = IMR_BCNDOK0,
	.maps[RTL_IMR_MGNTDOK] = IMR_MGNTDOK,
	.maps[RTL_IMR_TBDER] = IMR_TBDER,
	.maps[RTL_IMR_HIGHDOK] = IMR_HIGHDOK,
	.maps[RTL_IMR_TBDOK] = IMR_TBDOK,
	.maps[RTL_IMR_BKDOK] = IMR_BKDOK,
	.maps[RTL_IMR_BEDOK] = IMR_BEDOK,
	.maps[RTL_IMR_VIDOK] = IMR_VIDOK,
	.maps[RTL_IMR_VODOK] = IMR_VODOK,
	.maps[RTL_IMR_ROK] = IMR_ROK,
	.maps[RTL_IBSS_INT_MASKS] = (IMR_BCNDMAINT0 | IMR_TBDOK | IMR_TBDER),

	.maps[RTL_RC_CCK_RATE1M] = DESC_RATE1M,
	.maps[RTL_RC_CCK_RATE2M] = DESC_RATE2M,
	.maps[RTL_RC_CCK_RATE5_5M] = DESC_RATE5_5M,
	.maps[RTL_RC_CCK_RATE11M] = DESC_RATE11M,
	.maps[RTL_RC_OFDM_RATE6M] = DESC_RATE6M,
	.maps[RTL_RC_OFDM_RATE9M] = DESC_RATE9M,
	.maps[RTL_RC_OFDM_RATE12M] = DESC_RATE12M,
	.maps[RTL_RC_OFDM_RATE18M] = DESC_RATE18M,
	.maps[RTL_RC_OFDM_RATE24M] = DESC_RATE24M,
	.maps[RTL_RC_OFDM_RATE36M] = DESC_RATE36M,
	.maps[RTL_RC_OFDM_RATE48M] = DESC_RATE48M,
	.maps[RTL_RC_OFDM_RATE54M] = DESC_RATE54M,

	.maps[RTL_RC_HT_RATEMCS7] = DESC_RATEMCS7,
	.maps[RTL_RC_HT_RATEMCS15] = DESC_RATEMCS15,
};

static const struct pci_device_id rtl92fe_pci_ids[] = {
	{RTL_PCI_DEVICE(PCI_VENDOR_ID_REALTEK, 0x818c, rtl8192fe_hal_cfg)},
	{},
};

MODULE_DEVICE_TABLE(pci, rtl92fe_pci_ids);

MODULE_AUTHOR("Confiared <brule.herman@confiared.com>");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Realtek 8192FE 802.11n PCI wireless");
MODULE_FIRMWARE("rtlwifi/rtl8192fefw.bin");

module_param_named(swenc, rtl92fe_mod_params.sw_crypto, bool, 0444);
module_param_named(debug_level, rtl92fe_mod_params.debug_level, int, 0644);
module_param_named(debug_mask, rtl92fe_mod_params.debug_mask, ullong, 0644);
module_param_named(ips, rtl92fe_mod_params.inactiveps, bool, 0444);
module_param_named(swlps, rtl92fe_mod_params.swctrl_lps, bool, 0444);
module_param_named(fwlps, rtl92fe_mod_params.fwctrl_lps, bool, 0444);
module_param_named(msi, rtl92fe_mod_params.msi_support, bool, 0444);
module_param_named(dma64, rtl92fe_mod_params.dma64, bool, 0444);
module_param_named(aspm, rtl92fe_mod_params.aspm_support, int, 0444);
module_param_named(disable_watchdog, rtl92fe_mod_params.disable_watchdog,
		   bool, 0444);
MODULE_PARM_DESC(swenc, "Set to 1 for software crypto (default 0)\n");
MODULE_PARM_DESC(ips, "Set to 0 to not use link power save (default 1)\n");
MODULE_PARM_DESC(swlps, "Set to 1 to use SW control power save (default 0)\n");
MODULE_PARM_DESC(fwlps, "Set to 1 to use FW control power save (default 1)\n");
MODULE_PARM_DESC(msi, "Set to 1 to use MSI interrupts mode (default 1)\n");
MODULE_PARM_DESC(dma64, "Set to 1 to use DMA 64 (default 0)\n");
MODULE_PARM_DESC(aspm, "Set to 1 to enable ASPM (default 0: stability over energy)\n");
MODULE_PARM_DESC(debug_level, "Set debug level (0-5) (default 0)");
MODULE_PARM_DESC(debug_mask, "Set debug mask (default 0)");
MODULE_PARM_DESC(disable_watchdog, "Set to 1 to disable the watchdog (default 0)\n");

static SIMPLE_DEV_PM_OPS(rtlwifi_pm_ops, rtl_pci_suspend, rtl_pci_resume);

static struct pci_driver rtl92fe_driver = {
	.name = KBUILD_MODNAME,
	.id_table = rtl92fe_pci_ids,
	.probe = rtl_pci_probe,
	.remove = rtl_pci_disconnect,
	.driver.pm = &rtlwifi_pm_ops,
};

module_pci_driver(rtl92fe_driver);
