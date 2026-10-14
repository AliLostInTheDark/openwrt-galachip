// SPDX-License-Identifier: GPL-2.0-only
/*
 * GalaChip GC1601 Gigabit Ethernet Driver
 *
 * Copyright (C) 2026 Ifran Ali <khanu.i.i.k.2000@gmail.com>
 */

#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/etherdevice.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/netdevice.h>
#include <linux/of.h>
#include <linux/of_mdio.h>
#include <linux/of_net.h>
#include <linux/phy.h>
#include <linux/platform_device.h>
#include <linux/rtnetlink.h>
#include <linux/sizes.h>
#include <linux/spinlock.h>
#include <linux/timer.h>
#include <linux/workqueue.h>
#include <linux/soc/galachip/gc1601.h>
#include <linux/unaligned.h>

#define GC1601_BUFFER_COUNT	512
#define GC1601_BUFFER_SIZE	2048
#define GC1601_RING_SIZE	256
#define GC1601_IRQ_RETRY_MS	200	/* polling after an IRQ storm, then retry */
#define GC1601_IRQ_RETRY_MAX_MS	10000
#define GC_SR_PORT_VLAN		0x10004	/* switch_base: SRC bank, per-port VLAN controls */
/* Switch banks, offsets from switch_base (0x90f80000). */
#define GC_SR_HA		0x00000
#define GC_SR_L2		0x04000
#define GC_SR_L3		0x08000
#define GC_SR_HM		0x0c000
#define GC_SR_SRC		0x10000
#define GC1601_SLOT_PROBE_SIZE	320
#define GC1601_SLOT_PROBE_COUNT	4
/* GMAC TX octets count the FCS beyond the descriptor length (iperf: +4.0/frame). */
#define GC1601_PROBE_OCTET_EXTRA	4

/* Physical bases in loader-reserved memory (0x40800000..0x40911fff) */
#define GC1601_FREE_LIST_PHYS	0x40800000
#define GC1601_PKT_POOL_PHYS	0x40801000
#define GC1601_RX_RING_PHYS	0x40901000	/* the loader's: 16 containers, 4 KiB apart */
#define GC1601_TX_RING_PHYS	0x40911000

/* PMU_register_edev: one RX ring per container, container 15 carries OMCI. */
#define GC1601_RX_CONTAINERS	16
#define GC1601_RX_RING_STRIDE	SZ_4K
/* RX descriptor word 0 "in": the ingress port, 1 for the GE UNI, 0 for the PON. */
#define GC1601_RX_IN		GENMASK(31, 29)
#define GC1601_RX_IN_GE		1
#define GC1601_RX_MINFO		GENMASK(25, 14)	/* GEM port of a PON frame */

enum gc1601_window {
	GC1601_GMAC,
	GC1601_BUFFERS,
	GC1601_DESCRIPTORS,
	GC1601_RX,
	GC1601_TX_RING,
	GC1601_RX_PENDING,
	GC1601_TX_SPACE,
	GC1601_SCHEDULER_READY,
	GC1601_BUFFER_CONTROL,
	GC1601_BUFFER_ENABLE,
	GC1601_TX_CONFIG,
	GC1601_STATISTICS,
	GC1601_NUM_WINDOWS,
};

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
};

/* GMAC registers (0x90e00000) */
#define GC_GMAC_CFG		0x00
#define GC_GMAC_IER		0x04
#define GC_GMAC_MACCR		0x08
#define GC_GMAC_CFG_TX_EN	BIT(0)
#define GC_GMAC_CFG_RX_EN	BIT(1)
#define GC_GMAC_CFG_BASE	0x00bb0000
#define GC_GMAC_CFG_SPEED_1000	0x0000
#define GC_GMAC_CFG_SPEED_100	0x8000
#define GC_GMAC_CFG_SPEED_10	0xc000
#define GC_GMAC_CFG_SPEED_MASK	0xc000
#define GC_GMAC_CFG_FULL_DUPLEX	BIT(13)
#define GC_GMAC_ERCR		0xe0
#define GC_GMAC_ACIR		0xe4
#define GC_GMAC_TX_OCTETS	0x714

/* Buffer manager registers (0x90dc0000) */
#define GC_BUF_PM_SRAM_INIT	0x08
#define GC_BUF_PM_SRAM_STATUS	0x0c
#define GC_BUF_OS_SRAM_INIT	0x10
#define GC_BUF_OS_SRAM_STATUS	0x14
#define GC_BUF_READ_CLEAR	0x20
#define GC_BUF_READ_CLEAR_EN	BIT(1)
#define GC_BUF_CTRL		0x24
#define GC_BUF_FREE_LIST_BASE	0x28
#define GC_BUF_RX_RING_BASE	0x30
#define GC_BUF_PKT_POOL_BASE	0x34
#define GC_BUF_POOL_CFG		0x3c
#define GC_BUF_INTR_MASK	0x04
#define GC_BUF_INTR_STATUS	0x40
#define GC_BUF_EVENT_MASK	0x44
#define GC_BUF_MTU		0x48	/* frame limit, bits 13:0 */
#define GC_BUF_PM_CTRL		0x74

/* Descriptors registers (0x90dd0000) */
#define GC_DESC_CTRL		0x00
#define GC_DESC_TABLE_INDEX	0x14
#define GC_DESC_TABLE_DONE	0x18
#define GC_DESC_TABLE_READ	BIT(27)
#define GC_DESC_TABLE_DATA0	0x1c
#define GC_DESC_TABLE_DATA1	0x20
#define GC_DESC_TABLE_DATA2	0x24
#define GC_DESC_TABLE_DATA3	0x28
#define GC_STT_STATS_COUNT	64
#define GC_STT_STATS_WORDS	14
#define GC_DESC_ACK_COUNT	0x60
#define GC_DESC_ACK_STATUS	0x64

/* CVRD selectors at GC_DESC_TABLE_INDEX bits 26:22 (vendor CVRD_tbl_*). */
#define GC_CVRD_OUTMAX		0
#define GC_CVRD_OUTCNT		BIT(22)
#define GC_CVRD_INMAX		BIT(23)
#define GC_CVRD_INCNT		(BIT(23) | BIT(22))
#define GC_CVRD_ALLCFG		BIT(24)
#define GC_CVRD_ALLCNT		(BIT(24) | BIT(22))
/* Vendor buffer dump prints every max/count word as (w & 0x7ff) / (w >> 11). */
#define GC_CVRD_LO(w)		((w) & 0x7ff)
#define GC_CVRD_HI(w)		((w) >> 11)
#define GC_CVRD_TCONT_QUEUES	128	/* 16 T-CONTs x 8 queues, index tcont * 8 + q */
#define GC_CVRD_UNI0		0x140
#define GC_CVRD_CPU		0x180
#define GC_CVRD_UNI_QUEUES	8
#define GC_CVRD_CPU_QUEUES	16

/* RX door registers (0x90dd4000) */
#define GC_RX_ENABLE		0x00
#define GC_RX_ALLOC_RESP	0x0c
#define GC_RX_RELEASE_BUF	0x10
#define GC_RX_ALLOC_REQ		0x14
#define GC_RX_PARTITION		0x18
#define GC_RX_CTRL		0x20
#define GC_RX_CTRL_VAL		0x0f	/* as U-Boot */
#define GC_RX_PUBLISH_TX	0x38

/* TX ring registers (0x90ddc040) */
#define GC_TX_RING_BASE		0x00
#define GC_TX_DOORBELL		0x04
#define GC_TX_COMPLETIONS	0x08

/* Interrupt status / mask bits */
#define GC_INTR_RX		BIT(0)
#define GC_INTR_TX		BIT(1)

struct gc1601_desc {
	__le32 word[4];
};

struct gc1601_eth {
	struct net_device *netdev;
	struct device *dev;
	void __iomem *base[GC1601_NUM_WINDOWS];
	void __iomem *sched_base;
	void __iomem *os_base;		/* 0x90de0000: output scheduler, queue flush */
	/* Banks touched only by the cold (loader-equivalent) init. */
	void __iomem *switch_base;
	void __iomem *queue_base;
	int irq;

	struct device_node *phy_node;
	phy_interface_t phy_interface;
	int speed;
	int duplex;
	int link;

	struct napi_struct napi;
	struct timer_list poll_timer;
	/* Serializes interrupt masks and shutdown against NAPI completion. */
	spinlock_t irq_lock;
	bool running;
	bool uni_disabled;
	struct gc1601_uni_attrs uni_attrs;	/* last set by OMCI, applied if uni_omci_attrs */

	void *pkt_pool;
	struct gc1601_desc *rx_ring;
	dma_addr_t rx_ring_dma;
	struct gc1601_desc *tx_ring;
	dma_addr_t tx_ring_dma;

	u16 rx_consumer[GC1601_RX_CONTAINERS];
	u8 rx_next_container;
	u16 rx_stalled;		/* containers counting frames with no descriptor */
	u16 rx_synced;		/* containers whose consumer met the hardware */
	u32 rx_late_landed;	/* descriptors that landed while we waited */
	u32 rx_late_stalled;	/* ... and that had not landed after the wait */
	unsigned long rx_late_since[GC1601_RX_CONTAINERS];
	u32 rx_holes;		/* slots never written, stepped over */
	unsigned long rx_phantom_since[GC1601_RX_CONTAINERS];
	u32 rx_phantom_acks;	/* counted frames that never got a descriptor */

	/* Protects TX ring updates and doorbell writes */
	spinlock_t tx_lock;
	u16 tx_producer;
	u16 tx_consumer;
	u16 tx_pending;
	bool tx_replicated;
	struct sk_buff *tx_skb[GC1601_RING_SIZE];
	/* Diagnostic counters */
	u64 rx_crc_errors;
	u64 rx_invalid_len_errors;
	u64 rx_invalid_buf_errors;
	u64 rx_resync_events;
	u64 tx_busy_events;
	u16 tx_buf_id[GC1601_RING_SIZE];
	u32 rx_alloc_failures;
	u32 tx_completion_excess;
	u32 tx_completion_reads;
	u32 tx_completed_total;
	u32 tx_completion_last;
	u32 tx_completion_drained;
	int tx_detected_slot;
	u32 tx_detected_w3;
	int rx_first_slot;
	u32 pm_sram_status;
	u32 os_sram_status;
	u32 tx_probe_sent;
	u64 rx_omci, rx_omci_dropped, tx_omci;
	u64 rx_omci_desc, rx_omci_invalid;
	u64 rx_unrouted;	/* PON data until the pon0 data path exists */
	u32 rx_omci_last_words[4];
	bool tx_probe_aligned;
	bool tx_auto_scheduled;
	u32 tx_probe_completed_base;
	/* GMAC TX octets before each probe doorbell, plus one final sample. */
	u32 tx_probe_octets[GC1601_SLOT_PROBE_COUNT + 1];
	struct delayed_work tx_auto_work;
	struct work_struct tx_probe_work;
	u16 tx_resume_slot;
	u32 rx_poll_calls;
	u32 rx_pending_last;
	u32 rx_empty_desc;
	u32 rx_ack_count;
	u32 rx_release_count;
	u32 rx_last_slot;
	u32 rx_last_words[4];
	u32 irq_last_status;
	u32 irq_empty_streak;
	bool irq_poll_only;
	u32 irq_poll_ticks;	/* timer ticks spent polling since the last storm */
	u32 irq_retry_ms;	/* polling before the next IRQ retry, doubled per relapse */
	unsigned long irq_retry_at;	/* jiffies of the last IRQ retry */
	u32 irq_calls;
	u32 irq_empty_calls;
	u32 irq_duplicate_calls;
	u32 irq_storm_pending;
	u32 irq_storm_mask;
	u32 irq_storm_event_mask;
	u32 irq_storm_consumer;
	unsigned long irq_storm_jiffies;
	bool queue_read_failed;
	bool ingress_read_failed;
	bool sfu_read_failed;
	bool stats_read_failed;
	u8 stats_index;

	/* CVRD words written by setup, compared against hardware readback. */
	bool cvrd_inmax_written;
	bool sr_ready;		/* SR_hw_init has run */
	u32 sr_src8;		/* SRC+0x08 as SR_hw_init leaves it with sr_src8_dec24 off */
	bool sfu_rules;		/* stock traps, parser rules and index lists loaded */
	unsigned int sfu_flows;	/* services with flows installed */
	struct gc1601_sfu_flow sfu_active[GC1601_SFU_FLOWS];
	/* Hashed flow entries installed, as table << 16 | index. */
	u32 sfu_hashed[2 * GC1601_SFU_FLOWS];
	unsigned int sfu_hashed_count;
	u32 cvrd_uni_inmax;
	u32 cvrd_uni_outmax[GC_CVRD_UNI_QUEUES];
	u32 cvrd_uni_allcfg[GC_CVRD_UNI_QUEUES];
	u32 cvrd_cpu_outmax;
	u32 cvrd_mismatches;

	/* Buffer IDs handed out by hardware since probe, for free-list analysis. */
	DECLARE_BITMAP(rx_id_seen, GC1601_BUFFER_COUNT);
	DECLARE_BITMAP(tx_id_seen, GC1601_BUFFER_COUNT);

};



/* In-flight TX descriptors once the probe handoff has measured the ring. */
#define GC1601_TX_DEPTH	16

