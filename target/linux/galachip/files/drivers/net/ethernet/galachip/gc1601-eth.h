/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * GalaChip GC1601 packet engine: shared definitions
 *
 * Copyright (C) 2026 Ifran Ali <khanu.i.i.k.2000@gmail.com>
 */

#ifndef GC1601_ETH_H
#define GC1601_ETH_H

#include <linux/bitfield.h>
#include <linux/debugfs.h>
#include <linux/dsa/gc1601.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/etherdevice.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/netdevice.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_mdio.h>
#include <linux/of_net.h>
#include <linux/phy.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/rtnetlink.h>
#include <linux/seq_file.h>
#include <linux/sizes.h>
#include <linux/spinlock.h>
#include <linux/timer.h>
#include <linux/workqueue.h>
#include <linux/soc/galachip/gc1601.h>
#include <net/dsa.h>
#include <net/dst_metadata.h>
#include <linux/unaligned.h>

#define GC1601_BUFFER_COUNT	512
#define GC1601_BUFFER_SIZE	2048
#define GC1601_RING_SIZE	256
#define GC1601_DIAG_MACS	25	/* GMAC counters the diag work samples */
#define GC1601_IRQ_RETRY_MS	200	/* polling after an IRQ storm, then retry */
#define GC1601_IRQ_RETRY_MAX_MS	10000
#define GC_SR_PORT_VLAN		0x10004	/* switch_base: SRC bank, per-port VLAN controls */
/* Switch banks, offsets from switch_base (0x90f80000). */
#define GC_SR_HA		0x00000
#define GC_SR_L2		0x04000
#define GC_SR_L3		0x08000
#define GC_SR_HM		0x0c000
#define GC_SR_SRC		0x10000
#define GC_SR_CA		0x18000
#define GC_SR_CAR		0x1c000	/* CAR (rate limiter) bank */
#define GC1601_SLOT_PROBE_SIZE	320
#define GC1601_SLOT_PROBE_COUNT	4
#define GC1601_SLOT_PROBE_RETRIES	10
/* GMAC TX octets count the FCS beyond the descriptor length (iperf: +4.0/frame). */
#define GC1601_PROBE_OCTET_EXTRA	4

/* Physical bases in loader-reserved memory (0x40800000..0x40911fff) */
#define GC1601_FREE_LIST_PHYS	0x40800000
#define GC1601_PKT_POOL_PHYS	0x40801000
#define GC1601_RX_RING_PHYS	0x40901000	/* the loader's: 16 containers, 4 KiB apart */
#define GC1601_TX_RING_PHYS	0x40911000

/* One RX ring per container; container 15 carries OMCI. */
#define GC1601_RX_CONTAINERS	16
#define GC1601_RX_RING_STRIDE	SZ_4K
/* RX descriptor word 0 "in": the ingress port, 1 for the GE UNI, 0 for the PON. */
#define GC1601_RX_IN		GENMASK(31, 29)
#define GC1601_RX_IN_GE		1
/* TX descriptor word 1: send out of the GE UNI, whatever the switch learned. */
#define GC1601_TXD1_GE		BIT(9)
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
	GC1601_SCHEDULER,
	GC1601_OUTPUT_SCHEDULER,
	GC1601_QUEUES,
	GC1601_NUM_WINDOWS,
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
#define GC_GMAC_PAUSE_TX	0x70	/* quanta << 16 | send pause << 1 */
#define GC_GMAC_PAUSE_RX	0x90	/* honour received pause frames */
#define GC_GMAC_ERCR		0xe0
#define GC_GMAC_ACIR		0xe4
#define GC_GMAC_PAUSE_SA_HI	0x300
#define GC_GMAC_PAUSE_SA_LO	0x304
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

/* Queue-limit table selectors, GC_DESC_TABLE_INDEX bits 26:22. */
#define GC_CVRD_OUTMAX		0
#define GC_CVRD_OUTCNT		BIT(22)
#define GC_CVRD_INMAX		BIT(23)
#define GC_CVRD_INCNT		(BIT(23) | BIT(22))
#define GC_CVRD_ALLCFG		BIT(24)
#define GC_CVRD_ALLCNT		(BIT(24) | BIT(22))
/* Each limit/count word holds two 11-bit fields. */
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
#define GC_RX_PARTITION		0x18	/* pool size in bits 31:16; reads back other state */
#define GC_RX_CTRL		0x20
/* As U-Boot; 0x2c clears bit 1 and the CPU allocator returns no buffer. */
#define GC_RX_CTRL_VAL		0x0f
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

