/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef GC1601_PON_H
#define GC1601_PON_H

#include <linux/bitmap.h>
#include <linux/bits.h>
#include <linux/hrtimer.h>
#include <linux/if_ether.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/rcupdate.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/workqueue.h>
#include <linux/soc/galachip/gc1601.h>

/* GPON MAC blocks, relative to 0x90cc0000 */
#define PON_IRQ			0x00000
#define PON_IRQ_SN_TX		BIT(1)
#define PON_IRQ_RANGING_TX	BIT(2)
#define PON_IRQ_SN_GRANT		BIT(3)
#define PON_IRQ_RANGING_GRANT	BIT(4)
#define PON_IRQ_PLOAM		BIT(7)
#define PON_IRQ_FIFO_ERROR	BIT(10)
#define PON_ONU			0x04000
#define PON_OPT_TX		0x08000
#define PON_COUNTERS		0x0c000
#define PON_OPT_RX		0x14000
#define PON_AES			0x18000
#define PON_GEM			0x1c000
#define PON_OMCC		0x20000

#define PON_ONU_ID		GENMASK(7, 0)
#define PON_ONU_STATE		GENMASK(10, 8)
#define PON_OPT_TX_EN		BIT(0)
#define PON_OPT_TX_FORCE	BIT(2)
#define PON_OPT_TX_DELAY	0x04
#define PON_OPT_RX_EN		BIT(0)
#define PON_OPT_RX_STATUS	0x08
#define PON_OPT_RX_LOF		BIT(2)
#define PON_OPT_RX_LOS		BIT(3)

/* SCU PON PLL and SerDes reset window, relative to 0x90a00800 */
#define SCU_PON_STATUS		0x00
#define SCU_PON_PLL		0x04
#define SCU_PON_PLL_CFG0	0x08
#define SCU_PON_PLL_CFG1	0x0c
#define SCU_PON_RESET		0x14
#define SCU_PON_DG		0x18	/* bit 0 clear: hardware dying gasp on */

#define SCU_PON_STATUS_READY	BIT(2)
#define SCU_PON_STATUS_PLL_LOCK	BIT(8)

#define PON_LOCK_TIMEOUT_US	10000

/* UX3320S BOSA: SFF-8472 style A2 page at 0x51, page select at 0x7f */
#define BOSA_A2_ADDR		0x51
#define BOSA_PAGE		0x7f
#define BOSA_CTRL		0x6e
#define BOSA_SOFT_TX_DIS	BIT(6)
#define BOSA_CHUNK		16
#define BOSA_WRITE_DELAY_MS	5
#define BOSA_BLOB_OFFSET	0x1000	/* in the hwconfig partition */
#define BOSA_UX3320S_LEN	0x380

/* Global block 0x90c40020: manual TX power control (xpon_sdk txpwr_manual_op) */
#define PON_TXPWR_OFF		BIT(2)
#define PON_TXPWR_MANUAL	BIT(4)
#define PON_TXPWR_SEL		BIT(5)
#define PON_TXPWR_GLOBAL_EN	BIT(15)	/* set by GLOBAL_init */

/* Top-level block enables and WAN clocking, 0x90c00000 (shared with Ethernet) */
#define TOP_ENABLE		0x00
#define TOP_CLK_EN		0x04
#define TOP_WAN_CLK		0x0c
#define TOP_ENABLE_GPON		0xfffffff7	/* EPON MAC (bit 3) held off */
#define TOP_ENABLE_EPON		0xffffffef	/* GPON MAC (bit 4) held off */
#define TOP_WAN_CLK_GPON	0x4bef
#define TOP_WAN_CLK_EPON	0x7d08

/* Global block 0x90c40000 */
#define GLB_CTRL		0x04
#define GLB_CTRL_EPON		GENMASK(1, 0)
#define GLB_CTRL_EPON_SEL	BIT(0)
#define GLB_CTRL_RUN		BIT(3)
#define GLB_MAC_SEL		0x08	/* bit 0 EPON, bit 1 GPON: cleared for the active MAC */
#define GLB_CFG10		0x10
#define GLB_WAN_CFG		0x14
#define GLB_WAN_CFG_GPON	0x168
#define GLB_WAN_CFG_EPON	0x7ff
#define GLB_CFG18		0x18

