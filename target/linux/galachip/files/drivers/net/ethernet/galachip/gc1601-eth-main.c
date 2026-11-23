// SPDX-License-Identifier: GPL-2.0-only
/*
 * GalaChip GC1601 Gigabit Ethernet Driver
 *
 * Copyright (C) 2026 Ifran Ali <khanu.i.i.k.2000@gmail.com>
 */

#include "gc1601-eth.h"

static u32 gc1601_rx_pending(struct gc1601_eth *priv, unsigned int c);

static const char * const gc1601_reg_names[GC1601_NUM_WINDOWS] = {
	[GC1601_GMAC] = "gmac",
	[GC1601_BUFFERS] = "buffers",
	[GC1601_DESCRIPTORS] = "descriptors",
	[GC1601_RX] = "rx",
	[GC1601_TX_RING] = "tx-ring",
	[GC1601_RX_PENDING] = "rx-pending",
	[GC1601_TX_SPACE] = "tx-space",
	[GC1601_SCHEDULER_READY] = "scheduler-ready",
	[GC1601_BUFFER_CONTROL] = "buffer-control",
	[GC1601_BUFFER_ENABLE] = "buffer-enable",
	[GC1601_TX_CONFIG] = "tx-config",
	[GC1601_STATISTICS] = "statistics",
	[GC1601_SCHEDULER] = "scheduler",
	[GC1601_OUTPUT_SCHEDULER] = "output-scheduler",
	[GC1601_QUEUES] = "queues",
};

/* The LAN port's link changed: program the GMAC and start the TX probe. */
void gc1601_lan_link(struct gc1601_eth *priv, bool link, int speed, int duplex)
{
	unsigned long flags;
	u32 val, speed_bits;

	if (link) {
		switch (speed) {
		case SPEED_1000:
			speed_bits = GC_GMAC_CFG_SPEED_1000;
			break;
		case SPEED_100:
			speed_bits = GC_GMAC_CFG_SPEED_100;
			break;
		case SPEED_10:
			speed_bits = GC_GMAC_CFG_SPEED_10;
			break;
		default:
			return;
		}
		priv->speed = speed;
		priv->duplex = duplex;

		val = GC_GMAC_CFG_BASE | speed_bits;
		if (duplex == DUPLEX_FULL)
			val |= GC_GMAC_CFG_FULL_DUPLEX;

		spin_lock_irqsave(&priv->irq_lock, flags);
		if (!priv->uni_disabled)
			val |= GC_GMAC_CFG_TX_EN | GC_GMAC_CFG_RX_EN;
		writel(val, priv->base[GC1601_GMAC] + GC_GMAC_CFG);
		writel(0x0000fffe, priv->base[GC1601_GMAC] + GC_GMAC_IER);
		writel(0x80000001, priv->base[GC1601_GMAC] + GC_GMAC_MACCR);
		spin_unlock_irqrestore(&priv->irq_lock, flags);
		gc1601_flow_ctrl_apply(priv);
	} else if (priv->link) {
		spin_lock_irqsave(&priv->irq_lock, flags);
		writel(GC_GMAC_CFG_BASE, priv->base[GC1601_GMAC] + GC_GMAC_CFG);
		spin_unlock_irqrestore(&priv->irq_lock, flags);
		gc1601_flow_ctrl_apply(priv);
	}

	if (link == priv->link)
		return;
	spin_lock_irqsave(&priv->tx_lock, flags);
	priv->link = link;
	/* Reset probe failure on link restore so cable replug retries handoff */
	if (priv->link && !priv->tx_probe_aligned) {
		priv->tx_probe_failed = false;
		priv->tx_probe_retries = 0;
		if (!priv->tx_stopping && !priv->tx_pending)
			schedule_work(&priv->tx_probe_work);
	}
	spin_unlock_irqrestore(&priv->tx_lock, flags);
}

/* Without a DSA switch, eth0 is the LAN port and owns the PHY. */
static void gc1601_adjust_link(struct net_device *netdev)
{
	struct gc1601_eth *priv = netdev_priv(netdev);
	struct phy_device *phydev = netdev->phydev;
	bool change = phydev->link != priv->link ||
		      (phydev->link && (priv->speed != phydev->speed ||
					priv->duplex != phydev->duplex));

	gc1601_lan_link(priv, phydev->link, phydev->speed, phydev->duplex);
	if (change)
		phy_print_status(phydev);
}

struct phy_device *gc1601_lan_phy(struct gc1601_eth *priv)
{
	return priv->dsa_lan ? priv->lan_phy : priv->netdev->phydev;
}

/*
 * Frames that arrived after the loader stopped polling are still counted in
 * their container. Clearing the rings under them leaves counts without a
 * descriptor, which keep the level IRQ asserted. Acknowledge and release them
 * first, as the receive path would have.
 */
/* ACK a container's count down while the count follows; stop when it does not. */
static void gc1601_rx_ack_phantom(struct gc1601_eth *priv, int c)
{
	u32 pending = gc1601_rx_pending(priv, c), now;

	while (pending) {
		writel((c & 7) | 0x10 | (c >= 8 ? BIT(14) : 0),
		       priv->base[GC1601_DESCRIPTORS] + GC_DESC_ACK_STATUS);
		writel(1, priv->base[GC1601_DESCRIPTORS] + GC_DESC_ACK_COUNT);
		now = gc1601_rx_pending(priv, c);
		if (now >= pending)
			break;
		pending = now;
		priv->rx_phantom_acks++;
	}
}

/* A fresh free list already holds every ID, so release only into a live one. */
void gc1601_rx_drain_inherited(struct gc1601_eth *priv, bool release)
{
	struct gc1601_desc *live = priv->rx_ring;
	unsigned int c, s, drained = 0;

	/* The first open finds the loader's cached ring still installed. */
	if (readl(priv->base[GC1601_BUFFERS] + GC_BUF_RX_RING_BASE) != priv->rx_ring_dma) {
		live = phys_to_virt(GC1601_RX_RING_PHYS);
		dma_sync_single_for_cpu(priv->dev, GC1601_RX_RING_PHYS,
					GC1601_RX_CONTAINERS * GC1601_RX_RING_STRIDE,
					DMA_BIDIRECTIONAL);
	}
	for (c = 0; c < GC1601_RX_CONTAINERS; c++) {
		struct gc1601_desc *ring = (void *)live + c * GC1601_RX_RING_STRIDE;
		u32 val = readl(priv->base[GC1601_RX_PENDING] + 4 * (c & 7));
		u32 pending = c < 8 ? val & 0xffff : val >> 16;

		for (s = 0; pending && s < GC1601_RING_SIZE; s++) {
			u32 word0 = le32_to_cpu(ring[s].word[0]);
			u32 word1 = le32_to_cpu(ring[s].word[1]);
			u32 buf_id = le32_to_cpu(ring[s].word[2]) >> 17;
			u32 strobe = (le32_to_cpu(ring[s].word[3]) >> 16) & 1;

			if (!(word0 & 0x3fff))
				continue;
			writel((c & 7) | (strobe << 3) | 0x10 | ((word1 & BIT(12)) << 2),
			       priv->base[GC1601_DESCRIPTORS] + GC_DESC_ACK_STATUS);
			writel(1, priv->base[GC1601_DESCRIPTORS] + GC_DESC_ACK_COUNT);
			if (release && buf_id < GC1601_BUFFER_COUNT)
				writel(buf_id, priv->base[GC1601_RX] + GC_RX_RELEASE_BUF);
			pending--;
			drained++;
		}
	}
	if (drained)
		dev_dbg(priv->dev, "dropped %u frames left over from the loader\n", drained);

	/*
	 * What is still counted has no descriptor left: frames the hardware
	 * counted and never wrote. Left alone, the count holds the level IRQ up
	 * until the poll path trims it, a few "Stuck empty" storms after boot.
	 */
	for (c = 0; c < GC1601_RX_CONTAINERS; c++)
		gc1601_rx_ack_phantom(priv, c);
}

static void gc1601_log_handoff(struct gc1601_eth *priv, const char *stage)
{
	void __iomem *pm = priv->base[GC1601_BUFFERS];
	void __iomem *tx = priv->base[GC1601_TX_CONFIG];

	dev_dbg(priv->dev,
		 "handoff %s: gmac=%08x/%08x/%08x pm=%08x/%08x/%08x/%08x/%08x mtu=%08x ctrl=%08x\n",
		 stage, readl(priv->base[GC1601_GMAC] + GC_GMAC_CFG),
		 readl(priv->base[GC1601_GMAC] + GC_GMAC_IER),
		 readl(priv->base[GC1601_GMAC] + GC_GMAC_MACCR),
		 readl(pm + GC_BUF_READ_CLEAR), readl(pm + GC_BUF_CTRL),
		 readl(pm + GC_BUF_FREE_LIST_BASE),
		 readl(pm + GC_BUF_PKT_POOL_BASE), readl(pm + GC_BUF_POOL_CFG),
		 readl(pm + GC_BUF_MTU), readl(pm + GC_BUF_PM_CTRL));
	dev_dbg(priv->dev,
		 "handoff %s: rx=%08x/%08x/%08x tx=%08x/%08x/%08x/%08x/%08x sched=%08x switch=%08x/%08x\n",
		 stage, readl(priv->base[GC1601_RX] + GC_RX_ENABLE),
		 readl(priv->base[GC1601_RX] + GC_RX_PARTITION),
		 readl(priv->base[GC1601_RX] + GC_RX_CTRL),
		 readl(tx), readl(tx + 0x08), readl(tx + 0x0c),
		 readl(tx + 0x10), readl(tx + 0x14),
		 priv->sched_base ? readl(priv->sched_base) : 0,
		 priv->switch_base ? readl(priv->switch_base + 0x60) : 0,
		 priv->switch_base ? readl(priv->switch_base + 0x4198) : 0);
}

/* Pipelining needs a measured hardware ring position; until then one in flight. */
static u32 gc1601_tx_depth(struct gc1601_eth *priv)
{
	if (!priv->tx_probe_aligned)
		return 1;
	return GC1601_TX_DEPTH;
}

struct net_device *gc1601_pon_dev;	/* stopped while the shared ring is full */

