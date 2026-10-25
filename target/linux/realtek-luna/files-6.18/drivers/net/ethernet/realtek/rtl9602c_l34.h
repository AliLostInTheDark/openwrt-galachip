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

/* Indirect table-access block (byte offsets within the ...
 * dev/MEASURED-rtl9602c_l34.h.md sec 7. */
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
/* ⚠⚠ THESE BIT NAMES WERE WRONG UNTIL 2026-09-12, AND THE ...
 * dev/MEASURED-rtl9602c_l34.h.md sec 1. */
#define  L34_SWTCR0_V6FLRT_EN	BIT(29)		/* IPv6 flow routing	*/
#define  L34_SWTCR0_V4FLRT_EN	BIT(30)		/* IPv4 flow routing	*/
#define  L34_SWTCR0_CF_SIP_ARP_TRF_EN BIT(31)	/* NOT a routing enable */
#define  L34_SWTCR0_NATMODE_SH	10		/* [11:10] b0=L3 NAT en, b1=L4 NAT en */
/* [9:8] the chipdef calls this LIMDBC.  The old comment read "0=VLAN-base,
 * 2=MAC-base"; that reading is UNVERIFIED and is kept only as a lead -- what IS
 * measured is that stock holds 2 here and so do we. */
#define  L34_SWTCR0_LIMDBC_SH	8
/* WAN BINDING policy: what the die does with a frame whose WAN binding does NOT
 * match, one field per L2/L3/L34 combination. ⚠ THE ACTION VALUE 0 IS *DROP*
 * (the vendor's own `rtk_l34_bindAct_t`: L34_BIND_ACT_DROP = 0), and this
 * register reads ALL ZERO on our image while stock holds 0xa2400000 -- measured
 * on this board, both captures the same hour. With the BINDING table unwritten
 * nothing can ever match, so every unmatched combination takes the DROP action
 * and the accelerated path discards what the CPU then forwards. */
/* ★★★ THE DIE'S OWN VIEW OF THE LOOKUP -- the instrument that replaces guessing.
 * HSB is what the engine SAW (the hash source block it built from the frame) and
 * HSA is what it DECIDED. Stock runs with the log mode at LOG_ALL and its HSB
 * descriptors are populated; ours ran at NO_LOG, which is why every descriptor
 * read zero and two register hypotheses had to be settled by rebuilding the
 * image instead of by reading what the die already knows.
 * Mode values are the vendor's own (rtk_l34_hsba_mode_t): 0 BOTH_LOG, 1 NO_LOG,
 * 2 LOG_ALL, 3 LOG_FIRST_DROP, 4 LOG_FIRST_PASS, 5 LOG_FIRST_TO_CPU, 6 LOG. */
#define L34_HSBA_CTRL		0x800200
#define  L34_HSBA_TST_LOG_MD_SH	2		/* [4:2] */
#define  L34_HSBA_TST_LOG_MD_M	0x7u
#define  L34_HSBA_MODE_NO_LOG	1
#define  L34_HSBA_MODE_LOG_ALL	2		/* what stock holds */
/* ⚠⚠ LOG_ALL IS THE WRONG MODE FOR US, MEASURED 2026-09-19. It keeps the LAST
 * lookup, and the last lookup is always the ssh packet carrying the command that
 * reads the log: the first capture came back holding 192.168.1.2 -> 192.168.1.12
 * port 22, this host talking to the board. The instrument was measuring itself.
 * LOG_FIRST_DROP LATCHES the first dropped frame instead, so the event survives
 * every later lookup including the read's own. */
#define  L34_HSBA_MODE_FIRST_DROP 3
#define L34_HSB_DESC0		0x800204	/* 14 words */
#define L34_HSB_WORDS		14
#define L34_HSA_DESC0		0x800280	/* 5 words */
#define L34_HSA_WORDS		5

#define L34_BD_CFG		0x80002c
/* ⚠ NOT WRITTEN, and the refutation is why. Setting this to stock's value was
 * TESTED on 2026-09-18 -- one build, one boot, one 20 s transit of 204 896
 * frames -- and `hits_seen` moved from 0 to 1. One hit is not forwarding; it is
 * the order of magnitude the liveness probe itself touches. A speculative write
 * of a value only partly decoded, with no measured benefit, does not belong in
 * the shipped path, so the ADDRESS and the DECODE stay here for the next
 * attempt and the write does not. */
#define  L34_BD_CFG_STOCK	0xa2400000	/* this board's own vendor value */
#define L34_GLB_CFG		0x01106c	/* master L34 routing enable */
#define L34_NAPT_HIT		0x800400	/* outbound NAPT hit/age bitmap (idx/32 words) */

/* Table types (L34_CMD_TYPE). The NAPT path is a hashed pair: ...
 * dev/MEASURED-rtl9602c_l34.h.md sec 2. */
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

/* Entry field LAYOUTS (L34_NAPT_* / L34_NAPTR_* / L34_EXTIP_* ...
 * dev/MEASURED-rtl9602c_l34.h.md sec 3. */
#define L34_EXTIP_SLOTS		8	/* EXTIP/EXTIP_IDX is 3-bit: netif idx must be < 8 */
#define L34_NETIF_DEF_VLAN	1

/* The two interface slots this driver provisions. They are a ...
 * dev/MEASURED-rtl9602c_l34.h.md sec 4. */
