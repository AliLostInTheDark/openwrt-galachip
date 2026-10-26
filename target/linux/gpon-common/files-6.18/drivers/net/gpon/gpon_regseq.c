// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * gpon_regseq -- the interpreter.  See gpon_regseq.h for why it is core.
 */
#include <linux/errno.h>
#include <linux/kernel.h>

#include "gpon_regseq.h"

/* Read-modify-write bits [msb:lsb]. */
void gpon_regseq_fld(const struct gpon_regseq_io *io, u32 addr,
		     u8 msb, u8 lsb, u32 val)
{
	io->wr(addr, gpon_field_set(io->rd(addr), msb, lsb, val));
}

/* Run one sequence. -> 0, or -ETIMEDOUT naming nothing */
int gpon_regseq_run(const struct gpon_regseq_io *io,
		    const struct gpon_regseq_op *seq, unsigned int n)
{
	unsigned int i, k;

	if (!io || !io->rd || !io->wr || !io->delay_ms || !io->delay_us)
		return -EINVAL;

	for (i = 0; i < n; i++) {
		const struct gpon_regseq_op *p = &seq[i];

		switch (p->opc) {
		case GPON_REGSEQ_WR:
			io->wr(p->addr, p->val);
			break;
		case GPON_REGSEQ_FLD:
			gpon_regseq_fld(io, p->addr, p->msb, p->lsb, p->val);
			break;
		case GPON_REGSEQ_DLY:
			io->delay_ms(p->val);
			break;
		case GPON_REGSEQ_POLL:
			for (k = 0; k < p->val; k++) {
				if (io->rd(p->addr) & (1u << p->lsb))
					break;
				io->delay_us(GPON_REGSEQ_POLL_US);
			}
			if (k == p->val)
				return -ETIMEDOUT;
			break;
		default:
			/* An unknown opcode is a TABLE defect, and running the
			 * rest of a bring-up whose meaning we did not
			 * understand is how half-configured silicon reaches a
			 * measurement. */
			return -EINVAL;
		}
	}
	return 0;
}
