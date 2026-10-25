/* SPDX-License-Identifier: GPL-2.0-or-later */
/* TIER: CORE. ⚠ NOT in the STRICT subset -- it uses Linux's ...
 * dev/MEASURED-gpon_flow.h.md sec 1. */
#ifndef GPON_FLOW_H
#define GPON_FLOW_H

#include <linux/types.h>

#include "gpon_flow_diag.h"

struct flow_rule;

/* The 5-tuple, in HOST byte order and with no hardware in ...
 * dev/MEASURED-gpon_flow.h.md sec 2. */
struct gpon_flow_key {
	u32 ip_sa;		/* source address, host order		*/
	u32 ip_da;		/* destination address, host order	*/
	u16 l4_sport;
	u16 l4_dport;
	u8  ip_protocol;	/* IPPROTO_TCP or IPPROTO_UDP		*/
	u8  ip_ver;		/* 0 = IPv4.  IPv6 is not decoded yet	*/
};

/* Decode one TC flow rule into a key. -> 0, or -EOPNOTSUPP. ★ ...
 * dev/MEASURED-gpon_flow.h.md sec 3. */
/* @why (optional) receives the NAMED cause -- the four refusals here all
 * return -EOPNOTSUPP, so the errno alone cannot tell them apart. */
int gpon_flow_key_from_tc(struct flow_rule *rule, struct gpon_flow_key *key,
			  enum gpon_flow_refusal *why);

#endif /* GPON_FLOW_H */
