/* SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef GPON_REGSEQ_H
#define GPON_REGSEQ_H

#include <linux/types.h>

/* What a step does.  Kept small on purpose: an opcode nobody needs is an
 * opcode that will be used for something it does not mean. */
enum gpon_regseq_opc {
	GPON_REGSEQ_WR = 0,	/* write `val` to `addr`			*/
	GPON_REGSEQ_FLD,	/* read-modify-write bits [msb:lsb] = `val`	*/
	GPON_REGSEQ_DLY,	/* wait `val` milliseconds			*/
	GPON_REGSEQ_POLL,	/* poll bit `lsb` of `addr` up to `val` times	*/
};

struct gpon_regseq_op {
	u8  opc;
	u8  msb;
	u8  lsb;
	u32 addr;
	u32 val;
};

/* The shell's whole contract */
static inline u32 gpon_field_mask(u8 msb, u8 lsb)
{
	return (msb == 31 && lsb == 0)
	     ? 0xffffffffu
	     : (((1u << (msb - lsb + 1)) - 1u) << lsb);
}

static inline u32 gpon_field_set(u32 word, u8 msb, u8 lsb, u32 val)
{
	u32 mask = gpon_field_mask(msb, lsb);

	return (word & ~mask) | ((val << lsb) & mask);
}

struct gpon_regseq_io {
	u32  (*rd)(u32 addr);
	void (*wr)(u32 addr, u32 val);
	void (*delay_ms)(unsigned int ms);
	void (*delay_us)(unsigned int us);
};

/* How long one POLL step waits between reads.  A step's budget is a COUNT of
 * these, so a sequence author states "up to N tries" and the shell states how
 * long a try is. */
#define GPON_REGSEQ_POLL_US	200u

int gpon_regseq_run(const struct gpon_regseq_io *io,
		    const struct gpon_regseq_op *seq, unsigned int n);

void gpon_regseq_fld(const struct gpon_regseq_io *io, u32 addr,
		     u8 msb, u8 lsb, u32 val);

/* Step constructors, so a table reads as the bring-up and not as a struct. */
#define GPON_WR(a, v)		{ GPON_REGSEQ_WR,   0,   0,   (a), (v) }
#define GPON_FLD(a, m, l, v)	{ GPON_REGSEQ_FLD,  (m), (l), (a), (v) }
#define GPON_DLY(ms)		{ GPON_REGSEQ_DLY,  0,   0,   0,   (ms) }
#define GPON_POLL(a, bit, n)	{ GPON_REGSEQ_POLL, (bit), (bit), (a), (n) }

#endif /* GPON_REGSEQ_H */