/*
 * EPON MAC at 0x90c80000 (xpon_sdk EMAC_*). MPCP discovery, registration,
 * GATE and REPORT run in hardware for up to four LLIDs; LLID n uses T-CONT
 * queue set n.
 */
#define EMAC_CTRL		0x00
#define EMAC_CTRL_RX_EN		BIT(0)
#define EMAC_CTRL_TX_EN		(BIT(7) | BIT(8))
#define EMAC_LLID_EN		0x08	/* bit n: LLID n may register */
#define EMAC_HOLDOVER		0x10	/* bit 31 on, bits 26:0 time */
#define EMAC_RANDOM_DELAY	0x6c	/* bits 7:0 delay, bit 8 on */
#define EMAC_IRQ_LLID		0x40
#define EMAC_IRQ_MPCP		0x44
#define EMAC_IRQ_LLID_MASK	0x48
#define EMAC_IRQ_MPCP_MASK	0x4c
#define EMAC_REGISTERED		0x50	/* bit n: LLID n registered */
#define EMAC_OPT_DELAY		0x60
#define EMAC_CFG68		0x68
#define EMAC_CFG78		0x78
#define EMAC_CFG78_POLAR	BIT(1)	/* cleared with opt_polar_data_low */
#define EMAC_SIGNAL		0x7c	/* bit 0: optical signal */
#define EMAC_LLID_MAC_HI(n)	(0x80 + 8 * (n))
#define EMAC_LLID_MAC_LO(n)	(0x84 + 8 * (n))
#define EMAC_KEY		0xc0	/* churning key, 24 bits */
#define EMAC_KEY_CTRL		0xe4	/* bit n LLID, bit 8 key index, bit 9 load */
#define EMAC_SHAPER		0xf4	/* bit 31 on, bits 20:1 rate: LLID upstream shaper */
#define EMAC_CFGF8		0xf8
#define EMAC_CFG134		0x134
#define EMAC_MTU		0x13c	/* bits 26:16: frame limit */
#define EMAC_HOLDOVER_ON	0x16c	/* chipset_set_hold_over flag */
#define EMAC_SYNC		0x148	/* 0: downstream synchronised */
#define EMAC_CFG170		0x170
#define EMAC_SIZE		0x2000

/* EMAC_IRQ_LLID: LLID n in bit n of each byte */
#define EMAC_IRQ_ACK(n)		BIT(n)
#define EMAC_IRQ_NACK(n)	BIT(8 + (n))
#define EMAC_IRQ_DEREG(n)	BIT(16 + (n))
#define EMAC_IRQ_REREG(n)	BIT(24 + (n))
/* EMAC_IRQ_MPCP */
#define EMAC_IRQ_SYNC		BIT(0)
#define EMAC_IRQ_PREAMBLE_ERR	BIT(1)
#define EMAC_IRQ_EOP_ERR	BIT(2)
#define EMAC_IRQ_TIME_DRIFT	BIT(3)
#define EMAC_IRQ_ADDR_ERR	BIT(4)
#define EMAC_IRQ_LOS		BIT(5)
#define EMAC_IRQ_CRYPT(n)	BIT(8 + (n))
#define EMAC_IRQ_REG_DONE(n)	BIT(16 + (n))
#define EMAC_IRQ_MPCP_LOSS(n)	BIT(24 + (n))

#define EPON_LLIDS		4
#define EPON_EVENTS		64

#define GPON_PLOAM_LEN		13	/* ONU-ID, message ID, 10 data bytes, CRC */
#define GPON_PLOAM_LOG		32

struct gc1601_ploam {
	u64 ns;
	u32 generation;
	u8 msg[GPON_PLOAM_LEN];
};

#define GPON_TCONTS		16
#define GPON_COUNTERS		19
#define GPON_SERVICES		64	/* a Nokia OLT installs one per VLAN and priority */
#define GPON_STATE_O5		5

struct gc1601_dbru_stats {
	u64 detected, busy, passes;
	u64 rewrites, buffer_skips, qmu_errors;
	u64 flag_clears, flag_persists;
	/* Last recovery pass; readback only describes the hardware flag. */
	u32 before, after, rewritten, skipped, rate;
};

