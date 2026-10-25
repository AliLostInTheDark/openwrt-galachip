/* SPDX-License-Identifier: GPL-2.0-only */
/* rtl9602c_l34_logic.h -- logic hoisted out of the ...
 * dev/MEASURED-rtl9602c_l34_logic.h.md sec 1. */
#ifndef _RTL9602C_L34_LOGIC_H
#define _RTL9602C_L34_LOGIC_H

#include <linux/types.h>

struct luna_rx_layout;

void l34_cf_wan_encode(u32 rule[2], u32 mask[2], u32 action[3],
		       u8 netif, u8 sid);

void l34_field_set(u32 *w, unsigned int lsp, unsigned int width, u32 val);
u32 l34_field_get(const u32 *w, unsigned int lsp, unsigned int width);
u16 l34_hash_out(bool is_tcp, u32 sip, u16 sport, u32 dip, u16 dport);
u16 l34_hash_in(bool is_tcp, u32 dip, u16 dport);

/* L34/L2 entry LAYOUTS + their encoders -- dev/MEASURED-rtl9602c_l34_logic.h.md sec 2. */
#define L34_NAPT_HASHIN_IDX_LSP	0	/* -> NAPTR_IN entry index */
#define L34_NAPT_HASHIN_IDX_W	12
#define L34_NAPT_VALID_LSP	12
#define L34_NAPT_VALID_W	1
#define L34_NAPT_PRIVALID_LSP	13
#define L34_NAPT_PRIVALID_W	1
#define L34_NAPT_PRIORITY_LSP	14
#define L34_NAPT_PRIORITY_W	3

/* NAPTR_IN (type 9, 3 words): the inbound / rewrite entry. */
#define L34_NAPTR_INTIP_LSP	0	/* internal (LAN) host IP */
#define L34_NAPTR_INTIP_W	32
#define L34_NAPTR_INTPORT_LSP	32	/* internal host L4 port */
#define L34_NAPTR_INTPORT_W	16
#define L34_NAPTR_REMHASH_LSP	48	/* remote-peer confirm hash (type 2/3) */
#define L34_NAPTR_REMHASH_W	16
#define L34_NAPTR_EXTIPIDX_LSP	64	/* -> EXTIP slot (WAN src IP + next-hop) */
#define L34_NAPTR_EXTIPIDX_W	3
#define L34_NAPTR_EXTPORT_LSP	67	/* post-NAT (WAN) L4 port */
#define L34_NAPTR_EXTPORT_W	16
#define L34_NAPTR_TCP_LSP	83	/* 1 = TCP, 0 = UDP */
#define L34_NAPTR_TCP_W		1
#define L34_NAPTR_VALID_LSP	84	/* 2-bit valid / NAT-type (modes below) */
#define L34_NAPTR_VALID_W	2
#define L34_NAPTR_PRIVALID_LSP	86
#define L34_NAPTR_PRIVALID_W	1
#define L34_NAPTR_PRIORITY_LSP	87
#define L34_NAPTR_PRIORITY_W	3
/* NAPTR VALID/type: 0 invalid; 1 full-cone (remHash ignored); 2 port-restricted
 * (remHash = hash(remIP,remPort)); 3 restricted-cone (remHash = hash(remIP)). */
#define L34_NAPTR_TYPE_INVALID	0
#define L34_NAPTR_TYPE_FULLCONE	1
#define L34_NAPTR_TYPE_IPPORT	2
#define L34_NAPTR_TYPE_IP	3

/* EXTIP (type 4, 3 words, 8 slots): a NAPTR's EXTIP_IDX points here for the WAN
 * source address + the next-hop the rewritten frame egresses through. */
#define L34_EXTIP_INTIP_LSP	0
#define L34_EXTIP_INTIP_W	32
#define L34_EXTIP_EXTIP_LSP	32	/* WAN external IP */
#define L34_EXTIP_EXTIP_W	32
#define L34_EXTIP_VALID_LSP	64
#define L34_EXTIP_VALID_W	1
#define L34_EXTIP_TYPE_LSP	65	/* 0 = NAPT */
#define L34_EXTIP_TYPE_W	2
#define L34_EXTIP_NHIDX_LSP	67	/* -> NEXTHOP slot */
#define L34_EXTIP_NHIDX_W	4

