// SPDX-License-Identifier: GPL-2.0-or-later
/* TIER: CORE (prefix gpon_) — protocol only. NEVER touches ...
 * dev/MEASURED-gpon_gem_us.c.md sec 1. */

#include <linux/types.h>

#include "gpon_gem_us.h"

bool gpon_gem_us_range_ok(const struct gpon_gem_us_range *r)
{
	if (!r)
		return false;
	return GPON_GEM_US_RANGE_OK(r->base, r->count, r->index_max);
}

/* Alloc-ID -> T-CONT. ★ WHY THIS PREDICATE HAS A FILE TO ...
 * dev/MEASURED-gpon_gem_us.c.md sec 2. */
bool gpon_gem_us_ride_range(const struct gpon_gem_us_range *omcc,
			    struct gpon_gem_us_range *out)
{
	if (!omcc || !out || !gpon_gem_us_range_ok(omcc))
		return false;
	/* every slot but the reserved top ones; a run that cannot spare one is
	 * refused rather than served by stealing an OMCI slot */
	if (omcc->count <= GPON_GEM_US_OMCI_RESERVED_SLOTS)
		return false;
	out->base = omcc->base;
	out->count = (u16)(omcc->count - GPON_GEM_US_OMCI_RESERVED_SLOTS);
	out->index_max = omcc->index_max;
	return gpon_gem_us_range_ok(out);
}

enum gpon_gem_us_bind gpon_gem_us_tcont_decide(u16 alloc, u16 omcc_alloc,
					       bool already_bound)
{
	if (already_bound)
		return GPON_GEM_US_BIND_DONE;
	if (alloc == omcc_alloc)
		return GPON_GEM_US_BIND_IS_OMCC;
	return GPON_GEM_US_BIND_TCONT;
}

enum gpon_omcc_action gpon_omcc_decide(bool port_en, u16 want_gem,
				       bool installed, u16 cur_gem)
{
	if (!port_en)
		return GPON_OMCC_IGNORE;
	if (!installed)
		return GPON_OMCC_INSTALL;
	if (want_gem == cur_gem)
		return GPON_OMCC_UNCHANGED;
	return GPON_OMCC_REBIND;
}

const char *gpon_omcc_action_name(enum gpon_omcc_action a)
{
	switch (a) {
	case GPON_OMCC_IGNORE:
		return "ignore-enable-0";
	case GPON_OMCC_INSTALL:
		return "install";
	case GPON_OMCC_REBIND:
		return "rebind-transport";
	case GPON_OMCC_UNCHANGED:
		return "same-gem";
	}
	/* An unnamed verdict must READ as unknown, never as a plausible one. */
	return "unknown";
}

void gpon_omcc_tcont_decide(u16 alloc_override, u8 onu_id, bool alt_bind,
			  struct gpon_omcc_tcont_plan *out)
{
	if (!out)
		return;
	out->alloc = alloc_override ? alloc_override : onu_id;
	/* Never double-bind the plain ONU-ID: with no override the alternate
	 * T-CONT would take the SAME alloc as the real one and swallow the
	 * grant (see the header). */
	out->bind_alt = alt_bind && out->alloc != onu_id;
}

const char *gpon_gem_us_bind_name(enum gpon_gem_us_bind v)
{
	switch (v) {
	case GPON_GEM_US_BIND_TCONT:
		return "bind-data-tcont";
	case GPON_GEM_US_BIND_DONE:
		return "already-bound";
	case GPON_GEM_US_BIND_IS_OMCC:
		return "is-omcc-alloc";
	}
	/* An unnamed verdict must READ as unknown, never as a plausible one:
	 * a decision printed under the wrong name is worse than an unprinted
	 * decision. */
	return "unknown";
}

/* ★ TWO UPSTREAM FACTS THAT ARE *NOT* HERE, AND WHY — so a ...
 * dev/MEASURED-gpon_gem_us.c.md sec 3. */