struct gc1601_gpon {
	struct hrtimer poll;
	struct work_struct work;
	struct delayed_work timer;	/* activation (O3/O4) or popup (O6) */
	struct delayed_work watchdog;	/* O5: MAC hang and DBRu stall checks */
	struct delayed_work rei;
	struct delayed_work recovery;
	unsigned int recovery_attempt;
	u64 recovery_runs, recovery_successes;
	int recovery_error;
	bool retained_valid;
	u16 retained_omcc;
	bool key_valid;
	DECLARE_BITMAP(retained_aes, 4096);
	u32 ber_interval_frames;
	u8 rei_sequence;
	u64 rei_sent, rei_failed;
	u64 counter[GPON_COUNTERS];	/* clear-on-read PON counters, summed */
	u64 fec_seconds;		/* seconds with uncorrectable codewords */
	u64 fec_uncorr_seen;		/* uncorrectable total at the last second */
	u64 dbru_bytes;			/* upstream bytes at the last DBRu check */
	struct delayed_work identity_timer;
	spinlock_t lock;		/* ring and levels, shared with the poll */
	bool running;
	bool allow_tx;
	bool olt_tx_disabled;
	bool identity_wait;		/* ranging held for the userspace identity */
	bool identity_released;		/* userspace applied it: never wait again */

	struct gc1601_ploam ring[GPON_PLOAM_LOG];
	unsigned int head;		/* next slot written by the poll */
	unsigned int tail;		/* next slot consumed by the work */
	unsigned int dropped;
	bool los, lof;
	bool loss_pending;
	u32 generation;

	int state;			/* 1..7 for O1..O7 */
	u8 onu_id;			/* 0xff when unassigned; read by the poll */
	bool ranged;			/* O5 reached: an addressed popup resumes O5 */
	u8 serial[8];
	u8 password[10];
	u8 overhead[10];		/* last Upstream_Overhead data */
	bool have_overhead;
	u8 burst_t3[2];			/* Extended_Burst_Length: pre-ranged, ranged */
	bool have_burst;
	u8 key_index;
	u16 alloc[GPON_TCONTS];		/* Alloc-ID per T-CONT index, U16_MAX = free */
	DECLARE_BITMAP(tcont_dirty, GPON_TCONTS);	/* cleanup must succeed before reuse */
	u16 omcc_port;
	bool omcc_valid;		/* GEM port zero is a valid OMCC. */
	DECLARE_BITMAP(gem, 4096);
	DECLARE_BITMAP(gem_aes, 4096);	/* Encrypted_Port-ID state; PLOAM owns it */

	struct gc1601_ploam log[GPON_PLOAM_LOG];
	unsigned int log_pos;
	u64 ploam_rx, ploam_tx, tx_refused, los_events, lof_events;
	u64 ploam_crc_errors;
	u64 ploam_repeats;
	u64 olt_pee;			/* Physical_Equipment_Error from the OLT */
	u32 mac_irq_last;
	u64 sn_grant_samples, sn_tx_samples;
	u64 ranging_grant_samples, ranging_tx_samples;
	u64 ploam_irq_samples, fifo_error_samples;
	u64 mac_hangs;			/* AES +0x94 set in O5: MAC re-initialised */
	u64 mac_mismatches;		/* AES +0x9c/+0xac disagree on entering O1 */
	struct gc1601_dbru_stats dbru_stats;	/* published under g->lock */
	unsigned long dbru_last;	/* jiffies of the previous DBRu sample */
	struct gc1601_ploam last;	/* last PLOAM acted on */
};

/* One EMAC poll that saw an interrupt or a state change. */
struct gc1601_epon_event {
	u64 ns;
	u32 llid_irq;
	u32 mpcp_irq;
	bool signal;
	bool sync;
	u8 registered;
};

