// SPDX-License-Identifier: GPL-2.0-only
#include <linux/errno.h>
#include "gpon_range_gate.h"

void gpon_range_plan(const struct gpon_range_request *r,
		     struct gpon_range_action *a)
{
	/* ⚠ THE GUARD COMES FIRST. It used to sit after the initialiser, so a
	 * NULL output was dereferenced by the very statement meant to make the
	 * function safe -- and the test that claimed to cover it passed a real
	 * pointer, so the name was broader than the check. */
	if (!a)
		return;

	*a = (struct gpon_range_action){ .quiesce = false, .program = false,
					 .activated = false, .result = 0,
					 .reason = GPON_RANGE_GO };
	if (!r)
		return;

	/* NOTHING TO DO comes first AND touches no hardware: ...
	 * dev/MEASURED-gpon_range_gate.c.md sec 1. */
	if (r->ranging_now && r->identity_defined && !r->identity_changed &&
	    r->activation_wanted && r->laser_wanted) {
		a->activated = true;
		a->reason = GPON_RANGE_UNCHANGED;
		return;
	}

	/* Every refusal CLEARS the GO bit if it is set -- see the header. */
	if (!r->identity_defined) {
		a->quiesce = r->ranging_now;
		a->result = -ENXIO;
		a->reason = GPON_RANGE_NO_IDENTITY;
		return;
	}
	if (!r->activation_wanted) {
		a->quiesce = r->ranging_now;
		a->reason = GPON_RANGE_NOT_WANTED;
		return;		/* latched, deliberately not ranging: not an error */
	}
	if (!r->laser_wanted) {
		a->quiesce = r->ranging_now;
		a->result = -ENODEV;
		a->reason = GPON_RANGE_LASER_OFF;
		return;
	}

	/* A re-activation on a LIVE link quiesces before it programs anything. */
	a->quiesce = r->ranging_now;
	a->program = true;
	a->activated = true;
	a->reason = GPON_RANGE_GO;
}
const char *gpon_range_reason_str(enum gpon_range_reason why)
{
	switch (why) {
	case GPON_RANGE_GO:		return "arming";
	case GPON_RANGE_UNCHANGED:	return "already ranging with this identity";
	case GPON_RANGE_NO_IDENTITY:	return "no defined serial number";
	case GPON_RANGE_NOT_WANTED:	return "activation switched off";
	case GPON_RANGE_LASER_OFF:	return "the laser is deliberately unprogrammed";
	case GPON_RANGE_LASER_FAILED:	return "the laser could not be programmed";
	}
	return "unknown";
}
