/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * TIER: CORE (prefix gpon_) — decides, never touches hardware, and compiles for
 * MIPS-BE, ARM64-LE and x86.  Canonical tier rule and guard: see "THE THREE
 * TIERS" in gpon_common.h (this directory).
 *
 * gpon_gem_us.h — the UPSTREAM half of GEM provisioning: the two mappings a
 * G.984 ONU must get right before it may transmit.
 *   1. Alloc-ID -> T-CONT: may an OLT-assigned Alloc-ID be bound to the data
 *      T-CONT, or is it already the OMCC's?  Both targets implemented this
 *      predicate independently and BOTH produced a real outage from it.
 *   2. GEM Port-ID -> the upstream index slots that stamp it, as a DECLARED
 *      range the shell supplies — never a computed one.
 * A decision layer, not a lifecycle one: it holds no state, so the same inputs
 * are the same answer on any target, at any time, in any context.
 *
 * ★ NO GEM HEADER BUILD, FRAGMENTATION OR HEC HERE, and nothing to hoist:
 *   measured 2026-08-05, the GEM Transmission Convergence layer is SILICON on
 *   both shipping targets.  The INDEPENDENT model of it lives in
 *   dev/rtl9607c-oracle/ (gem_tc.c, gem_us_encap.c) and MUST stay there -- a
 *   reference merged into the thing it checks can no longer fail.
 */
#ifndef GPON_GEM_US_H
#define GPON_GEM_US_H

#include <linux/types.h>

/* ★ THREE DIFFERENT 12-BIT-ISH THINGS ARE ALL CALLED "gem" IN ...
 * dev/MEASURED-gpon_gem_us.h.md sec 1. */
#define GPON_GEM_US_PORT_MASK	0x0fffu
#define GPON_GEM_US_ALLOC_MASK	0x0fffu

/* The value written to an upstream port-map slot that must ...
 * dev/MEASURED-gpon_gem_us.h.md sec 7. */
#define GPON_GEM_US_PORT_NONE	0u

/* The upstream index slots that carry ONE GEM Port-ID, ...
 * dev/MEASURED-gpon_gem_us.h.md sec 2. */
struct gpon_gem_us_range {
	u16	base;		/* first upstream slot that stamps the Port-ID */
	u16	count;		/* how many consecutive slots (>= 1)           */
	u16	index_max;	/* last slot the chip's port-map array has     */
};

/* Compile-time range check, usable in static_assert() with literal per-chip
 * constants: a bad declared range fails the BUILD and emits no code. */
#define GPON_GEM_US_RANGE_OK(base, count, index_max)			\
	((count) >= 1u && (u32)(base) + (u32)(count) - 1u <= (u32)(index_max))

/* Runtime form of the same predicate.  Reports only; it never repairs. */
bool gpon_gem_us_range_ok(const struct gpon_gem_us_range *r);

/* The n-th upstream slot of @r.  Meaningful only when gpon_gem_us_range_ok(). */
static inline u16 gpon_gem_us_index(const struct gpon_gem_us_range *r,
				    unsigned int n)
{
	return (u16)(r->base + n);
}

/* Mask a GEM Port-ID / an Alloc-ID to its 12 on-wire bits, in one place. */
static inline u16 gpon_gem_us_port_id(u32 port_id)
{
	return (u16)(port_id & GPON_GEM_US_PORT_MASK);
}
static inline u16 gpon_gem_us_alloc_id(u32 alloc)
{
	return (u16)(alloc & GPON_GEM_US_ALLOC_MASK);
}

/* The Alloc-ID -> T-CONT verdict.  Three outcomes, because "do not bind" is
 * more than one fact and collapsing them is how a shell ends up unable to say
 * WHY the data path never came up.  BIND_IS_OMCC means binding would MOVE the
 * OMCC off its own T-CONT. */
enum gpon_gem_us_bind {
	GPON_GEM_US_BIND_TCONT = 0,
	GPON_GEM_US_BIND_DONE,
	GPON_GEM_US_BIND_IS_OMCC,
};

/* ★★★ SINGLE-ALLOC OLTs: THE DATA RIDES THE OMCC T-CONT. ...
 * dev/MEASURED-gpon_gem_us.h.md sec 3. */
#define GPON_GEM_US_OMCI_RESERVED_SLOTS	2	/* the top two: OMCI + PLOAM */

/* Derive the sub-range of @omcc the data GEM may use.  -> false when the OMCC
 * run is too short to spare a slot, and the caller must then install NO data
 * path rather than steal an OMCI slot. */
bool gpon_gem_us_ride_range(const struct gpon_gem_us_range *omcc,
			    struct gpon_gem_us_range *out);

/* Decide whether @alloc may be bound to the data T-CONT.  @omcc_alloc and
 * @already_bound are the CALLER's own values, passed through unchanged because
 * the two targets genuinely pass different ones and a code-motion refactor may
 * not change either.  Pure: safe from any context including softirq. */
enum gpon_gem_us_bind gpon_gem_us_tcont_decide(u16 alloc, u16 omcc_alloc,
					       bool already_bound);

/* One-line name for a verdict, for logs and for test failure messages. */
const char *gpon_gem_us_bind_name(enum gpon_gem_us_bind v);

/* ★★★ ONE PROTOCOL RULE THAT WAS WRITTEN THREE TIMES — the ...
 * dev/MEASURED-gpon_gem_us.h.md sec 4. */
enum gpon_omcc_action {
	GPON_OMCC_IGNORE,	/* enable=0: a Configure_Port-ID transient. Write
				 * NOTHING and keep the link; the following
				 * enable=1 re-latch carries the final id */
	GPON_OMCC_INSTALL,	/* not bound yet -> bind @want_gem */
	GPON_OMCC_REBIND,	/* bound to a DIFFERENT gem -> move the TRANSPORT
				 * only.  The responder session is NOT re-armed:
				 * re-arming resets the MIB under a live OLT */
	GPON_OMCC_UNCHANGED,	/* the same gem re-sent (PLOAMs come 3x): write
				 * nothing, so the proven LOS/fiber-pull keep
				 * path stays byte-identical */
};

/* gpon_omcc_decide - what a Configure_Port-ID means for the ...
 * dev/MEASURED-gpon_gem_us.h.md sec 5. */
enum gpon_omcc_action gpon_omcc_decide(bool port_en, u16 want_gem,
				       bool installed, u16 cur_gem);

/* One-line name for a verdict, for logs and for test failure messages. */
const char *gpon_omcc_action_name(enum gpon_omcc_action a);

/* ★★★ THE SECOND OMCC RULE THAT WAS WRITTEN TWICE: ... -- dev/MEASURED-gpon_gem_us.h.md sec 8. */
struct gpon_omcc_tcont_plan {
	u16  alloc;		/* Alloc-ID to bind the OMCC's own T-CONT to  */
	bool bind_alt;		/* ALSO bind @alloc to the ALTERNATE T-CONT   */
};

/* gpon_omcc_tcont_decide - which Alloc-ID the OMCC's ... -- dev/MEASURED-gpon_gem_us.h.md sec 6. */
void gpon_omcc_tcont_decide(u16 alloc_override, u8 onu_id, bool alt_bind,
			  struct gpon_omcc_tcont_plan *out);

#endif /* GPON_GEM_US_H */