/* NETIF (type 3, 4 words, 16 slots): per-interface MAC/VLAN/MTU/IP. */
#define L34_NETIF_SLOTS		16
#define L34_NETIF_VALID_LSP	0
#define L34_NETIF_VALID_W	1
#define L34_NETIF_VLANID_LSP	1
#define L34_NETIF_VLANID_W	12
#define L34_NETIF_GMAC_LSP	13	/* 48-bit gateway/source MAC, spans w0..w1 */
#define L34_NETIF_GMAC_W	48
#define L34_NETIF_MACMASK_LSP	61
#define L34_NETIF_MACMASK_W	3
#define L34_NETIF_ENRTR_LSP	64	/* enable routing */
#define L34_NETIF_ENRTR_W	1
#define L34_NETIF_MTU_LSP	65
#define L34_NETIF_MTU_W		14
#define L34_NETIF_DSLITE_LSP	79
#define L34_NETIF_DSLITE_W	1
#define L34_NETIF_DSLITE_IDX_LSP	80
#define L34_NETIF_DSLITE_IDX_W	2
#define L34_NETIF_L34_LSP	82	/* classify this interface into the L34 NAT domain */
#define L34_NETIF_L34_W		1
#define L34_NETIF_IP_LSP	83	/* interface IP, spans w2..w3 */
#define L34_NETIF_IP_W		32
#define L34_NETIF_CTAG_LSP	115
#define L34_NETIF_CTAG_W		1
#define L34_NETIF_IPV6_LSP	116
#define L34_NETIF_IPV6_W		1
#define L34_NETIF_DEF_MTU	1500
#define L34_NETIF_DEF_MACMASK	0x7

/* NEXTHOP (type 2, 1 word, 16 slots). nhIdx is an L2-unicast-table index. */
#define L34_NH_TYPE_LSP		0	/* 0 = ETHER, 1 = PPPOE */
#define L34_NH_TYPE_W		1
#define L34_NH_IFIDX_LSP	1	/* -> NETIF slot */
#define L34_NH_IFIDX_W		4
#define L34_NH_NHIDX_LSP	8	/* -> L2 unicast entry holding the dst MAC */
#define L34_NH_NHIDX_W		11
/* The two fields the ETHER path leaves at zero. A PPPoE egress is TYPE = 1 plus
 * an index into the PPPOE table; KEEPPPPOE says what happens to the session
 * header. Positions are the die's own (vendor chipdef NEXT_HOP_TABLE: TYPE 0/1,
 * IFIDX 1/4, PPPIDX 5/3, NXTHOPIDX 8/11, KEEPPPPOP 19/2 -- the first three of
 * which this header already carried). */
#define L34_NH_PPPIDX_LSP	5	/* -> PPPOE table entry holding the session id */
#define L34_NH_PPPIDX_W		3
#define L34_NH_KEEPPPP_LSP	19
#define L34_NH_KEEPPPP_W	2
#define L34_NH_TYPE_ETHER	0	/* the value L34_NH_TYPE already wrote */
#define L34_NH_TYPE_PPPOE	1
/* KEEPPPPOP selects what happens to the session header on egress. The die
 * offers three, and a router NATing LAN traffic onto a PPPoE WAN needs the
 * third: the upstream frame arrives with no header and must GAIN one. */
#define L34_NH_PPPOE_REPLACE	0	/* overwrite with PPPOE[pppidx]	*/
#define L34_NH_PPPOE_KEEP	1	/* leave whatever arrived	*/
#define L34_NH_PPPOE_KEEP_OR_ADD 2	/* keep, else add PPPOE[pppidx]	*/

/* PPPOE table: 8 entries of ONE word, holding the negotiated session id. The
 * session is a property of the EGRESS, not of a flow, so one entry serves every
 * flow leaving through that next hop. */
#define L34_PPPOE_SESID_LSP	0
#define L34_PPPOE_SESID_W	16
#define L34_PPPOE_SLOTS		8

/* ARP_CAM (type 13, 2 words, 128 slots).  The die searches it by the packet's
 * DESTINATION address, so an entry is one destination -> the L2 entry holding
 * that destination's MAC; the gateway is one such destination, not the field's
 * meaning.  Pinned by rtl9607c-test step 17p. */