static void gc1601_adjust_link(struct net_device *netdev)
{
	struct gc1601_eth *priv = netdev_priv(netdev);
	struct phy_device *phydev = netdev->phydev;
	unsigned long flags;
	bool status_change = false;

	if (phydev->link) {
		u32 val, speed_bits;

		if (priv->speed != phydev->speed || priv->duplex != phydev->duplex) {
			priv->speed = phydev->speed;
			priv->duplex = phydev->duplex;
			status_change = true;
		}

		switch (phydev->speed) {
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

		val = GC_GMAC_CFG_BASE | speed_bits;
		if (phydev->duplex == DUPLEX_FULL)
			val |= GC_GMAC_CFG_FULL_DUPLEX;

		spin_lock_irqsave(&priv->irq_lock, flags);
		if (!priv->uni_disabled)
			val |= GC_GMAC_CFG_TX_EN | GC_GMAC_CFG_RX_EN;
		writel(val, priv->base[GC1601_GMAC] + GC_GMAC_CFG);
		writel(0x0000fffe, priv->base[GC1601_GMAC] + GC_GMAC_IER);
		writel(0x80000001, priv->base[GC1601_GMAC] + GC_GMAC_MACCR);
		spin_unlock_irqrestore(&priv->irq_lock, flags);
	} else if (priv->link) {
		spin_lock_irqsave(&priv->irq_lock, flags);
		writel(GC_GMAC_CFG_BASE, priv->base[GC1601_GMAC] + GC_GMAC_CFG);
		spin_unlock_irqrestore(&priv->irq_lock, flags);
	}

	if (phydev->link != priv->link) {
		priv->link = phydev->link;
		status_change = true;
		/* Measure the TX handoff now rather than on the stack's first frames. */
		if (priv->link && !priv->tx_probe_aligned &&
		    !priv->tx_probe_sent)
			schedule_work(&priv->tx_probe_work);
	}

	if (status_change)
		phy_print_status(phydev);
}

static int gc1601_queue_read(struct gc1601_eth *priv, u32 selector,
			     unsigned int index, u32 *data, unsigned int words)
{
	void __iomem *base = priv->base[GC1601_DESCRIPTORS];
	u32 done;
	unsigned int i;
	int ret;

	ASSERT_RTNL();
	writel(0, base + GC_DESC_TABLE_DONE);
	writel(GC_DESC_TABLE_READ | selector | index, base + GC_DESC_TABLE_INDEX);
	ret = readl_poll_timeout(base + GC_DESC_TABLE_DONE, done, done == 1, 1, 1000);
	if (ret) {
		/* Do not issue further commands after an uncompleted transaction. */
		priv->queue_read_failed = true;
		return ret;
	}
	for (i = 0; i < words; i++)
		data[i] = readl(base + GC_DESC_TABLE_DATA0 + i * sizeof(u32));
	return 0;
}

static void gc1601_cvrd_check(struct gc1601_eth *priv, const char *name,
			      unsigned int index, u32 expected, u32 actual)
{
	if (expected == actual)
		return;
	priv->cvrd_mismatches++;
	dev_warn(priv->dev, "CVRD %s %03x wrote %08x read %08x\n",
		 name, index, expected, actual);
}

/* Read back every CVRD word setup programmed; the setup log is not evidence. */
static int gc1601_cvrd_verify(struct gc1601_eth *priv)
{
	u32 inmax, outmax, cfg[3], done;
	unsigned int i, index;
	int ret;

	/* Loader-style default writes do not poll; let the last one complete. */
	if (readl_poll_timeout(priv->base[GC1601_DESCRIPTORS] + GC_DESC_TABLE_DONE,
			       done, done == 1, 1, 1000))
		dev_warn(priv->dev, "CVRD completion %08x before readback\n", done);

	priv->cvrd_mismatches = 0;
	for (i = 0; i < GC_CVRD_UNI_QUEUES; i++) {
		index = GC_CVRD_UNI0 + i;
		ret = gc1601_queue_read(priv, GC_CVRD_INMAX, index, &inmax, 1);
		if (!ret)
			ret = gc1601_queue_read(priv, GC_CVRD_OUTMAX, index, &outmax, 1);
		if (!ret)
			ret = gc1601_queue_read(priv, GC_CVRD_ALLCFG, index, cfg, 3);
		if (ret) {
			dev_err(priv->dev, "CVRD readback timed out at %03x\n", index);
			return ret;
		}
		if (priv->cvrd_inmax_written)
			gc1601_cvrd_check(priv, "inmax", index, priv->cvrd_uni_inmax, inmax);
		gc1601_cvrd_check(priv, "outmax", index, priv->cvrd_uni_outmax[i], outmax);
		gc1601_cvrd_check(priv, "allcfg0", index, priv->cvrd_uni_allcfg[i], cfg[0]);
		gc1601_cvrd_check(priv, "allcfg1", index, 0, cfg[1]);
		gc1601_cvrd_check(priv, "allcfg2", index, 0, cfg[2]);
		dev_dbg(priv->dev, "CVRD %03x in %u/%u out %u/%u all %u (raw %08x %08x %08x)%s\n",
			index, GC_CVRD_LO(inmax), GC_CVRD_HI(inmax),
			GC_CVRD_LO(outmax), GC_CVRD_HI(outmax), cfg[0],
			inmax, outmax, cfg[0],
			priv->cvrd_inmax_written ? "" : " inmax inherited");
	}
	for (i = 0; i < GC_CVRD_CPU_QUEUES; i++) {
		index = GC_CVRD_CPU + i;
		ret = gc1601_queue_read(priv, GC_CVRD_OUTMAX, index, &outmax, 1);
		if (ret) {
			dev_err(priv->dev, "CVRD readback timed out at %03x\n", index);
			return ret;
		}
		gc1601_cvrd_check(priv, "cpu outmax", index, priv->cvrd_cpu_outmax, outmax);
	}
	if (priv->cvrd_mismatches)
		dev_warn(priv->dev, "CVRD readback: %u queue mismatches\n",
			 priv->cvrd_mismatches);
	return 0;
}

/*
 * gala_eth_initialize from U-Boot: SCU 0x90a00810 bit 13 enables the PHY block
 * clock/reset, followed by the 5-step PHY SerDes sequence at 0x91400000.
 * Cold boots from flash never run U-Boot networking, leaving the PHY at
 * 0x09e000b1 instead of 0x09e400b3.
 */
static void gc1601_cold_phy_init(struct gc1601_eth *priv)
{
	void __iomem *scu810 = ioremap(0x90a00810, 4);
	void __iomem *phy_serdes = ioremap(0x91400000, 4);

	if (scu810 && phy_serdes) {
		if (!(readl(scu810) & BIT(13))) {
			writel(readl(scu810) | BIT(13), scu810);
			usleep_range(50000, 51000);
			writel(0x4000, phy_serdes);
			usleep_range(10000, 11000);
			writel(0x20000, phy_serdes);
			usleep_range(10000, 11000);
			writel(0x00020002, phy_serdes);
			usleep_range(10000, 11000);
			writel(0x00020003, phy_serdes);
			usleep_range(10000, 11000);
			writel(0x3, phy_serdes);
			usleep_range(100000, 101000);
			dev_info(priv->dev, "cold init: PHY SerDes initialized\n");
		}
	}
	if (scu810)
		iounmap(scu810);
	if (phy_serdes)
		iounmap(phy_serdes);
}

/*
 * The loader's gala_eth_init up to its SRAM commands, minus the shared
 * global reset and optical writes: switch defaults, TX queue configuration
 * and buffer enable must be in place before SRAM initialisation.
 */
static int gc1601_cold_prepare(struct gc1601_eth *priv)
{
	if (!priv->switch_base || !priv->queue_base || !priv->sched_base ||
	    !priv->base[GC1601_BUFFER_ENABLE])
		return -ENODEV;

	gc1601_cold_phy_init(priv);
	writel(1, priv->base[GC1601_BUFFER_ENABLE]);
	writel(U32_MAX, priv->switch_base + 0x38);
	writel(U32_MAX, priv->switch_base + 0x3c);
	writel(0x000a0003, priv->switch_base + 0x60);
	writel(0x00001555, priv->switch_base + 0x800c);
	writel(0xfd, priv->switch_base + 0x100fc);
	writel(0x607, priv->base[GC1601_TX_CONFIG]);
	writel(100, priv->base[GC1601_TX_CONFIG] + 0x08);
	writel(0x20, priv->base[GC1601_TX_CONFIG] + 0x0c);
	return 0;
}

/* The loader steps that follow its descriptor table and ring setup. */
static void gc1601_cold_finish(struct gc1601_eth *priv)
{
	writel(0x10, priv->base[GC1601_TX_CONFIG] + 0x14);
	writel(0, priv->queue_base + 0x0c);
	writel(0x02000000, priv->base[GC1601_RX] + GC_RX_PARTITION);
	writel(0x51555555, priv->switch_base + 0x4198);
}

static int gc1601_cvrd_write(struct gc1601_eth *priv, u32 index, u32 data);

static int gc1601_descriptor_setup(struct gc1601_eth *priv)
{
	void __iomem *base = priv->base[GC1601_DESCRIPTORS];
	unsigned int i;

	writel(readl(base + GC_DESC_CTRL) | 0x14, base + GC_DESC_CTRL);

	/*
	 * Stock limits (PMU_buffer_init): UNI0 queue 0 may hold 1080 buffers,
	 * queues 1-7 720, CPU queues 128. The loader's 32 overflow when the
	 * 2.5G PON bursts into the 1G UNI.
	 */
	for (i = 0; i < GC_CVRD_UNI_QUEUES; i++) {
		u32 index = GC_CVRD_UNI0 + i;
		int ret;

		priv->cvrd_uni_outmax[i] = i ? 0x00168020 : 0x0021c020;
		priv->cvrd_uni_allcfg[i] = i ? 0x390 : 0x4f8;
		ret = gc1601_cvrd_write(priv, GC_CVRD_INMAX | index, 0x00040020);
		if (!ret)
			ret = gc1601_cvrd_write(priv, GC_CVRD_OUTMAX | index,
						priv->cvrd_uni_outmax[i]);
		if (!ret)
			ret = gc1601_cvrd_write(priv, GC_CVRD_ALLCFG | index,
						priv->cvrd_uni_allcfg[i]);
		if (ret)
			return ret;
	}
	for (i = 0; i < GC_CVRD_CPU_QUEUES; i++) {
		int ret = gc1601_cvrd_write(priv, GC_CVRD_OUTMAX | (GC_CVRD_CPU + i),
					    0x00040060);

		if (ret)
			return ret;
	}
	priv->cvrd_inmax_written = true;
	priv->cvrd_uni_inmax = 0x00040020;
	priv->cvrd_cpu_outmax = 0x00040060;
	return 0;
}

static int gc1601_cvrd_write(struct gc1601_eth *priv, u32 index, u32 data)
{
	void __iomem *base = priv->base[GC1601_DESCRIPTORS];
	u32 done;
	int ret;

	writel(0, base + GC_DESC_TABLE_DONE);
	writel(index, base + GC_DESC_TABLE_INDEX);
	writel(data, base + GC_DESC_TABLE_DATA0);
	ret = readl_poll_timeout(base + GC_DESC_TABLE_DONE, done, done == 1, 1, 1000);
	if (ret)
		dev_err(priv->dev, "CVRD write %08x timed out\n", index);
	return ret;
}

/*
 * PMU_buffer_init gives every T-CONT queue a buffer profile. With the reset
 * value of zero the packet engine drops each upstream frame on enqueue, so
 * the GPON MAC answers its grants with idle bursts only. PMU_hw_init also
 * selects the GPON upstream in the scheduler (bits 0 and 2; bit 1 is EPON).
 */
static int gc1601_tcont_queue_setup(struct gc1601_eth *priv)
{
	unsigned int i;
	int ret;

	for (i = 0; i < GC_CVRD_TCONT_QUEUES; i++) {
		bool q0 = !(i % 8);

		ret = gc1601_cvrd_write(priv, GC_CVRD_INMAX | i, 32 | (128 << 11));
		if (!ret)
			ret = gc1601_cvrd_write(priv, GC_CVRD_OUTMAX | i,
						32 | ((q0 ? 1080 : 720) << 11));
		if (!ret)
			ret = gc1601_cvrd_write(priv, GC_CVRD_ALLCFG | i, q0 ? 0x4f8 : 0x390);
		if (ret)
			return ret;
	}
	writel((readl(priv->sched_base) & ~BIT(1)) | BIT(2) | BIT(0), priv->sched_base);
	return 0;
}

/*
 * Switch port VLAN controls (SRC + 0x04). The loader leaves bit 16 set,
 * which drops VLAN-tagged frames on egress to the UNI: nothing tagged
 * leaves the LAN port, so a router tagging the service VLAN itself never
 * hears the OLT. SR_hw_init clears it and chipset_swport_vlan_check(port,
 * 0, 0, 1) sets each port's bit in the top byte.
 */
static void gc1601_port_vlan_setup(struct gc1601_eth *priv)
{
	void __iomem *reg;
	u32 val;

	if (!priv->switch_base)
		return;
	reg = priv->switch_base + GC_SR_PORT_VLAN;
	val = readl(reg);
	val &= ~(BIT(16) | GENMASK(9, 8) | GENMASK(1, 0));
	writel(val | GENMASK(25, 24), reg);
}

static void gc1601_sr_update(struct gc1601_eth *priv, u32 off, u32 clear, u32 set)
{
	void __iomem *reg = priv->switch_base + off;

	writel((readl(reg) & ~clear) | set, reg);
}

/*
 * SR_hw_init as xpon_sdk runs it with hw_br_flood=1: table init, L3 lookup
 * and port defaults, isolation, multicast permits and ageing. The loader
 * leaves the engine minimal, so frames the switch takes are dropped without
 * it. car_init (CPU trap rate limits) is left out.
 */
static void gc1601_uni_attrs_apply(struct gc1601_eth *priv);

/*
 * The 2024-12 stock image ends S03sdk with "w e4002 bd" on dbg_reg, setting
 * SRC+0x08 to 0xbd in every PON mode; the 2023 images leave the SR_hw_init
 * value. Its other users are the multicast tables, so this is A/B only.
 */
static bool sr_src8_dec24;

static void gc1601_sr_src8_apply(struct gc1601_eth *priv)
{
	writel(sr_src8_dec24 ? 0xbd : priv->sr_src8, priv->switch_base + GC_SR_SRC + 0x08);
}

static int gc1601_sr_hw_init(struct gc1601_eth *priv)
{
	void __iomem *sw = priv->switch_base;
	u32 src0 = readl(sw + GC_SR_SRC);
	u32 val;
	int ret;

	writel(U32_MAX, sw + GC_SR_HA + 0x38);
	writel(U32_MAX, sw + GC_SR_HA + 0x3c);
	ret = readx_poll_timeout(readl, sw + GC_SR_HA + 0x40, val,
				 val || readl(sw + GC_SR_HA + 0x44), 100, 2500000);
	if (ret)
		return ret;

	gc1601_sr_update(priv, GC_SR_L2, 0x0f000000, 0);
	gc1601_sr_update(priv, GC_SR_L2 + 0x40, 0x0f000000, 0);
	writel(0x00050003, sw + GC_SR_SRC + 0x60);
	writel(0xff, sw + GC_SR_SRC + 0x64);
	priv->sr_src8 = (readl(sw + GC_SR_SRC + 0x08) & ~0x42) | 0xb1;
	gc1601_sr_src8_apply(priv);
	writel(0x03000503, sw + GC_SR_SRC + 0x70);
	writel(0x00030000, sw + GC_SR_SRC + 0x74);
	writel(0x000a0000, sw + GC_SR_SRC + 0x78);

	val = readl(sw + GC_SR_L3);
	val = (val & 0xfff5ffff) | 0x80c0;
	writel((val & 0x9ffffdff) | 0x90f10900, sw + GC_SR_L3);
	val = readl(sw + GC_SR_L3 + 0x0c);
	val = (val & 0xffffffc5) | 0x05;
	writel((val & 0xffffc5ff) | 0x500, sw + GC_SR_L3 + 0x0c);

	gc1601_sr_update(priv, GC_SR_SRC + 0x0c, 0x4, 0);
	writel(0, sw + GC_SR_HA + 0x04);
	writel((src0 & ~0x20000) | 0x200000, sw + GC_SR_SRC);
	gc1601_sr_update(priv, GC_SR_L3 + 0x04, ~0xf744ff44, 0x07440044);
	gc1601_sr_update(priv, GC_SR_HM, 0x1f, 0x60);
	gc1601_sr_update(priv, GC_SR_HM + 0x40, ~0xff249249, 0x249249);
	gc1601_sr_update(priv, GC_SR_SRC + 0x04, BIT(16), 0);

	/* Isolation: UNI (port 0) and PON (port 1) only reach each other. */
	writel(0xfe, sw + GC_SR_SRC + 0xe0);
	writel(0xfd, sw + GC_SR_SRC + 0xe4);
	/* chipset_mcast_permits(0, 1, 0, 1, 1) and (1, 0, 1, 1, 1) */
	gc1601_sr_update(priv, GC_SR_SRC + 0x68,
			 0x0303000f, BIT(24) | BIT(17) | 0x5);
	/* chipset_set_aging_time(1, 300), as stock */
	gc1601_sr_update(priv, GC_SR_SRC, BIT(16), BIT(16));
	writel(0x367e * 300, sw + GC_SR_SRC + 0x58);
	/* SR_default_configs: L2+0x70 bits 2:0 */
	gc1601_sr_update(priv, GC_SR_L2 + 0x70, 0x7, 0x6);

	gc1601_sr_update(priv, GC_SR_HA + 0x2c, ~0xffffe328, 9000);
	gc1601_sr_update(priv, GC_SR_HA + 0x2c, 0x8000, 0x4000);
	gc1601_uni_attrs_apply(priv);
	return 0;
}

/* chipset_default_flow word 0: flow info 0, queue, egress GEM or UNI. */
#define GC_SR_FLOW_GEM(gem, q)	(0xc0000000 | ((gem) & 0xfff) << 18 | (q) << 9)
#define GC_SR_FLOW_UNI(q)	(0x80000000 | 1 << 18 | (q) << 9)

/*
 * chipset_bypass_mode: flow lookup off, everything from the UNI onto one
 * GEM (default class 1) and everything from the PON to the UNI (class 2),
 * forwarded by the switch without the CPU. Upstream carries no T-CONT and
 * leaves on instance 0.
 */
static void gc1601_sr_bypass(struct gc1601_eth *priv, bool on, u32 gem)
{
	void __iomem *sw = priv->switch_base;

	writel(0, sw + GC_SR_L3 + 0xa4);
	writel(on ? GC_SR_FLOW_GEM(gem, 0) : 0, sw + GC_SR_L3 + 0xa0);
	writel(0, sw + GC_SR_L3 + 0xac);
	writel(on ? GC_SR_FLOW_UNI(0) : 0, sw + GC_SR_L3 + 0xa8);
	writel(on ? 0x30000 : 0, sw + GC_SR_SRC + 0x74);
	writel(on ? 0x03000503 : 0xff, sw + GC_SR_SRC + 0x70);
	writel(on ? 0 : 0xff, sw + GC_SR_SRC + 0x64);
	writel(on ? 0 : 0x1000f, sw + GC_SR_SRC + 0x60);
	writel(on ? 0 : 0x515, sw + GC_SR_L3 + 0x0c);
	gc1601_sr_update(priv, GC_SR_L3, BIT(23), on ? 0 : BIT(23));
	writel(on ? 0xfe : 0, sw + GC_SR_SRC + 0xc0);
	writel(on ? 0xfd : 0, sw + GC_SR_SRC + 0xc4);
}

/* The packet engine is shared with the GPON driver, which reaches it here. */
static struct net_device *gc1601_pe_netdev;
static DEFINE_MUTEX(gc1601_uni_lock);

/* Applied at once when the switch is already set up, for back-to-back A/B. */
static int sr_src8_dec24_set(const char *val, const struct kernel_param *kp)
{
	struct net_device *netdev;
	int ret;

	rtnl_lock();
	ret = param_set_bool(val, kp);
	netdev = READ_ONCE(gc1601_pe_netdev);
	if (!ret && netdev && ((struct gc1601_eth *)netdev_priv(netdev))->sr_ready)
		gc1601_sr_src8_apply(netdev_priv(netdev));
	rtnl_unlock();
	return ret;
}

static const struct kernel_param_ops sr_src8_dec24_ops = {
	.set = sr_src8_dec24_set,
	.get = param_get_bool,
};
module_param_cb(sr_src8_dec24, &sr_src8_dec24_ops, &sr_src8_dec24, 0644);
MODULE_PARM_DESC(sr_src8_dec24, "set switch SRC+0x08 to 0xbd as the 2024-12 stock image does");

static int gc1601_sfu_apply(struct gc1601_eth *priv,
			    const struct gc1601_sfu_flow *flows, unsigned int n);

/*
 * When sfu_queue_zero is set, all upstream SFU flows forward to hardware
 * Queue 0 of their T-CONT (word 0 = 0x02200084 for GEM 256), matching the
 * known-fast 2026-10-12 image and stock firmware default.
 */
static bool sfu_queue_zero;

static int sfu_queue_zero_set(const char *val, const struct kernel_param *kp)
{
	struct net_device *netdev;
	struct gc1601_eth *priv;
	int ret;

	rtnl_lock();
	ret = param_set_bool(val, kp);
	netdev = READ_ONCE(gc1601_pe_netdev);
	if (!ret && netdev) {
		priv = netdev_priv(netdev);
		if (priv->switch_base && priv->sfu_flows)
			gc1601_sfu_apply(priv, priv->sfu_active, priv->sfu_flows);
	}
	rtnl_unlock();
	return ret;
}

static const struct kernel_param_ops sfu_queue_zero_ops = {
	.set = sfu_queue_zero_set,
	.get = param_get_bool,
};
module_param_cb(sfu_queue_zero, &sfu_queue_zero_ops, &sfu_queue_zero, 0644);
MODULE_PARM_DESC(sfu_queue_zero, "force SFU upstream flows to T-CONT hardware queue 0");

/* Indirect L3 table write: index, then the data words, last word first. */
static int gc1601_sr_tbl_write(struct gc1601_eth *priv, u32 table, u32 index,
			       const u32 *w, unsigned int n)
{
	void __iomem *l3 = priv->switch_base + GC_SR_L3;
	u32 done;
	int k;

	writel(table << 22 | index, l3 + 0x14);
	for (k = n - 1; k >= 0; k--)
		writel(w[k], l3 + 0x1c + 4 * k);
	return readl_poll_timeout(l3 + 0x18, done, done == 1, 1, 1000);
}

/*
 * L2+0x180..0x19c: 2-bit protocol actions, as the stock firmware leaves
 * them. The loader's values trap nearly every frame from the UNI to the CPU,
 * ahead of any flow or default class.
 */
static const u32 gc1601_sr_traps[] = {
	0x50555155, 0x95545555, 0x10554150, 0xaa550055,
	0x50555155, 0x95555555, 0x11554150, 0xaa550055,
};

/*
 * chipset_add_cpu_mac, slot 0: frames to the device's own MAC go to the CPU,
 * ahead of any flow, so the device stays reachable when an untagged flow
 * takes the UNI's frames. The slot's mode in L2+0x18c/0x19c bits 25:24 is 1
 * for a slot holding a MAC; the stock 2 is for an empty slot and drops
 * frames to the MAC.
 */
static void gc1601_sfu_cpu_mac(struct gc1601_eth *priv)
{
	const u8 *mac = priv->netdev->dev_addr;

	writel((u32)mac[2] << 24 | mac[3] << 16 | mac[4] << 8 | mac[5],
	       priv->switch_base + GC_SR_L2 + 0xa0);
	writel(mac[0] << 8 | mac[1], priv->switch_base + GC_SR_L2 + 0xa4);
	gc1601_sr_update(priv, GC_SR_L2 + 0x18c, 3 << 24, 1 << 24);
	gc1601_sr_update(priv, GC_SR_L2 + 0x19c, 3 << 24, 1 << 24);
}

/*
 * SR_set_default_rule, reduced to what VLAN services use: parser rule 3
 * (outer VID) and 7 (untagged), enabled in the direction 0 and 1 index
 * lists. Rule 3 matches the stock firmware's table 1 entry.
 */
static int gc1601_sfu_rules(struct gc1601_eth *priv)
{
	static const u32 rule_vid[17] = { [0] = 0xe, [4] = 0x00fff000, [14] = 0x00106000 };
	/* Stock rule 6, "svlan": the outer TCI less CFI, P-bit and VID. */
	static const u32 rule_tci[17] = { [0] = 0xe, [4] = 0x0efff000, [14] = 0x00106000 };
	static const u32 rule_untagged[17] = { [14] = 0x00006000 };
	/* Upstream tries rules 3, 6, 7 as stock does; downstream 3, 7. */
	static const u32 index_up[4] = { 0x7183, 0, 0, 0x00150007 };
	static const u32 index_down[4] = { 0x1c3, 0, 0, 0x00150003 };
	unsigned int i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(gc1601_sr_traps); i++)
		writel(gc1601_sr_traps[i], priv->switch_base + GC_SR_L2 + 0x180 + 4 * i);
	gc1601_sfu_cpu_mac(priv);
	ret = gc1601_sr_tbl_write(priv, 1, 3, rule_vid, 17);
	if (!ret)
		ret = gc1601_sr_tbl_write(priv, 1, 6, rule_tci, 17);
	if (!ret)
		ret = gc1601_sr_tbl_write(priv, 1, 7, rule_untagged, 17);
	if (!ret)
		ret = gc1601_sr_tbl_write(priv, 0, 0, index_up, 4);
	if (!ret)
		ret = gc1601_sr_tbl_write(priv, 0, 1, index_down, 4);
	return ret;
}

