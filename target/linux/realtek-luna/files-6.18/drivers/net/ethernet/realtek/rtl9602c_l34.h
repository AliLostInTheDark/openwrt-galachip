/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * RTL9602C "Luna" hardware L3/L4 forwarding + NAPT engine.
 *
 * The SoC switch core carries an L3/L4 (routing + NAPT) accelerator reached
 * through a single indirect table-access block. This header describes that
 * block and the per-flow NAPT table model the flow-offload path programs so
 * established WAN<->LAN connections are NAT'd/forwarded in hardware instead of
 * on the CPU. Clean-room: register offsets, field positions and table geometry
 * are hardware facts, re-expressed here as new idiomatic definitions.
 *
 * Copyright (C) 2026 Confiared <contact@confiared.com>
 */
#ifndef _RTL9602C_L34_H
#define _RTL9602C_L34_H

#include <linux/types.h>
#include <linux/if.h>
#include <linux/bitmap.h>
#include <linux/mutex.h>
#include <linux/io.h>

#include "gpon_edge.h"	/* core: the router edge, read from the live kernel */

/* Indirect table-access block */
#define L34_CMD			0x800100	/* command/trigger register */
#define  L34_CMD_RD_EXE		BIT(25)		/* start read  (self-clears when done) */
#define  L34_CMD_WR_EXE		BIT(24)		/* start write (self-clears when done) */
#define  L34_CMD_TYPE_SHIFT	16		/* [19:16] table type */
#define  L34_CMD_TYPE_MASK	0xf
#define  L34_CMD_IDX_MASK	0xffff		/* [15:0] entry index */

#define L34_CLR			0x800104	/* per-table-type reset, self-clearing */
#define L34_RDATA		0x800108	/* read-data bank base  (word0..) */
#define L34_WDATA		0x80011c	/* write-data bank base (word0..) */
#define L34_SWTCR0		0x800010	/* engine control (NAT mode / flow route) */
#define  L34_SWTCR0_TTL_MINUS	BIT(12)
#define  L34_SWTCR0_FRAG2CPU	BIT(23)
#define  L34_SWTCR0_V6FLRT_EN	BIT(29)		/* IPv6 flow routing	*/
#define  L34_SWTCR0_V4FLRT_EN	BIT(30)		/* IPv4 flow routing	*/
#define  L34_SWTCR0_CF_SIP_ARP_TRF_EN BIT(31)	/* NOT a routing enable */
#define  L34_SWTCR0_NATMODE_SH	10		/* [11:10] b0=L3 NAT en, b1=L4 NAT en */
/*
 * [9:8] the chipdef calls this LIMDBC. "0=VLAN-base, 2=MAC-base" is an
 * unverified lead; what is measured is that stock holds 2 here, and so does
 * this driver.
 */
#define  L34_SWTCR0_LIMDBC_SH	8
/*
 * WAN binding policy: what the die does with a frame whose WAN binding does not
 * match, one field per L2/L3/L34 combination. Action value 0 is drop (the
 * vendor's rtk_l34_bindAct_t: L34_BIND_ACT_DROP = 0). This register reads all
 * zero under this driver while stock holds 0xa2400000. With the binding table
 * unwritten nothing can match, so every unmatched combination takes the drop
 * action and the accelerated path discards what the CPU then forwards.
 */
/*
 * The die's own view of the lookup: HSB is what the engine saw (the hash source
 * block it built from the frame) and HSA is what it decided. Stock runs with
 * the log mode at LOG_ALL; at NO_LOG every descriptor reads zero. Mode values
 * are the vendor's (rtk_l34_hsba_mode_t): 0 BOTH_LOG, 1 NO_LOG, 2 LOG_ALL, 3
 * LOG_FIRST_DROP, 4 LOG_FIRST_PASS, 5 LOG_FIRST_TO_CPU, 6 LOG.
 */