static void gc1601_tx_backpressure(struct gc1601_eth *priv)
{
	struct net_device *pon_dev = READ_ONCE(gc1601_pon_dev);

	priv->tx_busy_events++;
	netif_stop_queue(priv->netdev);
	if (pon_dev)
		netif_stop_queue(pon_dev);
	/* Pool exhaustion need not leave a CPU TX completion to wake us. */
	timer_reduce(&priv->poll_timer, jiffies + 1);
}

static void gc1601_tx_probe_schedule_locked(struct gc1601_eth *priv)
{
	lockdep_assert_held(&priv->tx_lock);
	if (!priv->tx_stopping && !priv->tx_probe_failed && priv->running && priv->link &&
	    netif_running(priv->netdev) && !priv->tx_probe_aligned &&
	    !priv->tx_pending && !priv->tx_auto_scheduled && !priv->tx_probe_running) {
		priv->tx_auto_scheduled = true;
		schedule_delayed_work(&priv->tx_auto_work, msecs_to_jiffies(20));
	}
}

static u32 gc1601_tx_complete_locked(struct gc1601_eth *priv)
{
	struct net_device *pon_dev;
	u32 count, qdepth;
	unsigned int i;

	lockdep_assert_held(&priv->tx_lock);
	count = readl(priv->base[GC1601_TX_RING] + GC_TX_COMPLETIONS) & 0xffff;
	priv->tx_completion_reads++;
	priv->tx_completion_last = count;
	priv->tx_completed_total += count;
	if (count > priv->tx_pending)
		priv->tx_completion_excess++;

	if (count > 0 && priv->tx_replicated && priv->tx_detected_slot < 0) {
		for (i = 0; i < GC1601_RING_SIZE; i++) {
			u32 w3 = le32_to_cpu(priv->tx_ring[i].word[3]);

			if (w3 != BIT(31)) {
				priv->tx_detected_slot = i;
				priv->tx_detected_w3 = w3;
				break;
			}
		}
	}

	for (i = 0; i < count && priv->tx_pending > 0; i++) {
		u16 cons = priv->tx_consumer;
		struct sk_buff *skb = priv->tx_skb[cons];

		if (skb) {
			dev_consume_skb_any(skb);
			priv->tx_skb[cons] = NULL;
		}
		priv->tx_consumer = (cons + 1) % GC1601_RING_SIZE;
		priv->tx_pending--;
	}

	/* Let a completed probe leave the MAC before continuing or measuring. */
	gc1601_tx_probe_schedule_locked(priv);

	qdepth = gc1601_tx_depth(priv);
	if (!priv->tx_stopping && priv->tx_probe_aligned &&
	    netif_running(priv->netdev) && netif_queue_stopped(priv->netdev) &&
	    priv->tx_pending < qdepth)
		netif_wake_queue(priv->netdev);

	pon_dev = READ_ONCE(gc1601_pon_dev);
	if (!priv->tx_stopping && priv->tx_probe_aligned && pon_dev &&
	    netif_queue_stopped(pon_dev) && priv->tx_pending < qdepth)
		netif_wake_queue(pon_dev);

	return count;
}

static void gc1601_tx_complete(struct gc1601_eth *priv)
{
	unsigned long flags;

	spin_lock_irqsave(&priv->tx_lock, flags);
	gc1601_tx_complete_locked(priv);
	spin_unlock_irqrestore(&priv->tx_lock, flags);
}

/* Upstream frame for the GPON MAC: GEM port, T-CONT instance and queue. */
/*
 * Queue one frame. Without @pon it goes to the GE port. With it, it goes
 * upstream as G-PON needs it: minfo = GEM port, qid/tid =
 * queue and T-CONT instance, out/dir zero and direct set; an OMCI frame also
 * sets the omci bit and is not padded to 64 bytes.
 *
 * Returns 0 when queued, -EBUSY when the ring is full (the skb is kept) and
 * any other error when the frame was dropped (the skb is freed and counted).
 */
int gc1601_xmit(struct sk_buff *skb, struct net_device *netdev,
		const struct gc1601_pon_meta *pon)
{
	struct gc1601_eth *priv = netdev_priv(netdev);
	struct gc1601_desc desc;
	unsigned long flags;
	u32 val, buf_id, qdepth;
	u16 tx_idx;
	void *pkt_virt;
	phys_addr_t pkt_phys;
	unsigned int i;
	bool probe;
	int ret;

	if (skb->len > GC1601_BUFFER_SIZE - 1) {
		netdev->stats.tx_dropped++;
		dev_kfree_skb_any(skb);
		return -EMSGSIZE;
	}
	/* Hardware packet engine requires minimum 64-byte frame size */
	if ((!pon || !pon->omci) && skb_put_padto(skb, ETH_ZLEN + 4)) {
		netdev->stats.tx_dropped++;
		return -ENOMEM;
	}

	spin_lock_irqsave(&priv->tx_lock, flags);
	qdepth = gc1601_tx_depth(priv);

	probe = !priv->tx_probe_aligned;
	if (priv->tx_stopping || priv->tx_probe_failed ||
	    (probe && (!priv->link || priv->tx_probe_sent >= GC1601_SLOT_PROBE_COUNT))) {
		netif_stop_queue(netdev);
		spin_unlock_irqrestore(&priv->tx_lock, flags);
		return -EBUSY;
	}

	/* Reclaim completions if prior transmissions finished */
	if (priv->tx_pending > 0) {
		gc1601_tx_complete_locked(priv);
		if (qdepth == 1) {
			for (i = 0; priv->tx_pending && i < 100; i++) {
				udelay(1);
				gc1601_tx_complete_locked(priv);
			}
		}
	}

	if (priv->tx_pending >= qdepth) {
		gc1601_tx_backpressure(priv);
		spin_unlock_irqrestore(&priv->tx_lock, flags);
		return -EBUSY;
	}

	writel(1, priv->base[GC1601_RX] + GC_RX_ALLOC_REQ);
	ret = readl_poll_timeout_atomic(priv->base[GC1601_RX] + GC_RX_ALLOC_REQ,
					val, !(val & 1), 1, 1000);
	if (ret) {
		spin_unlock_irqrestore(&priv->tx_lock, flags);
		netdev->stats.tx_dropped++;
		dev_kfree_skb_any(skb);
		return -EIO;
	}

	buf_id = readl(priv->base[GC1601_RX] + GC_RX_ALLOC_RESP);
	if (!(buf_id & BIT(31))) {
		priv->tx_buf_wait = true;
		gc1601_tx_backpressure(priv);
		spin_unlock_irqrestore(&priv->tx_lock, flags);
		return -EBUSY;
	}
	priv->tx_buf_wait = false;

	buf_id &= 0x7fffffff;
	if (buf_id >= GC1601_BUFFER_COUNT) {
		spin_unlock_irqrestore(&priv->tx_lock, flags);
		netdev->stats.tx_dropped++;
		dev_kfree_skb_any(skb);
		return -EIO;
	}
	set_bit(buf_id, priv->tx_id_seen);

	pkt_phys = GC1601_PKT_POOL_PHYS + buf_id * GC1601_BUFFER_SIZE;
	pkt_virt = priv->pkt_pool + buf_id * GC1601_BUFFER_SIZE;
	if (probe) {
		static const u8 probe_mac[ETH_ALEN] = { 0x02, 0x47, 0x37, 0, 0, 1 };
		u8 *packet = pkt_virt;

		memset(packet, 0, GC1601_SLOT_PROBE_SIZE);
		eth_broadcast_addr(packet);
		ether_addr_copy(packet + ETH_ALEN, probe_mac);
		put_unaligned_be16(0x88b5, packet + 12);
		memcpy(packet + ETH_HLEN, "G7-SLOT-PROBE", 13);
		packet[32] = priv->tx_probe_sent + 1;
	} else if (skb_copy_bits(skb, 0, pkt_virt, skb->len)) {
		writel(buf_id, priv->base[GC1601_RX] + GC_RX_RELEASE_BUF);
		spin_unlock_irqrestore(&priv->tx_lock, flags);
		netdev->stats.tx_dropped++;
		dev_kfree_skb_any(skb);
		return -EIO;
	}

	/* Clean data cache lines so hardware DMA reads the new packet contents */
	dma_sync_single_for_device(priv->dev, pkt_phys,
				   probe ? GC1601_SLOT_PROBE_SIZE : skb->len,
				   DMA_TO_DEVICE);

	desc.word[0] = cpu_to_le32(skb->len);
	desc.word[1] = cpu_to_le32(0x1200 | (skb->priority & 7));
	desc.word[2] = cpu_to_le32((buf_id << 17) | 0x4000);
	desc.word[3] = cpu_to_le32(BIT(31));
	if (pon) {
		desc.word[0] = cpu_to_le32(skb->len | (pon->gem_port & 0xfff) << 14);
		desc.word[1] = cpu_to_le32((pon->queue & 7) | (pon->tcont & 0x3f) << 3);
		desc.word[2] = cpu_to_le32((buf_id << 17) | 0x4000 |
					   (pon->omci ? BIT(1) : 0));
		desc.word[3] = cpu_to_le32(BIT(28));
	}

	/* Replicate descriptor across all slots when ring is idle to lock onto
	 * whatever inherited read pointer the hardware DMA engine starts at.
	 */
	if (probe) {
		/* Capture length identifies the fetched slot; no writeback is required. */
		for (i = 0; i < GC1601_RING_SIZE; i++) {
			desc.word[0] = cpu_to_le32(64 + i);
			priv->tx_ring[i] = desc;
		}
		priv->tx_probe_sent++;
		priv->tx_replicated = true;
	} else if (!priv->tx_probe_aligned &&
		   qdepth == 1 && priv->tx_pending == 0) {
		for (i = 0; i < GC1601_RING_SIZE; i++)
			priv->tx_ring[i] = desc;
		priv->tx_replicated = true;
	} else {
		priv->tx_ring[priv->tx_producer] = desc;
		priv->tx_replicated = false;
	}
	dma_wmb();

	tx_idx = priv->tx_producer;
	priv->tx_skb[tx_idx] = skb;
	priv->tx_buf_id[tx_idx] = buf_id;
	priv->tx_producer = (tx_idx + 1) % GC1601_RING_SIZE;
	priv->tx_pending++;
	if (priv->tx_pending == 1)
		timer_reduce(&priv->poll_timer, jiffies + 1);

	/* Probes are the only TX before handoff, so octet deltas isolate each one. */
	if (probe)
		priv->tx_probe_octets[priv->tx_probe_sent - 1] =
			readl(priv->base[GC1601_GMAC] + GC_GMAC_TX_OCTETS);
	writel(buf_id, priv->base[GC1601_RX] + GC_RX_PUBLISH_TX);
	writel(1, priv->base[GC1601_TX_RING] + GC_TX_DOORBELL);

	netdev->stats.tx_packets++;
	if (!probe)
		netdev->stats.tx_bytes += skb->len;

	if (priv->tx_pending >= qdepth)
		netif_stop_queue(netdev);

	spin_unlock_irqrestore(&priv->tx_lock, flags);
	return 0;
}

