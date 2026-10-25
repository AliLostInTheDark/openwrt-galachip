/* SPDX-License-Identifier: GPL-2.0-only */
/* l34_decode.h -- the PURE half of the RTL9602C flow-offload ...
 * dev/MEASURED-rtl9602c_l34_decode.h.md sec 1. */
#ifndef L34_DECODE_H
#define L34_DECODE_H

/* ★★ ONE HEADER, TWO BUILDS -- and that is the point ...
 * dev/MEASURED-rtl9602c_l34_decode.h.md sec 2. */
#ifdef __KERNEL__
#include <linux/types.h>
#include <linux/string.h>
#else
#include <stdint.h>
#include <string.h>
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
#endif

/* Mirrors the kernel's struct l34_flow (rtl9602c_l34.h) field for field. */
struct l34_flow_d {
	u8  l4proto;
	u32 orig_sip, orig_dip;
	u16 orig_sport, orig_dport;
	u32 nat_sip, nat_dip;
	u16 nat_sport, nat_dport;
	u8  egress_netif;
	u8  nexthop;
	u16 hw_index;
};

/* What the shim hands over: the ORIGINAL direction and what NAT turned it into.
 * Both come from the offload rule; neither is invented here. */
struct l34_decode_in {
	u8  l4proto;
	u32 o_sip, o_dip;      u16 o_sport, o_dport;   /* original */
	u32 r_sip, r_dip;      u16 r_sport, r_dport;   /* post-NAT  */
	u8  egress_netif, nexthop;
	int      have_reply;        /* 0 = no NAT rewrite was described at all */
};

enum {
	L34_DEC_OK = 0,
	L34_DEC_BAD_PROTO,      /* the engine keys on TCP/UDP only            */
	L34_DEC_NO_TUPLE,       /* a zero address or port in the ORIGINAL side */
};

#define L34_IPPROTO_TCP 6
#define L34_IPPROTO_UDP 17

/* Decode ONE rule. -> L34_DEC_*, filling `out` only on OK. ★ ...
 * dev/MEASURED-rtl9602c_l34_decode.h.md sec 3. */
static inline int l34_decode(const struct l34_decode_in *in,
			     struct l34_flow_d *out)
{
	if (!in || !out)
		return L34_DEC_NO_TUPLE;
	if (in->l4proto != L34_IPPROTO_TCP && in->l4proto != L34_IPPROTO_UDP)
		return L34_DEC_BAD_PROTO;
	if (!in->o_sip || !in->o_dip || !in->o_sport || !in->o_dport)
		return L34_DEC_NO_TUPLE;

	memset(out, 0, sizeof(*out));
	out->l4proto    = in->l4proto;
	out->orig_sip   = in->o_sip;
	out->orig_dip   = in->o_dip;
	out->orig_sport = in->o_sport;
	out->orig_dport = in->o_dport;
	out->egress_netif = in->egress_netif;
	out->nexthop      = in->nexthop;

	if (!in->have_reply)
		return L34_DEC_OK;      /* routed, not NAT'd: all rewrites stay 0 */

	/* ONLY the fields that actually change. See the trap note above. */
	if (in->r_sip   && in->r_sip   != in->o_sip)   out->nat_sip   = in->r_sip;
	if (in->r_dip   && in->r_dip   != in->o_dip)   out->nat_dip   = in->r_dip;
	if (in->r_sport && in->r_sport != in->o_sport) out->nat_sport = in->r_sport;
	if (in->r_dport && in->r_dport != in->o_dport) out->nat_dport = in->r_dport;
	return L34_DEC_OK;
}

#endif /* L34_DECODE_H */