#define L34_HSBA_CTRL		0x800200
#define  L34_HSBA_TST_LOG_MD_SH	2		/* [4:2] */
#define  L34_HSBA_TST_LOG_MD_M	0x7u
#define  L34_HSBA_MODE_NO_LOG	1
#define  L34_HSBA_MODE_LOG_ALL	2		/* what stock holds */
/*
 * LOG_ALL keeps the last lookup, which is always the ssh packet carrying the
 * command that reads the log. LOG_FIRST_DROP latches the first dropped frame,
 * so the event survives every later lookup, including the read's own.
 */
#define  L34_HSBA_MODE_FIRST_DROP 3
#define L34_HSB_DESC0		0x800204	/* 14 words */
#define L34_HSB_WORDS		14
#define L34_HSA_DESC0		0x800280	/* 5 words */
#define L34_HSA_WORDS		5

#define L34_BD_CFG		0x80002c
/*
 * Not written. Stock's value was tried and moved hits_seen only from 0 to 1
 * over a 204896-frame transit, so a value only partly decoded stays out of the
 * shipped path; the address and decode remain for reference.
 */
#define  L34_BD_CFG_STOCK	0xa2400000	/* this board's own vendor value */
#define L34_GLB_CFG		0x01106c	/* master L34 routing enable */
#define L34_NAPT_HIT		0x800400	/* outbound NAPT hit/age bitmap (idx/32 words) */

/* Table types (L34_CMD_TYPE). The NAPT path is a hashed pair */
enum l34_tbl {
	L34_TBL_L3ROUTE		= 0,	/* 16 entries, 2 words: LPM route -> nexthop */
	L34_TBL_PPPOE		= 1,	/* 8 entries,  1 word:  the negotiated session id */
	L34_TBL_NEXTHOP		= 2,	/* 16 entries, 1 word:  egress intf + L2 (ARP) index */
	L34_TBL_NETIF		= 3,	/* 16 entries, 4 words: per-interface MAC/VLAN/MTU/IP */
	L34_TBL_EXTIP		= 4,	/* 8 entries,  3 words: external (WAN) IP -> nexthop */
	L34_TBL_NAPTR_IN	= 9,	/* 4096 entries, 3 words: inbound rewrite (NAT tuple) */
	L34_TBL_NAPT_OUT	= 10,	/* 4096 entries, 1 word:  outbound hash slot */
	L34_TBL_ARP		= 13,	/* 128 entries, 2 words: IP -> L2 (next-hop MAC) index */
};

/* Word counts per table type (data-bank words moved per op). */
#define L34_WORDS_L3ROUTE	2
#define L34_WORDS_PPPOE		1
#define L34_WORDS_NEXTHOP	1
#define L34_WORDS_NETIF		4
#define L34_WORDS_EXTIP		3
#define L34_WORDS_NAPTR_IN	3
#define L34_WORDS_NAPT_OUT	1
#define L34_WORDS_ARP		2

#define L34_NAPT_ENTRIES	4096
#define L34_NAPT_WAYS		4		/* 4-way bucket: index = (hash << 2) + way */

/* Entry field LAYOUTS */
#define L34_EXTIP_SLOTS		8	/* EXTIP/EXTIP_IDX is 3-bit: netif idx must be < 8 */
#define L34_NETIF_DEF_VLAN	1

/* The two interface slots this driver provisions. */
#define L34_NETIF_WAN		0
#define L34_NETIF_LAN		1

/* Two more index CHOICES the same argument covers, and */
#define L34_RT_CPU_SLOT_OFF	8	/* L3ROUTE[netif + 8] = that netif's CPU self-route */

/*
 * The general LPM pool: L3ROUTE[2..7]. The table's convention is L3ROUTE[netif]
 * = that netif's local route and L3ROUTE[netif + 8] = its CPU self-route, and
 * this driver provisions exactly two netifs (0 = WAN, 1 = LAN), so slots 2..7
 * are free for real routes. Provisioning a third netif would collide, so
 * l34_rt_pool_slot() refuses an index the pool cannot hold.
 */
#define L34_RT_POOL_BASE	2
#define L34_RT_POOL_N		6
#define L34_ARP_WAN_BASE	64	/* the WAN half of the 128-entry ARP table */