struct gc1601_epon {
	struct hrtimer poll;
	struct work_struct work;
	struct delayed_work discovery;
	struct delayed_work nack;	/* ends the silence after a REGISTER NACK */
	bool nack_silent;
	u32 holdover_ms;		/* 0: off */
	spinlock_t lock;		/* everything below, shared with the poll */
	bool running;
	bool allow_tx;
	bool register_en;		/* epon-oamd: off while silent after an auth failure */
	bool oam_link;			/* epon-oamd: OAM discovery done, data may flow */
	bool data_up;			/* registered with the OAM link up, as last reported */
	u32 generation;			/* OAM session, including batched loss */
	bool signal;
	bool sync;
	u8 registered;			/* LLID mask */
	u8 mac[ETH_ALEN];		/* LLID 0 MAC, sent in REGISTER_REQ */
	u32 llid_irq_last;
	u32 mpcp_irq_last;
	u64 llid_irq_count[32];
	u64 mpcp_irq_count[32];
	struct gc1601_epon_event ring[EPON_EVENTS];
	unsigned int head;		/* written by the poll */
	unsigned int tail;		/* read by the work */
	unsigned int dropped;
	bool loss_pending;		/* preserve transitions across event overflow */
	bool oam_reset_pending;
	bool nack_pending;
	u64 rx_frames;
	u64 rx_mpcp;
	u64 rx_oam;
	u64 tx_frames;
	u64 tx_refused;			/* not registered */
	u32 rx_minfo_seen;		/* bit n: descriptor minfo n seen, n < 32 */
	u16 rx_minfo_last;
	/* last state handed to the xPON core, owned by the work */
	bool reported;
	bool reported_signal;
	bool reported_sync;
	u8 reported_registered;
};

enum gc1601_pon_mode {
	GC1601_PON_GPON,
	GC1601_PON_EPON,
};

struct gc1601_pon;

/* Hooks for the gc1601-epon-diag module. */
struct gc1601_epon_diag_ops {
	void (*event)(struct gc1601_pon *pon, const struct gc1601_epon_event *ev);
	void (*rx)(struct gc1601_pon *pon, u16 minfo, const u8 *data, unsigned int len);
	void (*tx)(struct gc1601_pon *pon, const u8 *data, unsigned int len, int ret);
};

struct omci_service_config;

/* Hooks for the gc1601-gpon-diag module; all may run in atomic context. */
struct gc1601_gpon_diag_ops {
	/* OMCI-driven configuration, as the driver receives it */
	void (*tcont)(struct gc1601_pon *pon, u16 entity, u16 alloc_id, bool valid);
	void (*gem)(struct gc1601_pon *pon, u16 entity, u16 port, u16 tcont_entity,
		    u8 direction, bool valid, bool encrypted);
	void (*service)(struct gc1601_pon *pon, const struct omci_service_config *svc);
	void (*service_delete)(struct gc1601_pon *pon, u32 cookie);
	/* the switch flows built from the services; @ret from the packet engine */
	void (*flows)(struct gc1601_pon *pon, const struct gc1601_sfu_flow *flows,
		      unsigned int n, int ret);
	void (*state)(struct gc1601_pon *pon, int from, int to);
	void (*ploam_rx)(struct gc1601_pon *pon, const u8 *msg);	/* 13 bytes */
	void (*ploam_tx)(struct gc1601_pon *pon, const u8 *msg, int ret);	/* 12 bytes */
	void (*omci)(struct gc1601_pon *pon, bool tx, const u8 *data, unsigned int len,
		     int ret);
	void (*rx)(struct gc1601_pon *pon, u16 gem, const u8 *data, unsigned int len);
	void (*tx)(struct gc1601_pon *pon, u16 gem, int tcont, const u8 *data,
		   unsigned int len, int ret);
};

struct xpon_device;
struct omci_device;

