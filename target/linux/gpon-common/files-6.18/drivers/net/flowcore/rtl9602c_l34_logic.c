// SPDX-License-Identifier: GPL-2.0-only
/* See rtl9602c_l34_logic.h. Moved verbatim; no line was rewritten.
 */
#include <linux/types.h>
#include <linux/minmax.h>

/* The RX descriptor layout is stated ONCE, in luna_gmac_logic.h. This file
 * decodes the same words, so it asks for that declaration rather than
 * re-spelling the shifts -- which is what it used to do. */
#include "luna_gmac_logic.h"
#include "rtl9602c_l34_logic.h"

void l34_field_set(u32 *w, unsigned int lsp, unsigned int width, u32 val)
{
	unsigned int word = lsp / 32, bit = lsp % 32, take;

	val &= (width >= 32) ? ~0u : ((1u << width) - 1);
	while (width) {
		take = min(width, 32 - bit);
		w[word] &= ~((((take >= 32) ? ~0u : ((1u << take) - 1))) << bit);
		w[word] |= (val & ((take >= 32) ? ~0u : ((1u << take) - 1))) << bit;
		/* ⚠ `val >>= take` was UNDEFINED on the 32-bit-wide fields ...
		 * dev/MEASURED-rtl9602c_l34_logic.c.md sec 1. */
		val = (take >= 32) ? 0 : (val >> take);
		width -= take;
		word++;
		bit = 0;
	}
}

/* NAPT bucket hashes (clean-room: the arithmetic is ...
 * dev/MEASURED-rtl9602c_l34_logic.c.md sec 2. */
u16 l34_hash_out(bool is_tcp, u32 sip, u16 sport, u32 dip, u16 dport)
{
	/* low 16 bits of both addresses + both ports, summed then folded 18->10 */
	u32 lo   = (sip & 0xffff) + (dip & 0xffff) + sport + dport;
	u32 fold = (lo & 0x3ff) + ((lo >> 10) & 0xff);

	/* the address upper halves and the TCP/UDP selector, mixed in by XOR */
	u32 mix  = ((sip >> 16) & 0x3ff) ^ ((dip >> 16) & 0x3ff);

	mix ^= ((sip >> 26) & 0x3f) + ((u32)(is_tcp & 1) << 9);
	mix ^= ((dip >> 26) & 0x3f) << 4;

	return (fold ^ mix) & 0x3ff;
}

/* Inbound NAPTR bucket: keyed only on the post-NAT (WAN-side) dest IP + port. */
u16 l34_hash_in(bool is_tcp, u32 dip, u16 dport)
{
	u32 lo   = dport + (dip & 0xffff);
	u32 fold = (lo & 0x3ff) + ((lo >> 10) & 0x7f);
	u32 mix  = ((dip >> 16) & 0x3ff)
		 ^ (((dip >> 26) & 0x3f) + ((u32)(is_tcp & 1) << 9));

	return (fold ^ mix) & 0x3ff;
}

/* Inverse of l34_field_set: extract a (possibly word-straddling) field.
 * Moved verbatim from rtl9602c_l34.c (2026-09-02) so the set/get pair shares
 * one home -- the SID2QID lesson: a wrong get mirroring a wrong set is
 * invisible when the two live apart. */
u32 l34_field_get(const u32 *w, unsigned int lsp, unsigned int width)
{
	unsigned int word = lsp / 32, bit = lsp % 32, take, got = 0;
	u32 val = 0;

	while (width) {
		take = min(width, 32 - bit);
		val |= ((w[word] >> bit) & ((take >= 32) ? ~0u : ((1u << take) - 1)))
		       << got;
		got += take;
		width -= take;
		word++;
		bit = 0;
	}
	return val;
}

/* ===== entry encoders, SPLIT from rtl9602c_l34.c ...
 * dev/MEASURED-rtl9602c_l34_logic.c.md sec 3. */