struct gc1601_qos_tables {
	u32 sp;
	u32 fill[8];
	u32 inmax[8];
	u32 outmax[8];
	u32 cfg[8][3];
	u8 enable;
};

struct gc1601_eth {
	struct net_device *netdev;
	struct device *dev;
	void __iomem *base[GC1601_NUM_WINDOWS];
	void __iomem *sched_base;
	struct regmap *scu;
	struct regmap *ge_serdes;
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
	/* Set by the DSA switch driver: the LAN port's PHY and link come from it. */
	struct phy_device *lan_phy;
	bool dsa_lan;
	struct metadata_dst *port_dst[GC1601_PORTS];
	bool pause_autoneg;
	bool rx_pause;
	bool tx_pause;

	struct napi_struct napi;
	struct timer_list poll_timer;
	/* Serializes interrupt masks and shutdown against NAPI completion. */
	spinlock_t irq_lock;
	bool running;
	bool uni_disabled;
	/* Scheduler tables as the first open left them, restored on withdrawal. */
	struct gc1601_qos_tables tcont_boot[16];
	bool tcont_boot_valid;
	u16 sr_ha_frame;	/* HA frame limit as the switch set-up leaves it */

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
	bool tx_buf_wait;	/* pool empty: retry without discarding the skb */
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
	bool tx_stopping;
	bool tx_probe_failed;
	bool tx_probe_running;
	u8 tx_probe_retries;
	u32 tx_probe_completed_base;
	u32 tx_probe_excess_base;
	/* GMAC TX octets before each probe doorbell, plus one final sample. */
	u32 tx_probe_octets[GC1601_SLOT_PROBE_COUNT + 1];
	struct delayed_work tx_auto_work;
	struct work_struct tx_probe_work;
	struct delayed_work tx_loopback_work;
	struct delayed_work diag_work;
	u32 diag_mac[GC1601_DIAG_MACS];
	u32 diag_seq;
	unsigned int diag_ms;
	struct dentry *debugfs;
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
	bool sr_ready;		/* the switch set-up has run */
	bool sfu_rules;		/* stock traps, parser rules and index lists loaded */
	unsigned int sfu_flows;	/* services with flows installed */
	struct gc1601_sfu_flow sfu_active[GC1601_SFU_FLOWS];
	unsigned int sfu_logged;	/* flows in sfu_active, as last logged */
	/* Hashed flow entries installed, as table << 16 | index. */
	u32 sfu_hashed[2 * GC1601_SFU_FLOWS];
	bool sfu_hashed_up[2 * GC1601_SFU_FLOWS];
	unsigned int sfu_hashed_count;
	u32 cvrd_uni_inmax;
	u32 cvrd_uni_outmax[GC_CVRD_UNI_QUEUES];
	u32 cvrd_uni_allcfg[GC_CVRD_UNI_QUEUES];
	u32 cvrd_cpu_outmax;
	u32 cvrd_mismatches;

	/* Peak queue depths and watermarks for throughput/drop diagnostics */
	u16 peak_in[0x200];
	u16 peak_out[0x200];
	u16 peak_all[0x200];
	u16 min_left_iptr;

	/* Buffer IDs handed out by hardware since probe, for free-list analysis. */
	DECLARE_BITMAP(rx_id_seen, GC1601_BUFFER_COUNT);
	DECLARE_BITMAP(tx_id_seen, GC1601_BUFFER_COUNT);

};

/* In-flight TX descriptors once the probe handoff has measured the ring. */
#define GC1601_TX_DEPTH	16

/*
 * The stock switch defaults that differ from our live switch set-up: L3
 * DSCP->QID table 6 (dscp >> 3), HM trap-reason->QID table 12 and SRC+0x00
 * bit 20. L2+0x70, L2+0x180..0x19c, L3+0x0c and SRC+0x04 already match
 * stock.
 */
