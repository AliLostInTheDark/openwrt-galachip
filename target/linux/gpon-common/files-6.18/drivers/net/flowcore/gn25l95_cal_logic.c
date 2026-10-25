// SPDX-License-Identifier: GPL-2.0-only
/* GN25L95 per-unit calibration, expressed as an ordered ...
 * dev/MEASURED-gn25l95_cal_logic.c.md sec 1. */
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include "gn25l95_cal_logic.h"

/* CONFIRMED at G24W rtkbosa 0x40471c/20/28: the MCU loader ...
 * dev/MEASURED-gn25l95_cal_logic.c.md sec 2. */
#define SLAVE_CAL	0x51		/* the loader's slave                */
#define SLAVE_SFF	0x50		/* SFF-8472, used only by the G24 tail */
#define REG_PAGE	0x7f		/* page/table selector               */

/* The calibration file is eight 0x80-byte slots, and which ...
 * dev/MEASURED-gn25l95_cal_logic.c.md sec 3. */
#define CAL_PAGE2_HIGH	0x200u			/* page 2, registers 0x80..0xff */
#define CAL_P2(reg)	(CAL_PAGE2_HIGH + (reg) - 0x80u)
#define REG_BB		0xbbu			/* the one register read back   */

/* The two places the units' own loaders differ. Settled by comparing both
 * binaries; everything else about the sequence is still owed a trace. */
const struct gn_variant gn_variant_g24w    = { .bb_set = true,  .tail_c4_7f = true  };
const struct gn_variant gn_variant_x400axf = { .bb_set = false, .tail_c4_7f = false };

/* ★★★ THE SEQUENCE, AGAINST AN ACTUAL REPLAY OF BOTH STOCK ...
 * dev/MEASURED-gn25l95_cal_logic.c.md sec 4. */
static int emit(struct gn_op *o, u32 *n, u32 max, u8 kind, u8 slave,
		u16 reg, u8 val, u8 mask)
{
	if (*n >= max)
		return -ENOSPC;
	o[*n].kind = kind; o[*n].slave = slave; o[*n].reg = reg;
	o[*n].val = val;   o[*n].mask = mask;
	(*n)++;
	return 0;
}

#define EMIT(k, s, r, v, m) do { \
	int _e = emit(out, &n, out_max, (k), (s), (r), (v), (m)); \
	if (_e) return _e; \
} while (0)

int gn25l95_cal_ops(const u8 *cal, u32 cal_len, const struct gn_variant *v,
		    struct gn_op *out, u32 out_max)
{
	static const struct { u8 table; u16 off; u8 reg; u8 len; } BULK[] = {
		{ 4, 0x280, 0x80, 0x80 },	/* selector at index   0 */
		{ 5, 0x300, 0x80, 0x80 },	/*                   129 */
		{ 6, 0x380, 0x80, 0x80 },	/*                   258 */
		{ 2, CAL_PAGE2_HIGH, 0x80, 0x80 },	/*           387 */
	};
	static const u8 LOW_SPARSE[] = {
		0x6f, 0x72, 0x78, 0x79 /* page 2 low-half calibration offsets */
	};
	u32 n = 0;
	unsigned int i, k;

	if (!cal || cal_len != GN_CAL_LEN || !v || !out)
		return -EINVAL;

	for (k = 0; k < ARRAY_SIZE(BULK); k++) {
		EMIT(GN_SELECT, SLAVE_CAL, REG_PAGE, BULK[k].table, 0);
		for (i = 0; i < BULK[k].len; i++)
			EMIT(GN_WRITE, SLAVE_CAL, BULK[k].reg + i,
			     cal[BULK[k].off + i], 0);
	}

	/* Page 2 again: D2..E7 is written a SECOND time, from the SAME file
	 * bytes the bulk copy above already sent. Stock does it twice; the
	 * repeat is kept because the order is the thing being reproduced. */
	EMIT(GN_SELECT, SLAVE_CAL, REG_PAGE, 2, 0);		/* index 516 */
	for (i = 0; i <= 0xe7 - 0xd2; i++)
		EMIT(GN_WRITE, SLAVE_CAL, 0xd2 + i, cal[CAL_P2(0xd2) + i], 0);
	for (i = 0; i < 0x50; i++)
		EMIT(GN_WRITE, SLAVE_CAL, 0x00 + i, cal[0x100 + i], 0);
	for (i = 0; i < ARRAY_SIZE(LOW_SPARSE); i++)
		EMIT(GN_WRITE, SLAVE_CAL, LOW_SPARSE[i],
		     cal[0x100 + LOW_SPARSE[i]], 0);

	EMIT(GN_SELECT, SLAVE_CAL, REG_PAGE, 1, 0);		/* index 623 */
	EMIT(GN_WRITE,  SLAVE_CAL, 0xf8, cal[0x1f8], 0);
	EMIT(GN_RMW,    SLAVE_CAL, 0xf9, 0xc0, 0xc0);
	EMIT(GN_WRITE,  SLAVE_CAL, 0xfc, cal[0x1fc], 0);
	EMIT(GN_RMW,    SLAVE_CAL, 0xfd, 0xc0, 0xc0);
	/* ONE read, TWO writes from that same byte -- a pulse, not two RMWs. */
	EMIT(GN_PULSE,  SLAVE_CAL, 0x6e, 0x40, 0xbf);

	EMIT(GN_SELECT, SLAVE_CAL, REG_PAGE, 2, 0);		/* index 633 */
	EMIT(GN_WRITE,  SLAVE_CAL, 0xa0, 0x6a, 0);
	for (i = 0x7b; i <= 0x7e; i++)
		EMIT(GN_WRITE, SLAVE_CAL, i, 0xff, 0);