void l34_mac48_set(u32 *w, unsigned int lsp, const u8 *mac)
{
	l34_field_set(w, lsp, 32,
		      ((u32)mac[2] << 24) | ((u32)mac[3] << 16) |
		      ((u32)mac[4] << 8)  |  (u32)mac[5]);
	l34_field_set(w, lsp + 32, 16,
		      ((u32)mac[0] << 8) | (u32)mac[1]);
}

/* NAPTR_IN rewrite entry: internal host addr/port + post-NAT port, WAN side
 * via the EXTIP slot; written full-cone (remHash unused) so the return path
 * matches on the WAN addr/port alone. */
void l34_naptr_encode(u32 *w, u32 int_ip, u16 int_port, u8 extip_idx,
		      u16 ext_port, bool is_tcp)
{
	l34_field_set(w, L34_NAPTR_INTIP_LSP,    L34_NAPTR_INTIP_W,    int_ip);
	l34_field_set(w, L34_NAPTR_INTPORT_LSP,  L34_NAPTR_INTPORT_W,  int_port);
	l34_field_set(w, L34_NAPTR_EXTIPIDX_LSP, L34_NAPTR_EXTIPIDX_W, extip_idx);
	l34_field_set(w, L34_NAPTR_EXTPORT_LSP,  L34_NAPTR_EXTPORT_W,  ext_port);
	l34_field_set(w, L34_NAPTR_TCP_LSP,      L34_NAPTR_TCP_W,      is_tcp);
	l34_field_set(w, L34_NAPTR_VALID_LSP,    L34_NAPTR_VALID_W,    L34_NAPTR_TYPE_FULLCONE);
}

/* NAPT_OUT hash slot: the entry index IS the outbound hash; the word only
 * points at the rewrite entry. */
void l34_napt_encode(u32 *w, u16 naptr_idx)
{
	l34_field_set(w, L34_NAPT_HASHIN_IDX_LSP, L34_NAPT_HASHIN_IDX_W, naptr_idx);
	l34_field_set(w, L34_NAPT_VALID_LSP,      L34_NAPT_VALID_W,      1);
}

/* NETIF entry: egress source MAC + VLAN/MTU/IP, routing enabled, classified
 * into the L34 NAT domain.  Was spelled VERBATIM twice (wan_setup and
 * lan_setup, 11 identical l34_field_set lines differing only in arguments). */
void l34_netif_encode(u32 *w, const u8 *mac, u32 ip, u16 vlan)
{
	l34_field_set(w, L34_NETIF_VALID_LSP,   L34_NETIF_VALID_W,   1);
	l34_field_set(w, L34_NETIF_VLANID_LSP,  L34_NETIF_VLANID_W,  vlan);
	l34_mac48_set(w, L34_NETIF_GMAC_LSP, mac);
	l34_field_set(w, L34_NETIF_MACMASK_LSP, L34_NETIF_MACMASK_W, L34_NETIF_DEF_MACMASK);
	l34_field_set(w, L34_NETIF_ENRTR_LSP,   L34_NETIF_ENRTR_W,   1);
	l34_field_set(w, L34_NETIF_MTU_LSP,     L34_NETIF_MTU_W,     L34_NETIF_DEF_MTU);
	l34_field_set(w, L34_NETIF_L34_LSP,     L34_NETIF_L34_W,     1);
	l34_field_set(w, L34_NETIF_IP_LSP,      L34_NETIF_IP_W,      ip);
}

/* Lower-half WAN route via NEXTHOP; ARP would resolve the destination, not the gateway. */
void l34_rt_wan_encode(u32 *w, u8 nh_idx)
{
	l34_field_set(w, L34_RT_PROCESS_LSP,   L34_RT_PROCESS_W,   L34_RT_PROCESS_NH);
	l34_field_set(w, L34_RT_NHIDX_LSP,     L34_RT_NHIDX_W,     nh_idx);
	l34_field_set(w, L34_RT_RT2WANINF_LSP, L34_RT_RT2WANINF_W, 1);
	l34_field_set(w, L34_RT_VALID_LSP,     L34_RT_VALID_W,     1);
}