#define L34_NETIF_WAN		0
#define L34_NETIF_LAN		1

/* Two more index CHOICES the same argument covers, and they ...
 * dev/MEASURED-rtl9602c_l34.h.md sec 5. */
#define L34_RT_CPU_SLOT_OFF	8	/* L3ROUTE[netif + 8] = that netif's CPU self-route */

/* The general LPM pool: L3ROUTE[2..7].  The table's convention is
 * L3ROUTE[netif] = that netif's local route and L3ROUTE[netif + 8] = its CPU
 * self-route, and this driver provisions exactly TWO netifs (0 = WAN, 1 = LAN),
 * so slots 2..7 are unreachable by that convention and free for real routes.
 * ⚠ THE CONSTRAINT IS LOAD-BEARING: provisioning a third netif would collide,
 * so l34_rt_pool_slot() refuses an index the pool cannot hold. */
#define L34_RT_POOL_BASE	2
#define L34_RT_POOL_N		6
#define L34_ARP_WAN_BASE	64	/* the WAN half of the 128-entry ARP table */

/* L2 unicast table (the gateway/peer destination MAC). ...
 * dev/MEASURED-rtl9602c_l34.h.md sec 6. */
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
	/* ★★ THE IDENTITIES ARE RETAINED, NOT RE-READ FROM HARDWARE.  A write
	 * that TIMED OUT does not establish whether the engine took it, so
	 * neither index may be recycled and neither entry may be assumed gone.
	 * Re-reading the outbound slot to find the rewrite index cannot work
	 * either: retiring zeroes that slot, so a retry after a partial
	 * retirement would find nothing and orphan the rewrite entry for the
	 * life of the board.  Index 0 is VALID, so ownership is its own flag. */
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
	/* ★ AN INVALID HARDWARE ROW MAY STILL HAVE A LIVE OR PENDING OWNER, so
	 * the free-way scan may not offer it again: the row reads invalid while
	 * a timed-out write is still in flight, and handing it to a second flow
	 * puts two connections on one slot. 1 KiB, and it is the only thing that
	 * knows the difference. */
	DECLARE_BITMAP(out_reserved, L34_NAPT_ENTRIES);
	DECLARE_BITMAP(in_reserved, L34_NAPT_ENTRIES);
	struct l34_neigh lan_neigh[L34_ARP_WAN_BASE];
	u32		installs;	/* flows programmed into the engine	*/
	u32		removals;
	u32		hits_seen;	/* set hit bits observed -- see the node */
	/* ★★ ONE READER FOR A CLEAR-ON-READ BITMAP.  L34_NAPT_HIT clears on
	 * read, and it had TWO readers: the flowtable's per-flow liveness op
	 * and /proc/flowdump.  Each consumed the other's evidence -- the
	 * diagnostic could age a live flow OUT by clearing the bit its own GC
	 * was about to look for, and the witness read zeros because the GC had
	 * just harvested them.  The bitmap is now swept ONCE per jiffy into a
	 * sticky shadow; liveness consumes only its OWN slot, and the
	 * diagnostic consumes nothing, because a DIAG may not destroy evidence
	 * a control path needs. */
	u32		hit_shadow[L34_NAPT_ENTRIES / 32];
	unsigned long	hit_swept;	/* jiffies of the last sweep */
#define L34_REFUSE_REASONS 8	/* distinct refusal sites; a 9th is COUNTED as other */
	u32		refusals;	/* flows left on the software path	*/
	u32		ds_legs;	/* downstream neighbour requests */
	/* The last few NAPT indexes this driver INSTALLED, so the dump can read
	 * the entries BACK.  /proc/flowdump rendered the hit BITMAP and the
	 * interface tables and never the entries, so *does the installed key
	 * match the flow?* could not be asked from the board at all -- the shape
	 * this project calls a register the driver writes and cannot read back,
	 * where nothing can disagree with us.  MEASURED 2026-09-16: installs=15,
	 * live=1, hits=11 across a 105 Mbps flow, i.e. an entry exists and the
	 * traffic is not matching it, and no instrument could say which field is
	 * wrong.  A ring, not a list: it costs 16 bytes and answers the question
	 * for the flows that matter, which are the most recent. */
#define L34_RECENT		8
	u16		recent_idx[L34_RECENT];
	u8		recent_n;

	/* The last few flow keys the core OFFERED, recorded BEFORE any refusal so
	 * a declined one is visible too.  MEASURED 2026-09-16: offered=188,
	 * installed=7, live=1, why{dup-cookie=87 engine=94} -- and the single LIVE
	 * entry was UDP while the throughput legs were TCP, so what was being
	 * accelerated was the latency probe.  Whether a TCP 5-tuple was ever
	 * offered at all could not be asked: the counters name CAUSES, never the
	 * flow they were about. */
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
	/* ★ AND A TALLY PER REASON, because the LAST one answers the wrong
	 * question. MEASURED on the X111W: 722 engine refusals collapsed into one
	 * count and one string, so "why did 1.6% of upstream flows install" could
	 * not be asked at all -- the node named a cause and could not say whether
	 * it was 1 of 722 or 722 of 722.
	 * ⚠ The reasons are STATIC LITERALS (see l34_refuse), so the POINTER is the
	 * key: no allocation, no strcmp, and a slot per distinct site. */
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
