// SPDX-License-Identifier: GPL-2.0-only
/*
 * GalaChip GC1601 packet engine: its API for the PON and switch drivers
 *
 * Copyright (C) 2026 Ifran Ali <khanu.i.i.k.2000@gmail.com>
 */

#include "gc1601-eth.h"

/* The packet engine is shared with the GPON driver, which reaches it here. */
struct net_device *gc1601_pe_netdev;
/* Holds the packet engine for the PON driver's calls across probe and remove. */
DEFINE_MUTEX(gc1601_uni_lock);

/* Packet buffers left in the free list. */
int gc1601_pe_free_buffers(void)
{
	struct net_device *netdev = READ_ONCE(gc1601_pe_netdev);
	struct gc1601_eth *priv;

	if (!netdev)
		return -ENODEV;
	priv = netdev_priv(netdev);
	return readl(priv->base[GC1601_TX_SPACE]) & 0x2ff;
}
EXPORT_SYMBOL_GPL(gc1601_pe_free_buffers);

/*
 * The queue manager's almost-full level, bits 3:0 of
 * 0x90dd802c. A negative @level only reads it. Returns the previous level.
 */
int gc1601_pe_qmu_aful(int level)
{
	struct net_device *netdev = READ_ONCE(gc1601_pe_netdev);
	struct gc1601_eth *priv;
	u32 val;

	if (!netdev)
		return -ENODEV;
	priv = netdev_priv(netdev);
	if (!priv->queue_base)
		return -ENODEV;
	val = readl(priv->queue_base + 0x2c);
	if (level >= 0)
		writel((val & ~0xf) | (level & 0xf), priv->queue_base + 0x2c);
	return val & 0xf;
}
EXPORT_SYMBOL_GPL(gc1601_pe_qmu_aful);

/* The packet engine is shared with the GPON MAC; OMCI frames go to its driver. */
DEFINE_SPINLOCK(gc1601_omci_lock);
gc1601_omci_rx_fn gc1601_omci_rx;
void *gc1601_omci_ctx;
const struct gc1601_pon_port_ops *gc1601_pon_ops;
void *gc1601_pon_ctx;

/*
 * The DSA switch driver's LAN port. With no PHY yet it only claims the
 * port; its PHY is for the loopback probe and the UNI settings.
 */
int gc1601_pe_lan_attach(struct phy_device *phydev)
{
	struct net_device *netdev = READ_ONCE(gc1601_pe_netdev);
	struct gc1601_eth *priv;

	if (!netdev)
		return -EPROBE_DEFER;
	priv = netdev_priv(netdev);
	/* eth0 itself drives the PHY when the DT gives it one. */
	if (priv->phy_node)
		return -EBUSY;
	priv->dsa_lan = true;
	if (phydev)
		priv->lan_phy = phydev;
	return 0;
}
EXPORT_SYMBOL_GPL(gc1601_pe_lan_attach);

void gc1601_pe_lan_detach(void)
{
	struct net_device *netdev = READ_ONCE(gc1601_pe_netdev);

	if (netdev)
		((struct gc1601_eth *)netdev_priv(netdev))->lan_phy = NULL;
}
EXPORT_SYMBOL_GPL(gc1601_pe_lan_detach);

void gc1601_pe_lan_link_up(int speed, int duplex, bool tx_pause, bool rx_pause)
{
	struct net_device *netdev = READ_ONCE(gc1601_pe_netdev);
	struct gc1601_eth *priv;

	if (!netdev)
		return;
	priv = netdev_priv(netdev);
	priv->tx_pause = tx_pause;
	priv->rx_pause = rx_pause;
	gc1601_lan_link(priv, true, speed, duplex);
}
EXPORT_SYMBOL_GPL(gc1601_pe_lan_link_up);

void gc1601_pe_lan_link_down(void)
{
	struct net_device *netdev = READ_ONCE(gc1601_pe_netdev);

	if (netdev)
		gc1601_lan_link(netdev_priv(netdev), false, SPEED_UNKNOWN, DUPLEX_UNKNOWN);
}
EXPORT_SYMBOL_GPL(gc1601_pe_lan_link_down);

