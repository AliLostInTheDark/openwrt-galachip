// SPDX-License-Identifier: GPL-2.0-only
/* See cortina_ni_flowoffload_logic.h. Moved verbatim; no line was rewritten.
 */
#include <linux/types.h>
#include <linux/bitrev.h>
#include <linux/crc32.h>
#include <linux/errno.h>	/* the way-pick's -EEXIST / -ENOSPC verdicts */
#include <linux/in.h>		/* IPPROTO_TCP */
#include <linux/kernel.h>
#include <linux/string.h>

#include "cortina_ni_flowoffload_logic.h"

/* Steps 3-4 of the HW recipe (host-fuzz reference for the SW CRC path; the
 * runtime hash uses the SWO engine - see cn_l3e_key_hash). */
void cn_l3e_bitrev_key(u32 *w, int n_words)
{
	int i;
	u32 t;

	for (i = 0; i < n_words / 2; i++) {
		t = w[i];
		w[i] = bitrev32(w[n_words - 1 - i]);
		w[n_words - 1 - i] = bitrev32(t);
	}
	if (n_words & 1)
		w[i] = bitrev32(w[i]);
}

u32 cn_l3e_crc32(const u8 *p, size_t len)
{
	/* reflected CRC-32 (Ethernet poly), seed ~0, reflected output, no
	 * final xor - the engine's convention */
	return bitrev32(crc32_le(~0u, p, len));
}

u16 cn_l3e_crc16(const u8 *p, size_t len)
{
	/* reflected CRC-16/CCITT (poly 0x8408), seed 0xffff, reflected
	 * output, no final xor */
	u16 crc = 0xffff;
	int i;

	while (len--) {
		crc ^= *p++;
		for (i = 0; i < 8; i++)
			crc = (crc >> 1) ^ ((crc & 1) ? 0x8408 : 0);
	}
	return bitrev8(crc & 0xff) << 8 | bitrev8(crc >> 8);
}

/* ★ HW hash recipe - FULLY RECOVERED 2026-07-18 (tier-2 ...
 * dev/MEASURED-cortina_ni_flowoffload_logic.c.md sec 1. */
u32 cn_l3e_proc_parse_ip(const char *s)
{
	u8 b[4];
	unsigned int v;

	if (sscanf(s, "%hhu.%hhu.%hhu.%hhu", &b[0], &b[1], &b[2], &b[3]) == 4)
		return ((u32)b[0] << 24) | ((u32)b[1] << 16) |
		       ((u32)b[2] << 8) | b[3];
	if (kstrtouint(s, 0, &v) == 0)
		return v;
	return 0;
}

/* set `width` bits at LSB-first bit offset `off` in a little-endian buffer */
void cn_l3e_hdri_set(u8 *h, unsigned int off, unsigned int width, u64 val)
{
	unsigned int i;

	for (i = 0; i < width; i++, off++)
		if ((val >> i) & 1)
			h[off >> 3] |= 1u << (off & 7);
		/* buffer is pre-zeroed, so only 1-bits need writing */
}

/* ★ SW-tuple -> HW HDR_I packing (divergence-A fix, ...
 * dev/MEASURED-cortina_ni_flowoffload_logic.c.md sec 2. */
