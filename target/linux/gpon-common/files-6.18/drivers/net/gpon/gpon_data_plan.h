/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * TIER: CORE (prefix gpon_) — decides, never touches hardware, and compiles for
 * MIPS-BE, ARM64-LE and x86.  Canonical tier rule: see "THE THREE TIERS" in
 * gpon_common.h (this directory).
 *
 * gpon_data_plan.h — "is what is ARMED still what the OLT WANTS?", answered
 * once instead of three times.
 *
 * ★★★ THE SAME QUESTION WAS ANSWERED IN THREE PLACES, EACH COVERING A DIFFERENT
 *     SUBSET, AND NONE COULD CALL ANOTHER (measured 2026-09-04): the core's own
 *     re-arm notices only the GEM MOVING; Luna's stale-clear fires only on an
 *     explicit OLT Deallocate, so a REASSIGNMENT without one is invisible to
 *     it; the Cortina shell is the widest and the only one carrying both
 *     clauses below.  G.984.3 lets the OLT reassign an Alloc-ID or a GEM
 *     Port-ID at any time, so an ONU that latches "installed" and never re-asks
 *     bursts a stale GEM into a grant slot that now belongs to somebody else.
 *     WHICH REGISTERS hold the binding is silicon; WHETHER IT IS STILL RIGHT is
 *     protocol.
 *
 * ★ THE TWO CLAUSES THAT EXIST IN NEITHER OTHER COPY — the reason this file is
 *   not just a rename:
 *   (a) THE ALLOC MOVED WITH THE GEM UNCHANGED.  An OLT that re-provisions ME
 *       262 onto a different Alloc-ID while keeping ME 268 leaves the T-CONT
 *       CAM pointing at an Alloc-ID the OLT no longer grants: upstream simply
 *       stops, with every shadow reading healthy.
 *   (b) THE RIDE PREMISE WAS LOST.  On a single-alloc OLT the data path is
 *       installed on the OMCC's own T-CONT because the data Alloc-ID WAS the
 *       OMCC's.  If the OLT then moves the OMCC mid-O5 that premise is gone and
 *       user traffic silently follows whatever Alloc-ID the OMCC moved to.
 *       Neither {alloc, gem} comparison can see this: neither value changed.
 *
 * ★ IT DECIDES AND NEVER DOES, and does not even ASK for a teardown through an
 *   op table: the upstream teardown ORDER is a hardware requirement (drain the
 *   VoQs first), and the tree contains ZERO instances of struct gpon_shell_ops
 *   (checked 2026-09-04).  Everything below is a PURE VERDICT.
 *
 * ★ HEADER-ONLY, ON PURPOSE — static inline, so no Makefile line and no object.
 *   ⚠ THE CONSEQUENCE: the strict-subset host-build gate walks a list of .c
 *   files and does NOT reach this header.  Its host-build coverage is
 *   dev/rtl9607c-test/gpon_data_plan_diff_test.c, which includes and compiles
 *   it on x86 -- that binary IS the purity proof, and deleting it removes one.
 *
 * ★ THE HONEST CAVEAT: the RECONCILE half is the load-bearing one.  The UNDO
 *   half has ONE consumer today (Luna has no data teardown at all), so its
 *   value is preventing a future re-derivation, not deduplicating a copy.
 */
#ifndef GPON_DATA_PLAN_H
#define GPON_DATA_PLAN_H

#include <linux/types.h>

/* for the two 12-bit wire masks: the armed identity is stored MASKED and the
 * provisioned one is not, so the comparison must mask exactly where the shell
 * masked before. */
#include "gpon_gem_us.h"

/* What the shell has actually armed in silicon. Allocation ...
 * dev/MEASURED-gpon_data_plan.h.md sec 7. */
struct gpon_data_armed {
	u16	alloc;		/* Alloc-ID armed in the T-CONT CAM */
	bool	alloc_bound;	/* never inferred from numeric alloc or installed */
	u16	gem;		/* GEM Port-ID armed in the DS/US CAM (0 = none) */
	bool	rides_omcc;	/* armed on the OMCC's T-CONT, not a dedicated one */
	bool	installed;	/* the WHOLE path is armed */
};