/* Indirect HM table write: index and table, then the data words, last first. */
static int gc1601_hm_write(struct gc1601_eth *priv, u32 table, u32 index,
			   const u32 *w, unsigned int n)
{
	void __iomem *hm = priv->switch_base + GC_SR_HM;
	u32 done;
	int k;

	writel(table | index, hm + 0x14);
	for (k = n - 1; k >= 0; k--)
		writel(w[k], hm + 0x1c + 4 * k);
	return readl_poll_timeout(hm + 0x18, done, done == 1, 1, 1000);
}

#define GC_HM_FLOWINFO		0
#define GC_HM_VLANDAT		0x800000
#define GC_HM_CMDDAT		0xc00000
#define GC_HM_SUBDAT		0x1800000
#define GC_HM_CMD_END		BIT(25)
#define GC_HM_CMD_BASE		80	/* first 4-step cmddat list */

enum gc1601_hm_op {
	GC_HM_PUSH,		/* insert the vlandat tag, priority 0 */
	GC_HM_PUSH_COPY,	/* insert it with the priority of the tag below */
	GC_HM_POP,		/* delete the outer tag */
	GC_HM_REPLACE,		/* rewrite the outer tag's VID, keep the rest */
};

/*
 * VLAN header modifications, as the stock SDK compiles them
 * (sdk_create_modification_addtag/deltag/reptag): a flow info entry names a
 * sub data entry, which names a 4-step cmddat list and a vlandat entry. Each
 * list starts at the outer tag (offset 12). Flow info fi uses cmddat list
 * fi - 1 and vlandat fi.
 */
static const u32 gc1601_hm_cmds[][2] = {
	[GC_HM_PUSH]		= { 0x0002000c, 0x0006ba80 },
	[GC_HM_PUSH_COPY]	= { 0x0002000c, 0x0046a280 },
	[GC_HM_POP]		= { 0x0002000c, 0x00090000 },
	[GC_HM_REPLACE]		= { 0x0002000c, 0x0007d480 },
};

static int gc1601_hm_vlan_mod(struct gc1601_eth *priv, u32 fi,
			      enum gc1601_hm_op op, u16 vid)
{
	u32 cmd_addr = GC_HM_CMD_BASE + 4 * (fi - 1);
	u32 vlan_addr = op == GC_HM_POP ? 0 : fi;
	u32 cmds[4] = { gc1601_hm_cmds[op][0], gc1601_hm_cmds[op][1],
			GC_HM_CMD_END, GC_HM_CMD_END };
	u32 sub[2] = { vlan_addr << 26 | cmd_addr << 13, 0 };
	u32 info[3] = { 0, fi << 17, 0 };
	u32 vlan = vid;
	unsigned int i;
	int ret = 0;

	if (vlan_addr)
		ret = gc1601_hm_write(priv, GC_HM_VLANDAT, vlan_addr, &vlan, 1);
	for (i = 0; i < ARRAY_SIZE(cmds) && !ret; i++)
		ret = gc1601_hm_write(priv, GC_HM_CMDDAT, cmd_addr + i, &cmds[i], 1);
	if (!ret)
		ret = gc1601_hm_write(priv, GC_HM_SUBDAT, fi, sub, ARRAY_SIZE(sub));
	if (!ret)
		ret = gc1601_hm_write(priv, GC_HM_FLOWINFO, fi, info, ARRAY_SIZE(info));
	return ret;
}

/* The rewrite a flow direction needs for the service's LAN side, if any. */
static bool gc1601_sfu_flow_op(const struct gc1601_sfu_flow *f, bool up,
			       enum gc1601_hm_op *op, u16 *vid)
{
	*vid = f->vlan_id;
	switch (f->lan) {
	case GC1601_SFU_LAN_UNTAGGED:
		*op = up ? GC_HM_PUSH : GC_HM_POP;
		return true;
	case GC1601_SFU_LAN_TRANSLATE:
		*op = GC_HM_REPLACE;
		if (!up)
			*vid = f->lan_vid;
		return true;
	case GC1601_SFU_LAN_STACKED:
		*op = up ? GC_HM_PUSH_COPY : GC_HM_POP;
		return true;
	default:
		return false;
	}
}

/* Flow info of a slot's direction: 1 + 2 * slot up, 2 + 2 * slot down. */
static u32 gc1601_sfu_flow_fi(unsigned int slot, bool up)
{
	return up ? 1 + 2 * slot : 2 + 2 * slot;
}

/* chipset_create_flow binding a modification: flow info index split. */
static void gc1601_sfu_flow_mod(u32 *w, u32 fi)
{
	w[0] |= (fi & 0x3f) << 26;
	w[1] |= 0x400 | fi >> 6;
}

/*
 * chipset_create_flow for a VLAN-only match: parser rule 3 (outer VID) or 7
 * (untagged). The ingress key is the UNI (1) upstream or the GEM downstream,
 * split over word 3 [31:28] and word 4 [7:0]; the VID over word 4 [31:24]
 * and word 5 [7:0]. Upstream forwards to the GEM on the service's T-CONT,
 * downstream to the UNI. Upstream matches the LAN side of the service; a
 * P-bit flow uses parser rule 6 and the whole TCI (P-bit 15:13, VID 11:0).
 */