/*
 * A DSA user port's frame: take the tagger's port byte off the end. The
 * byte stays in the buffer, so a frame the ring cannot take yet gets it back.
 */
static int gc1601_dsa_untag(struct sk_buff *skb, u8 *ports)
{
	if (skb->len < ETH_HLEN + GC1601_TAG_LEN || skb_linearize(skb))
		return -EINVAL;
	*ports = skb->data[skb->len - GC1601_TAG_LEN];
	__skb_trim(skb, skb->len - GC1601_TAG_LEN);
	return 0;
}

/* A frame for pon0: the PON driver picks its GEM port, the ring sends it. */
static netdev_tx_t gc1601_dsa_xmit_pon(struct gc1601_eth *priv, struct sk_buff *skb)
{
	struct gc1601_pon_meta meta = {};
	struct sk_buff *frame;
	int ret = -ENODEV;

	/* The PON driver may rewrite the VLAN; a retry needs the frame unchanged. */
	frame = skb_clone(skb, GFP_ATOMIC);
	if (!frame)
		return NETDEV_TX_OK;
	spin_lock(&gc1601_omci_lock);
	if (gc1601_pon_ops)
		ret = gc1601_pon_ops->prepare(gc1601_pon_ctx, &frame, &meta);
	spin_unlock(&gc1601_omci_lock);
	if (ret) {
		dev_kfree_skb_any(frame);
		return NETDEV_TX_OK;
	}
	ret = gc1601_pon_send(priv, frame, &meta);
	if (ret == -EBUSY) {
		dev_kfree_skb_any(frame);
		return NETDEV_TX_BUSY;
	}
	return NETDEV_TX_OK;
}

static netdev_tx_t gc1601_start_xmit(struct sk_buff *skb, struct net_device *netdev)
{
	struct gc1601_eth *priv = netdev_priv(netdev);
	unsigned long flags;
	netdev_tx_t ret;
	u8 ports = 0;

	if (netdev_uses_dsa(netdev) && gc1601_dsa_untag(skb, &ports)) {
		netdev->stats.tx_dropped++;
		dev_kfree_skb_any(skb);
		return NETDEV_TX_OK;
	}

	spin_lock_irqsave(&priv->tx_lock, flags);
	if (!priv->tx_probe_aligned) {
		netif_stop_queue(netdev);
		spin_unlock_irqrestore(&priv->tx_lock, flags);
		ret = NETDEV_TX_BUSY;
		goto busy;
	}
	spin_unlock_irqrestore(&priv->tx_lock, flags);

	switch (ports) {
	case 0:		/* eth0 is the LAN port itself */
	case BIT(GC1601_PORT_LAN):
		ret = gc1601_xmit(skb, netdev, NULL) == -EBUSY ? NETDEV_TX_BUSY : NETDEV_TX_OK;
		break;
	case BIT(GC1601_PORT_PON):
		ret = gc1601_dsa_xmit_pon(priv, skb);
		if (ret == NETDEV_TX_OK)
			dev_consume_skb_any(skb);
		break;
	default:
		netdev->stats.tx_dropped++;
		dev_kfree_skb_any(skb);
		return NETDEV_TX_OK;
	}
busy:
	if (ret == NETDEV_TX_BUSY && ports)
		__skb_put(skb, GC1601_TAG_LEN);
	return ret;
}

/* NAPI context: hand an OMCI frame (RX descriptor bit 65) to the GPON driver. */
static bool gc1601_omci_deliver(struct gc1601_eth *priv, const u8 *data,
				unsigned int len)
{
	bool taken = false;

	spin_lock(&gc1601_omci_lock);
	if (gc1601_omci_rx) {
		gc1601_omci_rx(gc1601_omci_ctx, data, len);
		taken = true;
	}
	spin_unlock(&gc1601_omci_lock);
	if (taken)
		priv->rx_omci++;
	return taken;
}

/*
 * EPON offload bring-up: log the raw RX descriptors of the next N frames
 * from the PON side, to find which field carries the LLID the switch
 * flows would key on (minfo carries the GEM port in G-PON).
 */
u32 gc1601_pon_desc_log;

/* NAPI context: hand a downstream PON data frame to the GPON driver. */
static bool gc1601_pon_deliver(u16 gem_port, const u8 *data, unsigned int len,
			       struct napi_struct *napi, struct gc1601_eth *priv)
{
	bool taken = false;

	spin_lock(&gc1601_omci_lock);
	if (gc1601_pon_ops) {
		struct net_device *pon_dev = READ_ONCE(gc1601_pon_dev);
		struct sk_buff *skb;

		taken = true;
		skb = gc1601_pon_ops->rx(gc1601_pon_ctx, napi, gem_port, data, len);
		if (skb && pon_dev) {
			skb->protocol = eth_type_trans(skb, pon_dev);
			napi_gro_receive(napi, skb);
		} else if (skb) {
			/* pon0 is a DSA port: the frame enters through the conduit. */
			skb_dst_set_noref(skb, &priv->port_dst[GC1601_PORT_PON]->dst);
			skb->protocol = eth_type_trans(skb, priv->netdev);
			napi_gro_receive(napi, skb);
		}
	}
	spin_unlock(&gc1601_omci_lock);
	return taken;
}

/* EAM_check_pending: word c & 7 holds container c below 8, c + 8 above. */
static u32 gc1601_rx_pending(struct gc1601_eth *priv, unsigned int c)
{
	u32 val = readl(priv->base[GC1601_RX_PENDING] + 4 * (c & 7));

	return c < 8 ? val & 0xffff : val >> 16;
}

/*
 * A stalled container counts frames but has no descriptor in its ring. It is
 * left to the poll timer, so its count cannot keep the level IRQ and NAPI
 * spinning.
 */
static bool gc1601_rx_any_pending(struct gc1601_eth *priv)
{
	unsigned int c;

	for (c = 0; c < GC1601_RX_CONTAINERS; c++)
		if (!(priv->rx_stalled & BIT(c)) && gc1601_rx_pending(priv, c))
			return true;
	return false;
}

/* Round-robin over the containers with frames waiting; -1 when all are empty. */
static int gc1601_rx_next_container(struct gc1601_eth *priv)
{
	unsigned int i, c;

	for (i = 0; i < GC1601_RX_CONTAINERS; i++) {
		c = (priv->rx_next_container + i) % GC1601_RX_CONTAINERS;
		if (!(priv->rx_stalled & BIT(c)) && gc1601_rx_pending(priv, c)) {
			priv->rx_next_container = (c + 1) % GC1601_RX_CONTAINERS;
			return c;
		}
	}
	return -1;
}

/* A written descriptor has a length and a nonzero word 3 (600x00c8). */
static bool gc1601_rx_landed(const struct gc1601_desc *desc)
{
	return (le32_to_cpu(desc->word[0]) & 0x3fff) && desc->word[3];
}

/* Re-read a slot the count says is written until its descriptor lands. */
static bool gc1601_rx_wait_desc(struct gc1601_eth *priv, int c, u16 cons)
{
	struct gc1601_desc *ring = (void *)priv->rx_ring + c * GC1601_RX_RING_STRIDE;
	int n;

	for (n = 0; n < 20; n++) {
		udelay(1);
		dma_rmb();
		if (gc1601_rx_landed(&ring[cons]))
			return true;
	}
	return false;
}

static void gc1601_rx_clear_desc(struct gc1601_eth *priv, int c, u16 cons)
{
	struct gc1601_desc *ring = (void *)priv->rx_ring + c * GC1601_RX_RING_STRIDE;

	memset(&ring[cons], 0, sizeof(*ring));
}

/*
 * The ring is zeroed at open, but the hardware carries on from wherever the
 * loader left its write position. The descriptors a container has got since
 * are the only run in its ring, possibly wrapping: start at its head.
 */
static bool gc1601_rx_sync(struct gc1601_eth *priv, int c)
{
	struct gc1601_desc *ring = (void *)priv->rx_ring + c * GC1601_RX_RING_STRIDE;
	int s;

	dma_rmb();
	for (s = 0; s < GC1601_RING_SIZE; s++) {
		u16 prev = (s - 1) & (GC1601_RING_SIZE - 1);

		if ((le32_to_cpu(ring[s].word[0]) & 0x3fff) &&
		    !(le32_to_cpu(ring[prev].word[0]) & 0x3fff)) {
			priv->rx_consumer[c] = s;
			priv->rx_resync_events++;
			dev_dbg(priv->dev, "container %d starts at slot %d\n", c, s);
			return true;
		}
	}
	return false;
}

/*
 * A slot still empty 100 ms on while the next one has landed was never
 * written: step over it rather than block the container for good.
 */
static bool gc1601_rx_skip_hole(struct gc1601_eth *priv, int c, u16 cons)
{
	struct gc1601_desc *ring = (void *)priv->rx_ring + c * GC1601_RX_RING_STRIDE;
	u16 next = (cons + 1) & (GC1601_RING_SIZE - 1);

	if (!priv->rx_late_since[c]) {
		priv->rx_late_since[c] = jiffies | 1;
		return false;
	}
	if (time_before(jiffies, priv->rx_late_since[c] + HZ / 10))
		return false;
	dma_rmb();
	if (!gc1601_rx_landed(&ring[next]))
		return false;
	priv->rx_consumer[c] = next;
	priv->rx_late_since[c] = 0;
	priv->rx_holes++;
	return true;
}

static bool gc1601_rx_ring_empty(struct gc1601_eth *priv, int c)
{
	struct gc1601_desc *ring = (void *)priv->rx_ring + c * GC1601_RX_RING_STRIDE;
	int s;

	for (s = 0; s < GC1601_RING_SIZE; s++)
		if (le32_to_cpu(ring[s].word[0]) & 0x3fff)
			return false;
	return true;
}

