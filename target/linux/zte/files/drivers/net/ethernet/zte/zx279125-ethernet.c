// SPDX-License-Identifier: GPL-2.0-only
/* ZX279125 CPU packet engine and Ethernet UNI. */
#include <linux/bitmap.h>
#include <linux/delay.h>
#include <linux/etherdevice.h>
#include <linux/ethtool.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/if_vlan.h>
#include <linux/rtnetlink.h>
#include <linux/soc/zte/zx279125-pp.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/mii.h>
#include <linux/netdevice.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/of_mdio.h>
#include <linux/of_net.h>
#include <linux/of_reserved_mem.h>
#include <linux/phy.h>
#include <linux/platform_device.h>
#include <linux/soc/zte/zx279125-tagparam.h>
#include <linux/unaligned.h>
#include <linux/workqueue.h>

/* U-Boot initializes 4096 BMU entries; PP count is a separate value. */
#define ZX_BUFFERS	4096
#define ZX_PP_COUNT	2048
#define ZX_BUFFER_SIZE	2048
#define ZX_RING_SIZE	4096
#define ZX_QUEUES	16
/* Layout inside the no-map packet pool described by memory-region 0. */
#define ZX_POOL_SIZE	0xa00000
#define ZX_POOL_BPT	0x800000
#define ZX_POOL_AUX	0x820000
#define ZX_POOL_CURSORS	0x840000
#define ZX_POOL_RINGS	0x900000
#define ZX_RINGS_SIZE	(ZX_QUEUES * 0x10000)
static_assert(ZX_BUFFERS * ZX_BUFFER_SIZE <= ZX_POOL_BPT);
static_assert(ZX_POOL_RINGS + ZX_RINGS_SIZE <= ZX_POOL_SIZE);
#define ZX_CONTROL_MAX	(ZX_BUFFER_SIZE - 32)
#define ZX_FRAME_MAX	(ETH_FRAME_LEN + 2 * VLAN_HLEN)

struct zx_lan {
	struct net_device *dev;
	void __iomem *pp, *bmu, *mailbox, *mac, *dma, *intc;
	void __iomem *global, *spa;
	void __iomem *cursors, *rings, *buffers;
	phys_addr_t pool;
	struct napi_struct napi;
	int irq;
	struct delayed_work link_poll;
	struct work_struct address_work;
	struct notifier_block netdev_nb;
	u8 management_mac[ETH_ALEN];
	struct mii_bus *bus;
	u32 phy_addr;
	bool local_cursors;
	bool engine_active;
	int link_status;
	/* Serialize BMU commands, TX publication and MAC ingress changes. */
	spinlock_t lock;
	u32 consumer[ZX_QUEUES];
	u8 next_queue;
	struct zx279125_pp_transport transport;
	u32 tx_reused;
	DECLARE_BITMAP(tx_seen, ZX_BUFFERS);
	bool fault;
	u8 frame[ZX_FRAME_MAX];
};

static void zx_fault(struct zx_lan *lan, const char *reason)
{
	lan->fault = true;
	writel(readl(lan->pp + 0x44) | 7, lan->pp + 0x44);
	zx279125_pp_update_bits(lan->intc + 0x44, BIT(2), BIT(2));
	writel(readl(lan->mac) & ~BIT(2), lan->mac);
	readl(lan->mac);
	netif_stop_queue(lan->dev);
	netif_carrier_off(lan->dev);
	netdev_err(lan->dev, "%s; ingress stopped, DMA memory retained\n", reason);
}

static int zx_wait(void __iomem *reg, u32 mask)
{
	u32 val;

	return readl_poll_timeout_atomic(reg, val, !(val & mask), 1, 100);
}

/* Vendor pp_net_poll: queues 0..7 are low, queues 8..15 high halfwords. */
static u32 zx_rx_count(struct zx_lan *lan, unsigned int queue)
{
	u32 count = readl(lan->dma + 0x100 + (queue & 7) * 4);

	return (count >> (queue < 8 ? 0 : 16)) & 0xffff;
}

static bool zx_rx_pending(struct zx_lan *lan)
{
	unsigned int q;

	for (q = 0; q < ZX_QUEUES; q++)
		if (zx_rx_count(lan, q))
			return true;
	return false;
}

static int zx_release(struct zx_lan *lan, const u8 *desc, u32 bp,
		      unsigned int queue)
{
	/* U-Boot 41f1eb10: source, queue and completion flag are independent. */
	u32 command = 0x10 | (desc[6] & 7) |
		      ((desc[15] & BIT(5)) ? BIT(14) : 0) |
		      ((desc[9] & BIT(7)) ? BIT(3) : 0);
	int ret;

	if (!zx_rx_count(lan, queue) || zx_rx_count(lan, queue) >= ZX_RING_SIZE)
		return -EINVAL;
	ret = zx_wait(lan->mailbox + 0x64, 1);
	if (ret)
		return ret;
	writel(command, lan->mailbox + 0x68);
	writel(1, lan->mailbox + 0x64);
	ret = zx_wait(lan->mailbox + 0x64, 1);
	if (ret)
		return ret;
	if (zx_rx_count(lan, queue) >= ZX_RING_SIZE)
		return -EINVAL;
	lan->consumer[queue] = (lan->consumer[queue] + 1) % ZX_RING_SIZE;
	writel(lan->consumer[queue], lan->cursors + queue * 8 + 4);
	/* Ensure consumer cursor update is visible before returning buffer to BMU */
	wmb();
	ret = zx_wait(lan->bmu + 0xc0, 8);
	if (ret)
		return ret;
	writel(bp, lan->bmu + 0x0c);
	return zx_wait(lan->bmu + 0xc0, 8);
}