enum gc1601_hm_op {
	GC_HM_PUSH,		/* insert the vlandat tag, priority 0 */
	GC_HM_PUSH_COPY,	/* insert it with the priority of the tag below */
	GC_HM_POP,		/* delete the outer tag */
	GC_HM_REPLACE,		/* rewrite the outer tag's VID, keep the rest */
};

#define GC_HM_FLOWINFO		0
#define GC_HM_VLANDAT		0x800000
#define GC_HM_CMDDAT		0xc00000
#define GC_HM_SUBDAT		0x1800000
#define GC_HM_CMD_END		BIT(25)
#define GC_HM_CMD_BASE		80	/* first 4-step cmddat list */
#define GC_SR_FLOW_SLOTS	8	/* table 8, two per service */
#define GC_SR_FLOW_VALID	BIT(22)	/* word 3 */

/* gc1601-eth-main.c */
void gc1601_lan_link(struct gc1601_eth *priv, bool link, int speed, int duplex);
struct phy_device *gc1601_lan_phy(struct gc1601_eth *priv);
void gc1601_rx_drain_inherited(struct gc1601_eth *priv, bool release);
int gc1601_xmit(struct sk_buff *skb, struct net_device *netdev,
		const struct gc1601_pon_meta *pon);
extern struct net_device *gc1601_pon_dev;
extern u32 gc1601_pon_desc_log;
extern u32 gc1601_pool_warn;
extern const u16 gc1601_diag_mac[GC1601_DIAG_MACS];

/* gc1601-eth-hw.c */
int gc1601_queue_read(struct gc1601_eth *priv, u32 selector,
		      unsigned int index, u32 *data, unsigned int words);
void gc1601_sr_update(struct gc1601_eth *priv, u32 off, u32 clear, u32 set);
int gc1601_sr_hw_init(struct gc1601_eth *priv);
void gc1601_sr_bypass(struct gc1601_eth *priv, bool on, u32 gem);
int gc1601_tcont_limit_set(struct gc1601_eth *priv, unsigned int *limit, u64 value);
int gc1601_sr_tbl_write(struct gc1601_eth *priv, u32 table, u32 index,
			const u32 *w, unsigned int n);
void gc1601_sfu_cpu_mac(struct gc1601_eth *priv);
int gc1601_sfu_rules(struct gc1601_eth *priv);
int gc1601_hm_write(struct gc1601_eth *priv, u32 table, u32 index,
		    const u32 *w, unsigned int n);
void gc1601_gmac_pause_addr_apply(struct gc1601_eth *priv);
int gc1601_hw_setup(struct gc1601_eth *priv);
void gc1601_irq_mask(struct gc1601_eth *priv, bool enable);
void gc1601_hw_start(struct gc1601_eth *priv);
void gc1601_hw_stop(struct gc1601_eth *priv);
void gc1601_flow_ctrl_apply(struct gc1601_eth *priv);
int gc1601_qos_tables(struct gc1601_eth *priv, unsigned int tcont,
		      struct gc1601_qos_tables *tables, bool write);
extern unsigned int gc1601_tcont_depth;
extern unsigned int gc1601_tcont_out;
extern const u32 gc1601_hm_cmds[][2];

/* gc1601-eth-sfu.c */
int gc1601_sr_tbl_read(struct gc1601_eth *priv, u32 table, u32 index,
		       u32 *w, unsigned int n);

/* gc1601-eth-api.c */
int gc1601_pon_send(struct gc1601_eth *priv, struct sk_buff *skb,
		    const struct gc1601_pon_meta *meta);
extern struct mutex gc1601_uni_lock;
extern spinlock_t gc1601_omci_lock;
extern struct net_device *gc1601_pe_netdev;
extern gc1601_omci_rx_fn gc1601_omci_rx;
extern void *gc1601_omci_ctx;
extern const struct gc1601_pon_port_ops *gc1601_pon_ops;
extern void *gc1601_pon_ctx;

/* gc1601-eth-debugfs.c */
void gc1601_debugfs_init(struct gc1601_eth *priv);

#endif /* GC1601_ETH_H */
