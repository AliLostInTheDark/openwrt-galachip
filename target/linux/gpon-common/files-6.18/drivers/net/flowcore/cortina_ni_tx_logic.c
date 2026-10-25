// SPDX-License-Identifier: GPL-2.0-only
/* See cortina_ni_tx_logic.h. Moved verbatim; no line was rewritten.
 */
#include <linux/types.h>

#include "cortina_ni_tx_logic.h"

/* explicit byte math: this driver must stay endianness-agnostic */
u64 ca_ni_mac_key(const u8 *mac)
{
	return ((u64)mac[0] << 40) | ((u64)mac[1] << 32) | ((u64)mac[2] << 24) |
	       ((u64)mac[3] << 16) | ((u64)mac[4] << 8) | mac[5];
}

/* TX round (2026-09-02). Chip facts below are the RTL9607F's, ...
 * dev/MEASURED-cortina_ni_tx_logic.c.md sec 1. */

unsigned int ca_ni_mac_bucket(const u8 *mac, unsigned int mask)
{
	return (mac[3] ^ mac[4] ^ mac[5]) & mask;
}

/* One lan_fdb bucket, published atomically:
 * {mac[47:0], port[50:48], valid[51]} (ex-cortina-ni-tx.c
 * CA_NI_LAN_FDB_{MAC,PORT,VALID}; regs never spelled these). */
#define CA_NI_LAN_FDB_MAC_MASK	0x0000ffffffffffffull	/* GENMASK_ULL(47, 0) */
#define CA_NI_LAN_FDB_PORT_SHF	48			/* GENMASK_ULL(50, 48) */
#define CA_NI_LAN_FDB_PORT_MASK	0x7u
#define CA_NI_LAN_FDB_VALID	(1ull << 51)		/* BIT_ULL(51) */

u64 ca_ni_lan_fdb_ent(u64 mac_key, u32 port)
{
	return (mac_key & CA_NI_LAN_FDB_MAC_MASK) |
	       ((u64)(port & CA_NI_LAN_FDB_PORT_MASK) << CA_NI_LAN_FDB_PORT_SHF) |
	       CA_NI_LAN_FDB_VALID;
}

int ca_ni_lan_fdb_ent_port(u64 ent, u64 mac_key)
{
	if (!(ent & CA_NI_LAN_FDB_VALID) ||
	    (ent & CA_NI_LAN_FDB_MAC_MASK) != mac_key)
		return -1;
	return (int)((ent >> CA_NI_LAN_FDB_PORT_SHF) & CA_NI_LAN_FDB_PORT_MASK);
}

/* The egress port set for one frame, as a port bitmap (never ...
 * dev/MEASURED-cortina_ni_tx_logic.c.md sec 2. */
void cortina_ni_lan_tx_pick(int force_ldpid, int mode, u32 link,
			    unsigned int fixed_port, bool da_mc,
			    u64 fdb_ent, u64 da_key,
			    struct ca_ni_lan_tx_pick *p)
{
	int port;

	p->hit = false;
	p->flood = false;
	p->force_oor = false;

	/* Diagnostic knob still wins, but RANGE-CHECKED: the port set ...
	 * dev/MEASURED-cortina_ni_tx_logic.c.md sec 3. */
	if (force_ldpid >= 0) {
		if (force_ldpid < CA_NI_TX_DEST_LDPID_COUNT) {
			p->ports = 1u << force_ldpid;
			return;
		}
		p->force_oor = true;
	}
	/* Fail-safe: fall back to the single-port behaviour whenever we have
	 * no link information at all (the 1 Hz poll has not run yet, or MDIO
	 * failed).  Never "no port" - that would silence the only IP
	 * management channel to the board. */
	if (mode == CA_NI_LAN_TX_FIXED || !link) {
		p->ports = 1u << fixed_port;
		return;
	}

	if (mode == CA_NI_LAN_TX_LEARN && !da_mc) {
		port = ca_ni_lan_fdb_ent_port(fdb_ent, da_key);
		if (port >= 0 && (link & (1u << port))) {
			p->hit = true;
			p->ports = 1u << port;
			return;
		}
	}
	p->flood = true;
	p->ports = link;
}

unsigned int cortina_ni_ring_free_desc(unsigned int wptr,
				       unsigned int finished,
				       unsigned int size)
{
	if (wptr >= finished)
		return size - wptr - 1 + finished;
	return finished - wptr - 1;
}

/* AAL_LPORT_CPU_0: token-identical to the still-live cortina-ni-regs.h
 * spelling (cortina-ni-flowoffload.c uses it too). */
#define CA_DMA_LSO_LSPID_CPU0		0x10