/* L2 unicast table (the gateway/peer destination MAC). */
#define L2_CMD			0x12000
#define  L2_CMD_TYPE_SH		0	/* [2:0] table type (0 = L2_UC) */
#define  L2_CMD_WR		BIT(3)	/* 0 = read, 1 = write */
#define  L2_CMD_METHOD_SH	4	/* [6:4] 0 = MAC-hash, 1 = direct address */
#define L2_STS			0x12004
/* the ADDR/CAM/HIT decode bits moved to rtl9602c_l34_logic.h with
 * l34_l2uc_sts_index(); BUSY stays here with the poll that owns it */
#define  L2_STS_BUSY		BIT(13)
#define L2_WDATA		0x12008	/* word0..2 @ +4 */
#define L2_RDATA		0x1201c	/* word0..2 @ +4 */
#define L34_WORDS_L2UC		3
#define L2_METHOD_MAC		0
#define L2_METHOD_ADDR		1
#define L2_CMD_ADDR_SH		9
#define L2_CF_MATCH_TYPE		4
#define L2_CF_ACTION_TYPE		5
#define L2_CF_RULE_BASE		256
#define L34_CF_CFG		0x1600c
#define L34_CF_P1_COUNT_SH	5
#define L34_CF_L2_WAN_SH		17
#define L34_CF_ACTION_CTRL	0x16038
#define L34_CF_SID_ENABLE	BIT(5)

/* Per-flow programming request, filled by the flow-offload glue (endian-safe:
 * addresses/ports are kept in host order here and packed with explicit math). */
struct gpon_flow_offload;

struct l34_flow {
	u8	l4proto;		/* IPPROTO_TCP / IPPROTO_UDP */
	u32	orig_sip, orig_dip;	/* ingress (original-direction) 5-tuple */
	u16	orig_sport, orig_dport;
	u32	nat_sip, nat_dip;	/* post-NAT addresses (0 = unchanged) */
	u16	nat_sport, nat_dport;	/* post-NAT ports (0 = unchanged) */
	u8	egress_netif;		/* L34_TBL_NETIF index of the output interface */
	u8	nexthop;		/* L34_TBL_NEXTHOP index (gateway L2) */
	u16	hw_index;		/* assigned NAPT slot, valid after add (for del/stats) */
	/*
	 * The identities are retained, not re-read from hardware. A write that
	 * timed out does not establish whether the engine took it, so neither
	 * index may be recycled and neither entry may be assumed gone.
	 * Re-reading the outbound slot cannot work either: retiring zeroes that
	 * slot, so a retry after a partial retirement would orphan the rewrite
	 * entry for the life of the board. Index 0 is valid, so ownership is
	 * its own flag.
	 */
	u16	naptr_index;		/* retained after the outbound slot is cleared */
	bool	out_owned, in_owned;	/* index zero is valid; writes may time out */
	bool	installed;		/* this flow's contribution to `installs` */
	bool	neigh_owned;		/* downstream ARP reference at hw_index */
};

struct l34_neigh {
	u32 ip;
	u16 l2idx, users;
	u8 mac[6];
	bool ready, arp_live, l2_pinned, arp_was_used;
};