static int zx_napi_poll(struct napi_struct *napi, int budget)
{
	struct zx_lan *lan = container_of(napi, struct zx_lan, napi);
	struct net_device *dev = lan->dev;
	int work_done = 0;
	unsigned long flags;

	spin_lock_irqsave(&lan->lock, flags);
	if (lan->fault) {
		spin_unlock_irqrestore(&lan->lock, flags);
		napi_complete_done(napi, 0);
		return 0;
	}
	spin_unlock_irqrestore(&lan->lock, flags);

	while (work_done < budget) {
		struct sk_buff *skb;
		u8 desc[16];
		u32 count = 0, bp, len, offset;
		unsigned int q, scanned;
		bool control;
		int ret;

		for (scanned = 0; scanned < ZX_QUEUES; scanned++) {
			q = lan->next_queue;
			lan->next_queue = (q + 1) % ZX_QUEUES;
			count = zx_rx_count(lan, q);
			if (count)
				break;
		}
		if (!count)
			break;
		if (count >= ZX_RING_SIZE)
			goto invalid;
		/* Observe descriptors after the hardware advertises queued packets. */
		rmb();
		memcpy_fromio(desc, lan->rings + q * 0x10000 + lan->consumer[q] * 16, 16);
		bp = get_unaligned_le16(desc + 10) & 0x3fff;
		len = (get_unaligned_le32(desc + 12) >> 6) & 0x7ff;
		offset = desc[12] & 63;
		control = (desc[7] & BIT(7)) && (desc[15] & BIT(5));
		if (bp >= ZX_BUFFERS)
			goto invalid;
		if (!len || len > ZX_CONTROL_MAX ||
		    len + offset > ZX_BUFFER_SIZE) {
			spin_lock_irqsave(&lan->lock, flags);
			ret = zx_release(lan, desc, bp, q);
			if (ret)
				zx_fault(lan, "invalid RX frame release ownership uncertain");
			spin_unlock_irqrestore(&lan->lock, flags);
			dev->stats.rx_errors++;
			if (ret) {
				napi_complete_done(napi, work_done);
				return work_done;
			}
			work_done++;
			continue;
		}
		skb = napi_alloc_skb(napi, len);
		if (skb)
			memcpy_fromio(skb_put(skb, len),
				      lan->buffers + bp * ZX_BUFFER_SIZE + offset, len);
		spin_lock_irqsave(&lan->lock, flags);
		if (lan->fault) {
			spin_unlock_irqrestore(&lan->lock, flags);
			if (skb)
				dev_kfree_skb(skb);
			napi_complete_done(napi, work_done);
			return work_done;
		}
		ret = zx_release(lan, desc, bp, q);
		if (ret)
			zx_fault(lan, "RX release/return ownership uncertain");
		spin_unlock_irqrestore(&lan->lock, flags);
		if (ret) {
			if (skb)
				dev_kfree_skb(skb);
			napi_complete_done(napi, work_done);
			return work_done;
		}
		if (skb && control) {
			zx279125_pp_receive_control(skb,
				(get_unaligned_le16(desc + 8) >> 2) & 0xfff);
		} else if (skb && len >= ETH_HLEN &&
			   (ether_addr_equal(skb->data + ETH_ALEN, dev->dev_addr) ||
				   ether_addr_equal(skb->data + ETH_ALEN, lan->management_mac))) {
			/* Drop switch-reflected loopback of locally transmitted frames */
			dev_kfree_skb(skb);
			dev->stats.rx_dropped++;
		} else if (skb && (!netif_running(dev) || (desc[7] & BIT(7)) ||
				  (desc[15] & BIT(5)) ||
				  len < ETH_HLEN || len > ZX_FRAME_MAX)) {
			/* PON data exceptions need a subscriber consumer, not LAN injection. */
			dev_kfree_skb(skb);
			dev->stats.rx_dropped++;
		} else if (skb) {
			skb->protocol = eth_type_trans(skb, dev);
			dev->stats.rx_packets++;
			dev->stats.rx_bytes += len;
			napi_gro_receive(napi, skb);
		} else {
			dev->stats.rx_dropped++;
		}
		work_done++;
	}

	if (work_done < budget) {
		if (napi_complete_done(napi, work_done)) {
			spin_lock_irqsave(&lan->lock, flags);
			if (!lan->fault && lan->engine_active && lan->irq > 0)
				writel(readl(lan->pp + 0x44) & ~0x7, lan->pp + 0x44);
			spin_unlock_irqrestore(&lan->lock, flags);
		}
	}
	return work_done;

invalid:
	spin_lock_irqsave(&lan->lock, flags);
	zx_fault(lan, "invalid inherited RX descriptor/count");
	spin_unlock_irqrestore(&lan->lock, flags);
	napi_complete_done(napi, work_done);
	return work_done;
}

static irqreturn_t zx_isr(int irq, void *dev_id)
{
	struct zx_lan *lan = dev_id;
	u32 status;
	unsigned long flags;

	if (!lan->intc)
		return IRQ_NONE;

	status = readl(lan->intc + 0x40) & ~readl(lan->intc + 0x44);
	if (!(status & BIT(2)))
		return IRQ_NONE;

	/* EPON owns top-level bit 1. Only claim our PP source while running. */
	spin_lock_irqsave(&lan->lock, flags);
	if (lan->fault || !lan->engine_active) {
		spin_unlock_irqrestore(&lan->lock, flags);
		return IRQ_NONE;
	}
	writel(readl(lan->pp + 0x44) | 0x7, lan->pp + 0x44);
	napi_schedule(&lan->napi);
	spin_unlock_irqrestore(&lan->lock, flags);
	return IRQ_HANDLED;
}