/* Wanted OMCI mapping, kept across a temporary transport loss. alloc_known
 * is an accepted attr-1 write, not a boot default or numeric nonzero test.
 * Raw validity below does not establish PLOAM assignment/ownership; the caller
 * must preserve that separate admission premise. Never mask before validation. */
struct gpon_data_want {
	u16	alloc;		/* ME 262 Alloc-ID, as read */
	bool	alloc_known;	/* raw zero and raw ffff are both representable */
	u16	gem;		/* ME 268 GEM Port-ID, as read */
	u16	omcc_alloc;	/* the Alloc-ID actually bound to the OMCC T-CONT */
	bool	omcc_up;	/* the OMCC transport is up */
};

/* The verdict.  Five outcomes, because "do nothing" and "do nothing YET" are
 * different facts and a shell that collapses them cannot say why the data path
 * never came up. */
enum gpon_data_plan {
	/* preconditions unmet -> write NOTHING.  Not an error: this is where an
	 * ONU sits between O5 and the OLT's ME 262/268. */
	GPON_DATA_WAIT = 0,
	/* armed == provisioned -> the PROVEN KEEP-PATH.  Write nothing and take
	 * no branch: this is what makes a LOS/fiber-pull re-range byte-identical
	 * and is why the 30/30 soak holds. */
	GPON_DATA_KEEP,
	/* nothing armed, both halves provisioned -> install. */
	GPON_DATA_INSTALL,
	/* a DIFFERENT identity is armed -> tear the stale one down FIRST.  Never
	 * install over it: a stale CAM entry that outlives its grant assignment
	 * bursts into somebody else's slot. */
	GPON_DATA_REPLACE,
	/* armed, and the OLT has DEPROVISIONED (or moved) -> undo only. */
	GPON_DATA_TEARDOWN,
};

/* WHY the data path is not up -- and this is a SEPARATE ...
 * dev/MEASURED-gpon_data_plan.h.md sec 1. */
enum gpon_data_blocker {
	GPON_DATA_BLOCK_NONE = 0,	/* nothing blocks: KEEP / INSTALL / REPLACE */
	GPON_DATA_BLOCK_NO_INPUT,	/* a NULL reached us: OUR defect, not the OLT's */
	GPON_DATA_BLOCK_ALLOC_UNKNOWN,	/* ME 262 never wrote an Alloc-ID */
	GPON_DATA_BLOCK_ALLOC_INVALID,	/* the Alloc-ID is outside the user domain */
	GPON_DATA_BLOCK_NO_GEM,		/* ME 268 never named a GEM Port-ID */
	GPON_DATA_BLOCK_OMCC_DOWN,	/* both halves provisioned, transport down */
};

static inline const char *gpon_data_blocker_name(enum gpon_data_blocker b)
{
	switch (b) {
	case GPON_DATA_BLOCK_NONE:		return "none";
	case GPON_DATA_BLOCK_NO_INPUT:		return "no-input";
	case GPON_DATA_BLOCK_ALLOC_UNKNOWN:	return "alloc-unknown";
	case GPON_DATA_BLOCK_ALLOC_INVALID:	return "alloc-invalid";
	case GPON_DATA_BLOCK_NO_GEM:		return "no-gem";
	default:				return "omcc-down";
	}
}

/* GPON user-traffic Alloc-ID numeric domain: default 0..253 and additional
 * 256..4095. 254 is broadcast activation; 255 is unassigned. In particular,
 * OMCI 0xffff must not alias assignable 4095 through a 12-bit mask. */
static inline bool gpon_data_alloc_valid(u16 alloc)
{
	return alloc <= GPON_GEM_US_ALLOC_MASK && alloc != 0x00fe && alloc != 0x00ff;
}

/* gpon_data_armed_is_stale - is the armed identity no longer ...
 * dev/MEASURED-gpon_data_plan.h.md sec 2. */
