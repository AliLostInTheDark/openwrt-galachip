/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * regtable.h -- the OTHER half of a one-hour port: offsets as DATA.
 *
 * ★★★ hwio.h answered "how do I reach a register". This answers "which
 * register", and the two together are what make the operator's target real
 * (2026-08-28: *"todo lo que puede permitir reducir el port time, tener mas
 * abstraction, codigo mas limpio mejor, y de paso le hace mas facil de hacer
 * fuzzing y test"*).
 *
 *     logic(hwio, regs)   <- one implementation, every chip
 *     regs                <- one C initialiser per chip. THE PORT.
 *     hwio                <- how the shell reaches that block on that board.
 *
 * ★ WHY A STRUCT AND NOT #defines, when this tree just spent effort splitting
 * 180 #defines into a per-chip header. Because a header is chosen at COMPILE
 * time: it gives one chip per object, so shared logic still has to be compiled
 * twice and can still diverge between the two builds. A struct is chosen at
 * RUN time by pointer, so ONE compiled function serves every chip -- and a
 * second chip becomes an initialiser a human can read and diff against the
 * first, which is what "a list of registers per board" actually means.
 *   Both forms are legitimate and this tree uses both: the compile-time header
 *   for a value in a hot path, the table for anything a shared function reads.
 *
 * ★ AND IT IS WHAT MAKES THE LOGIC FUZZABLE. A function taking (hwio, regs)
 * has no silicon in it: on x86 the test supplies an hwio backed by an array
 * and any table it likes, then asserts on what the function WROTE and WHERE.
 * That is thousands of cases a second against roughly one 200-second boot --
 * and it is why "more abstraction" and "easier testing" are not two goals.
 *
 * ★★ A ZERO IS NOT AN ADDRESS -- AND IT IS NOT THE ABSENCE MARKER EITHER.
 * Every table declares which fields it does not have; a chip that lacks a
 * block sets the field to REG_ABSENT and the logic must ASK rather than
 * write to offset 0 of something. reg_has() is that question, spelled once.
 * The sentinel is deliberately NOT 0, because "the register at 0" is a real
 * address on these parts (GPON_INT_DLT is 0x0000) and a silent write there is
 * exactly the class of bug that reads back fine.
 * (Until 2026-09-02 this paragraph said an absent field "leaves it 0" --
 * following that would have marked absence as an offset reg_has() calls
 * PRESENT, and the logic would have written to offset 0.)
 *
 * ★★★ AND THE SENTENCE ABOVE WAS ONLY HALF THE MECHANISM UNTIL 2026-09-14:
 * NOTHING FORCED AN AUTHOR TO SET A FIELD AT ALL (operator: *"debe existir
 * mecanismo que donde hay que registrar algo como valor NO deja validar si
 * hay valor por defecto a la compilacion"*).  C zero-fills every member a
 * designated initialiser omits, and 0 is a legal offset on these parts, so a
 * FORGOTTEN field silently became "the register at offset 0" -- which
 * reg_has() reported as PRESENT and the logic happily wrote to.  The
 * absence marker was declared and the DEFAULT was not, which is the same
 * defect one layer up.
 *
 *   the measured instance, one layer down: the /proc GPON dump printed
 *   pi_rd(0x20ac) under the label `ctl_us`.  PI_PONIP_CTL_US is 0x020d8 here
 *   and 0x020ec on the RTL9603CVD; 0x20ac is CNT_MASK_US group 0's word for
 *   SIDs 32..63, enrolled on NEITHER board, so it reads 0 BY DESIGN -- and a
 *   comment then read that structural zero as a fact about stock.  A zero
 *   that is guaranteed looks exactly like a zero that was measured.
 *
 * ⇒ A REGISTER OFFSET IS NO LONGER A u32.  It is `struct reg`, holding the
 * offset BIASED BY ONE, so the value C leaves behind decodes to UNSET and
 * cannot be mistaken for offset 0, for any other offset, or for a declared
 * absence.  THREE answers, never two:
 *
 *     UNSET    nobody registered it. A DEFECT: reg_rc() answers -ENXIO and
 *              every helper refuses BEFORE any bus traffic.
 *     ABSENT   a human declared "this chip does not have it": -ENODEV.
 *     PRESENT  reg_at() yields the offset.
 *
 * It is a distinct TYPE on purpose: a raw offset passed where an encoded one
 * belongs is a compile ERROR, not an off-by-one nobody sees.  Crossing that
 * boundary deliberately is spelled reg_make(), once, at the call site.
 *
 * ⚠ THE OTHER ZERO CONVENTION IN THIS TREE IS NOT CHANGED AND IS NOT AN
 * EXCEPTION.  The luna_sw_map tables spell 0 as "this chip's bring-up does
 * not touch it" -- a per-field DECLARED answer, documented on each field.
 * What made the two conventions collide was never the meaning of 0; it was
 * that nothing made a human TYPE it.  reg_table_registered_guard.py requires
 * every field of every registered table to be initialised explicitly, so a 0
 * there can only exist because somebody wrote it down.
 */
#ifndef _REGTABLE_H
#define _REGTABLE_H

#include <linux/errno.h>
#include <linux/types.h>

#include "hwio.h"	/* the injected accessor the logic below writes through */

/**
 * struct reg - ONE register offset, encoded so that C's zero-fill is not a
 * valid value.  @enc is the offset PLUS ONE; 0 is what an omitted member
 * leaves and means UNSET.
 *
 * Same size as the u32 it replaces (asserted below), so a table costs no
 * more flash than it did -- the footprint rule binds this tier too.
 */
struct reg {
	u32	enc;
};

/** The encoding, spelled once so a static assert can pin it. */
#define REG_ENC_OF(off)	((u32)(off) + 1u)
#define REG_ENC_UNSET	0u		/* exactly what C zero-fill leaves */
#define REG_ENC_ABSENT	0xffffffffu	/* a human DECLARED the absence    */

/* The offset reg_at() answers for a field that is not PRESENT.  NOT 0, which
 * is a real register here; it shares REG_ENC_ABSENT's value only because both
 * want an address no block on these parts can answer. */
#define REG_BAD_OFF	0xffffffffu

/**
 * REG_AT() / REG_ABSENT - the two INITIALISER forms. Brace forms, so
 * `.field = REG_AT(0x1204),` keeps the literal that a human, regtable_vs_sdk.py
 * and gpon_regtable_diff_test all read; only the WRAPPER is new.
 */
#define REG_AT(off)	{ REG_ENC_OF(off) }
#define REG_ABSENT	{ REG_ENC_ABSENT }

/* The bias is the whole mechanism, so it is pinned AT COMPILE TIME: remove it
 * and this header stops building, naming the property it lost. */
_Static_assert(REG_ENC_UNSET == 0u,
	       "UNSET must be exactly what C's zero-fill leaves behind");
_Static_assert(REG_ENC_OF(0) != REG_ENC_UNSET,
	       "offset 0 is a real register here (GPON_INT_DLT): registering it "
	       "may never be indistinguishable from registering nothing");
_Static_assert(REG_ENC_OF(0) != REG_ENC_ABSENT,
	       "a registered offset may never encode as a declared absence");
_Static_assert(REG_ENC_ABSENT != REG_ENC_UNSET,
	       "a DECLARED absence and a field nobody registered are different "
	       "answers and must stay distinguishable");
_Static_assert(sizeof(struct reg) == sizeof(u32),
	       "a register table may not grow: the ONU flash budget is a rule");

/** What state is this field in? THREE answers, never two. */
enum reg_state {
	REG_STATE_UNSET = 0,	/* nobody registered it -- a DEFECT */
	REG_STATE_ABSENT,	/* a human declared this chip lacks it */
	REG_STATE_PRESENT,	/* an offset to use */
};

static inline enum reg_state reg_state(struct reg r)
{
	if (r.enc == REG_ENC_UNSET)
		return REG_STATE_UNSET;
	if (r.enc == REG_ENC_ABSENT)
		return REG_STATE_ABSENT;
	return REG_STATE_PRESENT;
}

/** Is @r a register this chip actually has? */
static inline bool reg_has(struct reg r)
{
	return reg_state(r) == REG_STATE_PRESENT;
}

/**
 * reg_rc() - the NAMED refusal a helper owes before any bus traffic.
 *
 * Return: 0 when usable; -ENXIO when NOBODY REGISTERED IT (our defect, and a
 * different repair from every other rc in this header); -ENODEV when the
 * chip's own table declares the block absent.
 */
static inline int reg_rc(struct reg r)
{
	switch (reg_state(r)) {
	case REG_STATE_PRESENT:
		return 0;
	case REG_STATE_ABSENT:
		return -ENODEV;
	default:
		return -ENXIO;
	}
}

/**
 * reg_at() - the offset to write.  REG_BAD_OFF when @r is not PRESENT, so a
 * caller that skipped the ask addresses 0xffffffff and not offset 0 -- the
 * failure lands somewhere a human notices instead of in GPON_INT_DLT.
 */
static inline u32 reg_at(struct reg r)
{
	return reg_has(r) ? r.enc - 1u : REG_BAD_OFF;
}

/** Cross from a raw offset (a #define, which cannot be silently defaulted). */
static inline struct reg reg_make(u32 off)
{
	struct reg r = { REG_ENC_OF(off) };

	return r;
}

/** The value form of REG_ABSENT, for an ARGUMENT rather than an initialiser. */
static inline struct reg reg_none(void)
{
	struct reg r = { REG_ENC_ABSENT };

	return r;
}

/**
 * gpon_ind_poll() - bounded wait for an indirect transaction's busy bit.
 * @off:   the register carrying the busy/GO bit, WITHIN the block.
 * @busy:  the bit that is SET while the hardware is working.
 * @tries: the caller's bound. A timeout is not success, so there is always one.
 * @pause: the shell's per-iteration pause. The core has no clock and no way to
 *         yield a CPU, so the wait's cost is the SHELL's to define -- the same
 *         reason gpon_gtc_cam_xact() takes @delay_us.
 *
 * Every indirect table in these chips is reached the same way: raise a request,
 * then watch one bit until the hardware drops it. That idea had SIX
 * implementations across the tree and they had drifted in the ways copies do --
 * one spun a core with no pause at all, three spelled one bound under three
 * names, and only one of the three recorded where the number came from.
 *
 * ★ WHY THIS IS CORE AND NOT FAMILY, because I got that wrong once and wrote
 * the wrong reason into a header (2026-09-04): a bounded busy-wait looks like
 * it is "about time", which the core forbids. It is not -- the WAITING is the
 * shell's, injected as @pause, and what is left is pure decision: read a bit,
 * count, refuse. That is precisely the split gpon_gtc_cam_xact() already used
 * on this exact problem, one file away, and the tree had the answer before I
 * claimed there wasn't one.
 *
 * Return: the iteration COMPL was seen at (>= 0), -ETIMEDOUT when the bound
 * expires, -ENODEV when this chip's table DECLARES no such register and
 * -ENXIO when NOBODY REGISTERED IT (both refused BEFORE any bus traffic),
 * -EINVAL with no pause op.
 */
static inline int gpon_ind_poll(const struct hwio *io, struct reg off, u32 busy,
				unsigned int tries, void (*pause)(void))
{
	unsigned int i;
	int rc;

	if (!pause)
		return -EINVAL;
	rc = reg_rc(off);
	if (rc)
		return rc;
	for (i = 0; i < tries; i++) {
		if (!(hwio_rd(io, reg_at(off)) & busy))
			return (int)i;
		pause();
	}
	return -ETIMEDOUT;
}

/**
 * gpon_ind_go() - write a request word, then wait for its busy bit to clear.
 *
 * The write-then-wait spelling of gpon_ind_poll(). Refuses an absent register
 * BEFORE the write, so a chip whose table lacks the block never emits a
 * transaction into empty space.
 */
static inline int gpon_ind_go(const struct hwio *io, struct reg off, u32 val,
			      u32 busy, unsigned int tries,
			      void (*pause)(void))
{
	int rc;

	if (!pause)
		return -EINVAL;
	rc = reg_rc(off);
	if (rc)
		return rc;
	hwio_wr(io, reg_at(off), val);
	return gpon_ind_poll(io, off, busy, tries, pause);
}

/**
 * struct gpon_gtc_regs - the GTC/OMCI offsets a shared GPON function needs.
 *
 * Offsets are WITHIN the GTC block, never absolute: the base belongs to the
 * shell's hwio context. Names follow the silicon's own names, per this tree's
 * rule that a misleading name is a defect.
 *
 * ★ ADDING A CHIP IS ADDING ONE INITIALISER BELOW. That is the deliverable.
 */
struct gpon_gtc_regs {
	struct reg	ds_omci_pti;	/* [6:4] PTI_MASK, [2:0] END_PTI      */
	struct reg	gem_us_port_map;/* array base; 32-bit words, stride 4 */
	u32	gem_us_port_stride;	/* the stride itself, in BYTES        */
	struct reg	gem_ds_mc_cfg;	/* broadcast / non-multicast / FCS    */
	struct reg	ds_traffic_cfg;	/* per-flow DS traffic config array   */
	u32	ds_traffic_stride;
	/* The two indirect GTC CAMs (gpon_gtc_cam_write() below).  Each is an
	 * op/index register plus a write-data register; the INDEX MASK is that
	 * CAM's own depth -- a per-CAM FACT (the GEM Port-ID CAM has 128 flows,
	 * OP_IDX[6:0]; the alloc CAM 32 T-CONTs, OP_IDX[4:0]) -- so it rides
	 * HERE with the registers, never as a magic number at a call site. */
	struct reg ds_port_ind;		/* GEM Port-ID CAM op: OP_MODE[9:8] OP_IDX[6:0] */
	struct reg ds_port_wr;		/* GEM Port-ID CAM write data: [11:0] gemPortId */
	struct reg ds_port_rd;		/* GEM Port-ID CAM read data: [11:0] RDATA      */
	u32	ds_port_idx_mask;	/* OP_IDX width as a mask (0x7f = 128 flows)    */
	struct reg ds_alloc_ind;	/* T-CONT alloc CAM op: OP_IDX[4:0]             */
	struct reg ds_alloc_wr;		/* alloc CAM write data: [11:0] allocateId      */
	struct reg ds_alloc_rd;		/* alloc CAM read data: [11:0] RDATA            */
	u32	ds_alloc_idx_mask;	/* OP_IDX width as a mask (0x1f = 32 T-CONTs)   */
	/* The three flow-indexed indirect COUNTER muxes (gpon_gtc_cntr_read()
	 * below).  Each is an index register plus a CLEAR-ON-READ status
	 * register; the shared handshake facts (the read-ack bit, the RSEL bit,
	 * the poll budget) live with the transaction, and the index width --
	 * IDX[6:0] on all three registers on both vendor chipdefs, the same
	 * 128-flow space as the GEM Port-ID CAM -- rides here as
	 * cntr_flow_idx_mask.  The MISC counter mux is deliberately NOT here:
	 * it has no ack (see the counter-transaction comment below) and its
	 * index width is the one counter field that DIFFERS between the two
	 * chips ([3:0] vs [2:0]). */
	struct reg gem_ds_rx_cntr_ind;	/* per-flow DS GEM ETH RX: IDX[6:0], ack[15]    */
	struct reg gem_ds_rx_cntr_stat;	/* ETH_PKT_RX[31:0], clear-on-read              */
	struct reg gem_ds_fwd_cntr_ind;	/* per-flow DS GEM forwarded-to-PON-IP          */
	struct reg gem_ds_fwd_cntr_stat;/* ETH_PKT_FWD[31:0], clear-on-read             */
	struct reg ds_port_cntr_ind;	/* per-flow DS de-encap: IDX[6:0] RSEL[8] ack[15] */
	struct reg ds_port_cntr_stat;	/* GEM_CNTR[31:0], clear-on-read                */
	u32	cntr_flow_idx_mask;	/* counter IDX width as a mask (0x7f = 128 flows) */
};

/**
 * struct gpon_chip - everything a shared GPON function needs to know about
 * one part. A port adds one of these.
 * @name: for messages only; never compared against.
 * @gtc:  the GTC/OMCI offsets.
 */
struct gpon_chip {
	const char		*name;
	struct gpon_gtc_regs	gtc;
};

/**
 * gpon_gtc_us_gem_stamp() - stamp a GEM Port-ID into one US port-map slot.
 * @io:      how to reach the GTC block (the shell's hwio over that block).
 * @r:       this chip's GTC offsets.
 * @flow:    the internal upstream flow/SID index (a slot of the port-map
 *           array; the caller owns the range check -- Luna pins its two flows
 *           with static_assert(GPON_GEM_US_RANGE_OK(...)) at compile time).
 * @port_id: the GEM Port-ID, ALREADY masked to its 12 on-wire bits --
 *           gpon_gem_us_port_id() (gpon_gem_us.h) is the one spelling of that
 *           mask, and this function writes exactly what it is handed.
 *
 * The FIRST logic(hwio, regs) function of this table -- the write that the
 * GEM_US_PORT_MAP stride regression (4 -> 0x20) once sent into a statistics
 * counter, leaving the real slot unmapped and the T-CONT silent ("Laser out").
 * The offset arithmetic now lives HERE, once, fed by per-chip DATA, and is
 * proven by a write-stream differential on x86
 * (dev/rtl9607c-test/gpon_regtable_diff_test) instead of by a boot.
 *
 * Return: true when the slot was written; false when this chip's table
 * declares no US port map (the reg_has() ask this header requires -- the
 * caller logs, because a chip without the array must never reach here).
 */
static inline bool gpon_gtc_us_gem_stamp(const struct hwio *io,
					 const struct gpon_gtc_regs *r,
					 u32 flow, u16 port_id)
{
	if (!reg_has(r->gem_us_port_map))
		return false;
	hwio_wr(io, reg_at(r->gem_us_port_map) + flow * r->gem_us_port_stride,
		port_id);
	return true;
}

/*
 * The indirect-CAM op words, shared by BOTH GTC CAMs and, per the vendor
 * chipdef, identical on the RTL9602C and the RTL9603CVD (PORTID_OP_* /
 * ALLOCID_OP_* fields of GPON_GTC_DS_PORT_IND 0x701100 / DS_ALLOC_IND
 * 0x7010C0: OP_MODE lsp 8 len 2, OP_REQ lsp 15, OP_COMPL lsp 14, OP_HIT
 * lsp 13; the WR registers carry WDATA lsp 0 len 12 and the RD registers
 * RDATA lsp 0 len 12).  Only the OP_IDX width differs between the two CAMs,
 * which is why THAT is table data and these are not.
 *
 * The OP_MODE value names are established twice over, independently: the
 * vendor's own op-mode constants say WRITE=1 / READ=2 / CLEAN=3 (the values
 * its DAL drives into PORTID_OP_MODEf / ALLOCID_OP_MODEf), and the
 * pre-conversion call sites in luna_gpon.c spelled the same three beside
 * their raw 1u<<8 / 2u<<8 / 3u<<8 ("OP_MODE=WRITE", "OP_MODE=READ",
 * "OP_MODE=CLEAN").  CLEAN invalidates ONE idx-addressed entry, like READ;
 * a "clear all" is a LOOP and stays at the call site.
 */
#define GPON_GTC_CAM_OP_WRITE	(1u << 8)	/* OP_MODE[9:8] = WRITE(1) */
#define GPON_GTC_CAM_OP_READ	(2u << 8)	/* OP_MODE[9:8] = READ(2)  */
#define GPON_GTC_CAM_OP_CLEAN	(3u << 8)	/* OP_MODE[9:8] = CLEAN(3) */
#define GPON_GTC_CAM_OP_REQ	(1u << 15)	/* trigger                 */
#define GPON_GTC_CAM_OP_COMPL	(1u << 14)	/* transaction complete    */
#define GPON_GTC_CAM_OP_HIT	(1u << 13)	/* READ matched a valid entry */
#define GPON_GTC_CAM_VAL_MASK	0xfffu		/* WDATA/RDATA[11:0]       */
/* The poll budget of the pre-conversion call sites, verbatim: up to 1000
 * reads at 1 us apart.  All four converted sites used exactly this bound, so
 * it lives with the transaction; a chip that needs another budget makes it a
 * parameter THEN, not speculatively. */
#define GPON_GTC_CAM_TRIES	1000u

/**
 * gpon_gtc_cam_xact() - one indirect GTC CAM transaction, any op mode.
 * @io:       how to reach the GTC block.
 * @ind:      the CAM's op/index register (offset within the block).
 * @op_mode:  GPON_GTC_CAM_OP_WRITE / _READ / _CLEAN.  Anything else is
 *            refused (-EINVAL): OP_MODE is a 2-bit field and 0 is the
 *            vendor's "no operation" -- a transaction nobody means.
 * @idx_mask: the CAM's OP_IDX width as a mask -- a fact of THAT CAM's depth.
 * @idx:      the entry to address (flow / T-CONT).
 * @wr:       the CAM's write-data register.  ONLY the WRITE op has a data
 *            phase (WDATA is consumed at REQ); READ and CLEAN callers pass
 *            reg_none() and the register is never touched.  A READ's data
 *            comes back through the RD register AFTER completion --
 *            gpon_gtc_cam_read() owns that half.
 * @val:      the 12-bit WRITE value; masked to WDATA[11:0] here, the same
 *            "& 0xfff" every pre-conversion site spelled.  Ignored for
 *            READ / CLEAN.
 * @delay_us: the shell's sleep -- time is an EXPLICIT INPUT in this tier
 *            (the gpon_regseq_io precedent), so the same function runs on
 *            x86 under the write-stream differential with a counting fake.
 *
 * The sequence luna_gpon.c spelled by hand at SEVEN sites (four WRITE, two
 * READ, one CLEAN loop) until 2026-09-03 -- and had let diverge repeatedly,
 * always in the same direction: the multicast WRITE shipped with NO
 * completion check (a timed-out install fell through to "datapath installed"
 * while the GTC dropped the broadcast DS, i.e. the DHCP OFFER), and BOTH
 * hand-spelled READs returned RDATA after an expired poll -- garbage dressed
 * as data.  One implementation is what stops that recurring.  Write the
 * index (REQ clear), write the value if this op has one, re-write the index
 * with REQ, poll for COMPL -- bounded, because a timeout is not success.
 *
 * Return: the poll iteration at which COMPL was seen (>= 0; the T-CONT bind
 * logs it), -ETIMEDOUT when the budget expires, -ENODEV when this chip's
 * table declares no such CAM, -EINVAL with no delay op or an unknown op
 * mode.  Proven against the pre-conversion forms on address, value, write
 * count, read count and delay count by
 * dev/rtl9607c-test/gpon_regtable_diff_test.
 */
static inline int gpon_gtc_cam_xact_pending(const struct hwio *io,
				    struct reg ind, u32 op_mode, u32 idx_mask,
				    u32 idx, struct reg wr, u16 val,
				    void (*delay_us)(unsigned int us),
				    bool *pending)
{
	u32 op = op_mode | (idx & idx_mask);
	unsigned int i;
	int rc;

	if (!delay_us)
		return -EINVAL;
	if (op_mode != GPON_GTC_CAM_OP_WRITE &&
	    op_mode != GPON_GTC_CAM_OP_READ &&
	    op_mode != GPON_GTC_CAM_OP_CLEAN)
		return -EINVAL;
	rc = reg_rc(ind);
	if (rc)
		return rc;
	if (op_mode == GPON_GTC_CAM_OP_WRITE) {
		rc = reg_rc(wr);
		if (rc)
			return rc;
	}
	/* The caller serializes this engine through its readback tail. A timed-out
	 * request remains owned until COMPL is observed, even if REQ falls. Luna
	 * stock leaves REQ set after completion: REQ-clear is not this protocol. */
	if (pending) {
		u32 status = hwio_rd(io, reg_at(ind));

		if (*pending || ((status & GPON_GTC_CAM_OP_REQ) &&
				 !(status & GPON_GTC_CAM_OP_COMPL))) {
			*pending = true;
			for (i = 0; i < GPON_GTC_CAM_TRIES; i++) {
				if (status & GPON_GTC_CAM_OP_COMPL)
					break;
				delay_us(1);
				status = hwio_rd(io, reg_at(ind));
			}
			if (i == GPON_GTC_CAM_TRIES)
				return -ETIMEDOUT;
			*pending = false;
		}
	}
	hwio_wr(io, reg_at(ind), op);
	if (op_mode == GPON_GTC_CAM_OP_WRITE)
		hwio_wr(io, reg_at(wr), val & GPON_GTC_CAM_VAL_MASK);
	if (pending)
		*pending = true;
	hwio_wr(io, reg_at(ind), op | GPON_GTC_CAM_OP_REQ);
	for (i = 0; i < GPON_GTC_CAM_TRIES; i++) {
		if (hwio_rd(io, reg_at(ind)) & GPON_GTC_CAM_OP_COMPL) {
			if (pending)
				*pending = false;
			return (int)i;
		}
		delay_us(1);
	}
	return -ETIMEDOUT;
}

/* Stateless compatibility entry point. Runtime users that retry after a
 * timeout use the tracked form above and retain one state per CAM engine. */
static inline int gpon_gtc_cam_xact(const struct hwio *io,
				    struct reg ind, u32 op_mode, u32 idx_mask,
				    u32 idx, struct reg wr, u16 val,
				    void (*delay_us)(unsigned int us))
{
	return gpon_gtc_cam_xact_pending(io, ind, op_mode, idx_mask, idx, wr,
					 val, delay_us, NULL);
}

/**
 * gpon_gtc_cam_write() - one indirect GTC CAM write transaction.
 *
 * The WRITE spelling of gpon_gtc_cam_xact(), kept as the named entry point
 * the four 2026-09-03-converted sites call.  Returns the poll iteration on
 * success because two call sites print `(compl %d)` with it.
 */
static inline int gpon_gtc_cam_write(const struct hwio *io,
				     struct reg ind, struct reg wr,
				     u32 idx_mask, u32 idx, u16 val,
				     void (*delay_us)(unsigned int us))
{
	return gpon_gtc_cam_xact(io, ind, GPON_GTC_CAM_OP_WRITE, idx_mask,
				 idx, wr, val, delay_us);
}

/**
 * gpon_gtc_cam_read() - one indirect GTC CAM read-back transaction.
 * @rd:  the CAM's read-data register (RDATA[11:0]).
 * @val: out -- the stored 12-bit value, masked to RDATA[11:0] (the same
 *       "& 0xfff" both pre-conversion read sites spelled).
 * @hit: out -- the IND register's OP_HIT: this entry would answer a lookup.
 *
 * Runs the READ op, then -- ONLY after COMPL -- re-reads the IND register
 * for OP_HIT and reads @rd for the data, the exact two trailing reads the
 * pre-conversion sites emitted.  On ANY failure the out-params are NOT
 * written and the rc is negative: the pre-conversion sites read them back
 * after an EXPIRED poll too, so a wedged CAM answered with whatever stale
 * RDATA the last transaction left -- garbage dressed as data, and the OMCC
 * readback print could "confirm" a bind that never landed.
 *
 * Return: the poll iteration (>= 0), -ETIMEDOUT, -ENODEV (IND or RD absent,
 * refused BEFORE any bus traffic), -EINVAL with no delay op.
 */
static inline int gpon_gtc_cam_read(const struct hwio *io,
				    struct reg ind, struct reg rd,
				    u32 idx_mask, u32 idx,
				    u16 *val, bool *hit,
				    void (*delay_us)(unsigned int us))
{
	int rc = reg_rc(rd);

	if (rc)
		return rc;
	rc = gpon_gtc_cam_xact(io, ind, GPON_GTC_CAM_OP_READ, idx_mask,
			       idx, reg_none(), 0, delay_us);
	if (rc < 0)
		return rc;
	*hit = !!(hwio_rd(io, reg_at(ind)) & GPON_GTC_CAM_OP_HIT);
	*val = (u16)(hwio_rd(io, reg_at(rd)) & GPON_GTC_CAM_VAL_MASK);
	return rc;
}

/** Write @gem_port into entry @flow of the DS GEM Port-ID CAM. */
static inline int gpon_gtc_ds_port_write(const struct hwio *io,
					 const struct gpon_gtc_regs *r,
					 u32 flow, u16 gem_port,
					 void (*delay_us)(unsigned int us))
{
	return gpon_gtc_cam_write(io, r->ds_port_ind, r->ds_port_wr,
				  r->ds_port_idx_mask, flow, gem_port,
				  delay_us);
}

/** Write @alloc into entry @tcont of the T-CONT alloc CAM. */
static inline int gpon_gtc_ds_alloc_write(const struct hwio *io,
					  const struct gpon_gtc_regs *r,
					  u32 tcont, u16 alloc,
					  void (*delay_us)(unsigned int us))
{
	return gpon_gtc_cam_write(io, r->ds_alloc_ind, r->ds_alloc_wr,
				  r->ds_alloc_idx_mask, tcont, alloc,
				  delay_us);
}

/** Read back entry @flow of the DS GEM Port-ID CAM (stored gem + OP_HIT). */
static inline int gpon_gtc_ds_port_read(const struct hwio *io,
					const struct gpon_gtc_regs *r,
					u32 flow, u16 *gem, bool *hit,
					void (*delay_us)(unsigned int us))
{
	return gpon_gtc_cam_read(io, r->ds_port_ind, r->ds_port_rd,
				 r->ds_port_idx_mask, flow, gem, hit,
				 delay_us);
}

/** Read back entry @tcont of the T-CONT alloc CAM (stored alloc + OP_HIT). */
static inline int gpon_gtc_ds_alloc_read(const struct hwio *io,
					 const struct gpon_gtc_regs *r,
					 u32 tcont, u16 *alloc, bool *hit,
					 void (*delay_us)(unsigned int us))
{
	return gpon_gtc_cam_read(io, r->ds_alloc_ind, r->ds_alloc_rd,
				 r->ds_alloc_idx_mask, tcont, alloc, hit,
				 delay_us);
}

/** Invalidate entry @flow of the DS GEM Port-ID CAM (the CLEAN op). */
static inline int gpon_gtc_ds_port_clean(const struct hwio *io,
					 const struct gpon_gtc_regs *r,
					 u32 flow,
					 void (*delay_us)(unsigned int us))
{
	return gpon_gtc_cam_xact(io, r->ds_port_ind, GPON_GTC_CAM_OP_CLEAN,
				 r->ds_port_idx_mask, flow, reg_none(), 0,
				 delay_us);
}


/*
 * The indirect COUNTER-read handshake, shared by the three flow-indexed
 * GTC/GEM counter muxes and, per the vendor chipdef, identical on the
 * RTL9602C and the RTL9603CVD (GPON_GEM_DS_RX_CNTR_IND 0x704040 /
 * GPON_GEM_DS_FWD_CNTR_IND 0x70404C / GPON_GTC_DS_PORT_CNTR_IND 0x701140:
 * the ack field -- ETH_PKT_RX_R_ACKf / ETH_PKT_FWD_R_ACKf / GEM_CNTR_R_ACKf
 * -- is lsp 15 len 1 on all three registers on BOTH chips, and the vendor's
 * own DAL polls every one of them through ONE constant,
 * GPON_REG_BITS_INDIRECT_ACK = 15).  GEM_CNTR_RSELf (the DS port counter
 * only) is lsp 8 len 1: 0 = packet count, 1 = byte count.
 *
 * ⚠ GPON_GEM_DS_MISC_IND (0x4064) IS NOT THIS TRANSACTION AND MUST NOT BE
 * ROUTED HERE.  Three independent sources agree it has no ack: both chipdefs
 * declare only MISC_CNTR_IDX on it ([3:0] on the RTL9602C, [2:0] on the
 * RTL9603CVD -- the one counter field whose width differs between the two
 * chips) with bit 15 reserved; the vendor DAL reads it with NO poll at all
 * (dal_rtl9602c_gpon_dsGemPortMiscCnt_get: write idx, read stat); and a live
 * stock capture shows bit 15 CLEAR at rest there while all three counters
 * above latch it SET (G24W gtcponip-stock.json: 0x04064=0x6 against
 * 0x04040=0x807f, 0x0404c=0x807f, 0x01140=0x8101).  A MISC read through this
 * helper would return -ETIMEDOUT on every call and never yield a count.
 * luna_gpon.c keeps its hand-spelled MISC read, with the finding recorded
 * beside it.
 */
#define GPON_GTC_CNTR_R_ACK	(1u << 15)	/* read-ack, all three counter INDs  */
#define GPON_GTC_CNTR_RSEL_BYTE	(1u << 8)	/* GEM_CNTR_RSEL: 1 = byte count     */
/* The poll budget of the pre-conversion call sites, verbatim: up to 1000
 * reads at 1 us apart.  The same numbers as the CAM transaction, spelled
 * separately because they are separate handshakes and either may move alone. */
#define GPON_GTC_CNTR_TRIES	1000u

/**
 * gpon_gtc_cntr_read() - one indirect GTC/GEM counter read.
 * @io:   how to reach the GTC block.
 * @ind:  the counter's index register (offset within the block).
 * @stat: the counter's status register.  ⚠ CLEAR-ON-READ: reading it is not
 *        an observation, it CONSUMES the count -- which is why the timeout
 *        path below must never touch it.
 * @sel:  the FULL index word to write (index bits already masked by the
 *        named table wrappers below, plus RSEL for the DS port counter).
 * @cnt:  out -- the 32-bit count.  NOT written on any failure.
 * @delay_us: the shell's sleep -- time is an EXPLICIT INPUT in this tier, so
 *        the same function runs on x86 under the write-stream differential.
 *
 * The sequence luna_gpon.c spelled by hand at FOUR sites until 2026-09-03:
 * write the flow/index, poll the SAME register for the read-ack -- bounded --
 * then read the count.  Three of the four were collapsed here (the MISC read
 * is refused above), and all three converted copies carried the same defect
 * the CAM family had just been cured of, one of them doubled: the two GEM
 * counter reads returned STAT after an EXPIRED poll -- and because STAT is
 * clear-on-read, a timed-out read did not merely return garbage, it STOLE
 * the count from the next reader.  The vendor DAL's posture agrees with the
 * repair: its gpon_indirect_wait() failure path returns an error and never
 * touches STAT.
 *
 * Return: the poll iteration at which the ack was seen (>= 0), -ETIMEDOUT
 * when the budget expires (STAT deliberately NOT read), -ENODEV when this
 * chip's table declares no such counter, -EINVAL with no delay op or no out
 * pointer.  Proven against the pre-conversion forms on address, value, write
 * count, read count and delay count by
 * dev/rtl9607c-test/gpon_regtable_diff_test.
 */
static inline int gpon_gtc_cntr_read(const struct hwio *io,
				     struct reg ind, struct reg stat, u32 sel,
				     u32 *cnt, void (*delay_us)(unsigned int us))
{
	unsigned int i;
	int rc;

	if (!delay_us || !cnt)
		return -EINVAL;
	rc = reg_rc(ind);
	if (!rc)
		rc = reg_rc(stat);
	if (rc)
		return rc;
	hwio_wr(io, reg_at(ind), sel);
	for (i = 0; i < GPON_GTC_CNTR_TRIES; i++) {
		if (hwio_rd(io, reg_at(ind)) & GPON_GTC_CNTR_R_ACK) {
			*cnt = hwio_rd(io, reg_at(stat));
			return (int)i;
		}
		delay_us(1);
	}
	return -ETIMEDOUT;
}

/** Read the per-flow DS GEM Ethernet RX packet count (clear-on-read). */
static inline int gpon_gtc_gem_ds_rx_cnt_read(const struct hwio *io,
					      const struct gpon_gtc_regs *r,
					      u32 flow, u32 *cnt,
					      void (*delay_us)(unsigned int us))
{
	return gpon_gtc_cntr_read(io, r->gem_ds_rx_cntr_ind,
				  r->gem_ds_rx_cntr_stat,
				  flow & r->cntr_flow_idx_mask, cnt, delay_us);
}

/** Read the per-flow DS GEM forwarded-to-PON-IP packet count (clear-on-read). */
static inline int gpon_gtc_gem_ds_fwd_cnt_read(const struct hwio *io,
					       const struct gpon_gtc_regs *r,
					       u32 flow, u32 *cnt,
					       void (*delay_us)(unsigned int us))
{
	return gpon_gtc_cntr_read(io, r->gem_ds_fwd_cntr_ind,
				  r->gem_ds_fwd_cntr_stat,
				  flow & r->cntr_flow_idx_mask, cnt, delay_us);
}

/**
 * Read the per-flow DS de-encapsulated GEM frame count (clear-on-read).
 * @bytes: false = packet count (RSEL 0), true = byte count (RSEL 1).
 */
static inline int gpon_gtc_ds_port_cnt_read(const struct hwio *io,
					    const struct gpon_gtc_regs *r,
					    u32 flow, bool bytes, u32 *cnt,
					    void (*delay_us)(unsigned int us))
{
	return gpon_gtc_cntr_read(io, r->ds_port_cntr_ind,
				  r->ds_port_cntr_stat,
				  (flow & r->cntr_flow_idx_mask) |
				  (bytes ? GPON_GTC_CNTR_RSEL_BYTE : 0),
				  cnt, delay_us);
}

#endif /* _REGTABLE_H */
