// SPDX-License-Identifier: GPL-2.0-only
/* ZX279125 GPON register interface, reconstructed from the shipped SDK. */
#include <linux/iopoll.h>
#include <linux/netdevice.h>
#include <linux/unaligned.h>
#include "zx279125-pon.h"

static void zx_update(void __iomem *reg, u32 mask, u32 value)
{
	writel((readl(reg) & ~mask) | (value & mask), reg);
}

void zx279125_gpon_hold_tx(struct zx279125_pon *pon, bool hold)
{
	unsigned long flags;

	if (!pon->laser_allowed)
		hold = true;

	spin_lock_irqsave(&pon->lock, flags);
	pon->tx_hold = hold;
	zx_update(pon->base_tx, ZX_PON_TX_FORCE_CLOSE,
		  hold ? ZX_PON_TX_FORCE_CLOSE : 0);
	spin_unlock_irqrestore(&pon->lock, flags);
}

int zx279125_gpon_set_state(struct zx279125_pon *pon,
			    enum zx_gpon_onu_state state)
{
	unsigned long flags;
	enum xpon_registration_state registration = XPON_REGISTRATION_DOWN;

	if (state < ZX_ONU_STATE_O1_INITIAL || state > ZX_ONU_STATE_O7_EMERGENCY_STOP)
		return -EINVAL;
	spin_lock_irqsave(&pon->lock, flags);
	zx_update(pon->base_onu, ZX_ONU_STATE_MASK, state);
	WRITE_ONCE(pon->state, state);
	spin_unlock_irqrestore(&pon->lock, flags);
	if (state == ZX_ONU_STATE_O2_STANDBY || state == ZX_ONU_STATE_O3_SERIAL_NUM)
		registration = XPON_REGISTRATION_DISCOVERY;
	else if (state == ZX_ONU_STATE_O4_RANGING)
		registration = XPON_REGISTRATION_REGISTERING;
	else if (state == ZX_ONU_STATE_O5_OPERATION)
		registration = XPON_REGISTRATION_OPERATIONAL;
	if (pon->xpon) {
		xpon_device_report_registration(pon->xpon, registration);
		xpon_device_report_carrier(pon->xpon,
			state == ZX_ONU_STATE_O5_OPERATION && !pon->emergency_stop);
	}
	if (pon->netdev) {
		if (state == ZX_ONU_STATE_O5_OPERATION && !pon->emergency_stop)
			netif_carrier_on(pon->netdev);
		else
			netif_carrier_off(pon->netdev);
	}
	dev_dbg(pon->dev, "GPON O%d\n", state);
	return 0;
}

int zx279125_gpon_set_alloc(struct zx279125_pon *pon, unsigned int index,
			    u16 alloc, bool valid)
{
	unsigned long flags;
	u32 offset, shift;

	/* SDK RxBwMapSet: slot0 is separate, slots1..32 are packed pairs. */
	if (index > 32 || alloc > 0xfff)
		return -EINVAL;
	offset = index ? 0x80 + ((index - 1) / 2) * 4 : 0x7c;
	shift = index && !(index & 1) ? 16 : 0;
	spin_lock_irqsave(&pon->lock, flags);
	zx_update(pon->base_onu + offset, 0x1fff << shift,
		  (alloc | (valid ? BIT(12) : 0)) << shift);
	spin_unlock_irqrestore(&pon->lock, flags);
	return 0;
}

int zx279125_gpon_set_omcc(struct zx279125_pon *pon, u16 gem, bool valid)
{
	unsigned long flags;
	u16 previous;

	if (gem > 0xfff)
		return -EINVAL;
	spin_lock_irqsave(&pon->lock, flags);
	previous = pon->omci_port;
	if (previous < 0x1000 && previous != gem)
		zx_update(pon->base_ports + previous * 4, 3, 0);
	zx_update(pon->base_ports + gem * 4, 3, valid ? 1 : 0);
	zx_update(pon->base_gem, GENMASK(31, 20), (u32)gem << 20);
	pon->omci_port = valid ? gem : 0xffff;
	spin_unlock_irqrestore(&pon->lock, flags);
	if (pon->odev)
		omci_device_set_channel(pon->odev, gem, valid);
	return 0;
}

int zx279125_gpon_send_ploam(struct zx279125_pon *pon, u8 msg,
			     const u8 data[10], unsigned int repeat)
{
	unsigned long flags;
	u32 val;
	int ret;

	if (!repeat || repeat > 0xffff || !READ_ONCE(pon->running))
		return -EINVAL;
	spin_lock_irqsave(&pon->lock, flags);
	ret = readl_poll_timeout_atomic(pon->base_tx + 0x48, val, !val, 1, 100);
	if (!ret) {
		writel((repeat << 16) | 3, pon->base_tx + 0x58);
		writel(((u32)pon->onu_id << 24) | ((u32)msg << 16) |
		       get_unaligned_be16(data), pon->base_tx + 0x4c);
		writel(get_unaligned_be32(data + 2), pon->base_tx + 0x50);
		writel(get_unaligned_be32(data + 6), pon->base_tx + 0x54);
		writel(1, pon->base_tx + 0x48);
	}
	spin_unlock_irqrestore(&pon->lock, flags);
	return ret;
}

