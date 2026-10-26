// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * TIER: CORE, strict host-buildable subset.  See gpon_ploam_diag.h for what
 * this is and why a missing arm must render as `n/a` and never as 0.
 */
#include <linux/kernel.h>
#include <linux/types.h>

#include "gpon_ploam_diag.h"

enum gpon_ploam_diag_point gpon_ploam_diag_point_of(enum gpon_ploam_ev ev)
{
	switch (ev) {
	case GPON_PLOAM_EV_ONU_ID:
		return GPON_PDIAG_ASSIGN;
	case GPON_PLOAM_EV_RANGING_TIME:
		return GPON_PDIAG_RANGING_TIME;
	case GPON_PLOAM_EV_EARLY_DWELL:
		return GPON_PDIAG_EARLY;
	case GPON_PLOAM_EV_DEACT:
		return GPON_PDIAG_DEACT;
	default:
		return GPON_PDIAG_NONE;
	}
}

const char *gpon_ploam_diag_point_name(enum gpon_ploam_diag_point p)
{
	switch (p) {
	case GPON_PDIAG_ASSIGN:
		return "assign";
	case GPON_PDIAG_RANGING_TIME:
		return "ranging_time";
	case GPON_PDIAG_EARLY:
		return "early_dwell";
	case GPON_PDIAG_DEACT:
		return "deact";
	default:
		return "?";
	}
}

/* One `name=value` or `name=n/a` field, appended at `*pos`.  The buffer is
 * the caller's; scnprintf never writes past it, and a full buffer simply
 * stops appending -- a clipped line is still a true line. */
static void field_u32(char *out, size_t sz, int *pos, const char *name,
		      bool has, u32 v)
{
	if ((size_t)*pos >= sz)
		return;
	if (has)
		*pos += scnprintf(out + *pos, sz - *pos, " %s=%u", name, v);
	else
		*pos += scnprintf(out + *pos, sz - *pos, " %s=n/a", name);
}

int gpon_ploam_diag_format(char *out, size_t sz, enum gpon_ploam_diag_point p,
			   u32 t_ms, u8 ostate, const struct gpon_ploam_diag *d)
{
	int pos;

	if (!out || !sz)
		return 0;
	pos = scnprintf(out, sz, "ploam-diag %s t=%ums O%u",
			gpon_ploam_diag_point_name(p), t_ms, ostate);
	if (!d) {
		/* No readings at all is a valid report: every arm is n/a. */
		static const struct gpon_ploam_diag none = { .valid = 0 };

		d = &none;
	}
	field_u32(out, sz, &pos, "ploam_acpt",
		  !!(d->valid & GPON_PDIAG_HAS_PLOAM_ACPT), d->ploam_acpt);
	field_u32(out, sz, &pos, "bwm_acpt",
		  !!(d->valid & GPON_PDIAG_HAS_BWM_ACPT), d->bwm_acpt);
	field_u32(out, sz, &pos, "bwm_fail",
		  !!(d->valid & GPON_PDIAG_HAS_BWM_FAIL), d->bwm_fail);
	field_u32(out, sz, &pos, "bwm_inv",
		  !!(d->valid & GPON_PDIAG_HAS_BWM_INV), d->bwm_inv);
	field_u32(out, sz, &pos, "us_ploam_tx",
		  !!(d->valid & GPON_PDIAG_HAS_US_PLOAM_TX), d->us_ploam_tx);
	field_u32(out, sz, &pos, "us_onu_id",
		  !!(d->valid & GPON_PDIAG_HAS_US_ONU_ID), d->us_onu_id);
	field_u32(out, sz, &pos, "ds_onu_id",
		  !!(d->valid & GPON_PDIAG_HAS_DS_ONU_ID), d->ds_onu_id);
	/* Appended after the original seven so a reader keyed on the 2026-09-06
	 * line still finds every field where it was. */
	field_u32(out, sz, &pos, "us_sn_tx",
		  !!(d->valid & GPON_PDIAG_HAS_US_SN_TX), d->us_sn_tx);
	field_u32(out, sz, &pos, "poll_gap_ms",
		  !!(d->valid & GPON_PDIAG_HAS_POLL_GAP), d->poll_gap_ms);
	field_u32(out, sz, &pos, "rx_burst",
		  !!(d->valid & GPON_PDIAG_HAS_RX_BURST), d->rx_burst_idx);
	field_u32(out, sz, &pos, "sn_req",
		  !!(d->valid & GPON_PDIAG_HAS_SN_REQ), d->sn_req);
	field_u32(out, sz, &pos, "rng_req",
		  !!(d->valid & GPON_PDIAG_HAS_RNG_REQ), d->rng_req);
	return pos;
}