static void gc1601_sfu_flow_words(u32 *w, bool up, const struct gc1601_sfu_flow *f,
				  unsigned int slot)
{
	u32 key = up ? 1 : f->gem_port;
	bool untagged = f->untagged || (up && f->lan == GC1601_SFU_LAN_UNTAGGED);
	u16 vid = f->vlan_id;
	enum gc1601_hm_op op;
	u16 mod_vid;

	if (up && (f->lan == GC1601_SFU_LAN_TRANSLATE ||
		   f->lan == GC1601_SFU_LAN_STACKED))
		vid = f->lan_vid;

	memset(w, 0, 15 * sizeof(*w));
	if (up) {
		u8 q = sfu_queue_zero ? 0 : (f->queue & 7);

		/* Vendor flow encoder: queue in bits 5:3, bit 6 when nonzero. */
		w[0] = BIT(25) | (f->gem_port & 0xfff) << 13 |
			(f->tcont & 0x3f) << 7 | (q & 7) << 3 |
			(q ? BIT(6) : 0) | 4;
		w[1] = 0x1000;
		w[3] = 0x0040c1c0;
	} else {
		w[0] = BIT(25) | 1 << 13 | 4;
		w[1] = 0x1c00;
		w[3] = 0x0040c180;
		w[4] = BIT(23);
	}
	w[3] |= (key & 0xf) << 28;
	w[4] |= key >> 4 & 0xff;
	if (untagged) {
		w[4] |= 7 << 14;
	} else if (up && f->pcp_match) {
		u16 tci = (f->pcp & 7) << 13 | (vid & 0xfff);

		w[3] = (w[3] & ~0x0007c000) | (5 << 14);
		w[4] |= (tci & 0xff) << 24 | 6 << 14 | 0x100;
		w[5] = tci >> 8;
	} else {
		w[4] |= (vid & 0xff) << 24 | 3 << 14 | 0x100;
		w[5] = vid >> 8;
	}
	if (gc1601_sfu_flow_op(f, up, &op, &mod_vid))
		gc1601_sfu_flow_mod(w, gc1601_sfu_flow_fi(slot, up));
}

#define GC_SR_FLOW_SLOTS	8	/* table 8, two per service */
#define GC_SR_FLOW_VALID	BIT(22)	/* word 3 */

/* Indirect L3 table read into w; bit 27 of the index word selects a read. */
static int gc1601_sr_tbl_read(struct gc1601_eth *priv, u32 table, u32 index,
			      u32 *w, unsigned int n)
{
	void __iomem *l3 = priv->switch_base + GC_SR_L3;
	unsigned int k;
	u32 done;
	int ret;

	writel(BIT(27) | table << 22 | index, l3 + 0x14);
	ret = readl_poll_timeout(l3 + 0x18, done, done == 1, 1, 1000);
	for (k = 0; k < n && !ret; k++)
		w[k] = readl(l3 + 0x1c + 4 * k);
	return ret;
}

/* local_crc32: MSB first, no reflection, initial and final value 0. */
static u32 gc1601_sr_crc(const u8 *data, unsigned int len, u32 poly)
{
	u32 crc = 0;
	int bit;

	while (len--) {
		crc ^= (u32)*data++ << 24;
		for (bit = 0; bit < 8; bit++)
			crc = crc & BIT(31) ? crc << 1 ^ poly : crc << 1;
	}
	return crc;
}

/*
 * FUN_00039bbc: the hash key is entry bits 119..471 (from word 3 bit 23,
 * bit 123 left out), packed into 45 bytes and byte-reversed. Checked against
 * the four default flows the stock firmware keeps in table 2.
 */
static u32 gc1601_sr_flow_hash(const u32 *w, u32 poly)
{
	u8 key[45] = { };
	unsigned int i;

	for (i = 0; i < 353; i++) {
		unsigned int src = 119 + i;

		if (src != 123 && (w[src / 32] >> (src % 32) & 1))
			key[44 - i / 8] |= BIT(i % 8);
	}
	return gc1601_sr_crc(key, sizeof(key), poly);
}

/*
 * FUN_00048b10: hashed tables 2..5 hold 256, 128, 64 and 64 flows, at the
 * key's CRC-32 (0x04c11db7) or CRC-32C (0x1edc6f41) modulo the size. An
 * entry goes to the first table whose slot is free.
 */
static int gc1601_sr_hash_insert(struct gc1601_eth *priv, const u32 *w)
{
	static const struct { u32 table, size, poly; } tables[] = {
		{ 2, 256, 0x04c11db7 }, { 3, 128, 0x1edc6f41 },
		{ 4, 64, 0x04c11db7 },  { 5, 64, 0x1edc6f41 },
	};
	struct {
		u32 index;
		u32 words[4];
	} occupied[ARRAY_SIZE(tables)];
	u32 old[15];
	unsigned int t;
	int ret;

	for (t = 0; t < ARRAY_SIZE(tables); t++) {
		u32 index = gc1601_sr_flow_hash(w, tables[t].poly) % tables[t].size;

		ret = gc1601_sr_tbl_read(priv, tables[t].table, index, old, ARRAY_SIZE(old));
		if (ret)
			return ret;
		if (old[3] & GC_SR_FLOW_VALID) {
			occupied[t].index = index;
			occupied[t].words[0] = old[0];
			occupied[t].words[1] = old[3];
			occupied[t].words[2] = old[4];
			occupied[t].words[3] = old[5];
			continue;
		}
		ret = gc1601_sr_tbl_write(priv, tables[t].table, index, w, 15);
		if (ret)
			return ret;
		priv->sfu_hashed[priv->sfu_hashed_count++] = tables[t].table << 16 | index;
		return 0;
	}
	dev_err(priv->dev, "SFU hash full: new=%08x/%08x/%08x/%08x\n",
		w[0], w[3], w[4], w[5]);
	for (t = 0; t < ARRAY_SIZE(tables); t++)
		dev_err(priv->dev,
			"SFU hash table=%u index=%u old=%08x/%08x/%08x/%08x\n",
			tables[t].table, occupied[t].index,
			occupied[t].words[0], occupied[t].words[1],
			occupied[t].words[2], occupied[t].words[3]);
	return -ENOSPC;
}

static int gc1601_sfu_apply(struct gc1601_eth *priv,
			    const struct gc1601_sfu_flow *flows, unsigned int n)
{
	u32 w[15] = { };
	unsigned int i;
	int ret;

	n = min_t(unsigned int, n, GC1601_SFU_FLOWS);
	if (n && !priv->sr_ready) {
		ret = gc1601_sr_hw_init(priv);
		if (ret)
			return ret;
		priv->sr_ready = true;
	}
	if (n && !priv->sfu_rules) {
		ret = gc1601_sfu_rules(priv);
		if (ret)
			return ret;
		priv->sfu_rules = true;
	}

	for (i = 0; i < 2 * n; i++) {
		bool up = !(i & 1);
		enum gc1601_hm_op op;
		u16 vid;

		if ((up && flows[i / 2].down_only) ||
		    (!up && flows[i / 2].up_only) ||
		    !gc1601_sfu_flow_op(&flows[i / 2], up, &op, &vid))
			continue;
		ret = gc1601_hm_vlan_mod(priv, gc1601_sfu_flow_fi(i / 2, up), op, vid);
		if (ret)
			return ret;
	}

	/* Take the hashed flows of the last set out first. */
	memset(w, 0, sizeof(w));
	for (i = 0; i < priv->sfu_hashed_count; i++) {
		ret = gc1601_sr_tbl_write(priv, priv->sfu_hashed[i] >> 16,
					  priv->sfu_hashed[i] & 0xffff, w, ARRAY_SIZE(w));
		if (ret)
			return ret;
	}
	priv->sfu_hashed_count = 0;

	/* The first services in the linear table 8, the rest hashed. */
	for (i = 0; i < GC_SR_FLOW_SLOTS || i < 2 * n; i++) {
		bool empty = i >= 2 * n || (!(i & 1) && flows[i / 2].down_only) ||
			     ((i & 1) && flows[i / 2].up_only);

		if (!empty)
			gc1601_sfu_flow_words(w, !(i & 1), &flows[i / 2], i / 2);
		else
			memset(w, 0, sizeof(w));
		if (i < GC_SR_FLOW_SLOTS)
			ret = gc1601_sr_tbl_write(priv, 8, i, w, ARRAY_SIZE(w));
		else if (!empty)
			ret = gc1601_sr_hash_insert(priv, w);
		if (ret && !empty) {
			dev_err(priv->dev, "SFU flow %u %s GEM %u VLAN %u failed: %d\n",
				i / 2, i & 1 ? "down" : "up",
				flows[i / 2].gem_port, flows[i / 2].vlan_id, ret);
			return ret;
		}
		if (ret)
			return ret;
	}

	/*
	 * As the stock firmware runs: flow lookup on, and whatever no flow
	 * matches goes to the CPU through default class 9 (queue 7). That keeps
	 * the device itself and services without a flow on the pon0 path.
	 */
	if (priv->sr_ready) {
		gc1601_sr_bypass(priv, false, 0);
		writel(0x00050003, priv->switch_base + GC_SR_SRC + 0x60);
		writel(0x3, priv->switch_base + GC_SR_SRC + 0x64);
		writel(0x03000503, priv->switch_base + GC_SR_SRC + 0x70);
		writel(0x00030000, priv->switch_base + GC_SR_SRC + 0x74);
		writel(0, priv->switch_base + GC_SR_L3 + 0xec);
		writel(7 << 9, priv->switch_base + GC_SR_L3 + 0xe8);
		writel(0x545, priv->switch_base + GC_SR_L3 + 0x0c);
	}
	priv->sfu_flows = n;
	return 0;
}

/* chipset_PMU_left_iptr_get: packet buffers left in the free list. */
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
 * chipset_qmu_aful_set: the queue manager's almost-full level, bits 3:0 of
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

int gc1601_pe_sfu_set(const struct gc1601_sfu_flow *flows, unsigned int n)
{
	struct net_device *netdev = READ_ONCE(gc1601_pe_netdev);
	struct gc1601_eth *priv;
	unsigned int i;
	int ret;

	if (!netdev)
		return -ENODEV;
	priv = netdev_priv(netdev);
	if (!priv->switch_base)
		return -ENODEV;

	rtnl_lock();
	ret = gc1601_sfu_apply(priv, flows, n);
	if (ret) {
		int restore;

		restore = gc1601_sfu_apply(priv, priv->sfu_active, priv->sfu_flows);
		if (restore)
			dev_err(priv->dev, "SFU rollback failed: %d\n", restore);
	} else if (n) {
		memcpy(priv->sfu_active, flows,
		       min_t(unsigned int, n, GC1601_SFU_FLOWS) * sizeof(*flows));
	}
	rtnl_unlock();
	if (ret) {
		dev_err(priv->dev, "SFU offload failed: %d\n", ret);
		return ret;
	}
	for (i = 0; i < priv->sfu_flows; i++) {
		char how[64];
		int len;

		len = scnprintf(how, sizeof(how), ", %s",
				flows[i].down_only ? "downstream only" :
				flows[i].up_only ? "upstream only" : "bidirectional");
		if (!flows[i].down_only) {
			len += scnprintf(how + len, sizeof(how) - len,
					 ", queue %u", flows[i].queue);
			if (flows[i].pcp_match)
				scnprintf(how + len, sizeof(how) - len,
					  ", P-bit %u", flows[i].pcp);
		}
		if (flows[i].untagged)
			dev_info(priv->dev, "SFU offload: untagged <-> GEM %u, T-CONT %u%s\n",
				 flows[i].gem_port, flows[i].tcont, how);
		else if (flows[i].lan == GC1601_SFU_LAN_UNTAGGED)
			dev_info(priv->dev, "SFU offload: LAN untagged <-> VLAN %u, GEM %u, T-CONT %u%s\n",
				 flows[i].vlan_id, flows[i].gem_port, flows[i].tcont, how);
		else if (flows[i].lan == GC1601_SFU_LAN_TRANSLATE)
			dev_info(priv->dev, "SFU offload: LAN VLAN %u <-> VLAN %u, GEM %u, T-CONT %u%s\n",
				 flows[i].lan_vid, flows[i].vlan_id,
				 flows[i].gem_port, flows[i].tcont, how);
		else if (flows[i].lan == GC1601_SFU_LAN_STACKED)
			dev_info(priv->dev, "SFU offload: LAN VLAN %u <-> VLAN %u.%u, GEM %u, T-CONT %u%s\n",
				 flows[i].lan_vid, flows[i].vlan_id, flows[i].lan_vid,
				 flows[i].gem_port, flows[i].tcont, how);
		else
			dev_info(priv->dev, "SFU offload: VLAN %u <-> GEM %u, T-CONT %u%s\n",
				 flows[i].vlan_id, flows[i].gem_port, flows[i].tcont, how);
	}
	if (!priv->sfu_flows)
		dev_info(priv->dev, "SFU offload off\n");
	return 0;
}
EXPORT_SYMBOL_GPL(gc1601_pe_sfu_set);

/*
 * Frames that arrived after the loader stopped polling are still counted in
 * their container. Clearing the rings under them leaves counts without a
 * descriptor, which keep the level IRQ asserted. Acknowledge and release them
 * first, as PMU_fetch_pkt would have.
 */
static u32 gc1601_rx_pending(struct gc1601_eth *priv, unsigned int c);

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

