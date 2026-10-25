// SPDX-License-Identifier: GPL-2.0-or-later
/* TIER: CORE, strict host-buildable subset.  It REPORTS: no register, no lock,
 * no allocation, no clock -- dev/MEASURED-gpon_flow_diag.c.md sec 1. */
#include <linux/kernel.h>
#include <linux/types.h>

#include "gpon_flow_diag.h"

static const char * const gpon_flow_refusal_names[GPON_FLOW_REF__COUNT] = {
	[GPON_FLOW_OK]			= "ok",
	[GPON_FLOW_REF_NO_ENGINE]	= "no-engine",
	[GPON_FLOW_REF_DUP_COOKIE]	= "dup-cookie",
	[GPON_FLOW_REF_BAD_ARGS]	= "bad-args",
	[GPON_FLOW_REF_NOT_IPV4]	= "not-ipv4",
	[GPON_FLOW_REF_NO_BASIC]	= "no-basic",
	[GPON_FLOW_REF_NOT_TCP_UDP]	= "not-tcp-udp",
	[GPON_FLOW_REF_NO_PORTS]	= "no-ports",
	[GPON_FLOW_REF_NAT_OFFSET]	= "nat-offset",
	[GPON_FLOW_REF_PORT_SHAPE]	= "port-shape",
	[GPON_FLOW_REF_MANGLE_HTYPE]	= "mangle-htype",
	[GPON_FLOW_REF_VLAN_ACTION]	= "vlan-action",
	[GPON_FLOW_REF_ACTION_ID]	= "action-id",
	[GPON_FLOW_REF_INCOMPLETE]	= "incomplete",
	[GPON_FLOW_REF_NOMEM]		= "nomem",
	[GPON_FLOW_REF_ENGINE]		= "engine",
	[GPON_FLOW_REF_BY_DESIGN]	= "by-design",
	[GPON_FLOW_REF_TABLE_INSERT]	= "table-insert",
};

const char *gpon_flow_refusal_name(enum gpon_flow_refusal r)
{
	if ((unsigned int)r >= GPON_FLOW_REF__COUNT ||
	    !gpon_flow_refusal_names[r])
		return "?";
	return gpon_flow_refusal_names[r];
}

void gpon_flow_tally_note(struct gpon_flow_tally *t, enum gpon_flow_refusal r)
{
	if (!t || (unsigned int)r >= GPON_FLOW_REF__COUNT)
		return;
	t->n[r]++;
	if (r != GPON_FLOW_OK)
		t->last = r;
}

/* Which ingress and leg the last REFUSED offer carried: a leg judged from the
 * wrong ingress is what a downstream refused as upstream looks like. */
void gpon_flow_tally_leg(struct gpon_flow_tally *t, int iif, bool ds_leg)
{
	if (!t)
		return;
	t->last_iif = iif;
	t->last_ds_leg = ds_leg;
}

/* A counter the family did not establish is n/a.  Rendering it as 0 turns an
 * unasked question into a device finding, which is the defect this whole flag
 * family exists to stop. */
static int diag_u32(char *out, size_t sz, const char *tag,
		    const struct gpon_flow_diag *d, u32 bit, u32 v)
{
	if (d && (d->valid & bit))
		return scnprintf(out, sz, " %s=%u", tag, v);
	return scnprintf(out, sz, " %s=n/a", tag);
}

int gpon_flow_diag_line(const struct gpon_flow_tally *t,
			const struct gpon_flow_diag *d, char *out, size_t sz)
{
	unsigned int i, refused = 0, by_design = 0;
	int pos;

	if (!out || !sz)
		return 0;

	pos = scnprintf(out, sz, "flow:");
	pos += diag_u32(out + pos, sz - pos, "offered", d,
			GPON_FDIAG_HAS_OFFERED, d ? d->offered : 0);
	pos += diag_u32(out + pos, sz - pos, "live", d,
			GPON_FDIAG_HAS_LIVE, d ? d->live : 0);
	pos += diag_u32(out + pos, sz - pos, "cap", d,
			GPON_FDIAG_HAS_CAPACITY, d ? d->capacity : 0);
	pos += diag_u32(out + pos, sz - pos, "hits", d,
			GPON_FDIAG_HAS_HITS, d ? d->hw_hits : 0);

	if (!t)
		return pos + scnprintf(out + pos, sz - pos,
				       " | installed=n/a refused=n/a (no tally)");

	/* ★ BY_DESIGN IS NOT A REFUSAL AND MAY NOT BE COUNTED AS ONE.  The enum's
	 * own comment says so -- the family DECLINED because this leg needs no
	 * flow of its own -- and on a board where every DS leg declines, summing
	 * it made `refused` read as a near-total refusal (14494 of 14502) of an
	 * engine that was working exactly as designed.
	 */
	by_design = t->n[GPON_FLOW_REF_BY_DESIGN];
	for (i = GPON_FLOW_OK + 1; i < GPON_FLOW_REF__COUNT; i++)
		if (i != GPON_FLOW_REF_BY_DESIGN)
			refused += t->n[i];
	pos += scnprintf(out + pos, sz - pos, " | installed=%u refused=%u",
			 t->n[GPON_FLOW_OK], refused);
	if (by_design)
		pos += scnprintf(out + pos, sz - pos, " by-design=%u", by_design);
	pos += scnprintf(out + pos, sz - pos, " last=%s",
			 (refused || by_design) ? gpon_flow_refusal_name(t->last)
						: "none");
	if (refused || by_design)
		pos += scnprintf(out + pos, sz - pos, " last_iif=%d last_ds_leg=%u",
				 t->last_iif, t->last_ds_leg);

	/* Only the causes that FIRED, so a healthy board prints nothing here. */
	pos += scnprintf(out + pos, sz - pos, " why{");
	for (i = GPON_FLOW_OK + 1; i < GPON_FLOW_REF__COUNT; i++)
		if (t->n[i])
			pos += scnprintf(out + pos, sz - pos, "%s=%u ",
					 gpon_flow_refusal_name(i), t->n[i]);
	pos += scnprintf(out + pos, sz - pos, "}");
	return pos;
}