static inline bool gpon_data_armed_is_stale(const struct gpon_data_armed *armed,
					    const struct gpon_data_want *want)
{
	if (!armed || !want || !armed->alloc_bound)
		return false;
	if (!want->alloc_known || !gpon_data_alloc_valid(want->alloc) || !want->gem)
		return true;
	return armed->alloc != gpon_gem_us_alloc_id(want->alloc) ||
	       armed->gem   != gpon_gem_us_port_id(want->gem)    ||
	       /* clause (b): compares the ARMED alloc against the OMCC's
		* CURRENT one, so an OMCC that moved makes the data path stale
		* even though neither ME 262 nor ME 268 changed. */
	       (armed->rides_omcc && armed->alloc != want->omcc_alloc);
}

/* gpon_data_plan_decide - what should happen to the WAN data ...
 * dev/MEASURED-gpon_data_plan.h.md sec 3. */
static inline enum gpon_data_plan
gpon_data_plan_decide(const struct gpon_data_armed *armed,
		      const struct gpon_data_want *want)
{
	bool stale;
	if (!armed || !want)
		return GPON_DATA_WAIT;
	stale = gpon_data_armed_is_stale(armed, want);
	/* Explicit withdrawal still owes teardown while OMCC transport is down.
	 * Transport loss alone retains the wanted mapping and takes WAIT below. */
	if (!want->alloc_known || !gpon_data_alloc_valid(want->alloc) || !want->gem)
		return stale ? GPON_DATA_TEARDOWN : GPON_DATA_WAIT;
	if (!want->omcc_up)
		return GPON_DATA_WAIT;
	if (stale)
		return GPON_DATA_REPLACE;
	if (armed->installed)
		return GPON_DATA_KEEP;
	return GPON_DATA_INSTALL;
}

/* gpon_data_plan_why - what is the data path WAITING FOR? ...
 * dev/MEASURED-gpon_data_plan.h.md sec 4. */
static inline enum gpon_data_blocker
gpon_data_plan_why(const struct gpon_data_armed *armed,
		   const struct gpon_data_want *want)
{
	if (!armed || !want)
		return GPON_DATA_BLOCK_NO_INPUT;
	if (!want->alloc_known)
		return GPON_DATA_BLOCK_ALLOC_UNKNOWN;
	if (!gpon_data_alloc_valid(want->alloc))
		return GPON_DATA_BLOCK_ALLOC_INVALID;
	if (!want->gem)
		return GPON_DATA_BLOCK_NO_GEM;
	if (!want->omcc_up)
		return GPON_DATA_BLOCK_OMCC_DOWN;
	return GPON_DATA_BLOCK_NONE;
}

/* One-line name for a verdict, for logs and for test failure messages. */
static inline const char *gpon_data_plan_name(enum gpon_data_plan p)
{
	switch (p) {
	case GPON_DATA_WAIT:		return "WAIT";
	case GPON_DATA_KEEP:		return "KEEP";
	case GPON_DATA_INSTALL:		return "INSTALL";
	case GPON_DATA_REPLACE:		return "REPLACE";
	case GPON_DATA_TEARDOWN:	return "TEARDOWN";
	}
	return "?";
}

/* Which parts of a teardown apply to the identity that is ...
 * dev/MEASURED-gpon_data_plan.h.md sec 5. */
struct gpon_data_undo {
	bool	drain_tcont;	/* disable + flush the DATA T-CONT's VoQs first */
	bool	unbind_gem;	/* invalidate the unicast DS GEM CAM entry */
	bool	unbind_alloc;	/* invalidate the data T-CONT CAM entry */
};

/* gpon_data_undo_plan - which teardown steps apply to what is ...
 * dev/MEASURED-gpon_data_plan.h.md sec 6. */
static inline void gpon_data_undo_plan(const struct gpon_data_armed *armed,
				       u16 omcc_alloc,
				       struct gpon_data_undo *out)
{
	/* never disable the OMCC T-CONT's queues to tear a rider down */
	out->drain_tcont  = !armed->rides_omcc;
	out->unbind_gem   = armed->gem != 0;
	/* never invalidate the OMCC's own CAM entry */
	out->unbind_alloc = armed->alloc_bound && armed->alloc != omcc_alloc;
}

#endif /* GPON_DATA_PLAN_H */