void cn_l3e_build_hdri(const struct cn_l3e_key *k, int profile,
		       u32 words[CN_L3E_HDRI_WORDS])
{
	u8 h[CN_L3E_HDRI_BYTES] = { 0 };

	/* L4 */
	cn_l3e_hdri_set(h, CN_HDRI_L4_DP, 16, k->l4_dport);
	cn_l3e_hdri_set(h, CN_HDRI_L4_SP, 16, k->l4_sport);
	/* IP DA/SA - 4x32b each, LSW first (IPv4 = word 0 only) */
	cn_l3e_hdri_set(h, CN_HDRI_IP_DA0 + 0,  32, k->ip_da_0);
	cn_l3e_hdri_set(h, CN_HDRI_IP_DA0 + 32, 32, k->ip_da_1);
	cn_l3e_hdri_set(h, CN_HDRI_IP_DA0 + 64, 32, k->ip_da_2);
	cn_l3e_hdri_set(h, CN_HDRI_IP_DA0 + 96, 32, k->ip_da_3);
	cn_l3e_hdri_set(h, CN_HDRI_IP_SA0 + 0,  32, k->ip_sa_0);
	cn_l3e_hdri_set(h, CN_HDRI_IP_SA0 + 32, 32, k->ip_sa_1);
	cn_l3e_hdri_set(h, CN_HDRI_IP_SA0 + 64, 32, k->ip_sa_2);
	cn_l3e_hdri_set(h, CN_HDRI_IP_SA0 + 96, 32, k->ip_sa_3);
	/* IP proto / L4 type / validity */
	cn_l3e_hdri_set(h, CN_HDRI_IP_L4_TYPE, 3, k->ip_l4_type);
	cn_l3e_hdri_set(h, CN_HDRI_IP_PROTO,   8, k->ip_protocol);
	cn_l3e_hdri_set(h, CN_HDRI_IP_VER,     1, k->ip_ver);
	cn_l3e_hdri_set(h, CN_HDRI_IP_VLD,     1, k->ip_vld);
	/* profile id stamp -> HDR_I t2_ctrl (mirrors stock; masked under mask 0) */
	cn_l3e_hdri_set(h, CN_HDRI_T2_CTRL,    4, profile & 0xf);

	memcpy(words, h, CN_L3E_HDRI_BYTES);
}

/* cn_fib_field() - read one field out of a raw 32-byte FIB ...
 * dev/MEASURED-cortina_ni_flowoffload_logic.c.md sec 3. */
u64 cn_fib_field(const void *fib, u32 bit, u32 width)
{
	const u8 *b = fib;
	u64 v = 0;
	u32 i;

	for (i = 0; i < width; i++)
		v |= (u64)((b[(bit + i) >> 3] >> ((bit + i) & 7)) & 1) << i;
	return v;
}

/* ★★ THE PPPoE-WAN LEG GATE - a PURE predicate (functional ...
 * dev/MEASURED-cortina_ni_flowoffload_logic.c.md sec 4. */
enum cn_pppoe_leg_verdict cn_pppoe_leg_check(bool pppoe_mode, bool ds_leg,
					     u16 rule_sid, u16 armed_sid)
{
	if (ds_leg && rule_sid)
		return CN_PPPOE_LEG_UNEXPECTED_PUSH;
	if (!rule_sid && !armed_sid)
		return CN_PPPOE_LEG_OK;		/* IPoE WAN - nothing to decide */
	if (!pppoe_mode)
		return CN_PPPOE_LEG_MODE_OFF;
	if (!ds_leg && !rule_sid)
		return CN_PPPOE_LEG_NO_PUSH;
	return CN_PPPOE_LEG_OK;
}

/* ★★ THE DISARM HALF OF GAP-3 - a PURE predicate (functional ...
 * dev/MEASURED-cortina_ni_flowoffload_logic.c.md sec 5. */
bool cn_pppoe_shadow_stale(bool ds_leg, bool egress_is_lan,
			   u16 rule_sid, u16 armed_sid)
{
	return !ds_leg && !egress_is_lan && armed_sid && !rule_sid;
}

/* PURE predicate (functional core - no MMIO, no state, no ...
 * dev/MEASURED-cortina_ni_flowoffload_logic.c.md sec 6. */
u32 cn_pppoe_punt_classify(const u8 *f, unsigned int len, u16 exp_sid,
			   struct cn_pppoe_punt_info *pi)
{
	unsigned int hdr = 14, off;
	const u8 *pppoe, *ip, *tcp;
	u32 v = CN_PPPOE_PUNT_SESSION;

	memset(pi, 0, sizeof(*pi));
	/* Ethernet, optionally one VLAN tag, then the session ethertype. */
	if (len < hdr + 8)
		return 0;
	if (f[12] == 0x81 && f[13] == 0x00) {
		hdr += 4;
		if (len < hdr + 8)
			return 0;
	}
	if (f[hdr - 2] != 0x88 || f[hdr - 1] != 0x64)
		return 0;