/* Provider state for the xPON subsystem and its OMCI agent. */
struct gc1601_xpon {
	struct list_head offload_node;
	struct net_device *netdev;	/* pon0, the WAN side of the bridge */
	struct xpon_device *xpon;
	struct omci_device *odev;
	bool started;
	u16 tcont_alloc[GPON_TCONTS];	/* OMCI T-CONT entity order -> Alloc-ID */
	u16 tcont_entity[GPON_TCONTS];
	u16 gem_tcont[4096];		/* upstream GEM association, U16_MAX = none */
	unsigned int services;
	spinlock_t service_lock;	/* services, read by the pon0 xmit path */
	struct delayed_work sfu_work;	/* services -> switch offload flows */
	struct work_struct reconcile_work;
	struct notifier_block netdev_nb;	/* warns about a bridged pon0.N */
	struct mutex sfu_mutex;		/* switch flow writes; nests inside pon->lock */
	unsigned int sfu_gen;		/* bumped by every clear, under pon->lock */
	struct gc1601_sfu_flow sfu[GC1601_SFU_FLOWS];	/* last flows handed to the switch */
	unsigned int sfu_count;
	int sfu_last_error;
	u8 sfu_retries;
	u16 sfu_untag_vlan;		/* VLAN the LAN side sends untagged, 0: none */
	struct {
		u16 vid;
		u8 pcp;
	} epon_learned[GC1601_SFU_FLOWS - 1];	/* reserve one untagged flow */
	unsigned int epon_learned_count;	/* under service_lock */
	unsigned long epon_capacity_misses;	/* eligible packets, not unique keys */
	/* Manual LAN-side VLANs (sfu_vlan_map), ahead of what OMCI asks for. */
	struct gc1601_vlan_map {
		u16 wan;
		u16 lan;
		u8 mode;		/* enum gc1601_sfu_lan */
	} vlan_map[8];
	unsigned int vlan_maps;
	/* What pon0 rewrites in software, as the switch does; under service_lock. */
	struct gc1601_vlan_map lan_map[GPON_SERVICES];
	unsigned int lan_maps;
	struct gc1601_service {
		u32 cookie;
		u16 gem_port;
		u16 alloc_id;
		u16 vlan_id;
		u8 queue;
		u8 pcp;
		bool pcp_valid;
		bool default_service;
		bool vlan_valid;
		bool used;
		u8 uni_mode;		/* enum gc1601_sfu_lan, from OMCI */
		u16 uni_vid;
	} service[GPON_SERVICES];
	/* Multicast has no pon0/SFU rule, but can share a hardware GEM. */
	struct {
		u32 cookie;
		u16 gem_port;
		bool used;
	} multicast[GPON_SERVICES];
	int olt_profile;		/* last effective profile, -1 before the first */
	unsigned long omci_rx_errors;	/* not OMCI-shaped */
	unsigned long omci_crc_errors;
	unsigned long omci_rx_seen;
	unsigned long omci_rx_to_core;
	unsigned long omci_rx_alloc_failures;
	u32 omci_rx_last_len;
	u32 omci_rx_last_header;
	u32 omci_rx_last_crc;
	u32 omci_rx_expected_crc;
	unsigned long omci_tx_attempts;
	unsigned long omci_tx_failures;
	int omci_tx_last_error;
};

enum gc1601_bosa_state {
	GC1601_BOSA_UNKNOWN,
	GC1601_BOSA_PROGRAMMING,
	GC1601_BOSA_READY,
	GC1601_BOSA_FAILED,
};

struct gc1601_pon {
	struct device *dev;
	void __iomem *mac;
	void __iomem *serdes;
	void __iomem *serdes_ctrl;
	void __iomem *scu;
	void __iomem *txpwr;
	void __iomem *top;
	void __iomem *global;
	void __iomem *emac;
	struct device_node *bosa_bus;
	struct mutex lock;
	bool tx_hold;			/* laser off for the OLT or a rogue state */
	bool tx_hold_admin;
	bool tx_hold_timed;
	bool removing;
	unsigned long tx_hold_deadline;
	struct delayed_work tx_hold_work;
	struct delayed_work bringup;
	struct delayed_work autostart;	/* starts the MAC if userspace does not */
	unsigned int bringup_tries;
	int bosa_state;			/* enum gc1601_bosa_state */
	int mode;			/* enum gc1601_pon_mode, under lock */
	struct gc1601_gpon gpon;
	struct gc1601_epon epon;
	struct gc1601_xpon xp;
};

