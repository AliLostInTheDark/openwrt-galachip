/* SPDX-License-Identifier: GPL-2.0-or-later */
/* TIER: CORE, and in the STRICT host-buildable subset -- a ...
 * dev/MEASURED-gpon_ploam_diag.h.md sec 1. */
#ifndef GPON_PLOAM_DIAG_H
#define GPON_PLOAM_DIAG_H

#include <linux/types.h>
#include "gpon_ploam.h"		/* enum gpon_ploam_ev: the WHEN the core already owns */

/* The WHEN, as data.  These are the activation transitions at which the
 * counters below decide something; every other event is GPON_PDIAG_NONE. */
enum gpon_ploam_diag_point {
	GPON_PDIAG_ASSIGN,		/* Assign_ONU-ID accepted -> O4 (one per copy the OLT sends) */
	GPON_PDIAG_RANGING_TIME,	/* Ranging_Time accepted -> O5: the ranging grant WAS answered */
	/* ★★★ THE EARLY LADDER: a dwell at O1, O2 or O3 (2026-09-08). ...
	 * dev/MEASURED-gpon_ploam_diag.h.md sec 7. */
	GPON_PDIAG_EARLY,
	GPON_PDIAG_DEACT,		/* Deactivate_ONU-ID: the window closed without ranging     */
	GPON_PDIAG_NONE,		/* not a sample point                                        */
};

#define GPON_PDIAG_HAS_PLOAM_ACPT	0x01u
#define GPON_PDIAG_HAS_BWM_ACPT		0x02u
#define GPON_PDIAG_HAS_BWM_FAIL		0x04u
#define GPON_PDIAG_HAS_BWM_INV		0x08u
#define GPON_PDIAG_HAS_US_ONU_ID	0x10u
#define GPON_PDIAG_HAS_DS_ONU_ID	0x20u
#define GPON_PDIAG_HAS_US_PLOAM_TX	0x40u
#define GPON_PDIAG_HAS_US_SN_TX		0x80u
#define GPON_PDIAG_HAS_POLL_GAP		0x100u
#define GPON_PDIAG_HAS_RX_BURST		0x200u
#define GPON_PDIAG_HAS_SN_REQ		0x400u
#define GPON_PDIAG_HAS_RNG_REQ		0x800u
#define GPON_PDIAG_HAS_ALL		0xfffu

/* ★ THE READER'S LATENESS IS PART OF THE READING ... -- dev/MEASURED-gpon_ploam_diag.h.md sec 2. */
struct gpon_ploam_diag {
	u32 valid;		/* GPON_PDIAG_HAS_*: a clear bit is COULD NOT ASK       */
	u32 ploam_acpt;		/* DS PLOAMs that passed the ONU-ID filter               */
	u32 bwm_acpt;		/* BWmap grants accepted for our Alloc-ID / ONU-ID       */
	u32 bwm_fail;		/* BWmap grants rejected (CRC / format)                  */
	u32 bwm_inv;		/* BWmap grants invalid                                  */
	u32 us_ploam_tx;	/* upstream PLOAMs the MAC actually transmitted, every kind (CPU normal+urgent, auto No_message+SN): the "did we ANSWER the grant" half */
	u32 us_sn_tx;		/* of those, the auto-fired Serial_Number_ONU -- the one a ranging grant is answered with */
	/* ★ THE TWO THAT DECIDE THE O4 FORK (2026-09-07). The Luna ...
	 * dev/MEASURED-gpon_ploam_diag.h.md sec 3. */
	u32 sn_req;		/* Serial_Number requests the GTC received since the previous read */
	u32 rng_req;		/* ranging requests the GTC received since the previous read */
	u32 poll_gap_ms;	/* ms between the start of the poll handling this message and the start of the previous poll */
	u8  rx_burst_idx;	/* DS PLOAMs this same poll dequeued BEFORE this one (0 = the first) */
	u8  us_onu_id;		/* the ONU-ID the upstream burst is stamped with (readback) */
	u8  ds_onu_id;		/* the ONU-ID the downstream filter holds (readback)     */
};

/* ★ THE ACCEPTED-GRANT CAPTURE, accumulated between two ...
 * dev/MEASURED-gpon_ploam_diag.h.md sec 4. */
#define GPON_BWCAP_HAS			0x01u

struct gpon_bwcap_diag {
	u32 valid;		/* GPON_BWCAP_HAS, or 0 = COULD NOT ASK                   */
	u32 harvests;		/* how many polls emptied the capture into this          */
	u32 nonempty;		/* of those, how many found at least one VALID entry     */
	u32 entries;		/* VALID allocations accepted, all T-CONTs               */
	u32 ploamu;		/* of those, carrying a PLOAMu slot                      */
	u32 omcc_ploamu;	/* of those, resolving to the OMCC T-CONT (the ranging grant's home) */
	u32 tconts;		/* bitmap of the T-CONT indexes the accepted grants resolved to */
	u32 overfl;		/* harvests that found the capture's overflow flag set   */
	u32 last_raw0;		/* the last OMCC-PLOAMu entry, raw, family layout         */
	u32 last_raw1;
};

/* One line, fields in a fixed order: bwcap-diag <point> ...
 * dev/MEASURED-gpon_ploam_diag.h.md sec 5. */
int gpon_bwcap_diag_format(char *out, size_t sz, enum gpon_ploam_diag_point p,
			   u32 t_ms, u8 ostate, const struct gpon_bwcap_diag *b);

/* Which sample point a core PLOAM event is, or GPON_PDIAG_NONE.  A shell
 * that dispatches through the core FSM asks this from its `trace` op; one
 * that still runs its own FSM names the point at its own handler. */
enum gpon_ploam_diag_point gpon_ploam_diag_point_of(enum gpon_ploam_ev ev);

/* "assign" / "ranging_time" / "deact"; "?" for anything else, never NULL. */
const char *gpon_ploam_diag_point_name(enum gpon_ploam_diag_point p);

/* One line, fields in a fixed order so a reader can grep it: ...
 * dev/MEASURED-gpon_ploam_diag.h.md sec 6. */
int gpon_ploam_diag_format(char *out, size_t sz, enum gpon_ploam_diag_point p,
			   u32 t_ms, u8 ostate, const struct gpon_ploam_diag *d);
/* The core's per-type PLOAM counters (struct gpon_ploam ds_own/ds_bcast/
 * ds_other/us_queued) as one line, no printk: what a quiet boot can be asked. */
int gpon_ploam_types_format(char *out, size_t sz, const struct gpon_ploam *o);

#endif /* GPON_PLOAM_DIAG_H */