/*
 * The hardware counts some frames it never writes a descriptor for: the
 * container count then sits above zero with an empty ring (256 with nothing
 * in it on container 8), holding the level IRQ up and leaving RX to the
 * 100 Hz poll. Once the ring has stayed empty for a tick, ACK the count down
 * one at a time and stop when an ACK does not take.
 */
static void gc1601_rx_trim_phantom(struct gc1601_eth *priv, int c)
{
	if (!gc1601_rx_ring_empty(priv, c)) {
		priv->rx_phantom_since[c] = 0;
		return;
	}
	if (!priv->rx_phantom_since[c]) {
		priv->rx_phantom_since[c] = jiffies | 1;
		return;
	}
	if (!time_after(jiffies, priv->rx_phantom_since[c]))
		return;

	priv->rx_phantom_since[c] = 0;
	gc1601_rx_ack_phantom(priv, c);
}

static void gc1601_rx_stall(struct gc1601_eth *priv, unsigned int container)
{
	priv->rx_stalled |= BIT(container);
	timer_reduce(&priv->poll_timer, jiffies + 1);
}

static int gc1601_rx_poll(struct napi_struct *napi, int budget)
{
	struct gc1601_eth *priv = container_of(napi, struct gc1601_eth, napi);
	int work_done = 0;

	priv->rx_poll_calls++;
	gc1601_tx_complete(priv);

	while (work_done < budget) {
		int container = gc1601_rx_next_container(priv);
		struct gc1601_desc *ring;
		u16 cons;
		struct gc1601_desc *desc;
		struct sk_buff *skb;
		u32 word0, word1, word2, word3;
		u32 len, buf_id, offset, strobe, crcerr;
		phys_addr_t pkt_phys;
		void *pkt_virt;
		bool omci_processed = false;

		if (container < 0)
			break;
		priv->rx_pending_last = gc1601_rx_pending(priv, container);
		ring = (void *)priv->rx_ring + container * GC1601_RX_RING_STRIDE;

		/*
		 * The container count is descriptors written and not yet ACKed,
		 * in ring order from the consumer. Read them in order and never
		 * skip one. The ring is uncached, as on the stock firmware: a
		 * cached ring lost descriptors
		 * to 32-byte line clears and stale lines. A consumed slot is
		 * zeroed, so a zero length means "not written yet".
		 */
		if (!(priv->rx_synced & BIT(container))) {
			if (!gc1601_rx_sync(priv, container)) {
				priv->rx_empty_desc++;
				gc1601_rx_stall(priv, container);
				continue;
			}
			priv->rx_synced |= BIT(container);
		}
		cons = priv->rx_consumer[container];
		dma_rmb();

		desc = &ring[cons];
		if (!gc1601_rx_landed(desc)) {
			if (!gc1601_rx_wait_desc(priv, container, cons)) {
				if (gc1601_rx_skip_hole(priv, container, cons))
					continue;
				gc1601_rx_trim_phantom(priv, container);
				priv->rx_late_stalled++;
				priv->rx_empty_desc++;
				gc1601_rx_stall(priv, container);
				continue;
			}
			priv->rx_late_landed++;
		}
		word0 = le32_to_cpu(desc->word[0]);

		if (priv->rx_first_slot < 0)
			priv->rx_first_slot = cons;

		word1 = le32_to_cpu(desc->word[1]);
		word2 = le32_to_cpu(desc->word[2]);
		word3 = le32_to_cpu(desc->word[3]);
		priv->rx_last_slot = cons;
		priv->rx_last_words[0] = word0;
		priv->rx_last_words[1] = word1;
		priv->rx_last_words[2] = word2;
		priv->rx_last_words[3] = word3;
		if (word2 & BIT(1)) {
			priv->rx_omci_desc++;
			priv->rx_omci_last_words[0] = word0;
			priv->rx_omci_last_words[1] = word1;
			priv->rx_omci_last_words[2] = word2;
			priv->rx_omci_last_words[3] = word3;
		}

		len = word0 & 0x3fff;
		crcerr = (word1 >> 31) & 1;
		buf_id = word2 >> 17;
		offset = (word2 >> 6) & 0x7f;
		strobe = (word3 >> 16) & 1;

		if (buf_id < GC1601_BUFFER_COUNT)
			pkt_phys = GC1601_PKT_POOL_PHYS + buf_id * GC1601_BUFFER_SIZE;
		else
			pkt_phys = 0;

		if (crcerr) {
			priv->rx_crc_errors++;
			priv->netdev->stats.rx_crc_errors++;
			priv->netdev->stats.rx_errors++;
			goto consume;
		}

		if (buf_id >= GC1601_BUFFER_COUNT) {
			priv->rx_invalid_buf_errors++;
			priv->netdev->stats.rx_errors++;
			goto consume;
		}
		set_bit(buf_id, priv->rx_id_seen);

		if (len < ETH_HLEN || len >= GC1601_BUFFER_SIZE ||
		    offset + len > GC1601_BUFFER_SIZE) {
			priv->rx_invalid_len_errors++;
			priv->netdev->stats.rx_length_errors++;
			priv->netdev->stats.rx_errors++;
			goto consume;
		}

		pkt_virt = priv->pkt_pool + buf_id * GC1601_BUFFER_SIZE;
		dma_sync_single_for_cpu(priv->dev, pkt_phys, GC1601_BUFFER_SIZE, DMA_FROM_DEVICE);
		dma_rmb();

		if (word2 & BIT(1)) {
			omci_processed = true;
			if (gc1601_omci_deliver(priv, pkt_virt + offset, len))
				goto consume;
			/*
			 * EPON has no OMCI channel; the engine flags its OAM
			 * frames the same way, so they go to the PON driver.
			 */
			if (FIELD_GET(GC1601_RX_IN, word0) == GC1601_RX_IN_GE) {
				priv->rx_omci_dropped++;
				goto consume;
			}
		}
		/* Only the GE port feeds eth0; PON data goes to the GPON driver. */
		if (FIELD_GET(GC1601_RX_IN, word0) != GC1601_RX_IN_GE) {
			if (unlikely(READ_ONCE(gc1601_pon_desc_log))) {
				WRITE_ONCE(gc1601_pon_desc_log, gc1601_pon_desc_log - 1);
				dev_info(priv->dev, "pon rx desc %08x %08x %08x %08x in %lu minfo %lu len %u type %04x %*phN\n",
					 word0, word1, word2, word3, FIELD_GET(GC1601_RX_IN, word0),
					 FIELD_GET(GC1601_RX_MINFO, word0), len,
					 get_unaligned_be16(pkt_virt + offset + 2 * ETH_ALEN),
					 min_t(int, len - 2 * ETH_ALEN, 8),
					 pkt_virt + offset + 2 * ETH_ALEN);
			}
			if (gc1601_pon_deliver(FIELD_GET(GC1601_RX_MINFO, word0),
					       pkt_virt + offset, len, napi, priv))
				goto consume;
			priv->rx_unrouted++;
			priv->netdev->stats.rx_dropped++;
			goto consume;
		}

		/* napi_alloc_skb() already reserves NET_IP_ALIGN for the IP header. */
		skb = napi_alloc_skb(napi, len);
		if (skb) {
			memcpy(skb_put(skb, len), pkt_virt + offset, len);
			/* DSA takes the ingress port from this, not from the frame. */
			if (netdev_uses_dsa(priv->netdev))
				skb_dst_set_noref(skb, &priv->port_dst[GC1601_PORT_LAN]->dst);
			skb->protocol = eth_type_trans(skb, priv->netdev);
			napi_gro_receive(napi, skb);

			priv->netdev->stats.rx_packets++;
			priv->netdev->stats.rx_bytes += len;
		} else {
			priv->rx_alloc_failures++;
			priv->netdev->stats.rx_dropped++;
		}

consume:
		if ((word2 & BIT(1)) && !omci_processed)
			priv->rx_omci_invalid++;
		/*
		 * 1. Acknowledge descriptor status and count. The ACK names the
		 * bank (container & 7) and copies descriptor byte 5 bit 4,
		 * set for containers 8..15, to ACK bit 14.
		 */
		writel((container & 7) | (strobe << 3) | 0x10 |
		       ((word1 & BIT(12)) << 2),
		       priv->base[GC1601_DESCRIPTORS] + GC_DESC_ACK_STATUS);
		writel(1, priv->base[GC1601_DESCRIPTORS] + GC_DESC_ACK_COUNT);
		priv->rx_ack_count++;

		/* 2. Clear the slot so the next lap finds it empty */
		gc1601_rx_clear_desc(priv, container, cons);

		/* 3. Invalidate CPU cache for the buffer and release back to HW pool */
		if (buf_id < GC1601_BUFFER_COUNT) {
			dma_sync_single_for_device(priv->dev, pkt_phys,
						   GC1601_BUFFER_SIZE,
						   DMA_FROM_DEVICE);
			dma_wmb();
			writel(buf_id, priv->base[GC1601_RX] + GC_RX_RELEASE_BUF);
			priv->rx_release_count++;
		}

		priv->rx_consumer[container] = (cons + 1) % GC1601_RING_SIZE;
		priv->rx_stalled &= ~BIT(container);
		priv->rx_late_since[container] = 0;
		priv->rx_phantom_since[container] = 0;
		work_done++;
	}

	if (work_done < budget) {
		if (napi_complete_done(napi, work_done)) {
			unsigned long flags;

			spin_lock_irqsave(&priv->irq_lock, flags);
			if (priv->running && !priv->irq_poll_only)
				gc1601_irq_mask(priv, true);
			spin_unlock_irqrestore(&priv->irq_lock, flags);
		}
	}

	return work_done;
}

