// SPDX-License-Identifier: GPL-2.0-or-later
/* TIER: CORE, strict host-buildable subset. See ... -- dev/MEASURED-gpon_gem_diag.c.md sec 1. */
#include <linux/kernel.h>
#include <linux/types.h>

#include "gpon_gem_diag.h"

int gpon_gem_diag_line(const struct gpon_data_armed *armed,
		       const struct gpon_data_want *want,
		       char *out, size_t sz)
{
	enum gpon_data_plan plan;
	enum gpon_data_blocker why;
	int pos;

	if (!out || !sz)
		return 0;
	plan = gpon_data_plan_decide(armed, want);
	why  = gpon_data_plan_why(armed, want);

	/* ★ A MISSING INPUT IS SAID, NEVER RENDERED AS ZEROS.  `want alloc=0x0000
	 *   known=0` and "nobody handed us a want at all" are different facts,
	 *   and printing the first for the second is how an absent measurement
	 *   comes to read as a device finding. */
	if (!armed || !want)
		return scnprintf(out, sz, "gem: plan=%s blocked=%s (%s%s missing)",
				 gpon_data_plan_name(plan),
				 gpon_data_blocker_name(why),
				 armed ? "" : "armed",
				 want ? "" : (armed ? "want" : "+want"));

	pos = scnprintf(out, sz,
			"gem: want alloc=0x%04x known=%u gem=0x%04x omcc=%s/0x%04x",
			want->alloc, want->alloc_known ? 1u : 0u, want->gem,
			want->omcc_up ? "up" : "down", want->omcc_alloc);
	pos += scnprintf(out + pos, sz - pos,
			 " | armed alloc=0x%04x bound=%u gem=0x%04x ride=%u installed=%u",
			 armed->alloc, armed->alloc_bound ? 1u : 0u, armed->gem,
			 armed->rides_omcc ? 1u : 0u, armed->installed ? 1u : 0u);
	pos += scnprintf(out + pos, sz - pos, " | plan=%s blocked=%s",
			 gpon_data_plan_name(plan), gpon_data_blocker_name(why));
	return pos;
}
