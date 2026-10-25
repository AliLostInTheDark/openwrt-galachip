// SPDX-License-Identifier: GPL-2.0
/* Copyright(c) 2026  Herman Brule */

#include "../wifi.h"
#include "../pci.h"
#include "reg.h"
#include "led.h"

/* The RTL8192F drives its software LEDs through the LED ... -- dev/MEASURED-led.c.md sec 1. */
#define	LEDCFG0_LED0_CTL_MASK		0x00000007	/* bits [2:0]  */
#define	LEDCFG0_LED0_SW_VAL		BIT(3)
#define	LEDCFG0_LED0_IO_OUTPUT		BIT(7)		/* 0 = output  */
#define	LEDCFG0_LED1_CTL_MASK		0x00000700	/* bits [10:8] */
#define	LEDCFG0_LED1_SW_VAL		BIT(11)
#define	LEDCFG0_LED1_IO_OUTPUT		BIT(15)		/* 0 = output  */
#define	LEDCFG0_LED_GPIO_ENABLE		BIT(21)

void rtl92fe_sw_led_on(struct ieee80211_hw *hw, enum rtl_led_pin pin)
{
	u32 ledcfg;
	struct rtl_priv *rtlpriv = rtl_priv(hw);

	rtl_dbg(rtlpriv, COMP_LED, DBG_LOUD,
		"LedAddr:%X ledpin=%d\n", REG_LEDCFG0, pin);

	ledcfg = rtl_read_dword(rtlpriv, REG_LEDCFG0);

	switch (pin) {
	case LED_PIN_GPIO0:
		break;
	case LED_PIN_LED0:
		/* enable the LED pad, select output + software control,
		 * then drive the software value high.
		 */
		ledcfg |= LEDCFG0_LED_GPIO_ENABLE;
		ledcfg &= ~LEDCFG0_LED0_IO_OUTPUT;
		ledcfg &= ~LEDCFG0_LED0_CTL_MASK;
		ledcfg |= LEDCFG0_LED0_SW_VAL;
		rtl_write_dword(rtlpriv, REG_LEDCFG0, ledcfg);
		break;
	case LED_PIN_LED1:
		ledcfg |= LEDCFG0_LED_GPIO_ENABLE;
		ledcfg &= ~LEDCFG0_LED1_IO_OUTPUT;
		ledcfg &= ~LEDCFG0_LED1_CTL_MASK;
		ledcfg |= LEDCFG0_LED1_SW_VAL;
		rtl_write_dword(rtlpriv, REG_LEDCFG0, ledcfg);
		break;
	default:
		rtl_dbg(rtlpriv, COMP_ERR, DBG_LOUD,
			"switch case %#x not processed\n", pin);
		break;
	}
}

void rtl92fe_sw_led_off(struct ieee80211_hw *hw, enum rtl_led_pin pin)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	u32 ledcfg;

	rtl_dbg(rtlpriv, COMP_LED, DBG_LOUD,
		"LedAddr:%X ledpin=%d\n", REG_LEDCFG0, pin);

	ledcfg = rtl_read_dword(rtlpriv, REG_LEDCFG0);

	switch (pin) {
	case LED_PIN_GPIO0:
		break;
	case LED_PIN_LED0:
		/* keep the pad enabled in output + software control mode,
		 * just clear the software value to drive the LED off.
		 */
		ledcfg |= LEDCFG0_LED_GPIO_ENABLE;
		ledcfg &= ~LEDCFG0_LED0_IO_OUTPUT;
		ledcfg &= ~LEDCFG0_LED0_CTL_MASK;
		ledcfg &= ~LEDCFG0_LED0_SW_VAL;
		rtl_write_dword(rtlpriv, REG_LEDCFG0, ledcfg);
		break;
	case LED_PIN_LED1:
		ledcfg |= LEDCFG0_LED_GPIO_ENABLE;
		ledcfg &= ~LEDCFG0_LED1_IO_OUTPUT;
		ledcfg &= ~LEDCFG0_LED1_CTL_MASK;
		ledcfg &= ~LEDCFG0_LED1_SW_VAL;
		rtl_write_dword(rtlpriv, REG_LEDCFG0, ledcfg);
		break;
	default:
		rtl_dbg(rtlpriv, COMP_ERR, DBG_LOUD,
			"switch case %#x not processed\n", pin);
		break;
	}
}

static void _rtl92fe_sw_led_control(struct ieee80211_hw *hw,
				    enum led_ctl_mode ledaction)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	enum rtl_led_pin pin0 = rtlpriv->ledctl.sw_led0;

	switch (ledaction) {
	case LED_CTL_POWER_ON:
	case LED_CTL_LINK:
	case LED_CTL_NO_LINK:
		rtl92fe_sw_led_on(hw, pin0);
		break;
	case LED_CTL_POWER_OFF:
		rtl92fe_sw_led_off(hw, pin0);
		break;
	default:
		break;
	}
}

/* The panel's lamp is LED0 (8192cd_led.c, set_sw_LED0); rtl_lamp_register owns its trigger. */
void rtl92fe_lamp_set(struct ieee80211_hw *hw, bool on)
{
	if (on)
		rtl92fe_sw_led_on(hw, LED_PIN_LED0);
	else
		rtl92fe_sw_led_off(hw, LED_PIN_LED0);
}

void rtl92fe_led_control(struct ieee80211_hw *hw, enum led_ctl_mode ledaction)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	struct rtl_ps_ctl *ppsc = rtl_psc(rtl_priv(hw));

	if ((ppsc->rfoff_reason > RF_CHANGE_BY_PS) &&
	    (ledaction == LED_CTL_TX ||
	     ledaction == LED_CTL_RX ||
	     ledaction == LED_CTL_SITE_SURVEY ||
	     ledaction == LED_CTL_LINK ||
	     ledaction == LED_CTL_NO_LINK ||
	     ledaction == LED_CTL_START_TO_LINK ||
	     ledaction == LED_CTL_POWER_ON)) {
		return;
	}
	rtl_dbg(rtlpriv, COMP_LED, DBG_TRACE, "ledaction %d,\n", ledaction);
	_rtl92fe_sw_led_control(hw, ledaction);
}