int gc1601_pe_omci_register(gc1601_omci_rx_fn fn, void *ctx)
{
	int ret = 0;

	spin_lock_bh(&gc1601_omci_lock);
	if (gc1601_omci_rx) {
		ret = -EBUSY;
	} else {
		gc1601_omci_rx = fn;
		gc1601_omci_ctx = ctx;
	}
	spin_unlock_bh(&gc1601_omci_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(gc1601_pe_omci_register);

void gc1601_pe_omci_unregister(void)
{
	spin_lock_bh(&gc1601_omci_lock);
	gc1601_omci_rx = NULL;
	gc1601_omci_ctx = NULL;
	spin_unlock_bh(&gc1601_omci_lock);
}
EXPORT_SYMBOL_GPL(gc1601_pe_omci_unregister);

int gc1601_pe_omci_xmit(const u8 *data, unsigned int len, u16 gem_port)
{
	struct gc1601_pon_meta pon = { .gem_port = gem_port, .omci = true };
	struct net_device *netdev = READ_ONCE(gc1601_pe_netdev);
	struct gc1601_eth *priv;
	struct sk_buff *skb;
	int ret;

	if (!netdev)
		return -ENODEV;
	if (len < 16 || len >= GC1601_BUFFER_SIZE || gem_port > 0xfff)
		return -EINVAL;
	priv = netdev_priv(netdev);
	/* Until the TX handoff is measured, frames become slot probes. */
	if (!netif_running(netdev) || !priv->tx_probe_aligned)
		return -EAGAIN;

	skb = netdev_alloc_skb(netdev, len);
	if (!skb)
		return -ENOMEM;
	skb_put_data(skb, data, len);

	netif_tx_lock_bh(netdev);
	ret = gc1601_xmit(skb, netdev, &pon);
	netif_tx_unlock_bh(netdev);
	/* Only a full ring leaves the skb with us. */
	if (ret == -EBUSY)
		dev_kfree_skb_any(skb);
	if (ret)
		return ret;
	priv->tx_omci++;
	return 0;
}
EXPORT_SYMBOL_GPL(gc1601_pe_omci_xmit);

/*
 * The PON driver's pon0. @dev is its own netdev, or NULL when pon0 is a
 * port of the DSA switch and its frames pass through eth0.
 */
int gc1601_pe_pon_register(struct net_device *dev, const struct gc1601_pon_port_ops *ops,
			   void *ctx)
{
	int ret = 0;

	spin_lock_bh(&gc1601_omci_lock);
	if (gc1601_pon_ops) {
		ret = -EBUSY;
	} else {
		gc1601_pon_ops = ops;
		gc1601_pon_ctx = ctx;
		WRITE_ONCE(gc1601_pon_dev, dev);
	}
	spin_unlock_bh(&gc1601_omci_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(gc1601_pe_pon_register);

void gc1601_pe_pon_unregister(void)
{
	spin_lock_bh(&gc1601_omci_lock);
	gc1601_pon_ops = NULL;
	gc1601_pon_ctx = NULL;
	WRITE_ONCE(gc1601_pon_dev, NULL);
	spin_unlock_bh(&gc1601_omci_lock);
	/* TX completion reads the device with interrupts off. */
	synchronize_net();
}
EXPORT_SYMBOL_GPL(gc1601_pe_pon_unregister);

/* Called with eth0's TX lock. Consumes @skb unless the ring is full (-EBUSY). */
int gc1601_pon_send(struct gc1601_eth *priv, struct sk_buff *skb,
		    const struct gc1601_pon_meta *meta)
{
	return gc1601_xmit(skb, priv->netdev, meta);
}

/*
 * Queue an upstream data frame on a GEM port for the PON driver's own pon0.
 * Consumes @skb unless the shared ring is full: then -EBUSY, the caller's
 * queue is already stopped and it is woken when a slot frees.
 */
int gc1601_pe_pon_xmit(struct sk_buff *skb, const struct gc1601_pon_meta *meta)
{
	struct net_device *netdev = READ_ONCE(gc1601_pe_netdev);
	struct gc1601_eth *priv;
	int ret;

	if (!netdev) {
		dev_kfree_skb_any(skb);
		return -ENODEV;
	}
	priv = netdev_priv(netdev);
	if (!netif_running(netdev) || !priv->tx_probe_aligned) {
		dev_kfree_skb_any(skb);
		return -EAGAIN;
	}

	netif_tx_lock_bh(netdev);
	ret = gc1601_pon_send(priv, skb, meta);
	netif_tx_unlock_bh(netdev);
	return ret;
}
EXPORT_SYMBOL_GPL(gc1601_pe_pon_xmit);

/* The PON port as the DSA switch driver sees it: link and counters. */
int gc1601_pe_pon_link(void)
{
	int speed = 0;

	spin_lock_bh(&gc1601_omci_lock);
	if (gc1601_pon_ops)
		speed = gc1601_pon_ops->link(gc1601_pon_ctx);
	spin_unlock_bh(&gc1601_omci_lock);
	return speed;
}
EXPORT_SYMBOL_GPL(gc1601_pe_pon_link);

int gc1601_pe_pon_sset_count(void)
{
	int n = 0;

	spin_lock_bh(&gc1601_omci_lock);
	if (gc1601_pon_ops)
		n = gc1601_pon_ops->get_sset_count(gc1601_pon_ctx);
	spin_unlock_bh(&gc1601_omci_lock);
	return n;
}
EXPORT_SYMBOL_GPL(gc1601_pe_pon_sset_count);

void gc1601_pe_pon_strings(u8 *data)
{
	spin_lock_bh(&gc1601_omci_lock);
	if (gc1601_pon_ops)
		gc1601_pon_ops->get_strings(gc1601_pon_ctx, data);
	spin_unlock_bh(&gc1601_omci_lock);
}
EXPORT_SYMBOL_GPL(gc1601_pe_pon_strings);

/* Reading the counters sleeps: the PON driver holds no lock of ours meanwhile. */
void gc1601_pe_pon_stats(u64 *data)
{
	const struct gc1601_pon_port_ops *ops;
	void *ctx;

	spin_lock_bh(&gc1601_omci_lock);
	ops = gc1601_pon_ops;
	ctx = gc1601_pon_ctx;
	spin_unlock_bh(&gc1601_omci_lock);
	if (ops)
		ops->get_stats(ctx, data);
}
EXPORT_SYMBOL_GPL(gc1601_pe_pon_stats);

/*
 * Drop what is queued on a T-CONT's eight queues,
 * as the stock EPON stack does for LLIDs 0-3 when the link falls back to
 * discovery.
 */
void gc1601_pe_flush_tcont(unsigned int tcont)
{
	struct net_device *netdev = READ_ONCE(gc1601_pe_netdev);
	struct gc1601_eth *priv;
	unsigned int q;

	if (!netdev)
		return;
	priv = netdev_priv(netdev);
	if (!priv->os_base)
		return;
	for (q = tcont * 8; q < tcont * 8 + 8; q++)
		writel((q & 0x1ff) | BIT(9), priv->os_base + 0x08);
}
EXPORT_SYMBOL_GPL(gc1601_pe_flush_tcont);

bool gc1601_pe_link_up(void)
{
	struct net_device *netdev = READ_ONCE(gc1601_pe_netdev);
	struct gc1601_eth *priv;

	if (!netdev)
		return false;
	priv = netdev_priv(netdev);
	return !READ_ONCE(priv->uni_disabled) && netif_carrier_ok(netdev);
}
EXPORT_SYMBOL_GPL(gc1601_pe_link_up);

int gc1601_pe_set_uni(bool enable)
{
	struct gc1601_eth *priv;
	unsigned long flags;
	u32 val;
	int ret = 0;

	mutex_lock(&gc1601_uni_lock);
	if (!gc1601_pe_netdev) {
		ret = -ENODEV;
		goto out;
	}
	priv = netdev_priv(gc1601_pe_netdev);
	spin_lock_irqsave(&priv->irq_lock, flags);
	priv->uni_disabled = !enable;
	if (priv->running) {
		val = readl(priv->base[GC1601_GMAC] + GC_GMAC_CFG);
		val &= ~(GC_GMAC_CFG_TX_EN | GC_GMAC_CFG_RX_EN);
		if (enable && priv->link)
			val |= GC_GMAC_CFG_TX_EN | GC_GMAC_CFG_RX_EN;
		writel(val, priv->base[GC1601_GMAC] + GC_GMAC_CFG);
	}
	spin_unlock_irqrestore(&priv->irq_lock, flags);
out:
	mutex_unlock(&gc1601_uni_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(gc1601_pe_set_uni);