static void gc1601_rx_drain_inherited(struct gc1601_eth *priv)
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
			if (buf_id < GC1601_BUFFER_COUNT)
				writel(buf_id, priv->base[GC1601_RX] + GC_RX_RELEASE_BUF);
			pending--;
			drained++;
		}
	}
	if (drained)
		dev_info(priv->dev, "dropped %u frames left over from the loader\n", drained);

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

	dev_info(priv->dev,
		 "handoff %s: gmac=%08x/%08x/%08x pm=%08x/%08x/%08x/%08x/%08x mtu=%08x ctrl=%08x\n",
		 stage, readl(priv->base[GC1601_GMAC] + GC_GMAC_CFG),
		 readl(priv->base[GC1601_GMAC] + GC_GMAC_IER),
		 readl(priv->base[GC1601_GMAC] + GC_GMAC_MACCR),
		 readl(pm + GC_BUF_READ_CLEAR), readl(pm + GC_BUF_CTRL),
		 readl(pm + GC_BUF_FREE_LIST_BASE),
		 readl(pm + GC_BUF_PKT_POOL_BASE), readl(pm + GC_BUF_POOL_CFG),
		 readl(pm + GC_BUF_MTU), readl(pm + GC_BUF_PM_CTRL));
	dev_info(priv->dev,
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

static int gc1601_hw_setup(struct gc1601_eth *priv)
{
	u32 mode = readl(priv->base[GC1601_BUFFERS] + GC_BUF_READ_CLEAR);
	bool cold;
	int ret;

	/* An engine the loader never initialised still has reset-default bases. */
	cold = readl(priv->base[GC1601_BUFFERS] + GC_BUF_FREE_LIST_BASE) !=
		GC1601_FREE_LIST_PHYS;
	if (cold) {
		ret = gc1601_cold_prepare(priv);
		if (ret) {
			dev_err(priv->dev, "Cold init needs the switch/queue/scheduler banks\n");
			return ret;
		}
	}

	if (cold) {
		__be16 *free_list = phys_to_virt(GC1601_FREE_LIST_PHYS);
		u32 val;
		int ret;
		unsigned int j;

		writel(U32_MAX, priv->base[GC1601_BUFFERS] + GC_BUF_PM_SRAM_INIT);
		ret = readl_poll_timeout_atomic(priv->base[GC1601_BUFFERS] + GC_BUF_PM_SRAM_STATUS,
						val, val != 0, 1, 1000);
		if (ret) {
			dev_err(priv->dev, "PM SRAM init timed out\n");
			return ret;
		}
		priv->pm_sram_status = val;

		writel(U32_MAX, priv->base[GC1601_BUFFERS] + GC_BUF_OS_SRAM_INIT);
		ret = readl_poll_timeout_atomic(priv->base[GC1601_BUFFERS] + GC_BUF_OS_SRAM_STATUS,
						val, val != 0, 1, 1000);
		if (ret) {
			dev_err(priv->dev, "OS SRAM init timed out\n");
			return ret;
		}
		priv->os_sram_status = val;
		dev_info(priv->dev, "SRAM init: PM %08x, OS %08x\n",
			 priv->pm_sram_status, priv->os_sram_status);
		writel(readl(priv->sched_base) | 0x8, priv->sched_base);

		for (j = 0; j < GC1601_BUFFER_COUNT; j++)
			free_list[j] = cpu_to_be16(j);
		dma_sync_single_for_device(priv->dev, GC1601_FREE_LIST_PHYS,
					   GC1601_BUFFER_COUNT * sizeof(__be16),
					   DMA_TO_DEVICE);
	}

	writel(mode | GC_BUF_READ_CLEAR_EN,
	       priv->base[GC1601_BUFFERS] + GC_BUF_READ_CLEAR);
	if (!(readl(priv->base[GC1601_BUFFERS] + GC_BUF_READ_CLEAR) &
	      GC_BUF_READ_CLEAR_EN)) {
		dev_err(priv->dev, "Failed to enable completion read-clear mode\n");
		return -EIO;
	}
	/* Discard inherited completions before publishing our descriptor ring. */
	priv->tx_completion_drained = readl(priv->base[GC1601_TX_RING] +
					 GC_TX_COMPLETIONS) & 0xffff;

	gc1601_rx_drain_inherited(priv);
	memset(priv->rx_ring, 0, GC1601_RX_CONTAINERS * GC1601_RX_RING_STRIDE);

	/* Invalidate entire packet pool cache at bringup so no stale lines linger */
	dma_sync_single_for_cpu(priv->dev, GC1601_PKT_POOL_PHYS,
				GC1601_BUFFER_COUNT * GC1601_BUFFER_SIZE,
				DMA_FROM_DEVICE);

	memset(priv->tx_ring, 0, GC1601_RING_SIZE * sizeof(struct gc1601_desc));
	memset(priv->rx_consumer, 0, sizeof(priv->rx_consumer));
	priv->rx_synced = 0;
	memset(priv->rx_late_since, 0, sizeof(priv->rx_late_since));
	memset(priv->rx_phantom_since, 0, sizeof(priv->rx_phantom_since));
	priv->rx_next_container = 0;
	priv->rx_stalled = 0;
	priv->tx_producer = 0;
	priv->tx_consumer = 0;
	priv->tx_pending = 0;
	priv->tx_detected_slot = -1;
	priv->rx_first_slot = -1;

	/* The hardware TX head persists across close; every open re-measures it. */
	priv->tx_probe_sent = 0;
	priv->tx_probe_aligned = false;
	priv->tx_auto_scheduled = false;
	priv->tx_replicated = false;
	priv->tx_probe_completed_base = priv->tx_completed_total;

	dma_wmb();

	writel(0x10, priv->base[GC1601_BUFFERS] + GC_BUF_CTRL);
	/* Stock value; the loader's 2 held the CPU path to 31 Mbit/s upstream. */
	writel(0xa, priv->base[GC1601_BUFFERS] + GC_BUF_PM_CTRL);
	/* chipset_pmu_mtu_set(2000) from PMU_init; the loader leaves 1800. */
	writel((readl(priv->base[GC1601_BUFFERS] + GC_BUF_MTU) & ~0x3fff) | 2000,
	       priv->base[GC1601_BUFFERS] + GC_BUF_MTU);
	/*
	 * OS_hw_init, run by stock right after the PMU init: T-CONT shaper
	 * (bit 0), T-CONT scheduler (bit 1) and UNI shaper (bit 4) on, plus
	 * the bits stock never names, over reset-default tables; +0x28 as
	 * stock (the reset value is 0x400).
	 */
	if (priv->os_base) {
		writel(readl(priv->os_base) | 0x8df, priv->os_base);
		writel(0x7cf, priv->os_base + 0x28);
	}
	ret = gc1601_descriptor_setup(priv);
	if (ret)
		return ret;
	ret = gc1601_tcont_queue_setup(priv);
	if (ret)
		return ret;
	gc1601_port_vlan_setup(priv);
	ret = gc1601_cvrd_verify(priv);
	if (ret)
		return ret;

	writel(GC1601_FREE_LIST_PHYS, priv->base[GC1601_BUFFERS] + GC_BUF_FREE_LIST_BASE);
	writel(GC1601_PKT_POOL_PHYS, priv->base[GC1601_BUFFERS] + GC_BUF_PKT_POOL_BASE);
	writel(priv->rx_ring_dma, priv->base[GC1601_BUFFERS] + GC_BUF_RX_RING_BASE);
	writel(priv->tx_ring_dma, priv->base[GC1601_TX_RING] + GC_TX_RING_BASE);

	writel(0x30000800, priv->base[GC1601_BUFFERS] + GC_BUF_POOL_CFG);
	writel(0x60, priv->base[GC1601_RX] + GC_RX_PARTITION);
	writel(GC_RX_CTRL_VAL, priv->base[GC1601_RX] + GC_RX_CTRL);

	dev_dbg(priv->dev, "RX setup: partition=%08x (readback %08x), rx_ctrl=%08x (readback %08x)\n",
		0x60, readl(priv->base[GC1601_RX] + GC_RX_PARTITION),
		GC_RX_CTRL_VAL,
		readl(priv->base[GC1601_RX] + GC_RX_CTRL));

	if (cold) {
		gc1601_cold_finish(priv);
		dev_info(priv->dev, "Cold init done: partition readback %08x, tx cfg 00/08/0c/14=%08x/%08x/%08x/%08x\n",
			 readl(priv->base[GC1601_RX] + GC_RX_PARTITION),
			 readl(priv->base[GC1601_TX_CONFIG]),
			 readl(priv->base[GC1601_TX_CONFIG] + 0x08),
			 readl(priv->base[GC1601_TX_CONFIG] + 0x0c),
			 readl(priv->base[GC1601_TX_CONFIG] + 0x14));
	}

	if (priv->base[GC1601_BUFFER_CONTROL])
		writel(0x0800001f, priv->base[GC1601_BUFFER_CONTROL]);
	if (priv->base[GC1601_BUFFER_ENABLE])
		writel(1, priv->base[GC1601_BUFFER_ENABLE]);

	dev_dbg(priv->dev, "SRAM status at end of setup: PM %08x OS %08x\n",
		readl(priv->base[GC1601_BUFFERS] + GC_BUF_PM_SRAM_STATUS),
		readl(priv->base[GC1601_BUFFERS] + GC_BUF_OS_SRAM_STATUS));
	return 0;
}

static void gc1601_irq_mask(struct gc1601_eth *priv, bool enable)
{
	u32 mask = enable ? ~(GC_INTR_RX | GC_INTR_TX) : U32_MAX;

	writel(mask, priv->base[GC1601_BUFFERS] + GC_BUF_INTR_MASK);
	writel(mask, priv->base[GC1601_BUFFERS] + GC_BUF_EVENT_MASK);
}

static void gc1601_hw_start(struct gc1601_eth *priv)
{
	unsigned long flags;
	u32 acir = 0;
	int ret;

	spin_lock_irqsave(&priv->irq_lock, flags);
	priv->running = true;
	gc1601_irq_mask(priv, true);
	spin_unlock_irqrestore(&priv->irq_lock, flags);

	/* Enable RX gate */
	writel(1, priv->base[GC1601_RX] + GC_RX_ENABLE);

	/* Set GMAC configuration (1000M full duplex default) */
	spin_lock_irqsave(&priv->irq_lock, flags);
	writel(GC_GMAC_CFG_BASE | GC_GMAC_CFG_FULL_DUPLEX |
	       (priv->uni_disabled ? 0 : GC_GMAC_CFG_TX_EN | GC_GMAC_CFG_RX_EN),
	       priv->base[GC1601_GMAC] + GC_GMAC_CFG);
	spin_unlock_irqrestore(&priv->irq_lock, flags);

	/* Align GMAC auxiliary control registers matching vendor & U-Boot */
	writel(0x0000fffe, priv->base[GC1601_GMAC] + GC_GMAC_IER);
	writel(0x00140001, priv->base[GC1601_GMAC] + GC_GMAC_ERCR);
	/*
	 * The loader polls ACIR bit 2 for 4096 reads and ignores the result.
	 * On the G7 ERCR reads back 0 and ACIR never sets, so keep it short.
	 */
	ret = readl_poll_timeout_atomic(priv->base[GC1601_GMAC] + GC_GMAC_ACIR,
					acir, (acir & 4) != 0, 1, 100);
	if (ret)
		dev_dbg(priv->dev, "GMAC ACIR bit 2 not set (acir=%08x)\n", acir);
	writel(0x80000001, priv->base[GC1601_GMAC] + GC_GMAC_MACCR);

	dev_dbg(priv->dev, "GMAC started: cfg=%08x ier=%08x ercr=%08x acir=%08x maccr=%08x\n",
		readl(priv->base[GC1601_GMAC] + GC_GMAC_CFG),
		readl(priv->base[GC1601_GMAC] + GC_GMAC_IER),
		readl(priv->base[GC1601_GMAC] + GC_GMAC_ERCR),
		readl(priv->base[GC1601_GMAC] + GC_GMAC_ACIR),
		readl(priv->base[GC1601_GMAC] + GC_GMAC_MACCR));
}

static void gc1601_hw_stop(struct gc1601_eth *priv)
{
	unsigned long flags;

	spin_lock_irqsave(&priv->irq_lock, flags);
	priv->running = false;
	gc1601_irq_mask(priv, false);
	spin_unlock_irqrestore(&priv->irq_lock, flags);

	/* Quiesce GMAC and RX gate */
	writel(GC_GMAC_CFG_BASE, priv->base[GC1601_GMAC] + GC_GMAC_CFG);
	writel(0, priv->base[GC1601_RX] + GC_RX_ENABLE);
}

/* Pipelining needs a measured hardware ring position; until then one in flight. */
static u32 gc1601_tx_depth(struct gc1601_eth *priv)
{
	if (!priv->tx_probe_aligned)
		return 1;
	return GC1601_TX_DEPTH;
}

static struct net_device *gc1601_pon_dev;	/* stopped while the shared ring is full */

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

	/* Let the last probe leave the MAC before sampling the final count. */
	if (!priv->tx_probe_aligned && !priv->tx_auto_scheduled &&
	    priv->tx_probe_sent == GC1601_SLOT_PROBE_COUNT && !priv->tx_pending) {
		priv->tx_auto_scheduled = true;
		schedule_delayed_work(&priv->tx_auto_work, msecs_to_jiffies(20));
	}

	qdepth = gc1601_tx_depth(priv);
	if (netif_running(priv->netdev) && netif_queue_stopped(priv->netdev) &&
	    priv->tx_pending < qdepth &&
	    (priv->tx_probe_aligned ||
	     priv->tx_probe_sent < GC1601_SLOT_PROBE_COUNT))
		netif_wake_queue(priv->netdev);

	pon_dev = READ_ONCE(gc1601_pon_dev);
	if (pon_dev && netif_queue_stopped(pon_dev) && priv->tx_pending < qdepth)
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

/* The packet engine is shared with the GPON MAC; OMCI frames go to its driver. */
static DEFINE_SPINLOCK(gc1601_omci_lock);
static gc1601_omci_rx_fn gc1601_omci_rx;
static void *gc1601_omci_ctx;
static gc1601_pon_rx_fn gc1601_pon_rx;
static void *gc1601_pon_ctx;

/* Upstream frame for the GPON MAC: GEM port, T-CONT instance and queue. */
struct gc1601_pon_meta {
	u16 gem_port;
	u8 tcont;
	u8 queue;
	bool omci;
};

/*
 * Queue one frame. Without @pon it goes to the GE port. With it, it goes
 * upstream as PMU_xmit builds it for G-PON: minfo = GEM port, qid/tid =
 * queue and T-CONT instance, out/dir zero and direct set; an OMCI frame also
 * sets the omci bit and is not padded to 64 bytes.
 *
 * Returns 0 when queued, -EBUSY when the ring is full (the skb is kept) and
 * any other error when the frame was dropped (the skb is freed and counted).
 */
static int gc1601_xmit(struct sk_buff *skb, struct net_device *netdev,
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
	if (probe && priv->tx_probe_sent >= GC1601_SLOT_PROBE_COUNT) {
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
		priv->tx_busy_events++;
		netif_stop_queue(netdev);
		/* Stopped under tx_lock, so the completion that frees a slot wakes it. */
		if (pon && !pon->omci && gc1601_pon_dev)
			netif_stop_queue(gc1601_pon_dev);
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
		spin_unlock_irqrestore(&priv->tx_lock, flags);
		netdev->stats.tx_dropped++;
		dev_kfree_skb_any(skb);
		return -EIO;
	}

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
	desc.word[1] = cpu_to_le32(0x1200);
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

static netdev_tx_t gc1601_start_xmit(struct sk_buff *skb, struct net_device *netdev)
{
	return gc1601_xmit(skb, netdev, NULL) == -EBUSY ? NETDEV_TX_BUSY : NETDEV_TX_OK;
}

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

int gc1601_pe_pon_register(struct net_device *dev, gc1601_pon_rx_fn fn, void *ctx)
{
	int ret = 0;

	spin_lock_bh(&gc1601_omci_lock);
	if (gc1601_pon_rx) {
		ret = -EBUSY;
	} else {
		gc1601_pon_rx = fn;
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
	gc1601_pon_rx = NULL;
	gc1601_pon_ctx = NULL;
	WRITE_ONCE(gc1601_pon_dev, NULL);
	spin_unlock_bh(&gc1601_omci_lock);
	/* TX completion reads the device with interrupts off. */
	synchronize_net();
}
EXPORT_SYMBOL_GPL(gc1601_pe_pon_unregister);

/*
 * Queue an upstream data frame on a GEM port. Consumes @skb unless the
 * shared ring is full: then -EBUSY, the caller's queue is already stopped
 * and it is woken when a slot frees.
 */
int gc1601_pe_pon_xmit(struct sk_buff *skb, u16 gem_port, u8 tcont, u8 queue)
{
	struct gc1601_pon_meta pon = {
		.gem_port = gem_port, .tcont = tcont, .queue = queue,
	};
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
	ret = gc1601_xmit(skb, netdev, &pon);
	netif_tx_unlock_bh(netdev);
	return ret;
}
EXPORT_SYMBOL_GPL(gc1601_pe_pon_xmit);

/*
 * chipset_flush_tcont_buff: drop what is queued on a T-CONT's eight queues,
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

/*
 * UNI attributes the OLT sets over OMCI (G.988 pause time, MAC ageing and
 * learning depth, UNI bridge port traffic descriptors), on the GC1601
 * blocks the vendor SDK uses for them. None of these registers has been
 * checked on hardware yet, so they stay off until uni_omci_attrs=1; the
 * requested values are always logged.
 */
static bool uni_omci_attrs;

#define GC_GMAC_PAUSE_RX	0x90	/* honour received pause frames */
#define GC_GMAC_PAUSE_TX	0x70	/* quanta << 16 | send pause << 1 */
#define GC_SR_CAR		0x1c000	/* CAR (rate limiter) bank, vendor DAT_000917cc */
#define GC_SR_AGE_UNIT		0x367e	/* chipset_set_aging_time: register per second */
#define GC_SR_LEARN_UNI		BIT(9)	/* chipset_mactbl_limit port 1 */

/* Indirect table write: index at +0x14, data from +0x1c, +0x18 reads 1 when done. */
static int gc1601_tbl_write(void __iomem *base, u32 index, const u32 *w, unsigned int n)
{
	unsigned int i;
	u32 done;

	writel(index, base + 0x14);
	for (i = 0; i < n; i++)
		writel(w[i], base + 0x1c + 4 * i);
	return readl_poll_timeout(base + 0x18, done, done == 1, 0, 100);
}

/* chipset_car_sl_port_set: CAR table B entry for the UNI, rate in kbit/s. */
static int gc1601_uni_police(struct gc1601_eth *priv, u32 kbps)
{
	void __iomem *car = priv->switch_base + GC_SR_CAR;
	u32 w[3] = {}, fill, base;

	if (kbps) {
		kbps = min(kbps, 1000000U);
		/* car_init: block on, 20M token reference, fill time 0x30d4 */
		writel(readl(car) | 1, car);
		writel(20000000, car + 0x0c);
		writel(0x30d4 - 1, car + 0x34);
		writel((readl(car + 0x120) & ~0x6) | 1, car + 0x120);
		base = readl(car + 0x28);
		fill = kbps * 0x20 + (base < 0x809 ? 0x10807 : base + 0xffff);
		w[0] = kbps << 3 | 1;
		w[1] = kbps >> 29 | fill << 5;
		w[2] = (fill >> 24 & 0xf) >> 3;
	}
	return gc1601_tbl_write(car, 1, w, ARRAY_SIZE(w));
}

/* chipset_update_shaper, UNI branch: egress shaper of UNI port 0, rate in kbit/s. */
static int gc1601_uni_shape(struct gc1601_eth *priv, u32 kbps)
{
	u32 max = 0x7ffffff, fill = 0x1fffff;
	int ret;

	if (!priv->os_base)
		return -ENODEV;
	if (kbps) {
		max = clamp(kbps / 4, 1000U, 0x1fffffU);
		fill = clamp(kbps * 8 / 25, 1U, 0x1fffffU);
	}
	ret = gc1601_tbl_write(priv->os_base, 0x2400000, &max, 1);	/* port_shap_max */
	if (!ret)
		ret = gc1601_tbl_write(priv->os_base, 0x2000000, &fill, 1);	/* port_shap_fill */
	return ret;
}

/* All zero gives the defaults: no pause, ageing 300 s, no MAC limit, no rate limits. */
static void gc1601_uni_attrs_write(struct gc1601_eth *priv, const struct gc1601_uni_attrs *a)
{
	void __iomem *gmac = priv->base[GC1601_GMAC];
	u32 ageing = a->ageing_s ?: 300;
	u16 learn = min_t(u16, a->learn_limit, 1023);

	if (!priv->switch_base)
		return;

	writel(!!a->pause_quanta, gmac + GC_GMAC_PAUSE_RX);
	writel((u32)a->pause_quanta << 16 | (a->pause_quanta ? BIT(1) : 0),
	       gmac + GC_GMAC_PAUSE_TX);
	gc1601_sr_update(priv, GC_SR_CAR + 0x2c, BIT(1), a->pause_quanta ? BIT(1) : 0);

	ageing = min(ageing, U32_MAX / GC_SR_AGE_UNIT);
	gc1601_sr_update(priv, GC_SR_SRC, BIT(16), BIT(16));
	writel(ageing * GC_SR_AGE_UNIT, priv->switch_base + GC_SR_SRC + 0x58);
	gc1601_sr_update(priv, GC_SR_SRC, GC_SR_LEARN_UNI, learn ? GC_SR_LEARN_UNI : 0);
	writel(learn, priv->switch_base + GC_SR_SRC + 0x84);

	if (gc1601_uni_shape(priv, a->down_kbps) || gc1601_uni_police(priv, a->up_kbps))
		dev_warn(priv->dev, "UNI rate limit tables did not answer\n");
}

static void gc1601_uni_attrs_apply(struct gc1601_eth *priv)
{
	if (uni_omci_attrs)
		gc1601_uni_attrs_write(priv, &priv->uni_attrs);
}

/* Writes take effect at once: the OLT's values on 1, the defaults back on 0. */
static int uni_omci_attrs_set(const char *val, const struct kernel_param *kp)
{
	static const struct gc1601_uni_attrs defaults;
	bool was = uni_omci_attrs;
	struct gc1601_eth *priv;
	int ret;

	mutex_lock(&gc1601_uni_lock);
	ret = param_set_bool(val, kp);
	if (!ret && was != uni_omci_attrs && gc1601_pe_netdev) {
		priv = netdev_priv(gc1601_pe_netdev);
		if (READ_ONCE(priv->running)) {
			gc1601_uni_attrs_write(priv, uni_omci_attrs ? &priv->uni_attrs : &defaults);
			dev_info(priv->dev, "UNI attributes from the OLT %s\n",
				 uni_omci_attrs ? "applied" : "removed, defaults restored");
		}
	}
	mutex_unlock(&gc1601_uni_lock);
	return ret;
}

static const struct kernel_param_ops uni_omci_attrs_ops = {
	.set = uni_omci_attrs_set,
	.get = param_get_bool,
};
module_param_cb(uni_omci_attrs, &uni_omci_attrs_ops, &uni_omci_attrs, 0644);
MODULE_PARM_DESC(uni_omci_attrs, "apply the OLT's UNI pause, MAC table and rate attributes");

int gc1601_pe_set_uni_attrs(const struct gc1601_uni_attrs *attrs)
{
	struct gc1601_eth *priv;
	int ret = 0;

	mutex_lock(&gc1601_uni_lock);
	if (!gc1601_pe_netdev) {
		ret = -ENODEV;
		goto out;
	}
	priv = netdev_priv(gc1601_pe_netdev);
	priv->uni_attrs = *attrs;
	dev_info(priv->dev,
		 "UNI attributes from the OLT: pause %u quanta, ageing %u s, MAC limit %u, down %u kbit/s, up %u kbit/s%s\n",
		 attrs->pause_quanta, attrs->ageing_s, attrs->learn_limit,
		 attrs->down_kbps, attrs->up_kbps,
		 uni_omci_attrs ? "" : " (not applied: uni_omci_attrs=0)");
	/* Interface start applies them too; the switch init resets the ageing. */
	if (READ_ONCE(priv->running))
		gc1601_uni_attrs_apply(priv);
out:
	mutex_unlock(&gc1601_uni_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(gc1601_pe_set_uni_attrs);

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
static unsigned int pon_desc_log;
module_param(pon_desc_log, uint, 0644);
MODULE_PARM_DESC(pon_desc_log, "log the raw RX descriptors of the next N PON frames");

/* NAPI context: hand a downstream PON data frame to the GPON driver. */
static bool gc1601_pon_deliver(u16 gem_port, const u8 *data, unsigned int len,
			       struct napi_struct *napi)
{
	bool taken = false;

	spin_lock(&gc1601_omci_lock);
	if (gc1601_pon_rx) {
		gc1601_pon_rx(gc1601_pon_ctx, napi, gem_port, data, len);
		taken = true;
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
			dev_info(priv->dev, "container %d starts at slot %d\n", c, s);
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
		 * PMU_fetch_pkt: the container count is descriptors written and
		 * not yet ACKed, in ring order from the consumer. Read them in
		 * order and never skip one. The ring is uncached, as the vendor's
		 * ioremap of its reserved DDR is: a cached ring lost descriptors
		 * to 32-byte line clears and stale lines. A consumed slot is
		 * zeroed, so a zero length means "not written yet".
		 */
		if (!(priv->rx_synced & BIT(container))) {
			if (!gc1601_rx_sync(priv, container)) {
				priv->rx_empty_desc++;
				priv->rx_stalled |= BIT(container);
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
				priv->rx_stalled |= BIT(container);
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
			if (unlikely(READ_ONCE(pon_desc_log))) {
				WRITE_ONCE(pon_desc_log, pon_desc_log - 1);
				dev_info(priv->dev, "pon rx desc %08x %08x %08x %08x in %lu minfo %lu len %u type %04x %*phN\n",
					 word0, word1, word2, word3, FIELD_GET(GC1601_RX_IN, word0),
					 FIELD_GET(GC1601_RX_MINFO, word0), len,
					 get_unaligned_be16(pkt_virt + offset + 2 * ETH_ALEN),
					 min_t(int, len - 2 * ETH_ALEN, 8), pkt_virt + offset + 2 * ETH_ALEN);
			}
			if (gc1601_pon_deliver(FIELD_GET(GC1601_RX_MINFO, word0),
					       pkt_virt + offset, len, napi))
				goto consume;
			priv->rx_unrouted++;
			priv->netdev->stats.rx_dropped++;
			goto consume;
		}

		skb = napi_alloc_skb(napi, len + 2);
		if (skb) {
			skb_reserve(skb, 2);
			memcpy(skb_put(skb, len), pkt_virt + offset, len);
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
		 * 1. Acknowledge descriptor status and count. PMU_fetch_pkt names
		 * the bank (container & 7) and copies descriptor byte 5 bit 4,
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
	if (gc1601_rx_any_pending(priv) || priv->tx_pending > 0) {
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
		mod_timer(&priv->poll_timer, jiffies +
			  max_t(unsigned long, 1,
				msecs_to_jiffies(priv->irq_poll_only ||
						 priv->rx_stalled ? 1 : 50)));
}

static int gc1601_eth_open(struct net_device *netdev)
{
	struct gc1601_eth *priv = netdev_priv(netdev);
	struct phy_device *phydev;
	int ret;

	if (priv->phy_node) {
		phydev = of_phy_connect(netdev, priv->phy_node,
					gc1601_adjust_link, 0,
					priv->phy_interface);
		if (!phydev) {
			dev_err(priv->dev, "Could not connect to PHY\n");
			return -ENODEV;
		}
	} else {
		dev_err(priv->dev, "No PHY node specified in device tree\n");
		return -ENODEV;
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
	phy_start(netdev->phydev);
	netif_start_queue(netdev);

	timer_setup(&priv->poll_timer, gc1601_poll_timer, 0);
	mod_timer(&priv->poll_timer, jiffies + msecs_to_jiffies(50));

	return 0;

err_phy_disconnect:
	phy_disconnect(netdev->phydev);
	return ret;
}

static int gc1601_eth_stop(struct net_device *netdev)
{
	struct gc1601_eth *priv = netdev_priv(netdev);
	unsigned long flags;
	unsigned int i;
	u16 pending;

	netif_tx_disable(netdev);
	cancel_work_sync(&priv->tx_probe_work);
	cancel_delayed_work_sync(&priv->tx_auto_work);

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
	phy_stop(netdev->phydev);
	gc1601_hw_stop(priv);
	synchronize_irq(priv->irq);
	napi_disable(&priv->napi);

	if (priv->irq_poll_only)
		enable_irq(priv->irq);
	free_irq(priv->irq, priv);
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
	return 0;
}

static const struct net_device_ops gc1601_netdev_ops = {
	.ndo_open		= gc1601_eth_open,
	.ndo_stop		= gc1601_eth_stop,
	.ndo_start_xmit		= gc1601_start_xmit,
	.ndo_set_mac_address	= gc1601_set_mac_address,
	.ndo_validate_addr	= eth_validate_addr,
	.ndo_eth_ioctl		= phy_do_ioctl_running,
};

static void gc1601_get_drvinfo(struct net_device *netdev, struct ethtool_drvinfo *info)
{
	strscpy(info->driver, "gc1601-eth", sizeof(info->driver));
}

static const char gc1601_ethtool_stats_keys[][ETH_GSTRING_LEN] = {
	"rx_crc_errors",
	"rx_invalid_len_errors",
	"rx_invalid_buf_errors",
	"rx_resync_events",
	"tx_busy_events",
};

static void gc1601_get_ethtool_stats(struct net_device *netdev,
				     struct ethtool_stats *stats, u64 *data)
{
	struct gc1601_eth *priv = netdev_priv(netdev);

	data[0] = priv->rx_crc_errors;
	data[1] = priv->rx_invalid_len_errors;
	data[2] = priv->rx_invalid_buf_errors;
	data[3] = priv->rx_resync_events;
	data[4] = priv->tx_busy_events;
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
	.get_strings		= gc1601_get_strings,
	.get_sset_count		= gc1601_get_sset_count,
	.get_ethtool_stats	= gc1601_get_ethtool_stats,
};

static ssize_t omci_stats_show(struct device *dev, struct device_attribute *attr,
			       char *buf)
{
	struct gc1601_eth *priv = netdev_priv(dev_get_drvdata(dev));

	return sysfs_emit(buf,
		"rx=%llu rx_dropped=%llu tx=%llu descriptors=%llu invalid=%llu\n"
		"last_words=%08x %08x %08x %08x\n",
		priv->rx_omci, priv->rx_omci_dropped, priv->tx_omci,
		priv->rx_omci_desc, priv->rx_omci_invalid,
		READ_ONCE(priv->rx_omci_last_words[0]),
		READ_ONCE(priv->rx_omci_last_words[1]),
		READ_ONCE(priv->rx_omci_last_words[2]),
		READ_ONCE(priv->rx_omci_last_words[3]));
}
static DEVICE_ATTR_RO(omci_stats);

static ssize_t diagnostics_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct gc1601_eth *priv = netdev_priv(dev_get_drvdata(dev));

	return sysfs_emit(buf,
		"pm_sram=%08x os_sram=%08x tx_detected_slot=%d tx_detected_w3=%08x rx_first_slot=%d tx_producer=%u tx_consumer=%u rx_consumer=%u tx_pending=%u rx_alloc_failures=%u tx_completion_excess=%u tx_completion_reads=%u tx_completed_total=%u tx_completion_last=%u tx_completion_drained=%u read_clear=%08x\n",
		priv->pm_sram_status, priv->os_sram_status,
		priv->tx_detected_slot,
		priv->tx_detected_w3, priv->rx_first_slot,
		READ_ONCE(priv->tx_producer), READ_ONCE(priv->tx_consumer),
		READ_ONCE(priv->rx_consumer[0]), READ_ONCE(priv->tx_pending),
		READ_ONCE(priv->rx_alloc_failures), READ_ONCE(priv->tx_completion_excess),
		READ_ONCE(priv->tx_completion_reads), READ_ONCE(priv->tx_completed_total),
		READ_ONCE(priv->tx_completion_last), READ_ONCE(priv->tx_completion_drained),
		readl(priv->base[GC1601_BUFFERS] + GC_BUF_READ_CLEAR));
}
static DEVICE_ATTR_RO(diagnostics);

static ssize_t tx_slots_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	struct gc1601_eth *priv = netdev_priv(dev_get_drvdata(dev));
	unsigned long flags;
	unsigned int i;
	ssize_t len = 0;

	spin_lock_irqsave(&priv->tx_lock, flags);
	dma_rmb();
	for (i = 0; i < 8; i++) {
		struct gc1601_desc *desc = &priv->tx_ring[i];

		len += sysfs_emit_at(buf, len, "%u: %08x %08x %08x %08x\n", i,
			le32_to_cpu(READ_ONCE(desc->word[0])),
			le32_to_cpu(READ_ONCE(desc->word[1])),
			le32_to_cpu(READ_ONCE(desc->word[2])),
			le32_to_cpu(READ_ONCE(desc->word[3])));
	}
	spin_unlock_irqrestore(&priv->tx_lock, flags);
	return len;
}
static DEVICE_ATTR_RO(tx_slots);

static ssize_t tx_config_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct gc1601_eth *priv = netdev_priv(dev_get_drvdata(dev));
	void __iomem *base = priv->base[GC1601_TX_CONFIG];

	/* Skip doorbells and completion counters: reads must not consume state. */
	return sysfs_emit(buf,
		"dma_base=%08x hw_base=%08x cfg00=%08x cfg08=%08x cfg0c=%08x cfg10=%08x cfg14=%08x probe_sent=%u\n",
		(u32)priv->tx_ring_dma,
		readl(priv->base[GC1601_TX_RING] + GC_TX_RING_BASE),
		readl(base), readl(base + 8), readl(base + 0xc),
		readl(base + 0x10), readl(base + 0x14),
		READ_ONCE(priv->tx_probe_sent));
}
static DEVICE_ATTR_RO(tx_config);

static ssize_t tx_resume_slot_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct gc1601_eth *priv = netdev_priv(dev_get_drvdata(dev));
	unsigned long flags;
	int slot;

	spin_lock_irqsave(&priv->tx_lock, flags);
	slot = priv->tx_probe_aligned ? priv->tx_resume_slot : -1;
	spin_unlock_irqrestore(&priv->tx_lock, flags);
	return sysfs_emit(buf, "%d\n", slot);
}

/* One-shot switch from probe replication to sequential TX at @slot. */
static int gc1601_tx_resume(struct gc1601_eth *priv, unsigned int slot)
{
	struct net_device *netdev = priv->netdev;
	unsigned long flags;
	unsigned int i;
	int ret;

	ASSERT_RTNL();
	spin_lock_irqsave(&priv->tx_lock, flags);
	ret = -EBUSY;
	if (!netif_running(netdev) || !priv->running ||
	    !netif_queue_stopped(netdev) || priv->tx_pending ||
	    priv->tx_probe_aligned ||
	    priv->tx_probe_sent != GC1601_SLOT_PROBE_COUNT ||
	    priv->tx_completed_total - priv->tx_probe_completed_base !=
	    GC1601_SLOT_PROBE_COUNT ||
	    priv->tx_completion_excess)
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
		dev_dbg(priv->dev, "Sequential TX resumed at slot %u\n", slot);
		netif_wake_queue(netdev);
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
static void gc1601_tx_probe_work(struct work_struct *work)
{
	struct gc1601_eth *priv = container_of(work, struct gc1601_eth,
					       tx_probe_work);
	struct net_device *netdev = priv->netdev;
	struct sk_buff *skb;
	netdev_tx_t ret;
	int i;

	for (i = 0; i < GC1601_SLOT_PROBE_COUNT; i++) {
		skb = netdev_alloc_skb(netdev, ETH_ZLEN);
		if (!skb)
			return;
		skb_put_zero(skb, ETH_ZLEN);

		netif_tx_lock_bh(netdev);
		if (!netif_running(netdev) || priv->tx_probe_aligned ||
		    priv->tx_probe_sent >= GC1601_SLOT_PROBE_COUNT)
			ret = NETDEV_TX_BUSY;
		else
			ret = gc1601_start_xmit(skb, netdev);
		netif_tx_unlock_bh(netdev);

		if (ret != NETDEV_TX_OK) {
			dev_kfree_skb_any(skb);
			return;
		}
	}
}

static void gc1601_tx_auto_work(struct work_struct *work)
{
	struct gc1601_eth *priv = container_of(to_delayed_work(work),
					       struct gc1601_eth, tx_auto_work);
	unsigned long flags;
	int slot;

	/* Trylock: interface stop holds RTNL while cancelling this work. */
	if (!rtnl_trylock()) {
		schedule_delayed_work(&priv->tx_auto_work, msecs_to_jiffies(10));
		return;
	}
	spin_lock_irqsave(&priv->tx_lock, flags);
	priv->tx_probe_octets[GC1601_SLOT_PROBE_COUNT] =
		readl(priv->base[GC1601_GMAC] + GC_GMAC_TX_OCTETS);
	slot = gc1601_tx_probe_slot(priv);
	spin_unlock_irqrestore(&priv->tx_lock, flags);

	dev_printk(slot < 0 ? KERN_WARNING : KERN_DEBUG, priv->dev,
		   "TX probe byte deltas %u %u %u %u: next slot %d\n",
		   priv->tx_probe_octets[1] - priv->tx_probe_octets[0],
		   priv->tx_probe_octets[2] - priv->tx_probe_octets[1],
		   priv->tx_probe_octets[3] - priv->tx_probe_octets[2],
		   priv->tx_probe_octets[4] - priv->tx_probe_octets[3], slot);
	if (slot >= 0 && READ_ONCE(priv->running) &&
	    gc1601_tx_resume(priv, slot))
		dev_err(priv->dev, "Automatic TX resume at slot %d rejected\n", slot);
	rtnl_unlock();
}
static DEVICE_ATTR_RO(tx_resume_slot);

static ssize_t rx_snapshot_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	static const u16 mac_counter_offsets[] = {
		0x714, 0x718, 0x724, 0x728, 0x72c, 0x730, 0x734, 0x738,
		0x740, 0x744, 0x778, 0x780, 0x784, 0x78c, 0x790, 0x794,
		0x7a4, 0x7a8, 0x7ac, 0x7b0, 0x7b4, 0x7b8, 0x7bc, 0x7c0,
		0x81c,
	};
	struct gc1601_eth *priv = netdev_priv(dev_get_drvdata(dev));
	void __iomem *buffers = priv->base[GC1601_BUFFERS];
	void __iomem *rx = priv->base[GC1601_RX];
	u16 cons = READ_ONCE(priv->rx_consumer[0]) % GC1601_RING_SIZE;
	struct gc1601_desc *desc = &priv->rx_ring[cons];
	u32 pending = readl(priv->base[GC1601_RX_PENDING]);
	ssize_t len = 0;
	unsigned int i;

	/* Approximate snapshot; no DMA invalidation or PMU completion reads. */
	len += sysfs_emit_at(buf, len,
		"running=%u napi_state=%lx timer_pending=%u irq_mask=%08x irq_last_status=%08x\n",
		READ_ONCE(priv->running), READ_ONCE(priv->napi.state),
		timer_pending(&priv->poll_timer), readl(buffers + GC_BUF_INTR_MASK),
		READ_ONCE(priv->irq_last_status));
	len += sysfs_emit_at(buf, len,
		"irq_poll_only=%u irq_empty_streak=%u event_mask=%08x\n",
		READ_ONCE(priv->irq_poll_only), READ_ONCE(priv->irq_empty_streak),
		readl(buffers + GC_BUF_EVENT_MASK));
	len += sysfs_emit_at(buf, len,
		"irq_calls=%u irq_empty_calls=%u irq_duplicate_calls=%u pending_raw=%08x\n"
		"storm_jiffies=%lu storm_pending=%08x storm_mask=%08x storm_event_mask=%08x storm_consumer=%u\n",
		READ_ONCE(priv->irq_calls), READ_ONCE(priv->irq_empty_calls),
		READ_ONCE(priv->irq_duplicate_calls),
		pending,
		READ_ONCE(priv->irq_storm_jiffies), READ_ONCE(priv->irq_storm_pending),
		READ_ONCE(priv->irq_storm_mask), READ_ONCE(priv->irq_storm_event_mask),
		READ_ONCE(priv->irq_storm_consumer));
	len += sysfs_emit_at(buf, len,
		"pending_now=%u pending_last=%u consumer=%u polls=%u empty_desc=%u ack_count=%u release_count=%u\n",
		pending & 0xffff,
		READ_ONCE(priv->rx_pending_last), cons, READ_ONCE(priv->rx_poll_calls),
		READ_ONCE(priv->rx_empty_desc), READ_ONCE(priv->rx_ack_count),
		READ_ONCE(priv->rx_release_count));
	len += sysfs_emit_at(buf, len,
		"last_slot=%u last_words=%08x %08x %08x %08x\n",
		READ_ONCE(priv->rx_last_slot), READ_ONCE(priv->rx_last_words[0]),
		READ_ONCE(priv->rx_last_words[1]), READ_ONCE(priv->rx_last_words[2]),
		READ_ONCE(priv->rx_last_words[3]));
	len += sysfs_emit_at(buf, len,
		"cached_current_words=%08x %08x %08x %08x\n",
		le32_to_cpu(READ_ONCE(desc->word[0])), le32_to_cpu(READ_ONCE(desc->word[1])),
		le32_to_cpu(READ_ONCE(desc->word[2])), le32_to_cpu(READ_ONCE(desc->word[3])));
	len += sysfs_emit_at(buf, len,
		"gmac=%08x ier=%08x maccr=%08x ercr=%08x acir=%08x rx_enable=%08x rx_partition=%08x rx_ctrl=%08x\n",
		readl(priv->base[GC1601_GMAC] + GC_GMAC_CFG),
		readl(priv->base[GC1601_GMAC] + GC_GMAC_IER),
		readl(priv->base[GC1601_GMAC] + GC_GMAC_MACCR),
		readl(priv->base[GC1601_GMAC] + GC_GMAC_ERCR),
		readl(priv->base[GC1601_GMAC] + GC_GMAC_ACIR),
		readl(rx + GC_RX_ENABLE), readl(rx + GC_RX_PARTITION),
		readl(rx + GC_RX_CTRL));
	len += sysfs_emit_at(buf, len,
		"buf_ctrl=%08x pm_ctrl=%08x pool_cfg=%08x free_base=%08x pool_base=%08x rx_base=%08x\n",
		readl(buffers + GC_BUF_CTRL), readl(buffers + GC_BUF_PM_CTRL),
		readl(buffers + GC_BUF_POOL_CFG), readl(buffers + GC_BUF_FREE_LIST_BASE),
		readl(buffers + GC_BUF_PKT_POOL_BASE), readl(buffers + GC_BUF_RX_RING_BASE));
	len += sysfs_emit_at(buf, len,
		"crc_errors=%llu invalid_len=%llu invalid_buf=%llu resync=%llu alloc_failures=%u\n",
		(unsigned long long)READ_ONCE(priv->rx_crc_errors),
		(unsigned long long)READ_ONCE(priv->rx_invalid_len_errors),
		(unsigned long long)READ_ONCE(priv->rx_invalid_buf_errors),
		(unsigned long long)READ_ONCE(priv->rx_resync_events),
		READ_ONCE(priv->rx_alloc_failures));
	len += sysfs_emit_at(buf, len, "late_landed=%u late_stalled=%u holes=%u phantom_acks=%u\n",
			     READ_ONCE(priv->rx_late_landed),
			     READ_ONCE(priv->rx_late_stalled),
			     READ_ONCE(priv->rx_holes),
			     READ_ONCE(priv->rx_phantom_acks));
	if (priv->switch_base) {
		void __iomem *l2 = priv->switch_base + GC_SR_L2;

		len += sysfs_emit_at(buf, len,
			"sr_a0=%08x sr_a4=%08x sr_188=%08x sr_18c=%08x sr_198=%08x sr_19c=%08x gmac_300=%08x gmac_304=%08x\n",
			readl(l2 + 0xa0),
			readl(l2 + 0xa4),
			readl(l2 + 0x188),
			readl(l2 + 0x18c),
			readl(l2 + 0x198),
			readl(l2 + 0x19c),
			readl(priv->base[GC1601_GMAC] + 0x300),
			readl(priv->base[GC1601_GMAC] + 0x304));
	}
	/* Vendor SMAC statistics reads; retain raw offsets until fields are decoded. */
	for (i = 0; i < ARRAY_SIZE(mac_counter_offsets); i++) {
		len += sysfs_emit_at(buf, len, "mac_%03x=%u%s",
			mac_counter_offsets[i],
			readl(priv->base[GC1601_GMAC] + mac_counter_offsets[i]),
			(i % 5 == 4 || i + 1 == ARRAY_SIZE(mac_counter_offsets)) ? "\n" : " ");
	}
	return len;
}
static DEVICE_ATTR_RO(rx_snapshot);

static ssize_t queue_snapshot_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	static const u16 queue_bases[] = { 0x140, 0x180, 0x188 };
	struct gc1601_eth *priv = netdev_priv(dev_get_drvdata(dev));
	u32 outmax, outcnt, inmax, incnt, cfg[3], allcnt;
	unsigned int bank, queue, index;
	ssize_t len = 0;
	int ret;

	/* Setup is the only other software user of this table interface. */
	if (!rtnl_trylock())
		return -EBUSY;
	if (!READ_ONCE(priv->running)) {
		len = -ENETDOWN;
		goto out;
	}
	if (priv->queue_read_failed) {
		len = sysfs_emit(buf, "blocked_after_timeout=1; reboot before retry\n");
		goto out;
	}
	len = sysfs_emit(buf, "setup_mismatches=%u\n", priv->cvrd_mismatches);
	for (bank = 0; bank < ARRAY_SIZE(queue_bases); bank++) {
		for (queue = 0; queue < 8; queue++) {
			index = queue_bases[bank] + queue;
			ret = gc1601_queue_read(priv, GC_CVRD_OUTMAX, index, &outmax, 1);
			if (ret)
				goto failed;
			ret = gc1601_queue_read(priv, GC_CVRD_OUTCNT, index, &outcnt, 1);
			if (ret)
				goto failed;
			if (!bank) {
				ret = gc1601_queue_read(priv, GC_CVRD_INMAX, index, &inmax, 1);
				if (ret)
					goto failed;
				ret = gc1601_queue_read(priv, GC_CVRD_INCNT, index, &incnt, 1);
				if (ret)
					goto failed;
				ret = gc1601_queue_read(priv, GC_CVRD_ALLCFG, index, cfg, 3);
				if (ret)
					goto failed;
				ret = gc1601_queue_read(priv, GC_CVRD_ALLCNT, index, &allcnt, 1);
				if (ret)
					goto failed;
				len += sysfs_emit_at(buf, len,
					"index=%03x inmax=%08x incnt=%08x outmax=%08x outcnt=%08x cfg=%08x,%08x,%08x allcnt=%08x | out %u/%u(%u/%u) in %u/%u(%u/%u) all %u(%u)\n",
					index, inmax, incnt, outmax, outcnt,
					cfg[0], cfg[1], cfg[2], allcnt,
					GC_CVRD_LO(outmax), GC_CVRD_HI(outmax),
					GC_CVRD_LO(outcnt), GC_CVRD_HI(outcnt),
					GC_CVRD_LO(inmax), GC_CVRD_HI(inmax),
					GC_CVRD_LO(incnt), GC_CVRD_HI(incnt),
					cfg[0] & 0xffff, allcnt & 0xffff);
			} else {
				len += sysfs_emit_at(buf, len,
					"index=%03x outmax=%08x outcnt=%08x | out %u/%u(%u/%u)\n",
					index, outmax, outcnt,
					GC_CVRD_LO(outmax), GC_CVRD_HI(outmax),
					GC_CVRD_LO(outcnt), GC_CVRD_HI(outcnt));
			}
		}
	}
	goto out;
failed:
	len += sysfs_emit_at(buf, len, "index=%03x error=%d; further reads blocked\n",
			     index, ret);
out:
	rtnl_unlock();
	return len;
}
static DEVICE_ATTR_RO(queue_snapshot);

static ssize_t gc1601_emit_id_ranges(char *buf, ssize_t len, const char *name,
				     const unsigned long *map)
{
	unsigned int start, end;

	len += sysfs_emit_at(buf, len, "%s distinct=%u ranges=", name,
			     bitmap_weight(map, GC1601_BUFFER_COUNT));
	for_each_set_bitrange(start, end, map, GC1601_BUFFER_COUNT)
		len += sysfs_emit_at(buf, len, " %u-%u", start, end - 1);
	return len + sysfs_emit_at(buf, len, "\n");
}

static ssize_t buffer_ids_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct gc1601_eth *priv = netdev_priv(dev_get_drvdata(dev));
	ssize_t len;

	len = gc1601_emit_id_ranges(buf, 0, "rx", priv->rx_id_seen);
	return gc1601_emit_id_ranges(buf, len, "tx", priv->tx_id_seen);
}
static DEVICE_ATTR_RO(buffer_ids);

/* CPU view of the loader free-list arena; no hardware registers are touched. */
static ssize_t free_list_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct gc1601_eth *priv = netdev_priv(dev_get_drvdata(dev));
	const __be16 *list = phys_to_virt(GC1601_FREE_LIST_PHYS);
	ssize_t len = 0;
	unsigned int i;

	dma_sync_single_for_cpu(priv->dev, GC1601_FREE_LIST_PHYS,
				GC1601_BUFFER_COUNT * sizeof(__be16), DMA_FROM_DEVICE);
	for (i = 0; i < GC1601_BUFFER_COUNT; i++)
		len += sysfs_emit_at(buf, len, "%s%04x",
				     i % 16 ? " " : (i ? "\n" : ""),
				     be16_to_cpu(list[i]));
	return len + sysfs_emit_at(buf, len, "\n");
}
static DEVICE_ATTR_RO(free_list);

static ssize_t ingress_snapshot_show(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	struct gc1601_eth *priv = netdev_priv(dev_get_drvdata(dev));
	void __iomem *base = priv->base[GC1601_STATISTICS];
	u32 credit, done;
	unsigned int index, word;
	ssize_t len;
	int ret;

	if (!rtnl_trylock())
		return -EBUSY;
	if (!READ_ONCE(priv->running)) {
		len = -ENETDOWN;
		goto out;
	}
	credit = readl(priv->base[GC1601_TX_SPACE]);
	len = sysfs_emit(buf, "left_iptr_raw=%08x left_iptr=%u read_clear=%u\n",
			 credit, credit & 0x2ff,
			 !!(readl(priv->base[GC1601_BUFFERS] + GC_BUF_READ_CLEAR) &
			    GC_BUF_READ_CLEAR_EN));
	if (priv->ingress_read_failed) {
		len += sysfs_emit_at(buf, len, "blocked_after_timeout=1; reboot before retry\n");
		goto out;
	}
	/* Vendor STT_tbl_dr_get: 16 rows, 12 raw words; reads may clear counters. */
	for (index = 0; index < 16; index++) {
		writel(0, base + GC_DESC_TABLE_DONE);
		writel(GC_DESC_TABLE_READ | BIT(22) | index,
		       base + GC_DESC_TABLE_INDEX);
		ret = readl_poll_timeout(base + GC_DESC_TABLE_DONE, done,
					 done == 1, 1, 1000);
		if (ret) {
			priv->ingress_read_failed = true;
			len += sysfs_emit_at(buf, len,
				"dr=%u error=%d; further reads blocked\n", index, ret);
			goto out;
		}
		len += sysfs_emit_at(buf, len, "dr=%u words=", index);
		for (word = 0; word < 12; word++)
			len += sysfs_emit_at(buf, len, "%08x%s",
				readl(base + GC_DESC_TABLE_DATA0 + word * sizeof(u32)),
				word == 11 ? "\n" : ",");
	}
out:
	rtnl_unlock();
	return len;
}
static DEVICE_ATTR_RO(ingress_snapshot);

static ssize_t stats_index_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct gc1601_eth *priv = netdev_priv(dev_get_drvdata(dev));

	return sysfs_emit(buf, "%u\n", READ_ONCE(priv->stats_index));
}

static ssize_t stats_index_store(struct device *dev,
				 struct device_attribute *attr,
				 const char *buf, size_t count)
{
	struct gc1601_eth *priv = netdev_priv(dev_get_drvdata(dev));
	unsigned int index;
	int ret;

	ret = kstrtouint(buf, 0, &index);
	if (ret)
		return ret;
	if (index >= GC_STT_STATS_COUNT)
		return -EINVAL;
	WRITE_ONCE(priv->stats_index, index);
	return count;
}
static DEVICE_ATTR_RW(stats_index);

static ssize_t stats_snapshot_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct gc1601_eth *priv = netdev_priv(dev_get_drvdata(dev));
	void __iomem *base = priv->base[GC1601_STATISTICS];
	unsigned int index = READ_ONCE(priv->stats_index);
	u32 done;
	ssize_t len;
	int ret, word;

	if (!rtnl_trylock())
		return -EBUSY;
	if (!READ_ONCE(priv->running)) {
		len = -ENETDOWN;
		goto out;
	}
	if (priv->stats_read_failed) {
		len = -EIO;
		goto out;
	}

	/* Vendor STT_tbl_st_get: selector 0x08000000, then 14 raw words. */
	writel(0, base + GC_DESC_TABLE_DONE);
	writel(GC_DESC_TABLE_READ | index, base + GC_DESC_TABLE_INDEX);
	ret = readl_poll_timeout(base + GC_DESC_TABLE_DONE, done,
				 done == 1, 1, 1000);
	if (ret) {
		priv->stats_read_failed = true;
		len = sysfs_emit(buf, "index=%u error=%d; reboot before retry\n",
				 index, ret);
		goto out;
	}
	len = sysfs_emit(buf, "index=%u words=", index);
	for (word = 0; word < GC_STT_STATS_WORDS; word++)
		len += sysfs_emit_at(buf, len, "%08x%s",
			readl(base + GC_DESC_TABLE_DATA0 + word * sizeof(u32)),
			word == GC_STT_STATS_WORDS - 1 ? "\n" : ",");
out:
	rtnl_unlock();
	return len;
}
static DEVICE_ATTR_RO(stats_snapshot);

static ssize_t sfu_snapshot_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct gc1601_eth *priv = netdev_priv(dev_get_drvdata(dev));
	void __iomem *l3;
	ssize_t len = 0;
	unsigned int i, count;
	u32 w[15];
	int ret;

	if (!rtnl_trylock())
		return -EBUSY;
	if (!priv->switch_base || !priv->running) {
		len = sysfs_emit(buf, "switch unavailable\n");
		goto out;
	}

	l3 = priv->switch_base + GC_SR_L3;
	len += sysfs_emit_at(buf, len,
		"ready=%u rules=%u services=%u hashed=%u lookup=%08x fallback=%08x class9=%08x\n",
		priv->sr_ready, priv->sfu_rules, priv->sfu_flows,
		priv->sfu_hashed_count, readl(l3), readl(l3 + 0x0c),
		readl(l3 + 0xe8));
	len += sysfs_emit_at(buf, len, "src=%08x/%08x/%08x/%08x src8=%08x\n",
		readl(priv->switch_base + GC_SR_SRC + 0x60),
		readl(priv->switch_base + GC_SR_SRC + 0x64),
		readl(priv->switch_base + GC_SR_SRC + 0x70),
		readl(priv->switch_base + GC_SR_SRC + 0x74),
		readl(priv->switch_base + GC_SR_SRC + 0x08));
	if (priv->sfu_read_failed) {
		len += sysfs_emit_at(buf, len, "table reads blocked after timeout\n");
		goto out;
	}

	count = min(2 * priv->sfu_flows, (unsigned int)GC_SR_FLOW_SLOTS);
	for (i = 0; i < count + priv->sfu_hashed_count; i++) {
		u32 table = i < count ? 8 : priv->sfu_hashed[i - count] >> 16;
		u32 index = i < count ? i : priv->sfu_hashed[i - count] & 0xffff;

		ret = gc1601_sr_tbl_read(priv, table, index, w, ARRAY_SIZE(w));
		if (ret) {
			priv->sfu_read_failed = true;
			len += sysfs_emit_at(buf, len,
				"table=%u index=%u error=%d; further reads blocked\n",
				table, index, ret);
			break;
		}
		len += sysfs_emit_at(buf, len,
			"table=%u index=%u direction=%s valid=%u words=%08x,%08x,%08x,%08x,%08x\n",
			table, index, i & 1 ? "down" : "up",
			!!(w[3] & GC_SR_FLOW_VALID), w[0], w[1], w[3], w[4], w[5]);
	}
out:
	rtnl_unlock();
	return len;
}
static DEVICE_ATTR_RO(sfu_snapshot);