/* LAN subnet route (process=ARP, internal=1).  ⚠ mask = prefix - 1: the
 * 5-bit MASK field is a prefix CODE (0 => /1 anchor, 31 => /32) -- the
 * off-by-one fact a host test pins here. */
void l34_rt_lan_encode(u32 *w, u32 lan_net, u8 prefix, u8 netif_idx)
{
	l34_field_set(w, L34_RT_IP_LSP,      L34_RT_IP_W,      lan_net);
	l34_field_set(w, L34_RT_MASK_LSP,    L34_RT_MASK_W,    prefix - 1);
	l34_field_set(w, L34_RT_PROCESS_LSP, L34_RT_PROCESS_W, L34_RT_PROCESS_ARP);
	l34_field_set(w, L34_RT_INT_LSP,     L34_RT_INT_W,     1);	/* LAN */
	l34_field_set(w, L34_RT_DENTIF_LSP,  L34_RT_DENTIF_W,  netif_idx);
	l34_field_set(w, L34_RT_VALID_LSP,   L34_RT_VALID_W,   1);
}

/* WAN prefix via NEXTHOP; the caller uses two /1 entries for full IPv4 coverage. */
void l34_rt_wan_net_encode(u32 *w, u32 net, u8 prefix, u8 nh_idx)
{
	l34_field_set(w, L34_RT_IP_LSP,        L34_RT_IP_W,        net);
	l34_field_set(w, L34_RT_MASK_LSP,      L34_RT_MASK_W,      prefix - 1);
	l34_field_set(w, L34_RT_PROCESS_LSP,   L34_RT_PROCESS_W,   L34_RT_PROCESS_NH);
	l34_field_set(w, L34_RT_INT_LSP,       L34_RT_INT_W,       0);	/* WAN */
	l34_field_set(w, L34_RT_NHIDX_LSP,     L34_RT_NHIDX_W,     nh_idx);
	l34_field_set(w, L34_RT_RT2WANINF_LSP, L34_RT_RT2WANINF_W, 1);
	l34_field_set(w, L34_RT_VALID_LSP,     L34_RT_VALID_W,     1);
}

/* The /32 CPU self-route (mask code 31): traffic addressed to ...
 * dev/MEASURED-rtl9602c_l34_logic.c.md sec 4. */
void l34_rt_cpu_encode(u32 *w, u32 own_ip, u8 netif_idx)
{
	l34_field_set(w, L34_RT_IP_LSP,      L34_RT_IP_W,      own_ip);
	l34_field_set(w, L34_RT_MASK_LSP,    L34_RT_MASK_W,    31);	/* /32 */
	l34_field_set(w, L34_RT_PROCESS_LSP, L34_RT_PROCESS_W, L34_RT_PROCESS_CPU);
	l34_field_set(w, L34_RT_INT_LSP,     L34_RT_INT_W,     1);
	l34_field_set(w, L34_RT_DENTIF_LSP,  L34_RT_DENTIF_W,  netif_idx);
	l34_field_set(w, L34_RT_VALID_LSP,   L34_RT_VALID_W,   1);
}

/* Run one ordered interface program, revoking the CLAIM if a ...
 * dev/MEASURED-rtl9602c_l34_logic.c.md sec 5. */
int l34_prog_run(const struct l34_prog_step *s, unsigned int n,
		 l34_tbl_wr_fn wr, void *ctx)
{
	u32 revoke[L34_WORDS_MAX] = { 0 };
	unsigned int i;
	int ret;

	for (i = 0; i < n; i++) {
		ret = wr(ctx, s[i].tbl, s[i].idx, s[i].w, s[i].words);
		if (!ret)
			continue;
		if (i)
			wr(ctx, s[0].tbl, s[0].idx, revoke, s[0].words);
		return ret;
	}
	return 0;
}