	pppoe = f + hdr;
	pi->sid = ((u16)pppoe[2] << 8) | pppoe[3];
	pi->pppoe_len = ((u16)pppoe[4] << 8) | pppoe[5];
	pi->ppp_proto = ((u16)pppoe[6] << 8) | pppoe[7];

	if (exp_sid && pi->sid != exp_sid)
		v |= CN_PPPOE_PUNT_SID_BAD;

	/* 0x0021 = PPP-IPv4 (data).  Everything else is control (LCP, IPCP,
	 * PAP/CHAP, IPv6CP): counted, not judged - those frames carry no inner
	 * IPv4 header for the length identity below. */
	if (pi->ppp_proto != 0x0021)
		return v | CN_PPPOE_PUNT_CTRL;
	v |= CN_PPPOE_PUNT_DATA;	/* the frame the identities below judge */

	ip = pppoe + 8;
	off = hdr + 8;
	if (len < off + 20)
		return v | CN_PPPOE_PUNT_SHORT;
	pi->ip_ver = ip[0] >> 4;
	pi->ihl = (ip[0] & 0xf) * 4;
	pi->ip_len = ((u16)ip[2] << 8) | ip[3];

	/* ★ THE identity a correctly-encapsulated session frame must satisfy:
	 * the PPPoE length field covers the 2-byte PPP protocol plus the whole
	 * inner IP datagram.  An 8-byte shift breaks it. */
	if (pi->ip_ver != 4 || pi->ihl < 20 ||
	    pi->pppoe_len != (u16)(pi->ip_len + 2)) {
		v |= CN_PPPOE_PUNT_LEN_BAD;
		/* ★ SHAPE the malformation instead of leaving it to be guessed
		 * dev/MEASURED-cortina_ni_flowoffload_logic.c.md sec 7. */
		if (len >= off + 8 && ip[0] == 0x11 && ip[1] == 0x00 &&
		    (((u16)ip[6] << 8) | ip[7]) == 0x0021)
			v |= CN_PPPOE_PUNT_DBLENC;
		if (len >= off + 8 + 20 && (ip[8] >> 4) == 4) {
			u16 l2 = ((u16)ip[10] << 8) | ip[11];

			if (pi->pppoe_len == (u16)(l2 + 2) ||
			    pi->pppoe_len == (u16)(l2 + 10))
				v |= CN_PPPOE_PUNT_SHIFT8;
		}
		return v;
	}

	/* An 8-byte shift ALSO lands garbage in the TCP data-offset nibble, so
	 * check that independently - two witnesses for one malformation. */
	if (ip[9] == IPPROTO_TCP) {
		off += pi->ihl;
		if (len < off + 20)
			return v | CN_PPPOE_PUNT_SHORT;
		tcp = ip + pi->ihl;
		pi->tcp_doff = (tcp[12] >> 4) * 4;
		pi->tcp_flags = tcp[13];
		if (pi->tcp_doff < 20 ||
		    (unsigned int)pi->ihl + pi->tcp_doff > pi->ip_len)
			return v | CN_PPPOE_PUNT_TCP_BAD;
	}
	return v;
}

/* One MSB-first CRC LFSR step, normal form: (d << 1) ^ (msb ? ...
 * dev/MEASURED-cortina_ni_flowoffload_logic.c.md sec 8. */
u32 cn_l3e_poly32_step(u32 d)
{
	return (d << 1) ^ ((d & BIT(31)) ? CN_L3E_SWO_POLY32 : 0);
}

u16 cn_l3e_poly16_step(u16 d)
{
	return ((d << 1) ^ ((d & BIT(15)) ? CN_L3E_SWO_POLY16 : 0)) & 0xffff;
}

/* The 4-slot TPID table is two 32-bit words, each holding two ...
 * dev/MEASURED-cortina_ni_flowoffload_logic.c.md sec 9. */
u16 cn_tpid_slot_at(const u32 w[2], unsigned int i)
{
	struct pi_packed_slot s = pi_packed_locate(0, i & 1, 16);

	return (u16)pi_packed_extract(w[i >> 1], &s);
}

int cn_tpid_find(const u32 w[2], u16 tpid)
{
	unsigned int i;

	for (i = 0; i < 4; i++)
		if (cn_tpid_slot_at(w, i) == tpid)
			return (int)i;
	return -1;
}