#define L34_ARP_IP_LSP		0
#define L34_ARP_IP_W		32
#define L34_ARP_VALID_LSP	32
#define L34_ARP_VALID_W		1
#define L34_ARP_NHIDX_LSP	33	/* -> L2 unicast entry */
#define L34_ARP_NHIDX_W		11

/* L3 ROUTE (type 0, 2 words, 16 slots). The per-netif local route (process=ARP)
 * classifies an ingress frame into the L34 NAT domain and sets US/DS direction;
 * it is what an offloaded NAPT flow needs in order to match. Slot = netif idx. */
#define L34_RT_IP_LSP		0
#define L34_RT_IP_W		32
#define L34_RT_MASK_LSP		32	/* prefix code 0..31 (0 => /1 anchor) */
#define L34_RT_MASK_W		5
#define L34_RT_VALID_LSP	37
#define L34_RT_VALID_W		1
#define L34_RT_PROCESS_LSP	38	/* 0=CPU 1=DROP 2=ARP(local) 3=NH(global) */
#define L34_RT_PROCESS_W	2
#define L34_RT_INT_LSP		40	/* 1 = LAN, 0 = WAN */
#define L34_RT_INT_W		1
#define L34_RT_DENTIF_LSP	41	/* netif index (local-route view of [44:41]) */
#define L34_RT_DENTIF_W		4
#define L34_RT_NHIDX_LSP	L34_RT_DENTIF_LSP /* global-route view */
#define L34_RT_NHIDX_W		L34_RT_DENTIF_W
#define L34_RT_RT2WANINF_LSP	45	/* route to WAN interface */
#define L34_RT_RT2WANINF_W	1
#define L34_RT_PROCESS_CPU	0	/* terminate locally (to the CPU) */
#define L34_RT_PROCESS_ARP	2
#define L34_RT_PROCESS_NH	3

/* L2_UC entry (3 words). The 48-bit MAC is packed octet[0] at the field MSB. */
#define L2UC_MAC_LSP		0
#define L2UC_STATIC_LSP		62	/* no-source-learn => static (not aged out) */
#define L2UC_STATIC_W		1
#define L2UC_SPA_LSP		65	/* [66:65] egress port */
#define L2UC_SPA_W		2
#define L2UC_AGE_LSP		67	/* static forces this non-zero */
#define L2UC_AGE_W		3
#define L2UC_ARPUSED_LSP	73
#define L2UC_ARPUSED_W		1
#define L2UC_VALID_LSP		77
#define L2UC_VALID_W		1

/* L2 insert-status DECODE bits (the engine's reply word).  They moved with
 * l34_l2uc_sts_index(); the register offset and the BUSY bit stay in
 * rtl9602c_l34.h with the poll that owns them.  Spelled without BIT() --
 * this header includes only <linux/types.h>. */
#define L2_STS_ADDR_MASK	0x3ff		/* [9:0] hardware-assigned entry address */
#define L2_STS_CAM		(1u << 10)	/* hash(0)/CAM(1): index = (cam << 10) | addr */
#define L2_STS_HIT		(1u << 12)

void l34_mac48_set(u32 *w, unsigned int lsp, const u8 *mac);
void l34_naptr_encode(u32 *w, u32 int_ip, u16 int_port, u8 extip_idx,
		      u16 ext_port, bool is_tcp);
void l34_napt_encode(u32 *w, u16 naptr_idx);
void l34_netif_encode(u32 *w, const u8 *mac, u32 ip, u16 vlan);
void l34_rt_wan_encode(u32 *w, u8 nh_idx);
void l34_rt_lan_encode(u32 *w, u32 lan_net, u8 prefix, u8 netif_idx);
void l34_rt_wan_net_encode(u32 *w, u32 net, u8 prefix, u8 nh_idx);
void l34_rt_cpu_encode(u32 *w, u32 own_ip, u8 netif_idx);
void l34_nexthop_encode(u32 *w, u8 ifidx, unsigned int l2idx);
void l34_nexthop_pppoe_encode(u32 *w, u8 ifidx, unsigned int l2idx,
			      unsigned int pppidx, unsigned int keep);
