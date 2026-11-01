/* SPDX-License-Identifier: GPL-2.0-only */
/* RTL9602C GMAC + switch: the state and registers shared by rtl9602c_eth.c and
 * the V2802RH L2 offload in rtl9601d_l2.c and rtl9601d_vlan.c. */
#ifndef _RTL9602C_ETH_H
#define _RTL9602C_ETH_H

#include <linux/gpio/consumer.h>
#include <linux/leds.h>
#include <linux/netdevice.h>
#include <linux/timer.h>
#include <linux/workqueue.h>

#include "luna_eth_regs.h"
#include "luna_gpon_regs.h"
#include "luna_gmac_logic.h"
#include "rtl9602c_l34_logic.h"
#include "luna_gpon_nic.h"

/* The per-jack steering VLANs, from the top of the range: an OLT's service
 * VLANs are low, and the L2 offload refuses a service on one of these. */
#define SW_LAN_STEER_VID_BASE	4092u
#define SW_LAN_STEER_VID(port)	(SW_LAN_STEER_VID_BASE + (port))
#define LAN_PORTS		2u	/* jacks a board may declare: switch ports 0..1 */

#define OTX_RING_SIZE	8	/* dedicated US-OMCI TX ring (low-rate control) */

#define SW_STORM_CTRL_UC_CTRL		0x17040

/*
 * VLAN filtering: VLAN_CTRL @ 0x13008 bit0 = VLAN_FILTERING; VLAN_INGRESS @
 * 0x13004 = per-port ingress filter, in luna_eth_regs.h. SW_VLAN_FILTERING has
 * no twin in that header.
 */
#define SW_VLAN_FILTERING	BIT(0)
#define SW_DEFAULT_VID		1

/* Indirect VLAN 4k-table access (field positions):
 * TBL_ACCESS_CTRL[31]=start [20:9]=addr/VID [6:4]=method(1) [3]=cmd(1=write)
 * [2:0]=type(1=VLAN); STS bit13=BUSY; WR_DATA holds the entry word. */
#define SW_TBL_CTRL		0x12000
#define SW_TBL_STS		0x12004
#define SW_TBL_WRDATA		0x12008
#define SW_TBL_BUSY		BIT(13)
#define SW_TBL_TRIES		1000u	/* the bound the poll below refuses at */

/* One VLAN 4k-table entry: ext[17:11] | IVL[10] | untag[7:4] | mbr[3:0], FID 0. The extension
 * ports are the CPU's: without them the VLAN filter drops its traffic. Bit 10 clear is SVL, one
 * MAC table for every VLAN (the SDK's fidMode_set writes it inverted): a WAN VLAN needs IVL, or
 * a reply to a LAN host is looked up where that host was learned, a LAN port outside the VLAN,
 * and dropped. */
#define SW_VLAN_IVL	BIT(10)


struct rtl9602c_eth {
	/* The per-chip table. */
	const struct luna_sw_map *swm;
	void __iomem	*base;
	void __iomem	*sw;	/* switch core */
	void __iomem	*txgo;	/* network-engine TX-fetch page 0x18001000; +0x38 bit31 = per-packet GO */
	struct net_device *ndev;
	struct net_device *wan_ndev;	/* gpon0: WAN data-GEM netdev (DS demux'd from PON port 2, US steered to GPON_DATA_FLOW) */
	struct device	*dev;

	struct rx_desc	*rx_ring;
	dma_addr_t	rx_ring_dma;
	struct page_pool *rx_pool;
	struct page	*rx_page[RX_RING_SIZE];
	unsigned int	rx_head;
	unsigned int	rx_n;		/* RX ring entries in use, <= RX_RING_SIZE (DT) */