/* A NETIF entry claims routed traffic once it is VALID with routing enabled
 * and the interface classified into the L34 domain -- all three, because any
 * one of them alone leaves the entry inert. */
bool l34_netif_claims(const u32 *netif)
{
	return l34_field_get(netif, L34_NETIF_VALID_LSP, L34_NETIF_VALID_W) &&
	       l34_field_get(netif, L34_NETIF_ENRTR_LSP, L34_NETIF_ENRTR_W) &&
	       l34_field_get(netif, L34_NETIF_L34_LSP, L34_NETIF_L34_W);
}

/* The CPU self-route: valid, /32 (mask code 31), PROCESS=CPU, on this netif,
 * for exactly this address. */
bool l34_rt_is_cpu_self(const u32 *rt, u32 ip, u8 netif_idx)
{
	return l34_field_get(rt, L34_RT_VALID_LSP, L34_RT_VALID_W) &&
	       l34_field_get(rt, L34_RT_MASK_LSP, L34_RT_MASK_W) == 31 &&
	       l34_field_get(rt, L34_RT_PROCESS_LSP, L34_RT_PROCESS_W) ==
		       L34_RT_PROCESS_CPU &&
	       l34_field_get(rt, L34_RT_DENTIF_LSP, L34_RT_DENTIF_W) ==
		       netif_idx &&
	       l34_field_get(rt, L34_RT_IP_LSP, L34_RT_IP_W) == ip;
}

/* The fault as a predicate: the engine claims traffic for this interface and
 * nothing terminates the interface's OWN address locally. */
bool l34_iface_blackholes(const u32 *netif, const u32 *cpu_rt, u32 own_ip,
			  u8 netif_idx)
{
	return l34_netif_claims(netif) &&
	       !l34_rt_is_cpu_self(cpu_rt, own_ip, netif_idx);
}

/* Ethernet next-hop via NETIF[ifidx], dst MAC = L2[l2idx]. */
void l34_nexthop_encode(u32 *w, u8 ifidx, unsigned int l2idx)
{
	l34_field_set(w, L34_NH_TYPE_LSP,  L34_NH_TYPE_W,  0);	/* ETHER */
	l34_field_set(w, L34_NH_IFIDX_LSP, L34_NH_IFIDX_W, ifidx);
	l34_field_set(w, L34_NH_NHIDX_LSP, L34_NH_NHIDX_W, l2idx);
}

/* The same next hop, leaving through a PPPoE session.  It REUSES the ethernet
 * encoder rather than repeating its three fields: the L2 destination and the
 * egress interface are identical -- what a PPPoE egress adds is the type, the
 * PPPOE table index and the keep mode.
 *
 * ⚠ THE SESSION ID IS NOT HERE. It lives in the PPPOE table entry this index
 * points at, so installing a flow without ALSO writing that entry would give
 * the engine a next hop that encapsulates with whatever the slot last held --
 * which reads healthy in every counter and is wrong on the wire.
 */
void l34_nexthop_pppoe_encode(u32 *w, u8 ifidx, unsigned int l2idx,
			      unsigned int pppidx, unsigned int keep)
{
	l34_nexthop_encode(w, ifidx, l2idx);
	l34_field_set(w, L34_NH_TYPE_LSP,    L34_NH_TYPE_W,    L34_NH_TYPE_PPPOE);
	l34_field_set(w, L34_NH_PPPIDX_LSP,  L34_NH_PPPIDX_W,  pppidx);
	l34_field_set(w, L34_NH_KEEPPPP_LSP, L34_NH_KEEPPPP_W, keep);
}

/* PPPOE table entry: the negotiated session id, host order. */
void l34_pppoe_encode(u32 *w, u16 sid)
{
	l34_field_set(w, L34_PPPOE_SESID_LSP, L34_PPPOE_SESID_W, sid);
}

