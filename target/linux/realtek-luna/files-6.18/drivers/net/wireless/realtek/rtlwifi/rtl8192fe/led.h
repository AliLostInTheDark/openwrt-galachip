/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright(c) 2026  Herman Brule */

#ifndef __RTL92FE_LED_H__
#define __RTL92FE_LED_H__

void rtl92fe_sw_led_on(struct ieee80211_hw *hw, enum rtl_led_pin pin);
void rtl92fe_sw_led_off(struct ieee80211_hw *hw, enum rtl_led_pin pin);
void rtl92fe_led_control(struct ieee80211_hw *hw, enum led_ctl_mode ledaction);
void rtl92fe_lamp_set(struct ieee80211_hw *hw, bool on);

#endif