/* Caller holds lock: BMU allocation and mailbox publication are indivisible. */
static int zx_submit(struct zx_lan *lan, const void *data, size_t len,
		     u16 gem, bool control)
{
	u8 desc[16];
	u32 bp, val, offset = control ? 32 : 16;

	if (lan->fault || !lan->engine_active || (!control && !netif_running(lan->dev)))
		return -ENETDOWN;
	if (zx_wait(lan->mailbox + 0x50, 1) || readl(lan->bmu + 0x10))
		goto fault;
	writel(1, lan->bmu + 0x10);
	if (zx_wait(lan->bmu + 0x10, ~0U))
		goto fault;
	val = readl(lan->bmu + 8);
	bp = val & 0xffff;
	if (!(val & BIT(31)))
		return -ENOBUFS;
	if (bp >= ZX_BUFFERS)
		goto fault;
	if (test_and_set_bit(bp, lan->tx_seen))
		lan->tx_reused++;
	memcpy_toio(lan->buffers + bp * ZX_BUFFER_SIZE + offset, data, len);
	/* Ensure packet payload is written before descriptor submission */
	wmb();
	put_unaligned_le32(0x2000, desc);
	put_unaligned_le32(control ? 0x80380000 : 0, desc + 4);
	put_unaligned_le32(control ? 0x8000 | (gem << 2) : 0x8002, desc + 8);
	put_unaligned_le16(bp, desc + 10);
	put_unaligned_le32((control ? 0x10000000 : 0x30000000) |
			   (len << 6) | offset, desc + 12);
	val = get_unaligned_le16(desc + 14);
	put_unaligned_le16((val & ~0xffe) | (len << 1), desc + 14);
	if (zx_wait(lan->mailbox + 0x50, 1))
		goto fault;
	writel(get_unaligned_le32(desc + 12), lan->mailbox + 0x54);
	writel(get_unaligned_le32(desc + 8), lan->mailbox + 0x58);
	writel(get_unaligned_le32(desc + 4), lan->mailbox + 0x5c);
	writel(get_unaligned_le32(desc), lan->mailbox + 0x60);
	writel(1, lan->mailbox + 0x50);
	return 0;
fault:
	zx_fault(lan, "TX allocation/publication ownership uncertain");
	return -EIO;
}

static int zx_send_control(void *priv, const void *data, size_t len, u16 gem)
{
	struct zx_lan *lan = priv;
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&lan->lock, flags);
	ret = zx_submit(lan, data, len, gem, true);
	spin_unlock_irqrestore(&lan->lock, flags);
	return ret;
}

static netdev_tx_t zx_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct zx_lan *lan = netdev_priv(dev);
	unsigned long flags;
	int ret = -EINVAL;

	if (skb->len > ZX_FRAME_MAX)
		goto drop;
	if (skb_put_padto(skb, ETH_ZLEN)) {
		dev->stats.tx_dropped++;
		return NETDEV_TX_OK;
	}
	spin_lock_irqsave(&lan->lock, flags);
	if (!skb_copy_bits(skb, 0, lan->frame, skb->len))
		ret = zx_submit(lan, lan->frame, skb->len, 0, false);
	if (!ret) {
		dev->stats.tx_packets++;
		dev->stats.tx_bytes += skb->len;
	}
	spin_unlock_irqrestore(&lan->lock, flags);
drop:
	if (ret)
		dev->stats.tx_dropped++;
	dev_kfree_skb(skb);
	if (zx_rx_pending(lan))
		napi_schedule(&lan->napi);
	return NETDEV_TX_OK;
}

static int zx_read_link(struct zx_lan *lan)
{
	int page, status, ret;

	mutex_lock(&lan->bus->mdio_lock);
	page = __mdiobus_read(lan->bus, lan->phy_addr, 0x1e);
	if (page < 0) {
		status = page;
		goto out;
	}
	if (page) {
		ret = __mdiobus_write(lan->bus, lan->phy_addr, 0x1e, 0);
		if (ret) {
			status = ret;
			goto restore;
		}
	}
	status = __mdiobus_read(lan->bus, lan->phy_addr, 0x1a);
	if (status >= 0)
		status = __mdiobus_read(lan->bus, lan->phy_addr, 0x1a);
restore:
	if (page) {
		ret = __mdiobus_write(lan->bus, lan->phy_addr, 0x1e, page);
		if (ret)
			status = ret;
	}
out:
	mutex_unlock(&lan->bus->mdio_lock);
	return status < 0 ? status : status & GENMASK(9, 6);
}

static void zx_update_link(struct zx_lan *lan, int status)
{
	struct net_device *dev = lan->dev;
	unsigned long flags;
	u32 config, mode;

	spin_lock_irqsave(&lan->lock, flags);
	if (lan->fault || lan->link_status == status)
		goto out;
	if (status < 0 || !(status & BIT(6))) {
		netif_stop_queue(dev);
		netif_carrier_off(dev);
		writel(readl(lan->mac) & ~BIT(2), lan->mac);
		readl(lan->mac);
		if (lan->link_status != status)
			netdev_info(dev, "link down/status=%d\n", status);
		lan->link_status = status;
		goto out;
	}
	mode = (status >> 7) & 7;
	if (mode > 5) {
		zx_fault(lan, "unsupported PHY link mode");
		goto out;
	}
	config = readl(lan->mac);
	if (mode >= 4) {
		config &= ~BIT(15);
		config |= BIT(11);
	} else {
		config |= BIT(15);
		config = (config & ~BIT(11)) | ((mode & 1) ? BIT(11) : 0);
		config = (config & ~BIT(14)) | ((mode >= 2) ? BIT(14) : 0);
	}
	writel(config | BIT(2) | BIT(3), lan->mac);
	readl(lan->mac);
	netif_carrier_on(dev);
	netif_wake_queue(dev);
	if (lan->link_status != status)
		netdev_info(dev, "link up %u Mbps %s duplex\n",
			    mode >= 4 ? 1000 : mode >= 2 ? 100 : 10,
			    mode & 1 ? "full" : "half");
	lan->link_status = status;
out:
	spin_unlock_irqrestore(&lan->lock, flags);
}