/* EXTIP slot: the WAN source IP a NAPT rewrite applies, via NEXTHOP[nhidx]. */
void l34_extip_encode(u32 *w, u32 wan_ip, u8 nhidx)
{
	l34_field_set(w, L34_EXTIP_EXTIP_LSP, L34_EXTIP_EXTIP_W, wan_ip);
	l34_field_set(w, L34_EXTIP_VALID_LSP, L34_EXTIP_VALID_W, 1);
	l34_field_set(w, L34_EXTIP_TYPE_LSP,  L34_EXTIP_TYPE_W,  0);	/* NAPT */
	l34_field_set(w, L34_EXTIP_NHIDX_LSP, L34_EXTIP_NHIDX_W, nhidx);
}

/* ARP CAM entry: one DESTINATION address -> the L2 entry holding its MAC.  The
 * die searches this table by the packet's destination, so the gateway is one
 * destination among many and not what the key means. */
void l34_arp_encode(u32 *w, u32 dst_ip, unsigned int l2idx)
{
	l34_field_set(w, L34_ARP_IP_LSP,    L34_ARP_IP_W,    dst_ip);
	l34_field_set(w, L34_ARP_VALID_LSP, L34_ARP_VALID_W, 1);
	l34_field_set(w, L34_ARP_NHIDX_LSP, L34_ARP_NHIDX_W, l2idx);
}

/* The 3-word static L2 unicast (FDB) key: MAC + static/spa/age/arpused/valid.
 * The Cortina family's cortina_ni_l2fe_fdb_key is the same job for its
 * silicon -- the pair now sits in the same tier. */
void l34_l2uc_encode(u32 *w, const u8 *mac, u8 port)
{
	/* 48-bit MAC: octet[0] sits at the field's most-significant byte. */
	l34_mac48_set(w, L2UC_MAC_LSP, mac);
	l34_field_set(w, L2UC_STATIC_LSP,  L2UC_STATIC_W,  1);
	l34_field_set(w, L2UC_SPA_LSP,     L2UC_SPA_W,     port);
	l34_field_set(w, L2UC_AGE_LSP,     L2UC_AGE_W,     1);
	l34_field_set(w, L2UC_ARPUSED_LSP, L2UC_ARPUSED_W, 1);
	l34_field_set(w, L2UC_VALID_LSP,   L2UC_VALID_W,   1);
}

/* Decode the L2 insert engine's reply: the hardware-assigned entry index
 * ((cam << 10) | addr), or -1 when the engine reports no hit (table full).
 * Declared deviation: the errno (-ENOSPC) stays shell-side -- this file
 * includes no errno.h. */
int l34_l2uc_sts_index(u32 sts)
{
	if (!(sts & L2_STS_HIT))
		return -1;
	return ((sts & L2_STS_CAM) ? (1 << 10) : 0) | (sts & L2_STS_ADDR_MASK);
}

/* ===== hoisted from rtl9602c_eth.c (same shell TU) ...
 * dev/MEASURED-rtl9602c_l34_logic.c.md sec 6. */
void rtl9602c_wan_mac_add(u8 *out, const u8 *base, unsigned int add)
{
	int i;

	for (i = 0; i < 6; i++)		/* was ether_addr_copy(out, base) */
		out[i] = base[i];
	for (i = 6 - 1; i >= 0 && add; i--) {
		unsigned int s = out[i] + (add & 0xff);

		out[i] = s & 0xff;
		add = (add >> 8) + (s >> 8);
	}
}

/* DS-OMCI classifier + length guard, SPLIT from ...
 * dev/MEASURED-rtl9602c_l34_logic.c.md sec 7. */
bool rtl9602c_rx_is_ds_omci(const struct luna_rx_layout *rxl,
			    bool trap_on, u32 opts2, u32 opts3,
			    const u8 *data, u32 len, unsigned int pon_port,
			    unsigned int omci_reason,
			    unsigned int cpu_prefix, u32 buf_size)
{
	if (!trap_on || len < cpu_prefix + 8 || len > buf_size)
		return false;
	if (luna_gmac_rx_reason(rxl, opts2, opts3) == omci_reason &&
	    luna_gmac_rx_src_port(rxl, opts3) == pon_port)
		return true;
	return (data[cpu_prefix + 3] == 0x0a || data[cpu_prefix + 3] == 0x0b) &&
	       !(data[cpu_prefix + 2] & 0x80);
}