struct rtl9602c_l34 {
	void __iomem	*sw;		/* switch-core MMIO base (offsets above are relative) */
	struct mutex	lock;		/* serialises table ops */
	bool		ready;		/* table-access plumbing usable */
	bool		engine_on;	/* NAT engine enabled (deferred, lazy) */
	bool		provisioned;	/* the interface tables hold real values */
	struct gpon_edge edge;		/* ...and THESE are the values they hold */
	/*
	 * An invalid hardware row may still have a live or pending owner, so
	 * the free-way scan may not offer it again: the row reads invalid while
	 * a timed-out write is still in flight, and handing it to a second flow
	 * puts two connections on one slot.
	 */
	DECLARE_BITMAP(out_reserved, L34_NAPT_ENTRIES);
	DECLARE_BITMAP(in_reserved, L34_NAPT_ENTRIES);
	struct l34_neigh lan_neigh[L34_ARP_WAN_BASE];
	u32		installs;	/* flows programmed into the engine	*/
	u32		removals;
	u32		hits_seen;	/* set hit bits observed -- see the node */
	/*
	 * One reader for a clear-on-read bitmap: with two, each consumes the
	 * other's evidence, and a diagnostic can age out a live flow. The
	 * bitmap is swept once per jiffy into a sticky shadow; liveness
	 * consumes only its own slot, and the diagnostic consumes nothing.
	 */
	u32		hit_shadow[L34_NAPT_ENTRIES / 32];
	unsigned long	hit_swept;	/* jiffies of the last sweep */
#define L34_REFUSE_REASONS 8	/* distinct refusal sites; a 9th is COUNTED as other */
	u32		refusals;	/* flows left on the software path	*/
	u32		ds_legs;	/* downstream neighbour requests */
	/*
	 * The last few NAPT indexes this driver installed, so the dump can read
	 * the entries back and show whether the installed key matches the flow.
	 * A ring: it costs 16 bytes and covers the most recent flows.
	 */
#define L34_RECENT		8
	u16		recent_idx[L34_RECENT];
	u8		recent_n;

	/*
	 * The last few flow keys the core offered, recorded before any refusal
	 * so a declined one is visible too. The counters name causes, never the
	 * flow they were about.
	 */
	struct l34_offer {
		u32	sip, dip;
		u16	sport, dport;
		u8	proto;
		u8	ds_leg;			/* the core's leg decision for this key */
		char	iif[IFNAMSIZ];		/* the ingress the flowtable offered */
	}		recent_offer[L34_RECENT];
	u8		offer_n;
	u32		binds;		/* flowtable blocks this driver accepted	*/
	u32		offered;	/* per-flow requests that REACHED this driver */
	struct gpon_flow_offload *fo;	/* the COMMON lifecycle, for its diag line */
	u32		vlan_refused;	/* ...of those, ones carrying a VLAN tag */
	u16		vlan_refused_vid;
	const char	*refuse_why;	/* ...and the reason for the last one	*/
	/*
	 * A tally per reason, because the last reason alone cannot say whether
	 * it was 1 of 722 or 722 of 722. The reasons are static literals (see
	 * l34_refuse), so the pointer is the key: no allocation, no strcmp, and
	 * a slot per distinct site.
	 */
	struct {
		const char	*why;
		u32		n;
	} refuse_tally[L34_REFUSE_REASONS];
};

/* Public API (mainline flow-offload glue calls these). */
int  rtl9602c_l34_init(struct rtl9602c_l34 *l, void __iomem *sw);
int  rtl9602c_l34_wan_setup(struct rtl9602c_l34 *l, u8 idx, u32 wan_ip,
			    const u8 *wan_mac, u32 gw_ip, const u8 *gw_mac,
			    u8 wan_port, u16 vlan,
			    u16 pppoe_sid);
int  rtl9602c_l34_lan_setup(struct rtl9602c_l34 *l, u8 idx, u32 lan_ip,
			    const u8 *lan_mac, u32 lan_net, u8 prefix, u16 vlan);
int  rtl9602c_l34_provision(struct rtl9602c_l34 *l, const struct gpon_edge *e);
int  rtl9602c_l34_wan_release(struct rtl9602c_l34 *l, bool core_empty);
bool rtl9602c_l34_has_owners(struct rtl9602c_l34 *l);
int  rtl9602c_l34_flow_add(struct rtl9602c_l34 *l, struct l34_flow *f);
int  rtl9602c_l34_flow_del(struct rtl9602c_l34 *l, struct l34_flow *f);
int  rtl9602c_l34_flow_hit(struct rtl9602c_l34 *l, u16 hw_index, bool *active);
void rtl9602c_l34_proc_init(struct rtl9602c_l34 *l);	/* bring-up test harness */

#endif /* _RTL9602C_L34_H */