static void zx_link_poll(struct work_struct *work)
{
	struct zx_lan *lan = container_of(to_delayed_work(work), struct zx_lan,
					link_poll);

	zx_update_link(lan, zx_read_link(lan));
	if (!READ_ONCE(lan->fault))
		schedule_delayed_work(&lan->link_poll, HZ);
}

static bool zx_ownership_idle(struct zx_lan *lan)
{
	unsigned int q;

	if (readl(lan->mailbox + 0x50) & 1 ||
	    readl(lan->mailbox + 0x64) & 1 || readl(lan->bmu + 0x10) ||
	    readl(lan->bmu + 0xc0) & 8 ||
	    readl(lan->bmu + 0x88) != readl(lan->bmu + 0x8c) ||
	    readl(lan->bmu + 0x80) + readl(lan->bmu + 0x84) != ZX_BUFFERS)
		return false;
	for (q = 0; q < 16; q++)
		if (zx_rx_count(lan, q))
			return false;
	return true;
}

static int zx_open(struct net_device *dev)
{
	struct zx_lan *lan = netdev_priv(dev);
	int status;

	if (lan->fault)
		return -EIO;
	status = zx_read_link(lan);
	if (status < 0)
		return status;
	lan->link_status = -1;
	zx_update_link(lan, status);
	if (lan->fault)
		return -EIO;

	schedule_delayed_work(&lan->link_poll, HZ);
	return 0;
}

static int zx_stop(struct net_device *dev)
{
	struct zx_lan *lan = netdev_priv(dev);
	bool idle;
	int ret;

	netif_stop_queue(dev);
	cancel_delayed_work_sync(&lan->link_poll);
	writel(readl(lan->mac) & ~BIT(2), lan->mac);
	readl(lan->mac);
	netif_carrier_off(dev);
	usleep_range(2000, 3000);
	ret = read_poll_timeout(zx_ownership_idle, idle, idle, 100, 20000,
				false, lan);
	if (!ret) {
		writel(readl(lan->mac) & ~BIT(3), lan->mac);
		readl(lan->mac);
	} else {
		netdev_warn(dev, "ownership pending; retaining TX enable and reserved pool\n");
	}
	netdev_info(dev, "stopped: consumer=%u TX reused=%u BMU free=%u/%u alloc=%u release=%u\n",
		    lan->consumer[0], lan->tx_reused, readl(lan->bmu + 0x80),
		    readl(lan->bmu + 0x84), readl(lan->bmu + 0x88),
		    readl(lan->bmu + 0x8c));
	return 0;
}

static void zx_engine_start(struct zx_lan *lan)
{
	napi_enable(&lan->napi);
	WRITE_ONCE(lan->engine_active, true);
	if (lan->irq > 0) {
		zx279125_pp_update_bits(lan->intc + 0x44, BIT(2), 0);
		writel(readl(lan->pp + 0x44) & ~0x7, lan->pp + 0x44);
	}
}

static void zx_engine_stop(struct zx_lan *lan)
{
	unsigned long flags;

	if (!lan->engine_active)
		return;
	spin_lock_irqsave(&lan->lock, flags);
	lan->engine_active = false;
	writel(readl(lan->pp + 0x44) | 7, lan->pp + 0x44);
	zx279125_pp_update_bits(lan->intc + 0x44, BIT(2), BIT(2));
	spin_unlock_irqrestore(&lan->lock, flags);
	if (lan->irq > 0)
		synchronize_irq(lan->irq);
	napi_disable(&lan->napi);
}

static int zx_mii_read(struct net_device *dev, int phy, int reg)
{
	struct zx_lan *lan = netdev_priv(dev);

	return mdiobus_read(lan->bus, phy, reg);
}

static int zx_get_link_ksettings(struct net_device *dev,
				 struct ethtool_link_ksettings *cmd)
{
	struct zx_lan *lan = netdev_priv(dev);
	struct mii_if_info mii = {
		.dev = dev, .phy_id = lan->phy_addr, .phy_id_mask = 31,
		.reg_num_mask = 31, .supports_gmii = true, .mdio_read = zx_mii_read,
	};

	mii_ethtool_get_link_ksettings(&mii, cmd);
	return 0;
}

static void zx_get_drvinfo(struct net_device *dev, struct ethtool_drvinfo *info)
{
	strscpy(info->driver, "zx279125-ethernet", sizeof(info->driver));
	strscpy(info->bus_info, dev_name(dev->dev.parent), sizeof(info->bus_info));
}

static const char zx_stat_names[][ETH_GSTRING_LEN] = {
	"bmu_free_external", "bmu_free_internal", "bmu_allocations", "bmu_releases",
	"tx_mailbox_busy", "rx_release_busy", "bmu_request", "bmu_return_busy",
	"tx_buffer_reuse", "engine_fault", "engine_active", "dma_control",
	"pp_aux_base",
};