static irqreturn_t gc1601_eth_irq(int irq, void *dev_id)
{
	struct gc1601_eth *priv = dev_id;
	u32 status, pending;
	unsigned long flags;

	spin_lock_irqsave(&priv->irq_lock, flags);
	if (!priv->running) {
		spin_unlock_irqrestore(&priv->irq_lock, flags);
		return IRQ_NONE;
	}
	status = readl(priv->base[GC1601_BUFFERS] + GC_BUF_INTR_STATUS);
	priv->irq_calls++;
	priv->irq_last_status = status;
	if (!(status & (GC_INTR_RX | GC_INTR_TX))) {
		spin_unlock_irqrestore(&priv->irq_lock, flags);
		return IRQ_NONE;
	}
	gc1601_irq_mask(priv, false);
	pending = readl(priv->base[GC1601_RX_PENDING]);
	if (!gc1601_rx_any_pending(priv) && !READ_ONCE(priv->tx_pending)) {
		priv->irq_empty_calls++;
		if (++priv->irq_empty_streak >= 64) {
			priv->irq_storm_pending = pending;
			priv->irq_storm_mask = readl(priv->base[GC1601_BUFFERS] +
						    GC_BUF_INTR_MASK);
			priv->irq_storm_event_mask = readl(priv->base[GC1601_BUFFERS] +
							  GC_BUF_EVENT_MASK);
			priv->irq_storm_consumer = priv->rx_consumer[0];
			priv->irq_storm_jiffies = jiffies;
			/* The PMU mask alone does not always deassert the level IRQ. */
			priv->irq_poll_only = true;
			priv->irq_poll_ticks = 0;
			/* A storm right after a retry is not passing: back off. */
			if (time_before(jiffies, priv->irq_retry_at + HZ))
				priv->irq_retry_ms = min(priv->irq_retry_ms * 2,
							 GC1601_IRQ_RETRY_MAX_MS);
			else
				priv->irq_retry_ms = GC1601_IRQ_RETRY_MS;
			disable_irq_nosync(irq);
			if (net_ratelimit())
				netdev_warn(priv->netdev,
					    "Stuck empty PMU IRQ; polling for a while\n");
			spin_unlock_irqrestore(&priv->irq_lock, flags);
			return IRQ_HANDLED;
		}
	} else {
		priv->irq_empty_streak = 0;
	}
	/* A duplicate level IRQ must not keep NAPI_STATE_MISSED latched. */
	if (test_bit(NAPI_STATE_SCHED, &priv->napi.state))
		priv->irq_duplicate_calls++;
	else if (napi_schedule_prep(&priv->napi))
		__napi_schedule_irqoff(&priv->napi);
	spin_unlock_irqrestore(&priv->irq_lock, flags);

	return IRQ_HANDLED;
}

static void gc1601_poll_timer(struct timer_list *t)
{
	struct gc1601_eth *priv = timer_container_of(priv, t, poll_timer);
	unsigned long flags;
	bool stalled = READ_ONCE(priv->rx_stalled);

	if (!priv->running)
		return;

	/* Give stalled containers another look; their descriptor may have landed. */
	WRITE_ONCE(priv->rx_stalled, 0);

	/*
	 * A storm is usually a descriptor landing late, not a dead container:
	 * polling at HZ caps the data path, so go back to the IRQ after a while.
	 */
	if (priv->irq_poll_only &&
	    ++priv->irq_poll_ticks >= msecs_to_jiffies(priv->irq_retry_ms)) {
		spin_lock_irqsave(&priv->irq_lock, flags);
		priv->irq_poll_only = false;
		priv->irq_empty_streak = 0;
		priv->irq_retry_at = jiffies;
		enable_irq(priv->irq);
		if (!test_bit(NAPI_STATE_SCHED, &priv->napi.state))
			gc1601_irq_mask(priv, true);
		spin_unlock_irqrestore(&priv->irq_lock, flags);
	}
	if (gc1601_rx_any_pending(priv) || READ_ONCE(priv->tx_pending) ||
	    READ_ONCE(priv->tx_buf_wait)) {
		spin_lock_irqsave(&priv->irq_lock, flags);
		if (priv->running &&
		    !test_bit(NAPI_STATE_SCHED, &priv->napi.state) &&
		    napi_schedule_prep(&priv->napi)) {
			gc1601_irq_mask(priv, false);
			__napi_schedule(&priv->napi);
		}
		spin_unlock_irqrestore(&priv->irq_lock, flags);
	}

	if (priv->running)
		timer_reduce(&priv->poll_timer, jiffies +
			  max_t(unsigned long, 1,
				msecs_to_jiffies(priv->irq_poll_only || stalled ||
						 READ_ONCE(priv->rx_stalled) ||
						 READ_ONCE(priv->tx_pending) ||
						 READ_ONCE(priv->tx_buf_wait) ? 1 : 50)));
}

/*
 * The TX handoff probe reads the GMAC byte counter, so it needs a UNI link.
 * Until it runs, nothing the CPU sends reaches the PON either: no EPON OAM
 * or GPON OMCI, and an OLT that wants OAM within ~20 s (HSGQ) deregisters
 * the ONU over and over while the LAN port is empty. With no link some
 * seconds after open, loop the PHY back on itself for the probe instead.
 */
#define GC1601_LOOPBACK_DELAY_S		5
#define GC1601_LOOPBACK_PROBE_MS	2000

static bool gc1601_tx_probe_pending(struct gc1601_eth *priv)
{
	unsigned long flags;
	bool pending;

	spin_lock_irqsave(&priv->tx_lock, flags);
	pending = !priv->tx_stopping && !priv->tx_probe_aligned;
	spin_unlock_irqrestore(&priv->tx_lock, flags);
	return pending;
}

static void gc1601_tx_loopback_work(struct work_struct *work)
{
	struct gc1601_eth *priv = container_of(to_delayed_work(work), struct gc1601_eth,
					       tx_loopback_work);
	struct phy_device *phydev = gc1601_lan_phy(priv);
	unsigned long flags, deadline;
	bool aligned;
	int ret;

	if (!phydev || READ_ONCE(priv->link) || !gc1601_tx_probe_pending(priv))
		return;
	/*
	 * A PHY that never linked has no duplex, and loopback then runs half
	 * duplex: the GMAC takes its own echo for collisions and counts no
	 * bytes. genphy_loopback() encodes phydev->duplex into BMCR.
	 */
	mutex_lock(&phydev->lock);
	phydev->duplex = DUPLEX_FULL;
	mutex_unlock(&phydev->lock);
	ret = phy_loopback(phydev, true, SPEED_1000);
	if (ret) {
		dev_warn(priv->dev, "no LAN link and PHY loopback failed (%d): PON transmit waits for a LAN link\n",
			 ret);
		return;
	}
	/* phy_loopback() signals link up, so the link hook starts the probe. */
	deadline = jiffies + msecs_to_jiffies(GC1601_LOOPBACK_PROBE_MS);
	while (gc1601_tx_probe_pending(priv) && time_before(jiffies, deadline))
		msleep(20);
	phy_loopback(phydev, false, 0);

	spin_lock_irqsave(&priv->tx_lock, flags);
	aligned = priv->tx_probe_aligned;
	/* Leave the probe to the first real link rather than a reopen. */
	if (!aligned && !priv->tx_pending) {
		priv->tx_probe_failed = false;
		priv->tx_probe_retries = 0;
		priv->tx_probe_sent = 0;
		priv->tx_probe_completed_base = priv->tx_completed_total;
		priv->tx_probe_excess_base = priv->tx_completion_excess;
		memset(priv->tx_probe_octets, 0, sizeof(priv->tx_probe_octets));
	}
	spin_unlock_irqrestore(&priv->tx_lock, flags);

	if (aligned)
		dev_dbg(priv->dev, "no LAN link: TX handoff measured over PHY loopback, slot %u\n",
			 priv->tx_resume_slot);
	else
		dev_warn(priv->dev, "no LAN link: TX probe over PHY loopback did not finish (sent %u); PON transmit waits for a LAN link\n",
			 priv->tx_probe_sent);
}

/* The engine has only ever handed out buffer IDs 96-255: 160 usable buffers. */
u32 gc1601_pool_warn = 200;

const u16 gc1601_diag_mac[GC1601_DIAG_MACS] = {
	0x714, 0x718, 0x724, 0x728, 0x72c, 0x730, 0x734, 0x738,
	0x740, 0x744, 0x778, 0x780, 0x784, 0x78c, 0x790, 0x794,
	0x7a4, 0x7a8, 0x7ac, 0x7b0, 0x7b4, 0x7b8, 0x7bc, 0x7c0,
	0x81c,
};

/* Non-zero OUTCNT/INCNT of T-CONTs 0-3, the UNI and the CPU queues. */
static int gc1601_diag_queues(struct gc1601_eth *priv, char *buf, size_t size,
			      u32 *queued)
{
	static const struct { u16 first, count; } ranges[] = {
		{ 0, 32 }, { GC_CVRD_UNI0, GC_CVRD_UNI_QUEUES },
		{ GC_CVRD_CPU, GC_CVRD_CPU_QUEUES },
	};
	unsigned int r, i;
	int len = 0;

	for (r = 0; r < ARRAY_SIZE(ranges); r++) {
		for (i = ranges[r].first; i < ranges[r].first + ranges[r].count; i++) {
			u32 out, in = 0;

			if (gc1601_queue_read(priv, GC_CVRD_OUTCNT, i, &out, 1) ||
			    (i < GC_CVRD_CPU &&
			     gc1601_queue_read(priv, GC_CVRD_INCNT, i, &in, 1)))
				return len + scnprintf(buf + len, size - len, " QFAIL@%x", i);
			*queued += GC_CVRD_HI(out) + GC_CVRD_HI(in);
			if (out || in)
				len += scnprintf(buf + len, size - len, " %x=%x/%x", i, out, in);
		}
	}
	return len;
}

/*
 * Upload crash hunt: while debugfs diag_ms is set, sample the engine that
 * often and print what moved, so the last console lines before a silent
 * reset show the engine state.
 */