/* ★ The under-encapsulation tail of the WAN-VLAN admission ...
 * dev/MEASURED-cortina_ni_flowoffload_logic.c.md sec 10. */
enum cn_wan_vlan_verdict cn_wan_vlan_walk_verdict(u16 want_vid, bool walk_ok,
						  int walk_vid,
						  bool tpid_8021q, int sid,
						  bool ac_mac_vld)
{
	if (!walk_ok || walk_vid != (int)want_vid)
		return CN_WAN_VLAN_WALK_MISMATCH;
	if (!tpid_8021q)
		return CN_WAN_VLAN_BAD_TPID;
	if (sid < 0 || sid > 0xffff)
		return CN_WAN_VLAN_NO_SID;
	if (!ac_mac_vld)
		return CN_WAN_VLAN_NO_MAC;
	return CN_WAN_VLAN_OK_PPPOE;
}

int cn_wan_tag_resolve(bool ds_leg, u16 dev_vid, u16 rule_vid, bool rule_pop,
		       u16 *vid)
{
	*vid = dev_vid;
	if (!ds_leg && rule_vid && dev_vid && rule_vid != dev_vid)
		return -EINVAL;
	if (dev_vid)
		return CN_WAN_TAG_DEV;
	if (ds_leg ? rule_pop : rule_vid != 0) {
		*vid = ds_leg ? 0 : rule_vid;
		return CN_WAN_TAG_RULE;
	}
	return CN_WAN_TAG_NONE;
}

/* ===== round 3 (2026-09-02): the packed-slot idioms spelled ...
 * dev/MEASURED-cortina_ni_flowoffload_logic.c.md sec 11. */
u32 cn_tpid_slot_store(u32 word, unsigned int i, u16 tpid)
{
	struct pi_packed_slot s = pi_packed_locate(0, i & 1, 16);

	return pi_packed_insert(word, &s, tpid);
}

/* See the header: reg = the word OFFSET within the 2-word age row (0 or 4),
 * NOT an address -- the two data words sit at DESCENDING register addresses,
 * so the shell maps the offset to its register names. */
struct pi_packed_slot cn_age2_slot(u32 idx)
{
	return pi_packed_locate(0, idx & (CN_L3E_AGE_SLOTS - 1), 2);
}

u32 cn_age2_sweep_word(u32 w, u16 *rearmed)
{
	u32 n = 0;
	u16 t = 0;
	int i;

	for (i = 0; i < 16; i++) {
		u32 age = (w >> (i * 2)) & 3;

		/* traffic = re-armed above IDLE (a HW hit set it to START(2));
		 * clear such a slot back to IDLE(1) so the next sweep detects a
		 * fresh re-arm.  Leave STATIC(3) and FREE(0) untouched. */
		if (age > CN_L3E_AGE_IDLE && age != CN_L3E_AGE_STATIC) {
			t |= BIT(i);
			age = CN_L3E_AGE_IDLE;
		}
		n |= age << (i * 2);
	}
	*rearmed = t;
	return n;
}

/* See the header for the verdicts.  The scans are kept EXACTLY as the shell
 * spelled them: the dup scan covers way 0 even in bucket 0 (an entry can never
 * BE there, so it can never match), while the free scan starts at way 1 there
 * (the entry-0 guard). */
int cn_hs_way_pick(const u32 *crc32_tbl, u32 crc16, u32 crc32, u32 *idx_out)
{
	u32 base = crc16 & ~(u32)(CN_L3E_HASH_WAYS - 1);
	int way;

	for (way = 0; way < CN_L3E_HASH_WAYS; way++)
		if (crc32_tbl[base + way] == crc32) {
			*idx_out = base + way;
			return -EEXIST;
		}
	way = (base == 0) ? 1 : 0;
	for (; way < CN_L3E_HASH_WAYS; way++)
		if (!crc32_tbl[base + way])
			break;
	if (way == CN_L3E_HASH_WAYS) {
		*idx_out = base;
		return -ENOSPC;
	}
	*idx_out = base + way;
	return 0;
}