static int zx_get_sset_count(struct net_device *dev, int sset)
{
	return sset == ETH_SS_STATS ? ARRAY_SIZE(zx_stat_names) + 2 * ZX_QUEUES :
		-EOPNOTSUPP;
}

static void zx_get_strings(struct net_device *dev, u32 sset, u8 *data)
{
	unsigned int q;

	if (sset != ETH_SS_STATS)
		return;
	memcpy(data, zx_stat_names, sizeof(zx_stat_names));
	data += sizeof(zx_stat_names);
	for (q = 0; q < ZX_QUEUES; q++) {
		snprintf(data, ETH_GSTRING_LEN, "rx_queue%u_pending", q);
		data += ETH_GSTRING_LEN;
		snprintf(data, ETH_GSTRING_LEN, "rx_queue%u_consumer", q);
		data += ETH_GSTRING_LEN;
	}
}

static void zx_get_ethtool_stats(struct net_device *dev,
				 struct ethtool_stats *stats, u64 *data)
{
	struct zx_lan *lan = netdev_priv(dev);
	unsigned long flags;
	unsigned int q;

	spin_lock_irqsave(&lan->lock, flags);
	*data++ = readl(lan->bmu + 0x80);
	*data++ = readl(lan->bmu + 0x84);
	*data++ = readl(lan->bmu + 0x88);
	*data++ = readl(lan->bmu + 0x8c);
	*data++ = readl(lan->mailbox + 0x50) & 1;
	*data++ = readl(lan->mailbox + 0x64) & 1;
	*data++ = readl(lan->bmu + 0x10);
	*data++ = !!(readl(lan->bmu + 0xc0) & 8);
	*data++ = lan->tx_reused;
	*data++ = lan->fault;
	*data++ = lan->engine_active;
	*data++ = readl(lan->dma);
	*data++ = readl(lan->pp + 0x34);
	for (q = 0; q < ZX_QUEUES; q++) {
		*data++ = zx_rx_count(lan, q);
		*data++ = lan->consumer[q];
	}
	spin_unlock_irqrestore(&lan->lock, flags);
}

static const struct ethtool_ops zx_ethtool_ops = {
	.get_drvinfo = zx_get_drvinfo,
	.get_link = ethtool_op_get_link,
	.get_link_ksettings = zx_get_link_ksettings,
	.get_sset_count = zx_get_sset_count,
	.get_strings = zx_get_strings,
	.get_ethtool_stats = zx_get_ethtool_stats,
};

static const struct net_device_ops zx_ops = {
	.ndo_open = zx_open,
	.ndo_stop = zx_stop,
	.ndo_start_xmit = zx_xmit,
	.ndo_validate_addr = eth_validate_addr,
	.ndo_set_mac_address = eth_mac_addr,
};

static void zx_unmap(struct zx_lan *lan)
{
	if (lan->irq > 0)
		free_irq(lan->irq, lan);
	if (lan->bus)
		put_device(&lan->bus->dev);
	if (lan->buffers)
		iounmap(lan->buffers);
	if (lan->rings)
		iounmap(lan->rings);
	if (lan->cursors)
		iounmap(lan->cursors);
}

