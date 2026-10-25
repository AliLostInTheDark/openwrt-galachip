/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright(c) 2026  Realtek RTL8192FE clean-room driver authors. */

#ifndef __RTL8192FE_RF_H__
#define __RTL8192FE_RF_H__

/* RF6052-style serial-bus TX power index is a 6-bit field (0..0x3F). */
#define RF6052_MAX_TX_PWR		0x3F

void rtl92fe_phy_rf6052_set_bandwidth(struct ieee80211_hw *hw,
					u8 bandwidth);
void rtl92fe_phy_rf6052_set_cck_txpower(struct ieee80211_hw *hw,
					  u8 *ppowerlevel);
void rtl92fe_phy_rf6052_set_ofdm_txpower(struct ieee80211_hw *hw,
					   u8 *ppowerlevel_ofdm,
					   u8 *ppowerlevel_bw20,
					   u8 *ppowerlevel_bw40,
					   u8 channel);
bool rtl92fe_phy_rf6052_config(struct ieee80211_hw *hw);

#endif
