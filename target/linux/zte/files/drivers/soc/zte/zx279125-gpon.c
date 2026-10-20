// SPDX-License-Identifier: GPL-2.0-only
/*
 * ITU-T G.984 GPON MAC state machine for ZTE Sanechips ZX279125
 *
 * Copyright (C) 2026 Ifran Ali <khanu.i.i.k.2000@gmail.com>
 */

#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/spinlock.h>

#include "zx279125-pon.h"

int zx279125_gpon_set_state(struct zx279125_pon *pon, enum zx_gpon_onu_state state)
{
	unsigned long flags;
	u32 val;

	spin_lock_irqsave(&pon->lock, flags);
	pon->state = state;

	val = readl(pon->base_onu + ZX_ONU_STATE_CTRL);
	val &= ~ZX_ONU_STATE_MASK;
	val |= (state & ZX_ONU_STATE_MASK);
	writel(val, pon->base_onu + ZX_ONU_STATE_CTRL);

	spin_unlock_irqrestore(&pon->lock, flags);

	dev_info(pon->dev, "GPON state changed to O%d\n", state);
	return 0;
}

int zx279125_gpon_init(struct zx279125_pon *pon)
{
	unsigned long flags;
	u32 val;

	spin_lock_irqsave(&pon->lock, flags);

	/* 1. Put ONU into State O1 (Initial) */
	pon->state = ZX_ONU_STATE_O1_INITIAL;
	val = readl(pon->base_onu + ZX_ONU_STATE_CTRL);
	val &= ~(ZX_ONU_STATE_MASK | ZX_ONU_ID_MASK);
	val |= ZX_ONU_STATE_O1_INITIAL;
	writel(val, pon->base_onu + ZX_ONU_STATE_CTRL);

	/* 2. Configure Downstream Rx */
	writel(ZX_PON_RX_CFG_EN, pon->base_gpon + ZX_PON_RX_CFG);

	/* 3. Enable Rx FEC */
	if (pon->fec_enabled)
		writel(ZX_PON_RX_FEC_EN, pon->base_gpon + ZX_PON_RX_FEC_CFG);
	else
		writel(0, pon->base_gpon + ZX_PON_RX_FEC_CFG);

	/* 4. Configure Frame lengths in GEM Engine */
	writel(0x05ee, pon->base_gem + ZX_GEM_ETH_LEN_CFG);  /* 1518 MTU */
	writel(0x0030, pon->base_gem + ZX_GEM_OMCI_LEN_CFG); /* 48 byte OMCI */
	writel(ZX_GEM_OMCI_DROP_DIS, pon->base_gem + ZX_GEM_OMCI_CTRL);

	/* 5. Set Serial Number */
	if (pon->serial_number[0]) {
		u32 sn_hi = (pon->serial_number[0] << 24) | (pon->serial_number[1] << 16) |
			    (pon->serial_number[2] << 8)  | (pon->serial_number[3]);
		u32 sn_lo = (pon->serial_number[4] << 24) | (pon->serial_number[5] << 16) |
			    (pon->serial_number[6] << 8)  | (pon->serial_number[7]);
		writel(sn_hi, pon->base_tx + ZX_PON_SERIAL_NUM);
		writel(sn_lo, pon->base_tx + ZX_PON_SERIAL_NUM + 4);
	}

	/* 6. Unmask GPON interrupts */
	val = ZX_PON_INT_PLOAM_RCV | ZX_PON_INT_STATE_CHANGE | ZX_PON_INT_RANGING;
	writel(val, pon->base_gpon + ZX_PON_INT_MASK);

	/* 7. Unmask top-level GMAC GPON interrupt */
	val = readl(pon->base_gmac_int + ZX_GMAC_INT_MASK);
	val &= ~ZX_GMAC_INT_GPON;
	writel(val, pon->base_gmac_int + ZX_GMAC_INT_MASK);

	spin_unlock_irqrestore(&pon->lock, flags);

	dev_info(pon->dev, "ZX279125 GPON MAC core initialized in state O1\n");
	return 0;
}

void zx279125_gpon_exit(struct zx279125_pon *pon)
{
	unsigned long flags;
	u32 val;

	spin_lock_irqsave(&pon->lock, flags);

	/* Mask interrupts */
	val = readl(pon->base_gmac_int + ZX_GMAC_INT_MASK);
	val |= ZX_GMAC_INT_GPON;
	writel(val, pon->base_gmac_int + ZX_GMAC_INT_MASK);
	writel(0, pon->base_gpon + ZX_PON_INT_MASK);

	/* Force optical Tx off */
	writel(0, pon->base_tx + ZX_PON_TX_CFG);

	/* Reset ONU state to O1 */
	pon->state = ZX_ONU_STATE_O1_INITIAL;
	val = readl(pon->base_onu + ZX_ONU_STATE_CTRL);
	val &= ~ZX_ONU_STATE_MASK;
	val |= ZX_ONU_STATE_O1_INITIAL;
	writel(val, pon->base_onu + ZX_ONU_STATE_CTRL);

	spin_unlock_irqrestore(&pon->lock, flags);
}

irqreturn_t zx279125_pon_isr(int irq, void *dev_id)
{
	struct zx279125_pon *pon = dev_id;
	u32 gmac_status, gpon_status;

	gmac_status = readl(pon->base_gmac_int + ZX_GMAC_INT_STATUS);
	if (!(gmac_status & ZX_GMAC_INT_GPON))
		return IRQ_NONE;

	spin_lock(&pon->lock);

	gpon_status = readl(pon->base_gpon + ZX_PON_INT_STATUS);
	/* Write-1-to-clear status bits */
	writel(gpon_status, pon->base_gpon + ZX_PON_INT_STATUS);

	if (gpon_status & ZX_PON_INT_STATE_CHANGE) {
		u32 cur_state = readl(pon->base_onu + ZX_ONU_STATE_CTRL) & ZX_ONU_STATE_MASK;
		pon->state = cur_state;
	}

	if (gpon_status & ZX_PON_INT_RANGING) {
		/* Equalization Delay received from OLT Ranging_Time message */
		u32 eqd = readl(pon->base_onu + ZX_ONU_EQD_DELAY);
		dev_dbg(pon->dev, "OLT Ranging EqD: 0x%08x\n", eqd);
	}

	spin_unlock(&pon->lock);
	return IRQ_HANDLED;
}
