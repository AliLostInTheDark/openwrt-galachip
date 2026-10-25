/* SPDX-License-Identifier: GPL-2.0-only */
/* MAY THIS ONU RANGE, AND WHAT MUST CHANGE IF NOT. The ...
 * dev/MEASURED-gpon_range_gate.h.md sec 1. */
#ifndef _GPON_RANGE_GATE_H
#define _GPON_RANGE_GATE_H

#include <linux/types.h>

enum gpon_range_reason {
	GPON_RANGE_GO,			/* arm it                            */
	GPON_RANGE_UNCHANGED,		/* already ranging with this identity */
	GPON_RANGE_NO_IDENTITY,		/* no defined serial number          */
	GPON_RANGE_NOT_WANTED,		/* activation switched off           */
	GPON_RANGE_LASER_OFF,		/* laser deliberately unprogrammed   */
	GPON_RANGE_LASER_FAILED,	/* the laser could not be programmed */
};

struct gpon_range_request {
	bool identity_defined;	/* the serial in force is a real one         */
	bool identity_changed;	/* it differs from the one last armed with   */
	bool ranging_now;	/* the GO bit is asserted RIGHT NOW          */
	bool activation_wanted;	/* the operator has not switched ranging off */
	bool laser_wanted;	/* the operator has not switched the laser off */
};

/* THE ORDER IS THE ANSWER, which is why this is three flags ...
 * dev/MEASURED-gpon_range_gate.h.md sec 2. */
struct gpon_range_action {
	bool quiesce;		/* FIRST: clear the GO bit                   */
	bool program;		/* THEN: program the laser, config, and arm  */
	bool activated;		/* the readiness flag IF those all succeed   */
	int  result;		/* what the caller that asked is told        */
	enum gpon_range_reason reason;
};

void gpon_range_plan(const struct gpon_range_request *r,
		     struct gpon_range_action *a);
const char *gpon_range_reason_str(enum gpon_range_reason why);

#endif /* _GPON_RANGE_GATE_H */