void gc1601_pon_tx_off(struct gc1601_pon *pon);
void gc1601_pon_tx_on(struct gc1601_pon *pon);
void gc1601_pon_gmac_init(struct gc1601_pon *pon);
int gc1601_bosa_soft_tx(struct gc1601_pon *pon, bool enable);
int gc1601_gpon_init(struct gc1601_pon *pon);
void gc1601_gpon_autostart(struct gc1601_pon *pon);
void gc1601_gpon_exit(struct gc1601_pon *pon);
extern const struct attribute_group gc1601_gpon_group;
extern const struct attribute_group gc1601_xpon_group;
int gc1601_xpon_init(struct gc1601_pon *pon);
void gc1601_xpon_exit(struct gc1601_pon *pon);
int gc1601_xpon_start(struct gc1601_pon *pon);
void gc1601_xpon_stop(struct gc1601_pon *pon);
void gc1601_xpon_report_state(struct gc1601_pon *pon, int state);
void gc1601_xpon_report_alloc(struct gc1601_pon *pon);
void gc1601_xpon_sfu_kick(struct gc1601_pon *pon);
void gc1601_xpon_report_signal(struct gc1601_pon *pon, bool los, bool lof);
void gc1601_xpon_report_onu_id(struct gc1601_pon *pon, u8 onu_id);
void gc1601_xpon_report_channel(struct gc1601_pon *pon, u16 port, bool valid);
void gc1601_xpon_reset_session(struct gc1601_pon *pon);
void gc1601_gpon_set_identity_locked(struct gc1601_pon *pon, const u8 *serial,
				     const u8 *password);
void gc1601_gpon_set_identity(struct gc1601_pon *pon, const u8 *serial,
			      const u8 *password);
void gc1601_gpon_omci_set_gem(struct gc1601_pon *pon, u16 port, bool valid,
			      bool aes);
int gc1601_gpon_alloc_index(struct gc1601_pon *pon, u16 alloc_id);
int gc1601_gpon_restore_service(struct gc1601_pon *pon, u16 gem_port, u16 alloc_id);
int gc1601_xpon_clear_sfu(struct gc1601_pon *pon);
int gc1601_xpon_withdraw_alloc(struct gc1601_pon *pon, u16 alloc_id);
int gc1601_bosa_read(struct gc1601_pon *pon, u8 reg, u8 *buf, u8 len);
int gc1601_pon_set_mode(struct gc1601_pon *pon, enum gc1601_pon_mode mode);
int gc1601_gpon_set_allow_tx(struct gc1601_pon *pon, bool allow);
void gc1601_gpon_start(struct gc1601_pon *pon);
void gc1601_gpon_stop(struct gc1601_pon *pon);

void gc1601_epon_tx_hold(struct gc1601_pon *pon, bool hold);
void gc1601_gpon_tx_hold(struct gc1601_pon *pon, bool hold);
u64 gc1601_gpon_bip_errors(struct gc1601_pon *pon);
struct omci_telemetry;
void gc1601_gpon_fec(struct gc1601_pon *pon, struct omci_telemetry *t);
int gc1601_epon_init(struct gc1601_pon *pon);
void gc1601_epon_exit(struct gc1601_pon *pon);
void gc1601_epon_hw_init(struct gc1601_pon *pon);
void gc1601_epon_start(struct gc1601_pon *pon);
void gc1601_epon_stop(struct gc1601_pon *pon);
int gc1601_epon_set_allow_tx(struct gc1601_pon *pon, bool allow);
bool gc1601_epon_registered(struct gc1601_pon *pon, unsigned int llid);
bool gc1601_epon_may_send(struct gc1601_pon *pon, bool oam);
void gc1601_epon_rx_seen(struct gc1601_pon *pon, u16 minfo, const u8 *data,
			 unsigned int len);
void gc1601_epon_tx_seen(struct gc1601_pon *pon, const u8 *data, unsigned int len,
			 int ret);
extern const struct attribute_group gc1601_epon_group;

struct gc1601_pon *gc1601_gpon_diag_register(const struct gc1601_gpon_diag_ops *ops);
void gc1601_gpon_diag_unregister(void);
extern const struct gc1601_gpon_diag_ops __rcu *gc1601_gpon_diag;

/* Call a gc1601-gpon-diag hook if the module is loaded. */
#define GPON_DIAG(fn, ...)							\
	do {									\
		const struct gc1601_gpon_diag_ops *d__;				\
										\
		rcu_read_lock();						\
		d__ = rcu_dereference(gc1601_gpon_diag);			\
		if (d__ && d__->fn)						\
			d__->fn(__VA_ARGS__);					\
		rcu_read_unlock();						\
	} while (0)

struct gc1601_pon *gc1601_epon_diag_register(const struct gc1601_epon_diag_ops *ops);
void gc1601_epon_diag_unregister(void);

#endif