	struct tx_desc	*tx_ring;
	dma_addr_t	tx_ring_dma;
	struct sk_buff	*tx_skb[TX_RING_SIZE];
	dma_addr_t	tx_buf_dma[TX_RING_SIZE];
	unsigned int	tx_buf_len[TX_RING_SIZE];
	unsigned int	tx_head, tx_dirty;
	spinlock_t	tx_lock;	/* serialises tx_head/tx_ring: xmit (process)
					 * vs OMCI inject (poll-timer softirq) */
	/* HW ring rotation: the slot TxFDP currently points at (0 after open).
	 * The stall recovery re-arms TxFDP at the first PENDING slot and moves
	 * EOR to the slot before it, so the 64 descriptors stay one ring, just
	 * rotated: HW index j <-> SW slot (rot + j) % size. SW head/dirty slot
	 * numbering is unchanged; only the EOR placement and the TxCDO->slot
	 * translation depend on rot. */
	unsigned int	tx_rot, otx_rot;
	/*
	 * TX-DMA park watchdog. stall_since = jiffies of the first tick that
	 * saw OWN stuck at the HW cursor with no dirty progress; 0 = healthy.
	 * recover_work runs the IP-block power-cycle + re-arm.
	 */
	unsigned long	stall_since;
	unsigned int	stall_lastdirty;
	unsigned int	stall_level;	/* 0: next escalation = soft re-arm;
					 * 1: soft re-arm failed -> IP cycle */
	u32		dbg_rearm;	/* soft TxFDP re-arms (publish + watchdog) */
	struct work_struct recover_work;
	bool		closing;	/* gate recover_work vs ndo_stop teardown */
	bool		hw_up;		/* rings + DMA live from the first open on: the OMCC
					 * and gpon0 ride them whatever eth0's state */
	unsigned int	lan_tx_default;
	bool		in_recovery;	/* GMAC block may be power-gated: /proc
					 * diag must not touch its MMIO (bus abort) */
	u32		dbg_tx_recover;	/* completed GMAC power-cycle recoveries */

	/* Dedicated US-OMCI TX ring (the stock OMCC TX ring, default ring 4).
	 * Small single-purpose ring: US OMCI is low-rate control traffic, so a
	 * handful of descriptors is ample, and keeping it separate from the LAN
	 * ring 0 means the OMCC steering descriptor never mixes with LAN frames. */
	struct tx_desc	*otx_ring;
	dma_addr_t	otx_ring_dma;
	struct sk_buff	*otx_skb[OTX_RING_SIZE];
	dma_addr_t	otx_buf_dma[OTX_RING_SIZE];
	unsigned int	otx_buf_len[OTX_RING_SIZE];
	unsigned int	otx_head, otx_dirty;
	/* Idle dummy TX ring armed on the UNUSED HW TX rings */
	struct tx_desc	*dummy_ring;
	dma_addr_t	dummy_ring_dma;
	/* Shared-ring-0 OMCI path (omci_tx_ring==0) */
	int		omci_r0_last_slot;
	/* US OMCI (OMCC) responder state. */
	u8		omci_sn[8];	/* G.984.3 ONU-SN (4 ASCII ID + 4 serial),
					 * for the ONU-G Vendor-ID/Serial GET reply */
	u8		omci_mds;	/* ONU-data (ME 2) MIB-Data-Sync counter */
	/* omci_audit_reads and omci_mds_tries USED TO BE HERE */
	u32		dbg_omci_tx;		/* US OMCI responses queued */
	u32		dbg_omci_tx_drop;	/* dropped: ring full / alloc / map */
	u32		dbg_omci_unhandled;	/* requests with no modelled reply */

	struct timer_list poll_timer;	/* IRQ-driven: 100ms TX-unpark backstop; no IRQ: 2ms pure-poll */
	struct napi_struct napi;
	int		irq;	/* GMAC0 INTC input (platform_get_irq); <=0 = pure-poll fallback */
	spinlock_t sw_table_lock;
	struct {
		u32 down, up;
		bool valid;
	} uni_rate[LAN_PORTS];
	struct {
		u32 ageing_s, learn_limit, max_frame;
	} uni_l2[LAN_PORTS];
	bool uni_l2_used;
	/* The LAN jacks as netdevs (the DT ethernet-ports node), by switch
	 * port. A frame from port p goes up lan_ndev[p] when that is running,
	 * else eth0, so a bridge built on eth0 alone keeps working. */
	struct net_device *lan_ndev[LAN_PORTS];
	struct rtl9602c_lan_link {
		bool		up;
		bool		full;
		u32		speed;		/* SPEED_* while up */
	} lan_link[LAN_PORTS];
	bool		lan_steer_blocked;	/* an OLT service took a steering VID */
	u32		dbg_lan_relay_skipped;	/* LAN->LAN copies the CPU refused: the switch did it */
	struct led_classdev *pon_led, *los_led;	/* the board's lamps, net/xpon drives them */