	/* BB is judged on the WHOLE byte, and the expected value is ...
	 * dev/MEASURED-gn25l95_cal_logic.c.md sec 5. */
	EMIT(GN_SELECT,      SLAVE_CAL, REG_PAGE, 2, 0);	/* index 639 */
	EMIT(GN_RMW,         SLAVE_CAL, REG_BB, v->bb_set ? 0x01 : 0x00, 0x01);
	EMIT(GN_READ_EXPECT, SLAVE_CAL, REG_BB,
	     (u8)((cal[CAL_P2(REG_BB)] & 0xfe) | (v->bb_set ? 0x01 : 0x00)), 0xff);

	EMIT(GN_SELECT,    SLAVE_CAL, REG_PAGE, 2, 0);		/* index 643 */
	EMIT(GN_READ_NOTE, SLAVE_CAL, 0xb1, 0, 0);
	EMIT(GN_READ_NOTE, SLAVE_CAL, 0xb7, 0, 0);

	/* the G24W tail, AFTER the diagnostics */
	if (v->tail_c4_7f) {
		EMIT(GN_SELECT, SLAVE_CAL, REG_PAGE, 2, 0);	/* index 646 */
		EMIT(GN_WRITE,  SLAVE_CAL, 0xc4, 0x7f, 0);
		EMIT(GN_WRITE,  SLAVE_SFF, 0x7f, cal[0x7f], 0);
	}
	return (int)n;
}

int gn25l95_probe_ops(struct gn_op *out, u32 out_max)
{
	static const u8 PASSWORD[] = {
		0x7b, 0x7c, 0x7d, 0x7e /* password byte addresses, in wire order */
	};
	static const u8 FAMILY_ID[GN_PROBE_NOTES] = { 0x80, 0x85, 0x86 };
	u32 n = 0;
	unsigned int i;

	if (!out)
		return -EINVAL;

	EMIT(GN_SELECT, SLAVE_CAL, REG_PAGE, 0x00, 0);
	for (i = 0; i < ARRAY_SIZE(PASSWORD); i++)
		EMIT(GN_READ_EXPECT, SLAVE_CAL, PASSWORD[i], 0x00, 0xff);

	EMIT(GN_SELECT, SLAVE_CAL, REG_PAGE, 0xff, 0);
	for (i = 0; i < ARRAY_SIZE(FAMILY_ID); i++)
		EMIT(GN_READ_NOTE, SLAVE_CAL, FAMILY_ID[i], 0, 0);

	EMIT(GN_SELECT,      SLAVE_CAL, REG_PAGE, 0x02, 0);
	EMIT(GN_READ_EXPECT, SLAVE_CAL, 0xd1, 0xa0, 0xf0);
	return (int)n;
}

bool gn25l95_is_gn28l9x(const u8 id[GN_PROBE_NOTES])
{
	static const u8 FAMILY[][GN_PROBE_NOTES] = {
		{ 0xa1, 0x00, 0x00 },
		{ 'G', '9', '6' },
		{ 'G', '9', '7' },
		{ 'G', '9', '8' },	/* the X400AXF detector alone knows this */
	};
	unsigned int i;

	if (!id)
		return false;
	for (i = 0; i < ARRAY_SIZE(FAMILY); i++)
		if (!memcmp(id, FAMILY[i], GN_PROBE_NOTES))
			return true;
	return false;
}

int gn25l95_cal_apply(const struct gn_op *ops, u32 n, struct gn_io *io,
		      struct gn_fail *f)
{
	u32 i;

	if (!ops || !io || !io->rd || !io->wr || !f)
		return -EINVAL;

	for (i = 0; i < n; i++) {
		const struct gn_op *o = &ops[i];
		u8 got = 0;
		int e = 0;

		switch (o->kind) {
		case GN_SELECT:
		case GN_WRITE:
			e = io->wr(io->ctx, o->slave, (u8)o->reg, o->val);
			break;
		case GN_RMW:
			e = io->rd(io->ctx, o->slave, (u8)o->reg, &got);
			if (!e)
				e = io->wr(io->ctx, o->slave, (u8)o->reg,
					   (u8)((got & ~o->mask) | o->val));
			break;
		case GN_READ_EXPECT:
			e = io->rd(io->ctx, o->slave, (u8)o->reg, &got);
			if (!e && (got & o->mask) != o->val) {
				*f = (struct gn_fail){ .op = i, .reg = (u8)o->reg,
						       .got = got, .want = o->val,
						       .mask = o->mask };
				return -EIO;
			}
			break;
		case GN_READ_NOTE:
			e = io->rd(io->ctx, o->slave, (u8)o->reg, &got);
			if (!e && io->notes && io->notes_n < io->notes_max)
				io->notes[io->notes_n++] = got;
			break;
		case GN_PULSE:
			e = io->rd(io->ctx, o->slave, (u8)o->reg, &got);
			if (!e)
				e = io->wr(io->ctx, o->slave, (u8)o->reg,
					   (u8)(got | o->val));
			if (!e)
				e = io->wr(io->ctx, o->slave, (u8)o->reg,
					   (u8)(got & o->mask));
			break;
		default:
			return -EINVAL;
		}
		if (e) {
			*f = (struct gn_fail){ .op = i, .reg = (u8)o->reg,
					       .got = got, .want = o->val,
					       .mask = o->mask, .err = e };
			return e;
		}
	}
	return 0;
}