/* WAN demux verdict, SPLIT from rtl9602c_eth_rx(): does this ...
 * dev/MEASURED-rtl9602c_l34_logic.c.md sec 8. */
bool rtl9602c_rx_wan_demux(const struct luna_rx_layout *rxl, u32 opts3,
			   const u8 *dst, const u8 *wan_mac,
			   unsigned int pon_port)
{
	unsigned int sp = luna_gmac_rx_src_port(rxl, opts3);
	bool wan_drain = (opts3 >> 20) == 0x23e;
	int i;

	if (sp == pon_port || wan_drain)
		return true;
	for (i = 0; i < 6; i++)		/* was ether_addr_equal(dst, wan_mac) */
		if (dst[i] != wan_mac[i])
			return false;
	return true;
}

/* The HW ring h behind the omci_tx_ring module param: instances >5 clamp to
 * ring 4.  Was spelled seven times across the shell (xmit, rekick, align,
 * hw_program, open x2, diag). */
unsigned int rtl9602c_omci_hwring(unsigned int omci_tx_ring)
{
	return omci_tx_ring > 5 ? 4 : omci_tx_ring;
}

/* Which doorbell kicks HW ring h, SPLIT from ... -- dev/MEASURED-rtl9602c_l34_logic.c.md sec 9. */
bool rtl9602c_omci_doorbell(unsigned int omci_tx_ring,
			    unsigned int doorbell_ovr,
			    unsigned int *hwring, u32 *mask)
{
	unsigned int h = rtl9602c_omci_hwring(omci_tx_ring);
	unsigned int dbit = (doorbell_ovr == 0xff)
		? (h & 0x1f)	/* R_IO_CMD bit number == HW ring (h<4) */
		: doorbell_ovr;

	*hwring = h;
	if (h == 4 && doorbell_ovr == 0xff) {
		*mask = 0x100;	/* R_IO_CMD1 TX_POLL5 (bit8 = 0x100) */
		return true;
	}
	*mask = 1u << dbit;
	return false;
}

/* rtl9602c_rxdesnum_pack / rtl9602c_rxcdo_pack MOVED to ...
 * dev/MEASURED-rtl9602c_l34_logic.c.md sec 10. */
/* Pattern 0: upstream unicast IPv4 on the routed WAN -> data GEM SID. */
void l34_cf_wan_encode(u32 rule[2], u32 mask[2], u32 action[3],
		       u8 netif, u8 sid)
{
	rule[0] = 0;
	rule[1] = (1u << 16) | ((u32)(netif & 15) << 12) | (1u << 10);
	mask[0] = 1u << 31;
	mask[1] = ((u32)(~netif & 15) << 12) | (1u << 8);
	action[0] = ((u32)(sid & 127) << 24) | (1u << 23);
	action[1] = 0;
	action[2] = 0;
}

u32 rtl9602c_omci_txd_word2(u32 ovr)
{
	return ovr ? ovr : (TXD2_OMCI_CPUTAG | TXD2_OMCI_EFID);
}

u32 rtl9602c_omci_txd_word3(u32 ovr, unsigned int sid)
{
	return ovr ? ovr : TXD3_OMCI_9602C(sid);
}

/* Bad-frame verdict, SPLIT from rtl9602c_eth_rx(): is a ...
 * dev/MEASURED-rtl9602c_l34_logic.c.md sec 11. */
bool rtl9602c_rx_frame_bad(u32 opts1, u32 err_mask, u32 len,
			   unsigned int cpu_prefix, u32 buf_size)
{
	return (opts1 & err_mask) ||
	       len < 60 + cpu_prefix ||	/* 60 = ETH_ZLEN, the min Ethernet frame */
	       len > buf_size;
}