static struct attribute *gc1601_eth_attrs[] = {
	&dev_attr_omci_stats.attr,
	&dev_attr_diagnostics.attr,
	&dev_attr_tx_slots.attr,
	&dev_attr_tx_config.attr,
	&dev_attr_tx_resume_slot.attr,
	&dev_attr_rx_snapshot.attr,
	&dev_attr_queue_snapshot.attr,
	&dev_attr_ingress_snapshot.attr,
	&dev_attr_stats_index.attr,
	&dev_attr_stats_snapshot.attr,
	&dev_attr_sfu_snapshot.attr,
	&dev_attr_buffer_ids.attr,
	&dev_attr_free_list.attr,
	NULL
};

static const struct attribute_group gc1601_eth_group = {
	.attrs = gc1601_eth_attrs,
};

static int gc1601_eth_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct net_device *netdev;
	struct gc1601_eth *priv;
	unsigned int i;
	int ret;

	netdev = alloc_etherdev(sizeof(struct gc1601_eth));
	if (!netdev)
		return -ENOMEM;

	SET_NETDEV_DEV(netdev, dev);
	platform_set_drvdata(pdev, netdev);

	priv = netdev_priv(netdev);
	priv->netdev = netdev;
	priv->dev = dev;
	spin_lock_init(&priv->tx_lock);
	spin_lock_init(&priv->irq_lock);
	INIT_DELAYED_WORK(&priv->tx_auto_work, gc1601_tx_auto_work);
	INIT_WORK(&priv->tx_probe_work, gc1601_tx_probe_work);
	priv->tx_detected_slot = -1;
	priv->rx_first_slot = -1;

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

	/* Scheduler bank: the cold init sets bit 3 as U-Boot does. */
	priv->sched_base = devm_ioremap(dev, 0x90de8000, 0x4);
	if (!priv->sched_base)
		dev_warn(dev, "Failed to map scheduler registers\n");

	/* Switch engine and queue config: SFU offload and the loader-equivalent cold init. */
	priv->switch_base = devm_ioremap(dev, 0x90f80000, 0x20000);
	priv->os_base = devm_ioremap(dev, 0x90de0000, 0x30);
	priv->queue_base = devm_ioremap(dev, 0x90dd8000, 0x30);
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
	netdev->max_mtu = GC1601_BUFFER_SIZE - ETH_HLEN - ETH_FCS_LEN;

	netif_napi_add(netdev, &priv->napi, gc1601_rx_poll);

	ret = register_netdev(netdev);
	if (ret) {
		dev_err(dev, "Failed to register netdevice\n");
		goto err_free_tx_ring;
	}
	ret = device_add_group(dev, &gc1601_eth_group);
	if (ret) {
		unregister_netdev(netdev);
		goto err_free_tx_ring;
	}

	dev_info(dev, "GC1601 Ethernet netdev %s initialized (irq %d, MAC %pM)\n",
		 netdev->name, priv->irq, netdev->dev_addr);
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
	device_remove_group(&pdev->dev, &gc1601_eth_group);
	unregister_netdev(netdev);
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