/* Only the reset fingerprint is accepted; this is never a live DMA takeover. */
static int zx_cold_start(struct zx_lan *lan, struct platform_device *pdev)
{
	enum { CLA, BRIDGE, SADM, SWGLB, SIP, MAC0, RESET, BPT, NUM_REGS };
	/* Only needed from reset; some ranges are shared, so they are not claimed. */
	static const char * const names[BPT] = {
		"cla", "bridge", "sadm", "swglb", "sip", "mac0", "reset",
	};
	static const struct {
		u16 offset;
		u32 value;
	} bridge[] = {
		{ 0x004, 0x040200ff }, { 0x340, 0xff5555ff },
		{ 0x344, 0x3e }, { 0x380, 0x3f }, { 0x63c, 0xaaaaaaaa },
		{ 0x1c0, 0xff }, { 0x1c4, 0x5555 }, { 0x188, 0xbf874 },
		{ 0x2c0, 0xff }, { 0x300, 0xffff }, { 0x304, 0x3e },
	};
	void __iomem *reg[NUM_REGS] = {};
	u32 val;
	unsigned int q;
	int ret = -EINVAL;

	for (q = 0; q < BPT; q++) {
		struct resource *res;

		res = platform_get_resource_byname(pdev, IORESOURCE_MEM, names[q]);
		if (!res) {
			ret = -ENODEV;
			goto out;
		}
		reg[q] = ioremap(res->start, resource_size(res));
		if (!reg[q]) {
			ret = -ENOMEM;
			goto out;
		}
	}
	reg[BPT] = ioremap_wc(lan->pool + ZX_POOL_BPT, ZX_BUFFERS * 2);
	if (!reg[BPT]) {
		ret = -ENOMEM;
		goto out;
	}
	if (readl(reg[RESET]) != 0xfeffffff || readl(reg[MAC0]) ||
	    readl(lan->mac) || readl(lan->bmu) ||
	    readl(lan->dma) != 0x136564 ||
	    readl(lan->pp + 0x18) != 0x50000000 ||
	    readl(lan->pp + 0x1c) != 0x50020000 ||
	    readl(lan->pp + 0x20) != 0x52020000 ||
	    readl(lan->pp + 0x24) != ZX_PP_COUNT ||
	    readl(lan->bmu + 0x80) || readl(lan->bmu + 0x84) ||
	    readl(lan->bmu + 0x88) || readl(lan->bmu + 0x8c))
		goto out;
	for (q = 0; q < 16; q++)
		if (zx_rx_count(lan, q))
			goto out;
	pr_info("zx279125-lan: verified reset state, starting local tables without shared reset\n");
	writel(0xfffff, lan->pp + 0x48);
	usleep_range(1000, 2000);
	ret = readl_poll_timeout(lan->pp + 0x4c, val,
				 (val & 0xfffff) == 0xfffff, 10, 100000);
	if (ret)
		goto out;
	for (q = 0; q < 0x111; q++) {
		ret = readl_poll_timeout(lan->mailbox + 0x18, val, val & 1,
					 10, 10000);
		if (ret)
			goto out;
		writel(q, lan->mailbox + 0x14);
		writel(0, lan->mailbox + 0x28);
		writel(0, lan->mailbox + 0x24);
		writel(0, lan->mailbox + 0x20);
		writel(0x800400, lan->mailbox + 0x1c);
	}
	ret = readl_poll_timeout(lan->mailbox + 0x18, val, val & 1, 10, 10000);
	if (ret)
		goto out;
	writel(0, lan->mailbox + 0x4c);
	writel((readl(lan->dma) & ~0xf0000) | 0xf0000, lan->dma);
	writel(0x309a, reg[CLA] + 0x40);
	writel(0, reg[CLA] + 0x48);
	writel(0, lan->bmu + 0x50);
	writel(0x100c040, lan->bmu + 4);
	writel(0x7f, lan->bmu + 0x54);
	for (q = 0; q < ZX_BUFFERS; q++)
		iowrite16be(q, reg[BPT] + q * 2);
	memset_io(lan->rings, 0, ZX_RINGS_SIZE);
	for (q = 0; q < 16; q++) {
		writel(lan->pool + ZX_POOL_RINGS + q * 0x10000, lan->cursors + q * 8);
		writel(0, lan->cursors + q * 8 + 4);
	}
	/* Initialize all pool/ring memory before exposing its physical bases. */
	wmb();
	writel(lan->pool, lan->pp + 0x20);
	writel(lan->pool + ZX_POOL_BPT, lan->pp + 0x18);
	writel(0x10000000, lan->bmu + 0x48);
	writel(0, lan->bmu + 0x10);
	writel(lan->pool + ZX_POOL_RINGS, lan->pp + 0x1c);
	writel(lan->pool + ZX_POOL_AUX, lan->pp + 0x34);
	writel(~0U, reg[SWGLB] + 8);
	usleep_range(1000, 2000);
	ret = readl_poll_timeout(reg[SWGLB] + 0x0c, val, val == ~0U, 10, 100000);
	if (ret)
		goto out;
	for (q = 0; q < 7; q++)
		writel(1, lan->spa + 0x40 + q * 4);
	/*
	 * Retain the tested base identification mask. zx_local_delivery adds
	 * the management-DMAC and IPv6 control identification bits afterward.
	 */
	writel(0x1100200, lan->spa + 0x11c);
	for (q = 0; q < ARRAY_SIZE(bridge); q++)
		writel(bridge[q].value, reg[BRIDGE] + bridge[q].offset);
	for (q = 0; q < 64; q++) {
		ret = readl_poll_timeout(reg[SADM] + 0x18, val, val & 1, 10, 10000);
		if (ret)
			goto out;
		writel(0x800000 | q, reg[SADM] + 0x14);
		writel(0, reg[SADM] + 0x20);
		writel(0x80, reg[SADM] + 0x1c);
	}
	ret = readl_poll_timeout(reg[SADM] + 0x18, val, val & 1, 10, 10000);
	if (ret)
		goto out;
	writel(0x1000, reg[SADM] + 0x44);
	writel(0x1000, reg[SADM] + 0x40);
	writel(1, reg[SIP]);
	writel(0, reg[SWGLB] + 0x50);
	writel(0x11, lan->global + 0x48);
	writel(0x80000001, lan->mac + 0x10);
	writel(0x300000, lan->mac + 0x60);
	writel(0x21000c0, lan->mac + 0x4060);
	writel(0x1000, lan->mac + 0x300);
	writel(0x20e0880, lan->mac);
	writel(1, lan->bmu);
	ret = readl_poll_timeout(lan->bmu + 0x80, val,
				 val + readl(lan->bmu + 0x84) == ZX_BUFFERS,
				 10, 100000);
	if (!ret)
		pr_info("zx279125-lan: local PP/switch tables and LAN MAC initialized\n");
out:
	if (ret)
		pr_err("zx279125-lan: cold initialization stopped (%d); pool remains reserved\n",
		       ret);
	for (q = 0; q < NUM_REGS; q++)
		if (reg[q])
			iounmap(reg[q]);
	return ret;
}

static int zx_local_delivery(struct zx_lan *lan, const u8 *addr)
{
	u32 low = get_unaligned_be32(addr + 2);
	u32 high = get_unaligned_be16(addr);
	u32 identify = readl(lan->spa + 0x11c);

	/* Disable the dedicated match while replacing its two address words. */
	writel(identify & ~BIT(8), lan->spa + 0x11c);
	writel(low, lan->global);
	writel(high, lan->global + 4);
	writel(low, lan->global + 0x40);
	writel(high, lan->global + 0x44);
	/* Bit 8 selects the management DMAC; bit 16 identifies IPv6 control. */
	identify |= BIT(8) | BIT(16);
	writel(identify, lan->spa + 0x11c);
	/* Preserve the action banks; subscriber forwarding is not validated here. */
	return readl(lan->global) == low && readl(lan->global + 4) == high &&
	       readl(lan->global + 0x40) == low &&
	       readl(lan->global + 0x44) == high &&
	       readl(lan->spa + 0x11c) == identify ? 0 : -EIO;
}

