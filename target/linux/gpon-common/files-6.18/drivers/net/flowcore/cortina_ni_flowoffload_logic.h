/* SPDX-License-Identifier: GPL-2.0-only */
/* cortina_ni_flowoffload_logic.h -- logic hoisted out of ...
 * dev/MEASURED-cortina_ni_flowoffload_logic.h.md sec 1. */
#ifndef _CORTINA_NI_FLOWOFFLOAD_LOGIC_H
#define _CORTINA_NI_FLOWOFFLOAD_LOGIC_H

#include <linux/types.h>
#include <linux/bits.h>		/* BIT() - the punt verdict bits */
#include <linux/build_bug.h>	/* static_assert on the packed key */
#include "flowcore.h"		/* pi_packed_* - the shared packed-slot math */

void cn_l3e_bitrev_key(u32 *w, int n_words);
u32 cn_l3e_crc32(const u8 *p, size_t len);
u16 cn_l3e_crc16(const u8 *p, size_t len);
u32 cn_l3e_proc_parse_ip(const char *s);

/* the 92-byte packed SW key below = the CRC input (chip fact) */
#define CN_L3E_KEY_BYTES		92	/* packed key = CRC input */

/* ★ HW HDR_I descriptor layout - the key the SWO engine ...
 * dev/MEASURED-cortina_ni_flowoffload_logic.h.md sec 2. */
#define CN_L3E_HDRI_BYTES		128
#define CN_L3E_HDRI_WORDS		(CN_L3E_HDRI_BYTES / 4)
/* 5-tuple + IP validity - each proven LIVE (moves the SWO CRC) on the real
 * engine, and each re-derived tier-2 from the stock HDR_I packer (above);
 * ip_ver/ip_vld sit at [504:505] in BOTH sources. */
#define CN_HDRI_L4_DP			74	/* dest L4 port, 16b */
#define CN_HDRI_L4_SP			90	/* src  L4 port, 16b */
#define CN_HDRI_IP_DA0			233	/* IPv4 DA / v6 DA LSW; 128b field [233:360] */
#define CN_HDRI_IP_SA0			361	/* IPv4 SA / v6 SA LSW; 128b field [361:488] */
#define CN_HDRI_IP_L4_TYPE		489	/* 3b; masked under mask 0, kept for other masks */
#define CN_HDRI_IP_PROTO		492	/* IP protocol, 8b */
#define CN_HDRI_IP_VER			504	/* 1b: 0 = IPv4 */
#define CN_HDRI_IP_VLD			505	/* 1b: 1 = has an IP header */
/* profile id stamp: HDR_I t2_ctrl (== the SW key's ...
 * dev/MEASURED-cortina_ni_flowoffload_logic.h.md sec 3. */
#define CN_HDRI_T2_CTRL			961	/* 4b, a_cut */

/* Flow key / action - packed to the engine's exact bit layout
 * dev/MEASURED-cortina_ni_flowoffload_logic.h.md sec 4. */

