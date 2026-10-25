/* SPDX-License-Identifier: GPL-2.0-or-later */
/* TIER: CORE, strict host-buildable subset.  CONFIG_GPON_FLOW_DIAG, the fourth
 * member of the CONFIG_GPON_<SUBSYSTEM>_DIAG family -- why a flow was NOT
 * accelerated.  Evidence: dev/MEASURED-gpon_flow_diag.h.md sec 1. */
#ifndef GPON_FLOW_DIAG_H
#define GPON_FLOW_DIAG_H

#include <linux/types.h>

/* WHY a TC replace did not end up in the engine.  A CLOSED set: every refusal
 * point in gpon_flow.c and gpon_flow_offload.c names exactly one of these, and
 * the three errnos they collapse into cannot tell them apart --
 * dev/MEASURED-gpon_flow_diag.h.md sec 2. */
enum gpon_flow_refusal {
	GPON_FLOW_OK = 0,		/* installed                            */
	GPON_FLOW_REF_NO_ENGINE,	/* no lifecycle / table not ready       */
	GPON_FLOW_REF_DUP_COOKIE,	/* the other leg already holds it       */
	GPON_FLOW_REF_BAD_ARGS,		/* the caller handed us nothing to read */
	GPON_FLOW_REF_NOT_IPV4,		/* the 5-tuple is not IPv4              */
	GPON_FLOW_REF_NO_BASIC,		/* no BASIC dissector: no L4 proto      */
	GPON_FLOW_REF_NOT_TCP_UDP,	/* L4 proto the engine cannot key on    */
	GPON_FLOW_REF_NO_PORTS,		/* no PORTS dissector                   */
	GPON_FLOW_REF_NAT_OFFSET,	/* the IP mangle is not this leg's      */
	GPON_FLOW_REF_PORT_SHAPE,	/* the L4 port mangle is not this leg's */
	GPON_FLOW_REF_MANGLE_HTYPE,	/* a header class we do not rewrite     */
	GPON_FLOW_REF_VLAN_ACTION,	/* a tag push/pop: no engine here can   */
	GPON_FLOW_REF_ACTION_ID,	/* an action verb we do not implement   */
	GPON_FLOW_REF_INCOMPLETE,	/* redirect / NAT / port: one missing   */
	GPON_FLOW_REF_NOMEM,
	GPON_FLOW_REF_ENGINE,		/* the family's install() FAILED        */
	GPON_FLOW_REF_BY_DESIGN,	/* the family DECLINED: this leg needs
					 * no flow of its own. NOT a refusal,
					 * and counting it as one made the
					 * ledger report half of every offer as
					 * an engine failure -- measured on the
					 * X111W: 14504 of 28988.             */
	GPON_FLOW_REF_TABLE_INSERT,	/* the cookie map refused the entry     */
	GPON_FLOW_REF__COUNT
};

/* `install()` says the leg needs no flow of its own.  POSITIVE on purpose: 0 is
 * installed and every negative is an errno, so this cannot collide -- and it
 * lives here, beside the function that READS it, because the two are ONE
 * convention and splitting them is how a sign rule drifts. */
#define GPON_FLOW_DECLINED	1

/* Which cause an install() return carries.  PURE, and it is the ONE place that
 * decision lives: `install()` returns 0 installed, a POSITIVE value for a
 * by-design decline, and a negative errno for a real refusal.  ⚠ THE ERRNO
 * CANNOT CARRY IT -- Luna answers -EOPNOTSUPP both for the reply leg and for
 * every genuine refusal, so a core that read the errno would have to guess. */
static inline enum gpon_flow_refusal gpon_flow_install_cause(int err)
{
	return err > 0 ? GPON_FLOW_REF_BY_DESIGN : GPON_FLOW_REF_ENGINE;
}

/* Never NULL, "?" for an out-of-enum value. */
const char *gpon_flow_refusal_name(enum gpon_flow_refusal r);

/* Record one outcome.  NULL-tolerant so a build with the flag off passes NULL
 * and the compiler drops the call. */
struct gpon_flow_tally {
	u32 n[GPON_FLOW_REF__COUNT];
	enum gpon_flow_refusal last;
	int last_iif;		/* the last REFUSED offer's ingress ifindex (0: none) */
	u8 last_ds_leg;		/* ...and the leg the core decided for it */
};

void gpon_flow_tally_note(struct gpon_flow_tally *t, enum gpon_flow_refusal r);
void gpon_flow_tally_leg(struct gpon_flow_tally *t, int iif, bool ds_leg);

/* What only the FAMILY can read: its own offer count and its engine's state.
 * A clear bit is COULD NOT ASK and renders n/a -- never 0, because "0 entries
 * live" is a device finding and "we never asked" is not --
 * dev/MEASURED-gpon_flow_diag.h.md sec 3. */
#define GPON_FDIAG_HAS_OFFERED		0x01u
#define GPON_FDIAG_HAS_LIVE		0x02u
#define GPON_FDIAG_HAS_CAPACITY		0x04u	/* a per-CHIP table fact */
#define GPON_FDIAG_HAS_HITS		0x08u
#define GPON_FDIAG_HAS_ALL		0x0fu

struct gpon_flow_diag {
	u32 valid;
	u32 offered;	/* TC replace requests that reached the family   */
	u32 live;	/* entries the engine holds right now            */
	u32 capacity;	/* the chip's table size, from its own table     */
	u32 hw_hits;	/* the hit witness, in the family's own units    */
};

/* ONE line, fields in a fixed order so a reader can grep it.  Only the causes
 * that FIRED are listed, so a healthy board prints a short line and this cannot
 * flood -- dev/MEASURED-gpon_flow_diag.h.md sec 4. */
int gpon_flow_diag_line(const struct gpon_flow_tally *t,
			const struct gpon_flow_diag *d, char *out, size_t sz);

/* The gate itself, so the lifecycle carries no #if of its own and the host
 * fixtures that extract its bodies see the same two spellings.  With the flag
 * off the struct loses the member and every note compiles away. */
#if IS_ENABLED(CONFIG_GPON_FLOW_DIAG)
#define GPON_FLOW_TALLY_FIELD	struct gpon_flow_tally tally;
#define GPON_FLOW_NOTE(fo, r)	gpon_flow_tally_note(&(fo)->tally, (r))
#define GPON_FLOW_NOTE_LEG(fo, iif, ds)	gpon_flow_tally_leg(&(fo)->tally, (iif), (ds))
#else
#define GPON_FLOW_TALLY_FIELD
#define GPON_FLOW_NOTE(fo, r)	do { } while (0)
#define GPON_FLOW_NOTE_LEG(fo, iif, ds)	do { } while (0)
#endif

#endif /* GPON_FLOW_DIAG_H */