u32 cortina_ni_tx_vp_lspid(unsigned int vp)
{
	return (vp >= 1 && vp <= 11) ? CA_DMA_LSO_LSPID_CPU0 + vp
				     : CA_DMA_LSO_LSPID_CPU0;
}

/* The L2FE ARB ldpid->pdpid map semantics (vendor aal_port.c ...
 * dev/MEASURED-cortina_ni_tx_logic.c.md sec 4. */
#define CA_NI_PPORT_OAM			0x0c	/* AAL_PPORT_OAM */
#define CA_NI_PPORT_QM			0x08	/* AAL_PPORT_QM (US PON data path) */
#define CA_NI_PPORT_BLACKHOLE		0x0f	/* AAL_PPORT_BLACKHOLE (drop) */
#define CA_NI_ARB_LDPID_PON		0x07	/* PON NI port: no direct egress */
#define CA_NI_ARB_LDPID_9QUEUE_LO	0x08	/* AAL_LPORT_9QUEUE_NI0 */
#define CA_NI_ARB_LDPID_9QUEUE_HI	0x0f	/* AAL_LPORT_9QUEUE_NI7 (PON 7 + 8) */
#define CA_NI_ARB_LDPID_CPU_MQ_LO	0x20	/* AAL_LPORT_CPU_MQ_0 / LLID_GEM_INDEX_0 */
#define CA_NI_ARB_LDPID_CPU_MQ_HI	0x3f	/* vendor maps all of 0x20..0x3f -> QM */

u32 cortina_ni_arb_idx(u32 my_mac, u32 dbuf, u32 ldpid)
{
	return ((my_mac & 1u) << 7) | ((dbuf & 1u) << 6) | (ldpid & 0x3fu);
}

int cortina_ni_arb_pdpid(u32 idx)
{
	u32 ldpid = idx & 0x3fu;
	u32 dbuf = (idx >> 6) & 1u;

	/* physical LAN NI ports 0..6: identity (an eth0 direct-TX frame's
	 * descriptor DEST is the ldpid and must egress physical port N);
	 * dbuf=1 rows -> QM (US-PON data path) */
	if (ldpid < CA_NI_ARB_LDPID_PON)
		return dbuf ? CA_NI_PPORT_QM : (int)ldpid;
	/* ldpid 7 (PON) -> blackhole, per the vendor map */
	if (ldpid == CA_NI_ARB_LDPID_PON)
		return CA_NI_PPORT_BLACKHOLE;
	/* 9th-queue (control-frame inject) -> OAM engine */
	if (ldpid >= CA_NI_ARB_LDPID_9QUEUE_LO &&
	    ldpid <= CA_NI_ARB_LDPID_9QUEUE_HI)
		return CA_NI_PPORT_OAM;
	/* CPU_MQ / LLID-GEM-index (US PON DATA inject) -> QM */
	if (ldpid >= CA_NI_ARB_LDPID_CPU_MQ_LO &&
	    ldpid <= CA_NI_ARB_LDPID_CPU_MQ_HI)
		return CA_NI_PPORT_QM;
	return -1;	/* not programmed by this driver's init */
}

/* Descriptor word1 + PON HEADER_A encoders. Bit positions are ...
 * dev/MEASURED-cortina_ni_tx_logic.c.md sec 5. */
#define CA_NI_TX_DESC1_DEST_SHF		1	/* GENMASK(4, 1): LAN port */
#define CA_NI_TX_DESC1_DEST_MASK	0xfu
#define CA_NI_TX_DESC1_COS_SHF		5	/* GENMASK(7, 5) */
#define CA_NI_TX_DESC1_COS_MASK		0x7u
#define CA_NI_TX_DESC1_LEN_SHF		8	/* GENMASK(18, 8): frame len, no FCS */
#define CA_NI_TX_DESC1_LEN_MASK		0x7ffu
#define CA_NI_TX_DESC1_CHK_AUTO		(1u << 19)	/* CHK_SEL[21:19] = 1 */
#define CA_NI_TX_DESC1_MODE_DIRECT	(1u << 22)	/* direct-TX format */
#define CA_NI_TX_DESC1_HP0		(1u << 24)
#define CA_NI_TX_DESC1_EOF		(1u << 29)	/* sop_eop = 01 */
#define CA_NI_TX_DESC1_SOF		(1u << 30)	/* sop_eop = 10 */
#define CA_NI_TX_DESC1_HP1		(1u << 31)
#define CA_NI_TX_DESC1_HDR_LEN_SHF	8	/* GENMASK(23, 8): buf_len, header mode */
#define CA_NI_TX_DESC1_HDR_LEN_MASK	0xffffu