int zx279125_gpon_init(struct zx279125_pon *pon)
{
	pon->omci_port = 0xffff;
	pon->onu_id = 0xff;
	zx279125_ploam_init(pon);
	zx279125_gpon_hold_tx(pon, true);
	writel(ZX_PON_INT_VALID, pon->base_gpon + ZX_PON_INT_MASK);
	zx279125_pp_update_bits(pon->base_gmac_int + ZX_GMAC_INT_MASK,
				ZX_GMAC_INT_GPON, ZX_GMAC_INT_GPON);
	return zx279125_gpon_set_state(pon, ZX_ONU_STATE_O1_INITIAL);
}

int zx279125_gpon_start(struct zx279125_pon *pon)
{
	unsigned long flags;
	u32 status;

	if (READ_ONCE(pon->running))
		return 0;
	if (!pon->serial_number[0] || pon->emergency_stop)
		return -EPERM;
	zx279125_gpon_hold_tx(pon, true);
	spin_lock_irqsave(&pon->lock, flags);
	/* SDK receive delineation: alpha2, delta5, K5, M4. */
	zx_update(pon->base_rx, 0xffff, 0x2524);
	zx_update(pon->base_rx + 0x24, 0xf, 8);
	zx_update(pon->base_rx + ZX_PON_RX_FEC_CFG, 0x77,
		  pon->fec_enabled ? 7 : 0);
	/* DRS length registers contain maximum in low bits, minimum in high. */
	writel((64 << 16) | 2048, pon->base_gem + 4);
	writel((48 << 16) | 48, pon->base_gem + 8);
	zx_update(pon->base_gem, 0xf, 1);
	writel(get_unaligned_be32(pon->serial_number), pon->base_tx + 0x40);
	writel(get_unaligned_be32(pon->serial_number + 4), pon->base_tx + 0x44);
	zx_update(pon->base_onu, ZX_ONU_ID_MASK, 0xff << ZX_ONU_ID_SHIFT);
	/* SDK TxConfigSet burst datapath; force-close remains held. */
	zx_update(pon->base_tx + 0xe8, 0xffff, 0x603);
	zx_update(pon->base_tx + 0xfc, 0x7f, 1);
	zx_update(pon->base_tx + 4, 1, 1);
	zx_update(pon->base_tx + 0xc, 1, 1);
	zx_update(pon->base_tx + 0xf4, 1, 1);
	pon->running = true;
	zx_update(pon->base_rx, 3, 3);
	writel(ZX_PON_INT_VALID, pon->base_gpon + ZX_PON_INT_STATUS);
	writel(ZX_PON_INT_VALID & ~(ZX_PON_INT_LOS | ZX_PON_INT_LOF |
					 ZX_PON_INT_PLOAM_RCV),
	       pon->base_gpon + ZX_PON_INT_MASK);
	status = readl(pon->base_rx + ZX_PON_RX_STAT);
	spin_unlock_irqrestore(&pon->lock, flags);
	zx279125_pp_update_bits(pon->base_gmac_int + ZX_GMAC_INT_MASK,
				ZX_GMAC_INT_GPON, 0);
	xpon_device_report_optical(pon->xpon, !(status & BIT(4)), status & BIT(4));
	if (!(status & BIT(4)))
		zx279125_gpon_set_state(pon, ZX_ONU_STATE_O2_STANDBY);
	return 0;
}

void zx279125_gpon_exit(struct zx279125_pon *pon)
{
	unsigned long flags;

	zx279125_gpon_hold_tx(pon, true);
	zx279125_pp_update_bits(pon->base_gmac_int + ZX_GMAC_INT_MASK,
				ZX_GMAC_INT_GPON, ZX_GMAC_INT_GPON);
	spin_lock_irqsave(&pon->lock, flags);
	pon->running = false;
	writel(ZX_PON_INT_VALID, pon->base_gpon + ZX_PON_INT_MASK);
	zx_update(pon->base_tx + 4, 1, 0);
	zx_update(pon->base_rx, 1, 0);
	spin_unlock_irqrestore(&pon->lock, flags);
	synchronize_irq(pon->irq);
	zx279125_ploam_stop(pon);
	zx279125_gpon_set_state(pon, ZX_ONU_STATE_O1_INITIAL);
}

irqreturn_t zx279125_pon_isr(int irq, void *dev_id)
{
	struct zx279125_pon *pon = dev_id;
	unsigned long flags;
	u32 pending;

	pending = readl(pon->base_gmac_int + ZX_GMAC_INT_STATUS) &
		  ~readl(pon->base_gmac_int + ZX_GMAC_INT_MASK);
	if (!(pending & ZX_GMAC_INT_GPON))
		return IRQ_NONE;
	spin_lock_irqsave(&pon->lock, flags);
	pending = readl(pon->base_gpon + ZX_PON_INT_STATUS) &
		  ~readl(pon->base_gpon + ZX_PON_INT_MASK) & ZX_PON_INT_VALID;
	if (pending) {
		writel(readl(pon->base_gpon + ZX_PON_INT_MASK) | pending,
		       pon->base_gpon + ZX_PON_INT_MASK);
		pon->pending_irqs |= pending;
		schedule_work(&pon->state_work);
	}
	spin_unlock_irqrestore(&pon->lock, flags);
	return pending ? IRQ_HANDLED : IRQ_NONE;
}