struct cn_l3e_key {
	/* L4 */
	u64 l4_chksum_zero	: 1;
	u64 tcp_flags		: 9;
	u64 l4_dport		: 16;
	u64 l4_sport		: 16;
	/* L3 */
	u64 l3_chksum_err	: 1;
	u64 spi			: 32;
	u64 spi_vld		: 3;
	u64 icmp_type		: 8;
	u64 icmp_vld		: 3;
	u64 ipv6_doh		: 1;
	u64 ipv6_rh		: 1;
	u64 ipv6_hbh		: 1;
	u64 ip_fragment		: 1;
	u64 ip_da_sa_equal	: 1;
	u64 ip_options		: 1;
	u64 ip_ttl		: 8;
	u64 ipv6_flow_lbl	: 20;
	u64 ip_da_0		: 32;	/* v4 DA or v6 DA LSW, host order */
	u64 ip_da_1		: 32;
	u64 ip_da_2		: 32;
	u64 ip_da_3		: 32;
	u64 ip_sa_0		: 32;	/* v4 SA or v6 SA LSW, host order */
	u64 ip_sa_1		: 32;
	u64 ip_sa_2		: 32;
	u64 ip_sa_3		: 32;
	u64 ip_l4_type		: 3;
	u64 ip_protocol		: 8;
	u64 ip_ecn		: 2;
	u64 ip_dscp		: 6;
	u64 ip_ver		: 1;
	u64 ip_vld		: 1;
	/* PPPoE */
	u64 ppp_proto_enc	: 4;
	u64 pppoe_session_id	: 16;
	u64 pppoe_code_enc	: 4;
	u64 pppoe_type		: 2;
	/* VLAN */
	u64 inner_dei		: 1;
	u64 inner_pcp		: 3;
	u64 inner_vid		: 12;
	u64 inner_tpid_enc	: 3;
	u64 top_dei		: 1;
	u64 top_pcp		: 3;
	u64 top_vid		: 12;
	u64 top_tpid_enc	: 3;
	u64 vlan_cnt		: 2;
	/* L2 format */
	u64 llc_type_enc	: 2;
	u64 llc_snap		: 2;
	u64 pktlen_rng_vec	: 4;
	u64 len_encoded		: 1;
	/* L2 */
	u64 ethertype_enc	: 6;
	u64 ethertype		: 16;
	u64 mac_sa_0		: 8;
	u64 mac_sa_1		: 8;
	u64 mac_sa_2		: 8;
	u64 mac_sa_3		: 8;
	u64 mac_sa_4		: 8;
	u64 mac_sa_5		: 8;
	u64 mac_da_rsvd		: 1;
	u64 mac_da_rng		: 1;
	u64 mac_da_ip_mc	: 1;
	u64 mac_da_an_sel	: 4;
	u64 mac_da_0		: 8;
	u64 mac_da_1		: 8;
	u64 mac_da_2		: 8;
	u64 mac_da_3		: 8;
	u64 mac_da_4		: 8;
	u64 mac_da_5		: 8;
	/* special packet */
	u64 spcl_pkt_hdr_mtch	: 8;
	u64 spcl_pkt_enc	: 6;
	/* metadata */
	u64 mdata		: 64;
	/* policer / cos */
	u64 qos_premark		: 1;
	u64 pol_grp_id		: 3;
	u64 pol_id		: 9;
	u64 cos			: 3;
	/* dest / source port */
	u64 mcgid		: 10;
	u64 mc			: 1;
	u64 mc_idx_vld		: 1;
	u64 orig_lspid		: 6;
	u64 lspid		: 6;
	/* hash control */
	u64 hkey_id		: 6;	/* mask-table index */
	u64 ctrl_set_id		: 4;	/* profile id (CRC input, then zeroed by its mask bit) */
	u64 table_id		: 4;
	u64 reserved		: 4;
} __packed;

static_assert(sizeof(struct cn_l3e_key) == CN_L3E_KEY_BYTES);

/* HDR_I packing - the divergence-A fix (see cn_l3e_build_hdri) */
void cn_l3e_hdri_set(u8 *h, unsigned int off, unsigned int width, u64 val);
void cn_l3e_build_hdri(const struct cn_l3e_key *k, int profile,
		       u32 words[CN_L3E_HDRI_WORDS]);

/* independent bit-numbered FIB readback (the anti-SID2QID instrument) */
u64 cn_fib_field(const void *fib, u32 bit, u32 width);

enum cn_pppoe_leg_verdict {
	CN_PPPOE_LEG_OK = 0,
	CN_PPPOE_LEG_MODE_OFF,		/* hw_pppoe=0: PPPoE stays in software */
	CN_PPPOE_LEG_NO_PUSH,		/* US rule cannot express the encap */
	CN_PPPOE_LEG_UNEXPECTED_PUSH,	/* push on the reply leg = un-RE'd model */
};

enum cn_pppoe_leg_verdict cn_pppoe_leg_check(bool pppoe_mode, bool ds_leg,
					     u16 rule_sid, u16 armed_sid);
bool cn_pppoe_shadow_stale(bool ds_leg, bool egress_is_lan,
			   u16 rule_sid, u16 armed_sid);

/* cn_pppoe_punt_classify() verdict bits */
#define CN_PPPOE_PUNT_SESSION		BIT(0)	/* it IS a 0x8864 session frame */
#define CN_PPPOE_PUNT_CTRL		BIT(1)	/* PPP control, no inner IPv4 */
#define CN_PPPOE_PUNT_SID_BAD		BIT(2)
#define CN_PPPOE_PUNT_LEN_BAD		BIT(3)	/* ★ the mangle signature */
#define CN_PPPOE_PUNT_TCP_BAD		BIT(4)	/* ★ the mangle signature */
#define CN_PPPOE_PUNT_SHORT		BIT(5)	/* truncated before a needed header */
#define CN_PPPOE_PUNT_DATA		BIT(6)	/* inner PPP proto is IPv4 = judged */
#define CN_PPPOE_PUNT_SHIFT8		BIT(7)	/* ★ malformed AND = an 8-byte insert */
#define CN_PPPOE_PUNT_DBLENC		BIT(8)	/* ★ malformed AND = a double encap */

/* what the classifier decoded, for the diagnostic line */
struct cn_pppoe_punt_info {
	u16	sid;
	u16	ppp_proto;
	u16	pppoe_len;
	u16	ip_len;
	u8	ip_ver;
	u8	ihl;
	u8	tcp_doff;
	u8	tcp_flags;
};