static void gc1601_diag_work(struct work_struct *work)
{
	struct gc1601_eth *priv = container_of(to_delayed_work(work),
					       struct gc1601_eth, diag_work);
	void __iomem *gmac = priv->base[GC1601_GMAC];
	char mac[256], queues[512];
	int mac_len = 0, q_len = 0;
	unsigned int i, delay = READ_ONCE(priv->diag_ms);
	u32 queued = 0;

	if (!delay || !priv->running)
		return;

	for (i = 0; i < ARRAY_SIZE(gc1601_diag_mac); i++) {
		u32 val = readl(gmac + gc1601_diag_mac[i]);

		if (val != priv->diag_mac[i])
			mac_len += scnprintf(mac + mac_len, sizeof(mac) - mac_len, " %x+%u",
					     gc1601_diag_mac[i], val - priv->diag_mac[i]);
		priv->diag_mac[i] = val;
	}

	queues[0] = '\0';
	if (rtnl_trylock()) {
		if (!priv->queue_read_failed)
			q_len = gc1601_diag_queues(priv, queues, sizeof(queues),
						   &queued);
		rtnl_unlock();
	}

	if (mac_len || q_len || priv->tx_pending)
		dev_info(priv->dev, "diag %u: queued=%u txp=%u busy=%llu allocf=%u q[%s ] mac[%s ]\n",
			 priv->diag_seq++, queued, priv->tx_pending, priv->tx_busy_events,
			 priv->rx_alloc_failures, queues, mac);
	if (gc1601_pool_warn && queued >= gc1601_pool_warn) {
		dev_warn(priv->dev, "diag: pool low, %u buffers queued\n", queued);
		delay = 20;
	}
	schedule_delayed_work(&priv->diag_work, msecs_to_jiffies(delay));
}

static int gc1601_eth_open(struct net_device *netdev)
{
	struct gc1601_eth *priv = netdev_priv(netdev);
	struct phy_device *phydev;
	int ret;

	/* Under a DSA switch the LAN port owns the PHY; eth0 is the conduit. */
	if (priv->phy_node) {
		phydev = of_phy_connect(netdev, priv->phy_node,
					gc1601_adjust_link, 0,
					priv->phy_interface);
		if (!phydev) {
			dev_err(priv->dev, "Could not connect to PHY\n");
			return -ENODEV;
		}
		phy_support_asym_pause(phydev);
		if (priv->pause_autoneg)
			phy_set_asym_pause(phydev, priv->rx_pause, priv->tx_pause);
	}

	priv->irq_empty_streak = 0;
	priv->irq_poll_only = false;
	priv->irq_retry_ms = GC1601_IRQ_RETRY_MS;
	priv->irq_retry_at = jiffies - HZ;
	ret = request_irq(priv->irq, gc1601_eth_irq, 0, netdev->name, priv);
	if (ret) {
		dev_err(priv->dev, "Failed to request IRQ %d\n", priv->irq);
		goto err_phy_disconnect;
	}

	napi_enable(&priv->napi);
	gc1601_log_handoff(priv, "open");
	ret = gc1601_hw_setup(priv);
	if (ret) {
		napi_disable(&priv->napi);
		free_irq(priv->irq, priv);
		goto err_phy_disconnect;
	}
	gc1601_hw_start(priv);
	netif_stop_queue(netdev);
	if (netdev->phydev)
		phy_start(netdev->phydev);
	else
		netif_carrier_on(netdev);
	schedule_delayed_work(&priv->tx_loopback_work,
			      secs_to_jiffies(GC1601_LOOPBACK_DELAY_S));

	timer_setup(&priv->poll_timer, gc1601_poll_timer, 0);
	mod_timer(&priv->poll_timer, jiffies + msecs_to_jiffies(50));
	if (READ_ONCE(priv->diag_ms))
		schedule_delayed_work(&priv->diag_work, 0);

	return 0;

err_phy_disconnect:
	if (netdev->phydev)
		phy_disconnect(netdev->phydev);
	return ret;
}

static int gc1601_eth_stop(struct net_device *netdev)
{
	struct gc1601_eth *priv = netdev_priv(netdev);
	unsigned long flags;
	unsigned int i;
	u16 pending;

	spin_lock_irqsave(&priv->tx_lock, flags);
	priv->tx_stopping = true;
	spin_unlock_irqrestore(&priv->tx_lock, flags);
	netif_tx_disable(netdev);
	cancel_delayed_work_sync(&priv->tx_loopback_work);
	cancel_work_sync(&priv->tx_probe_work);
	cancel_delayed_work_sync(&priv->tx_auto_work);
	cancel_delayed_work_sync(&priv->diag_work);

	/* Let queued descriptors finish while the GMAC is still transmitting. */
	for (i = 0; i < 100; i++) {
		spin_lock_irqsave(&priv->tx_lock, flags);
		gc1601_tx_complete_locked(priv);
		pending = priv->tx_pending;
		spin_unlock_irqrestore(&priv->tx_lock, flags);
		if (!pending)
			break;
		usleep_range(500, 1000);
	}
	if (pending)
		dev_warn(priv->dev, "%u TX descriptors still pending at stop\n", pending);
	timer_delete_sync(&priv->poll_timer);
	if (netdev->phydev)
		phy_stop(netdev->phydev);
	else
		netif_carrier_off(netdev);
	gc1601_hw_stop(priv);
	synchronize_irq(priv->irq);
	napi_disable(&priv->napi);

	if (priv->irq_poll_only)
		enable_irq(priv->irq);
	free_irq(priv->irq, priv);
	if (netdev->phydev)
		phy_disconnect(netdev->phydev);

	for (i = 0; i < GC1601_RING_SIZE; i++) {
		if (priv->tx_skb[i]) {
			dev_kfree_skb_any(priv->tx_skb[i]);
			priv->tx_skb[i] = NULL;
		}
	}

	return 0;
}

static int gc1601_set_mac_address(struct net_device *netdev, void *p)
{
	struct gc1601_eth *priv = netdev_priv(netdev);
	int ret;

	ret = eth_mac_addr(netdev, p);
	if (ret)
		return ret;

	/* Frames to the new address must keep reaching the CPU. */
	if (priv->sfu_rules)
		gc1601_sfu_cpu_mac(priv);
	if (priv->running)
		gc1601_gmac_pause_addr_apply(priv);
	return 0;
}

static int gc1601_change_mtu(struct net_device *netdev, int new_mtu)
{
	struct gc1601_eth *priv = netdev_priv(netdev);

	WRITE_ONCE(netdev->mtu, new_mtu);
	if (priv->switch_base && priv->sr_ready) {
		u32 frame = new_mtu + VLAN_ETH_HLEN + VLAN_HLEN + ETH_FCS_LEN + 34;

		gc1601_sr_update(priv, GC_SR_HA + 0x2c, 0x3fff,
				 max_t(u32, frame, priv->sr_ha_frame));
	}
	return 0;
}

static const struct net_device_ops gc1601_netdev_ops = {
	.ndo_open		= gc1601_eth_open,
	.ndo_stop		= gc1601_eth_stop,
	.ndo_start_xmit		= gc1601_start_xmit,
	.ndo_set_mac_address	= gc1601_set_mac_address,
	.ndo_validate_addr	= eth_validate_addr,
	.ndo_eth_ioctl		= phy_do_ioctl_running,
	.ndo_change_mtu		= gc1601_change_mtu,
};

static void gc1601_get_pauseparam(struct net_device *netdev,
				  struct ethtool_pauseparam *pause)
{
	struct gc1601_eth *priv = netdev_priv(netdev);

	pause->autoneg = priv->pause_autoneg;
	pause->rx_pause = priv->rx_pause;
	pause->tx_pause = priv->tx_pause;
}

static int gc1601_set_pauseparam(struct net_device *netdev,
				 struct ethtool_pauseparam *pause)
{
	struct gc1601_eth *priv = netdev_priv(netdev);
	struct phy_device *phydev = netdev->phydev;

	priv->pause_autoneg = !!pause->autoneg;
	priv->rx_pause = !!pause->rx_pause;
	priv->tx_pause = !!pause->tx_pause;

	if (phydev)
		phy_set_asym_pause(phydev, priv->rx_pause, priv->tx_pause);
	if (priv->running)
		gc1601_flow_ctrl_apply(priv);
	return 0;
}

static void gc1601_get_drvinfo(struct net_device *netdev, struct ethtool_drvinfo *info)
{
	strscpy(info->driver, "gc1601-eth", sizeof(info->driver));
}

static const char gc1601_ethtool_stats_keys[][ETH_GSTRING_LEN] = {
	"rx_crc_errors",
	"rx_invalid_len_errors",
	"rx_invalid_buf_errors",
	"rx_resync_events",
	"rx_alloc_failures",
	"rx_late_landed",
	"rx_late_stalled",
	"rx_holes",
	"rx_phantom_acks",
	"tx_busy_events",
	"tx_completion_excess",
	"omci_rx",
	"omci_rx_dropped",
	"omci_rx_descriptors",
	"omci_rx_invalid",
	"omci_tx",
};

static void gc1601_get_ethtool_stats(struct net_device *netdev,
				     struct ethtool_stats *stats, u64 *data)
{
	struct gc1601_eth *priv = netdev_priv(netdev);

	*data++ = READ_ONCE(priv->rx_crc_errors);
	*data++ = READ_ONCE(priv->rx_invalid_len_errors);
	*data++ = READ_ONCE(priv->rx_invalid_buf_errors);
	*data++ = READ_ONCE(priv->rx_resync_events);
	*data++ = READ_ONCE(priv->rx_alloc_failures);
	*data++ = READ_ONCE(priv->rx_late_landed);
	*data++ = READ_ONCE(priv->rx_late_stalled);
	*data++ = READ_ONCE(priv->rx_holes);
	*data++ = READ_ONCE(priv->rx_phantom_acks);
	*data++ = READ_ONCE(priv->tx_busy_events);
	*data++ = READ_ONCE(priv->tx_completion_excess);
	*data++ = READ_ONCE(priv->rx_omci);
	*data++ = READ_ONCE(priv->rx_omci_dropped);
	*data++ = READ_ONCE(priv->rx_omci_desc);
	*data++ = READ_ONCE(priv->rx_omci_invalid);
	*data++ = READ_ONCE(priv->tx_omci);
}

static void gc1601_get_strings(struct net_device *netdev, u32 stringset, u8 *data)
{
	if (stringset == ETH_SS_STATS)
		memcpy(data, gc1601_ethtool_stats_keys, sizeof(gc1601_ethtool_stats_keys));
}

static int gc1601_get_sset_count(struct net_device *netdev, int sset)
{
	if (sset == ETH_SS_STATS)
		return ARRAY_SIZE(gc1601_ethtool_stats_keys);
	return -EOPNOTSUPP;
}