void l34_pppoe_encode(u32 *w, u16 sid);
void l34_extip_encode(u32 *w, u32 wan_ip, u8 nhidx);
void l34_arp_encode(u32 *w, u32 dst_ip, unsigned int l2idx);
void l34_l2uc_encode(u32 *w, const u8 *mac, u8 port);
int l34_l2uc_sts_index(u32 sts);

/* blackhole-safety: an interface program is ALL-OR-NOTHING
 * dev/MEASURED-rtl9602c_l34_logic.h.md sec 3. */
#define L34_WORDS_MAX		4	/* NETIF, the widest entry programmed here */

struct l34_prog_step {
	u8	tbl;			/* table id: TRANSPORT, the shell supplies it */
	u16	idx;
	u8	words;
	u32	w[L34_WORDS_MAX];
};

/* The shell's indirect-table write, passed in because flowcore holds no MMIO. */
typedef int (*l34_tbl_wr_fn)(void *ctx, u8 tbl, u16 idx, const u32 *w,
			     unsigned int words);

int l34_prog_run(const struct l34_prog_step *s, unsigned int n,
		 l34_tbl_wr_fn wr, void *ctx);

/* The two questions a read-back asks of what actually landed in the silicon:
 * does this NETIF entry CLAIM routed traffic, and is there a CPU self-route
 * behind it?  A claiming NETIF whose self-route is absent black-holes
 * management, which is the fault above expressed as a predicate. */
bool l34_netif_claims(const u32 *netif);
bool l34_rt_is_cpu_self(const u32 *rt, u32 ip, u8 netif_idx);
bool l34_iface_blackholes(const u32 *netif, const u32 *cpu_rt, u32 own_ip,
			  u8 netif_idx);

/* hoisted from rtl9602c_eth.c (same shell TU) -- dev/MEASURED-rtl9602c_l34_logic.h.md sec 4. */
#define GMAC_PON_PORT		2	/* GMAC tx_portmask PON bit (stock = 1<<2 -> word3 0x02400000) */
#define TXD2_OMCI_CPUTAG	0x80000000u	/* opts2 bit31 cputag */
#define TXD2_OMCI_EFID		0x00080000u	/* opts2 bit19 efid (stock word2 = 0x80080000) */
#define TXD3_9602C_PMASK(p)	(((p) & 0x3Fu) << 23)	/* opts3[28:23] tx_portmask */
#define TXD3_9602C_DST_SID(s)	(((s) & 0x7Fu) << 16)	/* opts3[22:16] tx_dst_stream_id (the steering SID) */
#define TXD3_OMCI_9602C(sid)	(TXD3_9602C_PMASK(1u << GMAC_PON_PORT) | \
				 TXD3_9602C_DST_SID(sid))	/* = 0x08400000 for SID 64 */

void rtl9602c_wan_mac_add(u8 *out, const u8 *base, unsigned int add);
bool rtl9602c_rx_is_ds_omci(const struct luna_rx_layout *rxl,
			    bool trap_on, u32 opts2, u32 opts3,
			    const u8 *data, u32 len, unsigned int pon_port,
			    unsigned int omci_reason,
			    unsigned int cpu_prefix, u32 buf_size);
bool rtl9602c_rx_wan_demux(const struct luna_rx_layout *rxl, u32 opts3,
			   const u8 *dst, const u8 *wan_mac,
			   unsigned int pon_port);
bool rtl9602c_rx_frame_bad(u32 opts1, u32 err_mask, u32 len,
			   unsigned int cpu_prefix, u32 buf_size);
unsigned int rtl9602c_omci_hwring(unsigned int omci_tx_ring);
bool rtl9602c_omci_doorbell(unsigned int omci_tx_ring,
			    unsigned int doorbell_ovr,
			    unsigned int *hwring, u32 *mask);
/* rtl9602c_rxdesnum_pack / rtl9602c_rxcdo_pack: moved to luna_gmac_logic.h
 * (renamed luna_gmac_*) -- family GMAC facts, and this TU's config gate
 * excludes the second caller.  See the note in rtl9602c_l34_logic.c. */
u32 rtl9602c_omci_txd_word2(u32 ovr);
u32 rtl9602c_omci_txd_word3(u32 ovr, unsigned int sid);

#endif /* _RTL9602C_L34_LOGIC_H */