int gpon_bwcap_diag_format(char *out, size_t sz, enum gpon_ploam_diag_point p,
			   u32 t_ms, u8 ostate, const struct gpon_bwcap_diag *b)
{
	int pos;
	bool has;

	if (!out || !sz)
		return 0;
	pos = scnprintf(out, sz, "bwcap-diag %s t=%ums O%u",
			gpon_ploam_diag_point_name(p), t_ms, ostate);
	if (!b) {
		static const struct gpon_bwcap_diag none = { .valid = 0 };

		b = &none;
	}
	has = !!(b->valid & GPON_BWCAP_HAS);
	field_u32(out, sz, &pos, "harvests", has, b->harvests);
	field_u32(out, sz, &pos, "nonempty", has, b->nonempty);
	field_u32(out, sz, &pos, "entries", has, b->entries);
	field_u32(out, sz, &pos, "ploamu", has, b->ploamu);
	field_u32(out, sz, &pos, "omcc_ploamu", has, b->omcc_ploamu);
	if ((size_t)pos < sz)
		pos += has ? scnprintf(out + pos, sz - pos, " tconts=0x%x", b->tconts)
			   : scnprintf(out + pos, sz - pos, " tconts=n/a");
	field_u32(out, sz, &pos, "overfl", has, b->overfl);
	if ((size_t)pos < sz) {
		if (!has)
			pos += scnprintf(out + pos, sz - pos, " last=n/a");
		else if (!b->omcc_ploamu)
			pos += scnprintf(out + pos, sz - pos, " last=-");
		else
			pos += scnprintf(out + pos, sz - pos, " last=%08x/%08x",
					 b->last_raw0, b->last_raw1);
	}
	return pos;
}

/* One quiet line of the core's per-type PLOAM accounting: only the non-zero
 * types, in hex, "1f+" for the shared last slot. -> bytes written. */
static void types_u32(char *out, size_t sz, int *pos, const char *label,
		      const u32 *v)
{
	unsigned int t;

	if ((size_t)*pos < sz)
		*pos += scnprintf(out + *pos, sz - *pos, " %s", label);
	for (t = 0; t < GPON_PLOAM_TYPE_SLOTS && (size_t)*pos < sz; t++)
		if (v[t])
			*pos += scnprintf(out + *pos, sz - *pos, " %02x%s:%u", t,
					  t == GPON_PLOAM_TYPE_SLOTS - 1 ? "+" : "", v[t]);
}

int gpon_ploam_types_format(char *out, size_t sz, const struct gpon_ploam *o)
{
	int pos;

	if (!out || !sz)
		return 0;
	pos = scnprintf(out, sz, "ploam-types");
	if (!o)
		return pos + ((size_t)pos < sz ? scnprintf(out + pos, sz - pos, " n/a") : 0);
	types_u32(out, sz, &pos, "ds_own", o->ds_own);
	types_u32(out, sz, &pos, "| ds_bcast", o->ds_bcast);
	if ((size_t)pos < sz)
		pos += scnprintf(out + pos, sz - pos, " | ds_other %u |", o->ds_other);
	types_u32(out, sz, &pos, "us_queued", o->us_queued);
	return pos;
}