	/* Port 0 runs over the SerDes to an external PHY (DT realtek,port0-serdes):
	 * its internal GPHY stays powered down and the MAC link is forced, following
	 * the SerDes link polled by link_work. */
	bool		port0_serdes;
	bool		stock_switch;
	int		loop_mode;	/* the lan_loop_guard the registers hold */
	u32		loop_ports;	/* jacks RLDP last called looped */
	int		loop_cut_port;	/* the jack held down for a loop, or -1 */
	unsigned int	loop_cut_left;	/* seconds until it comes back up */
	struct delayed_work link_work;	/* once a second: the SerDes mirror, every jack's carrier */
	u32		lan_drops[LAN_PORTS];	/* link losses, including those a poll would miss */
	u8		lan_own_drop;	/* ports whose next link loss the driver caused itself */
	u32		port0_pause_off;	/* pause bits taken out of port 0's forced ability */
	u8		uni_autodetect[LAN_PORTS];	/* the OLT's last ME 11 auto-detection value */
	u16		uni_pause[LAN_PORTS];	/* its last ME 11 pause time */

	/* The external PHY's MDIO bus, bit-banged on two GPIOs as stock does it
	 * (DT realtek,ext-mdio-gpios = MDC, MDIO; realtek,ext-phy-addr). */
	struct gpio_desc *ext_mdc, *ext_mdio;
	u8		ext_phy;
	bool		ext_phy_ok;	/* answered on that bus; drives port 0's link */
	/* DT realtek,ext-phy-reset-gpios: stock pulses it before every SerDes
	 * option change (rtk_rtl8226b_moudle_init1, GPIO 27 on the V2802RH). */
	struct gpio_desc *ext_phy_reset;
	bool		lan_sds_hsgmii;	/* the LAN SerDes runs HSGMII (2.5G); else SGMII */
	bool		lan_sds_set;	/* rtl9602c_lan_sds_program ran since the bring-up */
	u8		ext_phy_option;	/* verified RTL8221B SerDes option; 0 if unknown */
	u8		ext_phy_target;	/* retained across a failed reset/configuration */

	/* Bootloader GMAC0 control snapshot (inherited). */
	u32		ub_tcr, ub_rcr, ub_config, ub_cputagcr, ub_cputag1cr;
	u32		ub_iocmd, ub_iocmd1;

	/* RX datapath debug counters (see /proc/ethdump). */
	u32		dbg_poll;	/* poll-timer ticks */
	u32		dbg_filled;	/* RX descriptors HW handed back (D_OWN cleared) */
	u32		dbg_good;	/* frames pushed up the stack */
	u32		dbg_err;	/* RX descriptors with error/oversize */
	u32		dbg_rx_repoll;	/* frames found waiting after the IRQ re-arm, re-polled */
	u32		dbg_rxlen;	/* raw length of the last captured RX frame */
	u8		dbg_rxbuf[48];	/* first bytes of the last RX frame (pre-pull) */

	/* OMCI (OMCC stream) trap, armed by the GPON driver once it installs the
	 * OMCC GEM datapath (rtl9602c_eth_set_omci_sid). */
	bool		omci_trap_on;
	u32		dbg_omci_rx;	/* DS OMCI frames trapped to the CPU */
	u32		dbg_omci_rxlen;	/* length of the last OMCI frame */
	u8		dbg_omci_rxbuf[48];	/* the last DS OMCI baseline message */
};

extern uint rtl9602c_pon_vid;

void rtl9602c_sw_tbl_pause(void);

/* The L2 offload's interface to the driver. */
int v2802_l2_init(struct rtl9602c_eth *ep);
int v2802_l2_storm(struct rtl9602c_eth *ep, bool hardware_only);
bool v2802_l2_wan_allowed(struct rtl9602c_eth *ep, const struct sk_buff *skb);
bool v2802_l2_forwarded(struct sk_buff *skb, unsigned int port, unsigned int reason);
int v2802_l2_lan_prepare(struct rtl9602c_eth *ep, struct sk_buff *skb, unsigned int port);
int v2802_l2_parent_id(struct net_device *dev, struct netdev_phys_item_id *id);
int v2802_l2_netdev_vid(struct rtl9602c_eth *ep, u16 vid, bool add);
bool v2802_l2_vlan_filtering(struct rtl9602c_eth *ep);
int v2802_l2_priority(struct rtl9602c_eth *ep, struct sk_buff *skb);
#endif /* _RTL9602C_ETH_H */