static const struct ethtool_ops gc1601_ethtool_ops = {
	.get_drvinfo		= gc1601_get_drvinfo,
	.get_link		= ethtool_op_get_link,
	.get_link_ksettings	= phy_ethtool_get_link_ksettings,
	.set_link_ksettings	= phy_ethtool_set_link_ksettings,
	.get_pauseparam		= gc1601_get_pauseparam,
	.set_pauseparam		= gc1601_set_pauseparam,
	.get_strings		= gc1601_get_strings,
	.get_sset_count		= gc1601_get_sset_count,
	.get_ethtool_stats	= gc1601_get_ethtool_stats,
};

/* One-shot switch from probe replication to sequential TX at @slot. */
static int gc1601_tx_resume(struct gc1601_eth *priv, unsigned int slot)
{
	struct net_device *netdev = priv->netdev;
	struct net_device *pon_dev;
	unsigned long flags;
	unsigned int i;
	int ret;

	ASSERT_RTNL();
	spin_lock_irqsave(&priv->tx_lock, flags);
	ret = -EBUSY;
	if (!netif_running(netdev) || !priv->running || !priv->link || priv->tx_stopping ||
	    priv->tx_probe_failed || slot >= GC1601_RING_SIZE ||
	    !netif_queue_stopped(netdev) || priv->tx_pending ||
	    priv->tx_probe_aligned ||
	    priv->tx_probe_sent != GC1601_SLOT_PROBE_COUNT ||
	    priv->tx_completed_total - priv->tx_probe_completed_base !=
	    GC1601_SLOT_PROBE_COUNT ||
	    priv->tx_completion_excess != priv->tx_probe_excess_base)
		goto out;
	for (i = 0; i < GC1601_RING_SIZE; i++) {
		if (priv->tx_skb[i])
			goto out;
	}
	if (readl(priv->base[GC1601_TX_RING] + GC_TX_RING_BASE) !=
	    priv->tx_ring_dma)
		goto out;

	/* The slot was measured on this boot, from the wire or the byte counter. */
	memset(priv->tx_ring, 0, GC1601_RING_SIZE * sizeof(struct gc1601_desc));
	dma_wmb();
	priv->tx_producer = slot;
	priv->tx_consumer = slot;
	priv->tx_resume_slot = slot;
	priv->tx_replicated = false;
	priv->tx_probe_aligned = true;
	ret = 0;
out:
	spin_unlock_irqrestore(&priv->tx_lock, flags);
	if (!ret) {
		dev_dbg(priv->dev, "sequential TX resumed at slot %u\n", slot);
		netif_wake_queue(netdev);
		pon_dev = READ_ONCE(gc1601_pon_dev);
		if (pon_dev && netif_running(pon_dev))
			netif_wake_queue(pon_dev);
	}
	return ret;
}

/*
 * Each probe descriptor length is 64 + slot, so the GMAC TX byte counter
 * delta across one probe identifies the slot the hardware fetched.
 */
static int gc1601_tx_probe_slot(struct gc1601_eth *priv)
{
	int slot = -1, next;
	unsigned int i;

	for (i = 0; i < GC1601_SLOT_PROBE_COUNT; i++) {
		next = (int)(priv->tx_probe_octets[i + 1] - priv->tx_probe_octets[i]) -
		       64 - GC1601_PROBE_OCTET_EXTRA;
		if (next < 0 || next >= GC1601_RING_SIZE ||
		    (slot >= 0 && next != (slot + 1) % GC1601_RING_SIZE))
			return -1;
		slot = next;
	}
	return (slot + 1) % GC1601_RING_SIZE;
}

/* Send the slot probes ourselves so no stack frame is turned into one. */
/* Reap TX completions until the ring is idle; false if it still is after 100 ms. */
static bool gc1601_tx_wait_idle(struct gc1601_eth *priv)
{
	unsigned long timeout = jiffies + msecs_to_jiffies(100);
	unsigned long flags;

	while (READ_ONCE(priv->tx_pending) > 0) {
		if (time_after(jiffies, timeout))
			return false;
		spin_lock_irqsave(&priv->tx_lock, flags);
		gc1601_tx_complete_locked(priv);
		spin_unlock_irqrestore(&priv->tx_lock, flags);
		if (!READ_ONCE(priv->tx_pending))
			break;
		usleep_range(500, 1000);
	}
	return true;
}

static void gc1601_tx_probe_work(struct work_struct *work)
{
	struct gc1601_eth *priv = container_of(work, struct gc1601_eth,
					       tx_probe_work);
	struct net_device *netdev = priv->netdev;
	struct sk_buff *skb;
	unsigned long flags;
	int i, ret = 0;

	spin_lock_irqsave(&priv->tx_lock, flags);
	priv->tx_probe_running = true;
	spin_unlock_irqrestore(&priv->tx_lock, flags);

	for (i = 0; i < GC1601_SLOT_PROBE_COUNT; i++) {
		if (!gc1601_tx_wait_idle(priv)) {
			ret = -ETIMEDOUT;
			break;
		}

		skb = netdev_alloc_skb(netdev, ETH_ZLEN);
		if (!skb) {
			ret = -ENOMEM;
			break;
		}
		skb_put_zero(skb, ETH_ZLEN);

		netif_tx_lock_bh(netdev);
		if (!netif_running(netdev) || !READ_ONCE(priv->link) ||
		    READ_ONCE(priv->tx_stopping) ||
		    READ_ONCE(priv->tx_probe_failed) || READ_ONCE(priv->tx_probe_aligned) ||
		    READ_ONCE(priv->tx_probe_sent) >= GC1601_SLOT_PROBE_COUNT)
			ret = -EBUSY;
		else
			ret = gc1601_xmit(skb, netdev, NULL);
		netif_tx_unlock_bh(netdev);

		if (ret) {
			if (ret == -EBUSY)
				dev_kfree_skb_any(skb);
			break;
		}
	}
	if (ret)
		dev_warn_ratelimited(priv->dev,
				     "TX probe %d/%d stopped (%d), pending %u buf_wait %d\n",
				     i + 1, GC1601_SLOT_PROBE_COUNT, ret,
				     READ_ONCE(priv->tx_pending), READ_ONCE(priv->tx_buf_wait));

	/* The last probe must finish before the auto work looks at the ring. */
	gc1601_tx_wait_idle(priv);

	spin_lock_irqsave(&priv->tx_lock, flags);
	priv->tx_probe_running = false;
	gc1601_tx_probe_schedule_locked(priv);
	spin_unlock_irqrestore(&priv->tx_lock, flags);
}

/* Retry only a drained software ring; never reset or guess the hardware head. */
static int gc1601_tx_probe_retry_locked(struct gc1601_eth *priv)
{
	unsigned int i;

	lockdep_assert_held(&priv->tx_lock);
	if (priv->tx_pending)
		return -EBUSY;
	for (i = 0; i < GC1601_RING_SIZE; i++)
		if (priv->tx_skb[i])
			return -EBUSY;
	if (readl(priv->base[GC1601_TX_RING] + GC_TX_RING_BASE) != priv->tx_ring_dma)
		return -EBUSY;
	if (priv->tx_probe_retries >= GC1601_SLOT_PROBE_RETRIES)
		return -ETIMEDOUT;
	priv->tx_probe_retries++;
	priv->tx_probe_sent = 0;
	priv->tx_probe_completed_base = priv->tx_completed_total;
	priv->tx_probe_excess_base = priv->tx_completion_excess;
	memset(priv->tx_probe_octets, 0, sizeof(priv->tx_probe_octets));
	schedule_work(&priv->tx_probe_work);
	return 0;
}

static void gc1601_tx_auto_work(struct work_struct *work)
{
	struct gc1601_eth *priv = container_of(to_delayed_work(work),
					       struct gc1601_eth, tx_auto_work);
	unsigned long flags;
	int slot = -1, ret;

	/* Trylock: interface stop holds RTNL while cancelling this work. */
	if (!rtnl_trylock()) {
		spin_lock_irqsave(&priv->tx_lock, flags);
		if (!priv->tx_stopping)
			schedule_delayed_work(&priv->tx_auto_work, msecs_to_jiffies(10));
		spin_unlock_irqrestore(&priv->tx_lock, flags);
		return;
	}
	spin_lock_irqsave(&priv->tx_lock, flags);
	priv->tx_auto_scheduled = false;
	if (priv->tx_stopping || priv->tx_probe_failed || !priv->running || !priv->link ||
	    !netif_running(priv->netdev) || priv->tx_probe_aligned)
		goto out;
	/* The probe worker reschedules us when its run is finished. */
	if (priv->tx_probe_running)
		goto out;
	if (priv->tx_pending) {
		gc1601_tx_complete_locked(priv);
		if (priv->tx_pending) {
			priv->tx_auto_scheduled = true;
			schedule_delayed_work(&priv->tx_auto_work, msecs_to_jiffies(20));
			goto out;
		}
	}
	if (priv->tx_probe_sent != GC1601_SLOT_PROBE_COUNT)
		goto retry;
	priv->tx_probe_octets[GC1601_SLOT_PROBE_COUNT] =
		readl(priv->base[GC1601_GMAC] + GC_GMAC_TX_OCTETS);
	slot = gc1601_tx_probe_slot(priv);
	spin_unlock_irqrestore(&priv->tx_lock, flags);

	if (slot < 0)
		dev_warn(priv->dev, "TX probe byte deltas %u %u %u %u: no slot\n",
			 priv->tx_probe_octets[1] - priv->tx_probe_octets[0],
			 priv->tx_probe_octets[2] - priv->tx_probe_octets[1],
			 priv->tx_probe_octets[3] - priv->tx_probe_octets[2],
			 priv->tx_probe_octets[4] - priv->tx_probe_octets[3]);
	else
		dev_dbg(priv->dev, "TX probe: next slot %d\n", slot);
	if (slot >= 0 && !gc1601_tx_resume(priv, slot)) {
		rtnl_unlock();
		return;
	}
	spin_lock_irqsave(&priv->tx_lock, flags);
retry:
	if (!priv->link)
		goto out;
	ret = gc1601_tx_probe_retry_locked(priv);
	if (ret == -EBUSY) {
		priv->tx_auto_scheduled = true;
		schedule_delayed_work(&priv->tx_auto_work, msecs_to_jiffies(20));
	} else if (ret) {
		priv->tx_probe_failed = true;
		dev_err(priv->dev,
			"TX probe recovery stopped (%d), retries=%u; reopen interface to retry\n",
			ret, priv->tx_probe_retries);
	}
out:
	spin_unlock_irqrestore(&priv->tx_lock, flags);
	rtnl_unlock();
}

