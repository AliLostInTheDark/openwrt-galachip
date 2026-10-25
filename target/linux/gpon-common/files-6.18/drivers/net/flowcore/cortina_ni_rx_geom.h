/* SPDX-License-Identifier: GPL-2.0-only */
/* cortina_ni_rx_geom.h -- RX BUFFER GEOMETRY, lifted out of ...
 * dev/MEASURED-cortina_ni_rx_geom.h.md sec 1. */
#ifndef _CORTINA_NI_RX_GEOM_H
#define _CORTINA_NI_RX_GEOM_H

#include <linux/types.h>

/* 1. WHICH POOL OWNS THIS BUFFER, AND IS IT OURS TO RECYCLE ...
 * dev/MEASURED-cortina_ni_rx_geom.h.md sec 2. */
enum ca_ni_rx_pool {
	CA_NI_RX_POOL_BAD = 0,	/* PA outside the mapped window - not ours at all */
	CA_NI_RX_POOL_CPU0,	/* first CPU pool  - ours, hand the PA back */
	CA_NI_RX_POOL_CPU1,	/* second CPU pool - ours, hand the PA back */
	CA_NI_RX_POOL_DQ,	/* deep-queue pool - HW-managed, NEVER push it back */
};

/* The board's pool geometry. Every field is a SHELL constant, ...
 * dev/MEASURED-cortina_ni_rx_geom.h.md sec 3. */
struct ca_ni_rx_pool_geom {
	u32 map_size;
	u32 pool0_bytes;
	u32 dq_off;
	u32 pool0_bufsz;
	u32 pool1_bufsz;
	u32 dq_bufsz;
	u32 tailroom;
};

/* @off_in_region: byte offset of the buffer inside the ...
 * dev/MEASURED-cortina_ni_rx_geom.h.md sec 4. */
struct ca_ni_rx_buf {
	u32 off_in_region;
	u32 buf_max;
	u32 rpa;
};

/* The usable payload window of one buffer. Clamped at zero: a ...
 * dev/MEASURED-cortina_ni_rx_geom.h.md sec 5. */
static inline u32 ca_ni_rx_usable_end(u32 bufsz, u32 tailroom)
{
	return bufsz > tailroom ? bufsz - tailroom : 0;
}

/* ca_ni_rx_buf_locate() - place a descriptor's buffer PA in ...
 * dev/MEASURED-cortina_ni_rx_geom.h.md sec 6. */
static inline enum ca_ni_rx_pool
ca_ni_rx_buf_locate(u32 pa, u32 base, const struct ca_ni_rx_pool_geom *g,
		    struct ca_ni_rx_buf *out)
{
	u32 off = pa - base;

	out->off_in_region = 0;
	out->buf_max = 0;
	out->rpa = 0;

	if (off >= g->map_size)
		return CA_NI_RX_POOL_BAD;

	out->off_in_region = off;

	/* ⚠ OPEN, and deliberately NOT repaired here: the descriptor's PA field is
	 * 128-byte grained while the pool grid is the buffer size, so a MID-BUFFER
	 * PA is representable and would be recycled verbatim.
	 * dev/MEASURED-cortina_ni_rx_geom.h.md sec 12. */
	if (off < g->pool0_bytes) {
		out->buf_max = ca_ni_rx_usable_end(g->pool0_bufsz, g->tailroom);
		out->rpa = pa;
		return CA_NI_RX_POOL_CPU0;
	}
	if (off < g->dq_off) {
		out->buf_max = ca_ni_rx_usable_end(g->pool1_bufsz, g->tailroom);
		out->rpa = pa;
		return CA_NI_RX_POOL_CPU1;
	}
	out->buf_max = ca_ni_rx_usable_end(g->dq_bufsz, g->tailroom);
	out->rpa = 0;			/* hardware-managed: never recycle it */
	return CA_NI_RX_POOL_DQ;
}

/* ------------------------------------------------------------------ */
/* 2. WHERE THE FRAME STARTS, HOW LONG IT IS, AND IS IT WELL FORMED    */
/* ------------------------------------------------------------------ */

enum ca_ni_rx_geom_verdict {
	CA_NI_RX_GEOM_OK = 0,
	CA_NI_RX_GEOM_RUNT,	/* shorter than an Ethernet header: a bad frame */
	CA_NI_RX_GEOM_OVERSIZE,	/* a good frame that does not fit the usable window */
};

/* The buffer's fixed layout, all of it the shell's: ...
 * dev/MEASURED-cortina_ni_rx_geom.h.md sec 7. */
struct ca_ni_rx_layout {
	u32 hdra_off;
	u32 frame_off;
	u32 hdr_cpu_len;
	u32 desc_hdr_len;
	u32 eth_hlen;
};

/* @off: where the frame starts inside the buffer @len: how ...
 * dev/MEASURED-cortina_ni_rx_geom.h.md sec 8. */
struct ca_ni_rx_geom {
	u32 off;
	int len;
	bool stale;
};

/* ca_ni_rx_frame_geom() - decide a received frame's offset, ...
 * dev/MEASURED-cortina_ni_rx_geom.h.md sec 9. */
static inline enum ca_ni_rx_geom_verdict
ca_ni_rx_frame_geom(const struct ca_ni_rx_layout *l, u32 swid, u32 dlen,
		    bool hdra_cpu_flg, int hdra_pkt_size, u32 buf_max,
		    struct ca_ni_rx_geom *out)
{
	u32 off;
	int len;

	out->stale = false;

	if (!swid) {
		len = hdra_pkt_size;
		out->stale = ((u32)(len + (int)(l->frame_off - l->hdra_off)) !=
			      dlen);
		off = l->frame_off;
		if (hdra_cpu_flg) {
			off += l->hdr_cpu_len;
			len -= (int)l->hdr_cpu_len;
		}
	} else {
		/* headerless format: frame at a fixed offset, length from the
		 * descriptor.  There is no second witness here, so no staleness
		 * verdict is available -- and none is invented. */
		off = l->desc_hdr_len;
		len = (int)dlen - (int)l->desc_hdr_len;
	}

	out->off = off;
	out->len = len;

	if (len < (int)l->eth_hlen)
		return CA_NI_RX_GEOM_RUNT;
	/* len >= eth_hlen here, so the widening to u32 cannot go wrong */
	if (off + (u32)len > buf_max)
		return CA_NI_RX_GEOM_OVERSIZE;
	return CA_NI_RX_GEOM_OK;
}

/* 3. THE PDC's CONTROL PUNT: is this a PON control frame, and ...
 * dev/MEASURED-cortina_ni_rx_geom.h.md sec 10. */
struct ca_ni_pon_pdu {
	u32 off;
	u32 len;
};

/* ca_ni_rx_pon_ctrl() - is this the PDC's control punt, and ...
 * dev/MEASURED-cortina_ni_rx_geom.h.md sec 11. */
static inline bool
ca_ni_rx_pon_ctrl(const u8 *frame, int len, u16 lnk_type, u32 hdr_len,
		  struct ca_ni_pon_pdu *out)
{
	out->off = 0;
	out->len = 0;

	if (!frame || len < 14)
		return false;
	if ((u16)(((u16)frame[12] << 8) | frame[13]) != lnk_type)
		return false;

	if ((u32)len > hdr_len) {
		out->off = hdr_len;
		out->len = (u32)len - hdr_len;
	}
	return true;
}

#endif /* _CORTINA_NI_RX_GEOM_H */