u32 cn_pppoe_punt_classify(const u8 *f, unsigned int len, u16 exp_sid,
			   struct cn_pppoe_punt_info *pi);

/* SWO CRC algebra - one MSB-first CRC LFSR step per polynomial
 * dev/MEASURED-cortina_ni_flowoffload_logic.h.md sec 5. */
#define CN_L3E_SWO_POLY32		0x04C11DB7u
#define CN_L3E_SWO_POLY16		0x1021u

u32 cn_l3e_poly32_step(u32 d);
u16 cn_l3e_poly16_step(u16 d);

/* The packed 4-slot TPID table: slots 0-3 as {lo, hi} halves ...
 * dev/MEASURED-cortina_ni_flowoffload_logic.h.md sec 6. */
u16 cn_tpid_slot_at(const u32 w[2], unsigned int i);
int cn_tpid_find(const u32 w[2], u16 tpid);	/* slot index, or -1 */

/* cn_wan_vlan_walk_verdict() verdicts - the under-encap tail ...
 * dev/MEASURED-cortina_ni_flowoffload_logic.h.md sec 7. */
enum cn_wan_vlan_verdict {
	CN_WAN_VLAN_OK_PPPOE = 0,	/* tag + session both expressible */
	CN_WAN_VLAN_WALK_MISMATCH,	/* walk failed, or resolved another vid */
	CN_WAN_VLAN_BAD_TPID,		/* not 0x8100: no registered TPID slot */
	CN_WAN_VLAN_NO_SID,		/* a tag under something not RE'd */
	CN_WAN_VLAN_NO_MAC,		/* no access-concentrator MAC resolved */
};

enum cn_wan_vlan_verdict cn_wan_vlan_walk_verdict(u16 want_vid, bool walk_ok,
						  int walk_vid,
						  bool tpid_8021q, int sid,
						  bool ac_mac_vld);

/* Where a leg's WAN 802.1Q tag comes from.  DEV: the WAN-side device IS (or
 * rides) the VLAN upper, so the chain walk resolves it.  RULE: the real device
 * is in the flowtable, so nf_flow_table emits the tag as the rule's own
 * VLAN_PUSH (US) / VLAN_POP (DS), carried by the core as act->vlan_*. */
enum cn_wan_tag_src {
	CN_WAN_TAG_NONE = 0,
	CN_WAN_TAG_DEV,
	CN_WAN_TAG_RULE,
};

/* -> the source, or -EINVAL when the rule pushes a VID the device does not
 * carry.  *vid: the tag's VID; 0 on a DS RULE leg, whose pop names none. */
int cn_wan_tag_resolve(bool ds_leg, u16 dev_vid, u16 rule_vid, bool rule_pop,
		       u16 *vid);

/* ===== round 3 (2026-09-02): the packed-slot idioms spelled ...
 * dev/MEASURED-cortina_ni_flowoffload_logic.h.md sec 8. */
u32 cn_tpid_slot_store(u32 word, unsigned int i, u16 tpid);

/* ★ MAIN-HASH age SRAM slot geometry, ONE fact (this die - ...
 * dev/MEASURED-cortina_ni_flowoffload_logic.h.md sec 9. */
struct pi_packed_slot cn_age2_slot(u32 idx);

/* geometry + the 2-bit age codes (moved from the shell with the helpers
 * that encode them; names kept so no call site changed) */
#define CN_L3E_HASH_WAYS	8	/* hb_size = 1 (stock live) */
#define CN_L3E_AGE_SLOTS	32	/* slots per age row, fixed */
/* 2-bit MAIN-HASH age codes (this die): 0=free/invalid, 1..2 = valid+aging (HW
 * re-arms on hit), 3 = static.  START(2) = go-live / HW-hit re-arm. */
#define CN_L3E_AGE_FREE		0
#define CN_L3E_AGE_IDLE		1	/* set by the stats sweep; HW re-arms on hit */
#define CN_L3E_AGE_START	2
#define CN_L3E_AGE_STATIC	3

/* One 16-slot age word of the batch traffic sweep: every slot ...
 * dev/MEASURED-cortina_ni_flowoffload_logic.h.md sec 10. */
u32 cn_age2_sweep_word(u32 w, u16 *rearmed);

/* SW way-pick inside the 8-way hash bucket (stock hb_size = ...
 * dev/MEASURED-cortina_ni_flowoffload_logic.h.md sec 11. */
int cn_hs_way_pick(const u32 *crc32_tbl, u32 crc16, u32 crc32, u32 *idx_out);

#endif /* _CORTINA_NI_FLOWOFFLOAD_LOGIC_H */