static void zx_address_work(struct work_struct *work)
{
	struct zx_lan *lan = container_of(work, struct zx_lan, address_work);
	struct net_device *dev = lan->dev, *master;
	int ret;

	rtnl_lock();
	master = netdev_master_upper_dev_get(dev);
	if (master)
		dev = master;
	if (!ether_addr_equal(dev->dev_addr, lan->management_mac)) {
		ret = zx_local_delivery(lan, dev->dev_addr);
		if (ret)
			netdev_err(lan->dev, "management classifier update failed: %d\n", ret);
		else
			ether_addr_copy(lan->management_mac, dev->dev_addr);
	}
	rtnl_unlock();
}

static int zx_netdev_event(struct notifier_block *nb, unsigned long event,
			   void *ptr)
{
	struct zx_lan *lan = container_of(nb, struct zx_lan, netdev_nb);

	switch (event) {
	case NETDEV_CHANGEADDR:
	case NETDEV_CHANGEUPPER:
	case NETDEV_UP:
	case NETDEV_DOWN:
		schedule_work(&lan->address_work);
		break;
	}
	return NOTIFY_DONE;
}

/* Resolve memory-region @index, which must be a whole no-map reservation. */
static int zx_reserved_region(struct device_node *np, int index, struct resource *res)
{
	struct device_node *node = of_parse_phandle(np, "memory-region", index);
	struct reserved_mem *rmem;
	int ret;

	if (!node)
		return -EPERM;
	ret = of_address_to_resource(node, 0, res);
	rmem = of_reserved_mem_lookup(node);
	if (!ret && (!rmem || rmem->base != res->start ||
		     rmem->size != resource_size(res) ||
		     !of_property_read_bool(node, "no-map")))
		ret = -EPERM;
	of_node_put(node);
	return ret;
}