static void gc1601_port_dst_free(struct gc1601_eth *priv)
{
	unsigned int i;

	for (i = 0; i < GC1601_PORTS; i++)
		if (priv->port_dst[i])
			metadata_dst_free(priv->port_dst[i]);
}

/* Received frames carry their ingress port to DSA as one of these. */
static int gc1601_port_dst_alloc(struct gc1601_eth *priv)
{
	unsigned int i;

	for (i = 0; i < GC1601_PORTS; i++) {
		priv->port_dst[i] = metadata_dst_alloc(0, METADATA_HW_PORT_MUX, GFP_KERNEL);
		if (!priv->port_dst[i]) {
			gc1601_port_dst_free(priv);
			return -ENOMEM;
		}
		priv->port_dst[i]->u.port_info.port_id = i;
		priv->port_dst[i]->u.port_info.lower_dev = priv->netdev;
	}
	return 0;
}

static int gc1601_eth_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *switch_np;
	struct net_device *netdev;
	struct gc1601_eth *priv;
	unsigned int i;
	int ret;

	netdev = alloc_etherdev(sizeof(struct gc1601_eth));
	if (!netdev)
		return -ENOMEM;

	SET_NETDEV_DEV(netdev, dev);
	/* A DSA switch finds its conduit by this node. */
	netdev->dev.of_node = dev->of_node;
	platform_set_drvdata(pdev, netdev);

	priv = netdev_priv(netdev);
	priv->netdev = netdev;
	priv->dev = dev;
	spin_lock_init(&priv->tx_lock);
	spin_lock_init(&priv->irq_lock);
	INIT_DELAYED_WORK(&priv->tx_auto_work, gc1601_tx_auto_work);
	INIT_WORK(&priv->tx_probe_work, gc1601_tx_probe_work);
	INIT_DELAYED_WORK(&priv->tx_loopback_work, gc1601_tx_loopback_work);
	INIT_DELAYED_WORK(&priv->diag_work, gc1601_diag_work);
	priv->tx_detected_slot = -1;
	priv->rx_first_slot = -1;
	priv->pause_autoneg = true;
	priv->rx_pause = true;
	priv->tx_pause = true;

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		goto err_free_netdev;

	for (i = 0; i < GC1601_NUM_WINDOWS; i++) {
		static const resource_size_t min_size[GC1601_NUM_WINDOWS] = {
			0x820, 0x78, 0x68, 0x3c, 0x0c, 0x20, 0x04, 0x10, 0x04, 0x04, 0x18,
			0x4c,
		};
		struct resource *res;

		res = platform_get_resource_byname(pdev, IORESOURCE_MEM,
						   gc1601_reg_names[i]);
		if (!res || resource_size(res) < min_size[i]) {
			ret = -EINVAL;
			goto err_free_netdev;
		}
		priv->base[i] = devm_platform_ioremap_resource_byname(pdev, gc1601_reg_names[i]);
		if (IS_ERR(priv->base[i])) {
			ret = PTR_ERR(priv->base[i]);
			dev_err(dev, "Failed to map register window %s\n", gc1601_reg_names[i]);
			goto err_free_netdev;
		}
	}

	priv->irq = platform_get_irq(pdev, 0);
	if (priv->irq < 0) {
		ret = priv->irq;
		goto err_free_netdev;
	}

	priv->sched_base = priv->base[GC1601_SCHEDULER];
	priv->os_base = priv->base[GC1601_OUTPUT_SCHEDULER];
	priv->queue_base = priv->base[GC1601_QUEUES];
	priv->scu = syscon_regmap_lookup_by_phandle(dev->of_node, "galachip,scu");
	if (IS_ERR(priv->scu)) {
		ret = dev_err_probe(dev, PTR_ERR(priv->scu), "no SCU\n");
		goto err_free_netdev;
	}
	priv->ge_serdes = syscon_regmap_lookup_by_phandle(dev->of_node, "galachip,ge-serdes");
	if (IS_ERR(priv->ge_serdes)) {
		ret = dev_err_probe(dev, PTR_ERR(priv->ge_serdes), "no GE SerDes\n");
		goto err_free_netdev;
	}
	/* The switch tables, which the SFU offload and the cold init program. */
	switch_np = of_parse_phandle(dev->of_node, "galachip,switch", 0);
	priv->switch_base = switch_np ? devm_of_iomap(dev, switch_np, 0, NULL) : NULL;
	of_node_put(switch_np);
	if (IS_ERR_OR_NULL(priv->switch_base)) {
		ret = priv->switch_base ? PTR_ERR(priv->switch_base) : -ENODEV;
		dev_err(dev, "cannot map the switch: %d\n", ret);
		goto err_free_netdev;
	}
	gc1601_log_handoff(priv, "probe");

	priv->phy_node = of_parse_phandle(dev->of_node, "phy-handle", 0);
	ret = of_get_phy_mode(dev->of_node, &priv->phy_interface);
	if (ret)
		priv->phy_interface = PHY_INTERFACE_MODE_SGMII;

	/*
	 * The MAC lives in the misc partition. Wait for it while deferred
	 * probing is still possible, then fall back to a random address
	 * instead of leaving the interface missing.
	 */
	ret = of_get_ethdev_address(dev->of_node, netdev);
	if (ret == -EPROBE_DEFER) {
		ret = driver_deferred_probe_check_state(dev);
		if (ret == -EPROBE_DEFER)
			goto err_free_netdev;
	}
	if (ret) {
		eth_hw_addr_random(netdev);
		dev_warn(dev, "no factory MAC (%d), using random %pM\n", ret,
			 netdev->dev_addr);
	}

	/* Initialize memory pointers to loader-reserved arena */
	priv->pkt_pool = phys_to_virt(GC1601_PKT_POOL_PHYS);
	priv->rx_ring = dmam_alloc_coherent(dev,
					    GC1601_RX_CONTAINERS * GC1601_RX_RING_STRIDE,
					    &priv->rx_ring_dma, GFP_KERNEL);
	if (!priv->rx_ring) {
		ret = -ENOMEM;
		goto err_free_netdev;
	}

	/* Allocate coherent TX descriptor ring (1 page = 4096 bytes) */
	priv->tx_ring = dma_alloc_coherent(dev,
					   GC1601_RING_SIZE * sizeof(struct gc1601_desc),
					   &priv->tx_ring_dma, GFP_KERNEL);
	if (!priv->tx_ring) {
		ret = -ENOMEM;
		goto err_free_netdev;
	}

	netdev->netdev_ops = &gc1601_netdev_ops;
	netdev->ethtool_ops = &gc1601_ethtool_ops;
	netdev->min_mtu = ETH_MIN_MTU;
	/* The default stays 1500; the limit leaves room for QinQ in 1966 bytes. */
	/* Room for the DSA tag over a user port at its largest MTU. */
	netdev->max_mtu = GC1601_MAX_MTU + GC1601_TAG_LEN;
	priv->min_left_iptr = 0x3ff;

	netif_napi_add(netdev, &priv->napi, gc1601_rx_poll);

	/*
	 * Bring a cold engine up now, before pon0 can share its buffer pool:
	 * a first open after pon0 is running rebuilds the free list under
	 * buffers the PON still holds. Every open then takes the warm path.
	 */
	rtnl_lock();
	ret = gc1601_hw_setup(priv);
	gc1601_hw_stop(priv);
	rtnl_unlock();
	if (ret) {
		dev_err(dev, "Packet engine setup failed: %d\n", ret);
		goto err_free_tx_ring;
	}

	ret = gc1601_port_dst_alloc(priv);
	if (ret)
		goto err_free_tx_ring;
	ret = register_netdev(netdev);
	if (ret) {
		dev_err(dev, "Failed to register netdevice\n");
		gc1601_port_dst_free(priv);
		goto err_free_tx_ring;
	}
	gc1601_debugfs_init(priv);

	netdev_dbg(netdev, "GC1601 packet engine at irq %d, MAC %pM\n",
		   priv->irq, netdev->dev_addr);
	mutex_lock(&gc1601_uni_lock);
	WRITE_ONCE(gc1601_pe_netdev, netdev);
	mutex_unlock(&gc1601_uni_lock);

	return 0;

err_free_tx_ring:
	dma_free_coherent(dev, GC1601_RING_SIZE * sizeof(struct gc1601_desc),
			  priv->tx_ring, priv->tx_ring_dma);
	netif_napi_del(&priv->napi);
err_free_netdev:
	of_node_put(priv->phy_node);
	free_netdev(netdev);
	return ret;
}

static void gc1601_eth_remove(struct platform_device *pdev)
{
	struct net_device *netdev = platform_get_drvdata(pdev);
	struct gc1601_eth *priv = netdev_priv(netdev);

	mutex_lock(&gc1601_uni_lock);
	WRITE_ONCE(gc1601_pe_netdev, NULL);
	mutex_unlock(&gc1601_uni_lock);
	debugfs_remove_recursive(priv->debugfs);
	unregister_netdev(netdev);
	gc1601_port_dst_free(priv);
	dma_free_coherent(priv->dev, GC1601_RING_SIZE * sizeof(struct gc1601_desc),
			  priv->tx_ring, priv->tx_ring_dma);
	netif_napi_del(&priv->napi);
	if (priv->phy_node)
		of_node_put(priv->phy_node);
	free_netdev(netdev);
}

static const struct of_device_id gc1601_eth_match[] = {
	{ .compatible = "galachip,gc1601-eth" },
	{ .compatible = "galachip,gc1601-packet-engine" },
	{ }
};
MODULE_DEVICE_TABLE(of, gc1601_eth_match);

static struct platform_driver gc1601_eth_driver = {
	.probe = gc1601_eth_probe,
	.remove = gc1601_eth_remove,
	.driver = {
		.name = "gc1601-eth",
		.suppress_bind_attrs = true,
		.of_match_table = gc1601_eth_match,
	},
};
module_platform_driver(gc1601_eth_driver);

MODULE_AUTHOR("Ifran Ali <khanu.i.i.k.2000@gmail.com>");
MODULE_DESCRIPTION("GalaChip GC1601 Gigabit Ethernet Driver");
MODULE_LICENSE("GPL");