u32 cortina_ni_tx_desc1_direct(u32 len, u32 cos)
{
	return CA_NI_TX_DESC1_HP1 | CA_NI_TX_DESC1_HP0 |
	       CA_NI_TX_DESC1_MODE_DIRECT | CA_NI_TX_DESC1_CHK_AUTO |
	       ((len & CA_NI_TX_DESC1_LEN_MASK) << CA_NI_TX_DESC1_LEN_SHF) |
	       ((cos & CA_NI_TX_DESC1_COS_MASK) << CA_NI_TX_DESC1_COS_SHF);
}

u32 cortina_ni_tx_desc1_dest(u32 port)
{
	return (port & CA_NI_TX_DESC1_DEST_MASK) << CA_NI_TX_DESC1_DEST_SHF;
}

u32 cortina_ni_tx_desc1_pon_sof(void)
{
	return CA_NI_TX_DESC1_SOF | CA_NI_TX_DESC1_HP0 |
	       ((u32)CA_NI_PON_HDR_BLK_LEN << CA_NI_TX_DESC1_HDR_LEN_SHF);
}

u32 cortina_ni_tx_desc1_pon_eof(u32 frame_len)
{
	return CA_NI_TX_DESC1_EOF |
	       ((frame_len & CA_NI_TX_DESC1_HDR_LEN_MASK) <<
		CA_NI_TX_DESC1_HDR_LEN_SHF);
}

/* HEADER_A of a CPU-injected US PON frame (stock word order, proven on the
 * wire: the word at +8 is the PKT_INFO half - no_drop/pol_id, spec bits
 * 32..63 - and the word at +12 is the cos/ldpid/lspid/pkt_size half). */
#define CA_NI_PON_HDRA_LO_COS_MASK	0x7u	/* GENMASK(2, 0) */
#define CA_NI_PON_HDRA_LO_LDPID_SHF	3	/* GENMASK(8, 3) */
#define CA_NI_PON_HDRA_LO_LDPID_MASK	0x3fu
#define CA_NI_PON_HDRA_LO_LSPID_SHF	9	/* GENMASK(14, 9) */
#define CA_NI_PON_HDRA_LO_PKT_SIZE_SHF	15	/* GENMASK(28, 15) */
#define CA_NI_PON_HDRA_LO_PKT_SIZE_MASK	0x3fffu
#define CA_NI_PON_HDRA_LO_FE_BYPASS	(1u << 29)
#define CA_NI_PON_HDRA_HI_NO_DROP	(1u << 13)
#define CA_NI_PON_HDRA_HI_POL_ID_SHF	18	/* GENMASK(26, 18) */
#define CA_NI_PON_HDRA_HI_POL_ID_MASK	0x1ffu
#define CA_NI_PON_LSPID			0x10	/* CPU0 logical port */

static void ca_ni_le32_put(u8 *p, u32 v)
{
	p[0] = v & 0xff;
	p[1] = (v >> 8) & 0xff;
	p[2] = (v >> 16) & 0xff;
	p[3] = (v >> 24) & 0xff;
}

void cortina_ni_pon_hdr_blk_fill(u8 *blk, u32 pkt_size, u32 cos, u32 ldpid,
				 u32 pol_id)
{
	u32 lo, hi;

	lo = (cos & CA_NI_PON_HDRA_LO_COS_MASK) |
	     ((ldpid & CA_NI_PON_HDRA_LO_LDPID_MASK) <<
	      CA_NI_PON_HDRA_LO_LDPID_SHF) |
	     ((u32)CA_NI_PON_LSPID << CA_NI_PON_HDRA_LO_LSPID_SHF) |
	     ((pkt_size & CA_NI_PON_HDRA_LO_PKT_SIZE_MASK) <<
	      CA_NI_PON_HDRA_LO_PKT_SIZE_SHF) |
	     CA_NI_PON_HDRA_LO_FE_BYPASS;
	hi = CA_NI_PON_HDRA_HI_NO_DROP |
	     ((pol_id & CA_NI_PON_HDRA_HI_POL_ID_MASK) <<
	      CA_NI_PON_HDRA_HI_POL_ID_SHF);

	ca_ni_le32_put(blk, 0);			/* +0:  LSO para0 */
	ca_ni_le32_put(blk + 4, pkt_size);	/* +4:  LSO para1 = pkt_size */
	ca_ni_le32_put(blk + 8, hi);		/* +8:  pkt_info word (stock order) */
	ca_ni_le32_put(blk + 12, lo);		/* +12: cos/ldpid/pkt_size word */
}