static int zx_probe(struct platform_device *pdev)
{
	struct device_node *node;
	struct resource res, *shared;
	struct device *parent = &pdev->dev;
	struct net_device *dev;
	struct zx_lan *lan;
	u8 addr[ETH_ALEN];
	phys_addr_t pool;
	u32 cursor_phys;
	bool cold_init;
	unsigned int q;
	int ret;

	ret = of_property_read_u32(parent->of_node, "zte,bootloader-cursors", &cursor_phys);
	if (ret || !IS_ALIGNED(cursor_phys, 4))
		return -EPERM;
	if (zx_reserved_region(parent->of_node, 0, &res) ||
	    resource_size(&res) != ZX_POOL_SIZE || !IS_ALIGNED(res.start, SZ_64K) ||
	    upper_32_bits(res.end))
		return -EPERM;
	pool = res.start;
	/* Accept the source monitor's table or a reserved relocated stock table. */
	if (cursor_phys != pool + ZX_POOL_CURSORS &&
	    (zx_reserved_region(parent->of_node, 1, &res) ||
	     cursor_phys < res.start || cursor_phys > res.end - 127))
		return -EPERM;
	ret = of_get_mac_address(parent->of_node, addr);
	if (ret == -EPROBE_DEFER)
		return ret;
	if (ret)
		ret = zx279125_tagparam_lan_mac(addr);
	if (ret)
		return dev_err_probe(parent, ret, "no valid factory LAN MAC\n");
	dev = alloc_etherdev(sizeof(*lan));
	if (!dev)
		return -ENOMEM;
	lan = netdev_priv(dev);
	lan->dev = dev;
	lan->pool = pool;
	lan->irq = -1;
	SET_NETDEV_DEV(dev, parent);
	spin_lock_init(&lan->lock);
	netif_napi_add(dev, &lan->napi, zx_napi_poll);
	INIT_DELAYED_WORK(&lan->link_poll, zx_link_poll);
	INIT_WORK(&lan->address_work, zx_address_work);
	lan->netdev_nb.notifier_call = zx_netdev_event;
	{
		const char * const names[] = { "pp", "bmu", "mailbox", "mac", "dma", "global", "spa" };
		void __iomem **bases[] = { &lan->pp, &lan->bmu, &lan->mailbox,
					 &lan->mac, &lan->dma, &lan->global, &lan->spa };

		for (q = 0; q < ARRAY_SIZE(names); q++) {
			*bases[q] = devm_platform_ioremap_resource_byname(pdev, names[q]);
			if (IS_ERR(*bases[q])) {
				ret = PTR_ERR(*bases[q]);
				goto fail;
			}
		}
	}
	/* Shared with the optical MAC; mask writes use the common PP lock. */
	shared = platform_get_resource_byname(pdev, IORESOURCE_MEM, "intc");
	if (!shared) {
		ret = -EINVAL;
		goto fail;
	}
	lan->intc = devm_ioremap(parent, shared->start, resource_size(shared));
	lan->cursors = ioremap_wc(cursor_phys, 128);
	lan->local_cursors = cursor_phys == pool + ZX_POOL_CURSORS;
	lan->rings = ioremap_wc(pool + ZX_POOL_RINGS, ZX_RINGS_SIZE);
	lan->buffers = ioremap_wc(pool, ZX_BUFFERS * ZX_BUFFER_SIZE);
	if (!lan->pp || !lan->bmu || !lan->mailbox || !lan->mac ||
	    !lan->dma || !lan->intc || !lan->cursors || !lan->rings || !lan->buffers) {
		ret = -ENOMEM;
		goto fail;
	}
	cold_init = readl(lan->pp + 0x18) == 0x50000000;
	if (cold_init) {
		ret = zx_cold_start(lan, pdev);
		if (ret)
			goto fail;
	}
	ret = -EINVAL;
	if (readl(lan->pp + 0x18) != pool + ZX_POOL_BPT ||
	    readl(lan->pp + 0x20) != pool ||
	    readl(lan->pp + 0x1c) != pool + ZX_POOL_RINGS ||
	    readl(lan->pp + 0x24) != ZX_PP_COUNT || readl(lan->bmu) != 1 ||
	    !zx_ownership_idle(lan) ||
	    (readl(lan->mac) & BIT(2)))
		goto fail;
	for (q = 0; q < 16; q++) {
		/* Empty queues do not imply a zero hardware producer index. */
		if (readl(lan->cursors + q * 8) != pool + ZX_POOL_RINGS + q * 0x10000 ||
		    readl(lan->cursors + q * 8 + 4) >= ZX_RING_SIZE ||
		    zx_rx_count(lan, q)) {
			dev_err(parent, "queue %u has no valid idle bootloader handoff\n", q);
			goto fail;
		}
	}
	node = of_parse_phandle(parent->of_node, "phy-handle", 0);
	if (!node) {
		ret = -EINVAL;
		goto fail;
	}
	ret = of_property_read_u32(node, "reg", &lan->phy_addr);
	if (ret || lan->phy_addr > 31) {
		of_node_put(node);
		ret = -EINVAL;
		goto fail;
	}
	{
		struct device_node *bus = of_get_parent(node);

		lan->bus = of_mdio_find_bus(bus);
		of_node_put(bus);
	}
	of_node_put(node);
	if (!lan->bus) {
		ret = -EPROBE_DEFER;
		goto fail;
	}
	ret = zx_local_delivery(lan, addr);
	if (ret)
		goto fail;
	ether_addr_copy(lan->management_mac, addr);
	for (q = 0; q < ZX_QUEUES; q++)
		lan->consumer[q] = readl(lan->cursors + q * 8 + 4);
	strscpy(dev->name, "lan%d", IFNAMSIZ);
	eth_hw_addr_set(dev, addr);
	dev->netdev_ops = &zx_ops;
	dev->ethtool_ops = &zx_ethtool_ops;
	dev->max_mtu = ETH_DATA_LEN;
	netif_carrier_off(dev);

	lan->irq = platform_get_irq(pdev, 0);
	if (lan->irq > 0) {
		ret = request_irq(lan->irq, zx_isr, IRQF_SHARED, "zx279125-lan", lan);
		if (ret) {
			dev_warn(&pdev->dev, "failed to request IRQ %d: %d\n", lan->irq, ret);
			lan->irq = -1;
		}
	}

	if (lan->irq <= 0) {
		ret = -ENXIO;
		goto fail;
	}
	ret = register_netdev(dev);
	if (ret)
		goto fail;
	netdev_info(dev, "registered IRQ %d for PP/GMAC interrupt\n", lan->irq);
	zx_engine_start(lan);
	ret = register_netdevice_notifier(&lan->netdev_nb);
	if (ret)
		goto unregister;
	lan->transport.send_control = zx_send_control;
	lan->transport.priv = lan;
	ret = zx279125_pp_register_transport(&lan->transport);
	if (ret) {
		unregister_netdevice_notifier(&lan->netdev_nb);
		goto unregister;
	}
	schedule_work(&lan->address_work);
	platform_set_drvdata(pdev, dev);
	netdev_info(dev, "LAN (%s), NAPI CPU queues, consumer=%u\n",
		    cold_init ? "cold start" : "adopted", lan->consumer[0]);
	return 0;
unregister:
	cancel_work_sync(&lan->address_work);
	unregister_netdev(dev);
fail:
	zx_engine_stop(lan);
	netif_napi_del(&lan->napi);
	zx_unmap(lan);
	free_netdev(dev);
	return ret;
}

static void zx_remove(struct platform_device *pdev)
{
	struct net_device *dev = platform_get_drvdata(pdev);
	struct zx_lan *lan = netdev_priv(dev);

	zx279125_pp_unregister_transport(&lan->transport);
	unregister_netdevice_notifier(&lan->netdev_nb);
	cancel_work_sync(&lan->address_work);
	unregister_netdev(dev);
	zx_engine_stop(lan);
	netif_napi_del(&lan->napi);
	/* Hardware can only target the permanently reserved physical pool. */
	zx_unmap(lan);
	free_netdev(dev);
}
/*
 * DRAM survives a soft reset. Once no more releases can run, invalidate the
 * source monitor's table so a later monitor never adopts it as live state;
 * the stock loader recreates its own table on every boot.
 */
static void zx_shutdown(struct platform_device *pdev)
{
	struct zx_lan *lan = netdev_priv(platform_get_drvdata(pdev));

	zx_engine_stop(lan);
	if (lan->local_cursors) {
		memset_io(lan->cursors, 0, 128);
		/* Complete the clear before the reset that follows shutdown. */
		wmb();
	}
}

static const struct of_device_id zx_of_match[] = {
	{ .compatible = "zte,zx279125-ethernet" },
	{ }
};
MODULE_DEVICE_TABLE(of, zx_of_match);

static struct platform_driver zx_driver = {
	.probe = zx_probe,
	.remove = zx_remove,
	.shutdown = zx_shutdown,
	.driver = {
		.name = "zx279125-ethernet",
		.of_match_table = zx_of_match,
	},
};
module_platform_driver(zx_driver);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("ZTE Sanechips ZX279125 Ethernet and CPU packet engine");
