/* SPDX-License-Identifier: GPL-2.0-only */
/* GN25L95 per-unit calibration: the ORDERED OPERATIONS, as ...
 * dev/MEASURED-gn25l95_cal_logic.h.md sec 1. */
#ifndef _GN25L95_CAL_LOGIC_H
#define _GN25L95_CAL_LOGIC_H

#include <linux/types.h>

#define GN_CAL_LEN	1024u		/* the file is exactly this, or refused */

enum gn_op_kind {
	GN_SELECT,		/* choose a page/table: write `val` to `reg`   */
	GN_WRITE,		/* reg <- val                                  */
	GN_RMW,			/* reg <- (reg & ~mask) | val                  */
	GN_READ_EXPECT,		/* read reg; (v & mask) must equal val         */
	GN_READ_NOTE,		/* read reg; recorded, not judged (diagnostic) */
	GN_PULSE,		/* read reg once, then write (v|val) and (v&mask) */
};

struct gn_op {
	u8  kind;
	u8  slave;
	u16 reg;
	u8  val;
	u8  mask;
};

/* The two places the units' own binaries differ. */
struct gn_variant {
	bool bb_set;		/* G24W: BB |= 1   ·  X400AXF: BB &= ~1        */
	bool tail_c4_7f;	/* G24W only: page2 C4 = 0x7f, then 0x50:7f    */
};

extern const struct gn_variant gn_variant_g24w;
extern const struct gn_variant gn_variant_x400axf;

/* Build the sequence for `cal` (GN_CAL_LEN bytes) into `out`. ...
 * dev/MEASURED-gn25l95_cal_logic.h.md sec 2. */
int gn25l95_cal_ops(const u8 *cal, u32 cal_len, const struct gn_variant *v,
		    struct gn_op *out, u32 out_max);

#define GN_OPS_MAX	1200u

/* Byte I/O, supplied by the family shell. The core builds and ...
 * dev/MEASURED-gn25l95_cal_logic.h.md sec 3. */
struct gn_io {
	void *ctx;
	int (*rd)(void *ctx, u8 slave, u8 reg, u8 *val);
	int (*wr)(void *ctx, u8 slave, u8 reg, u8 val);
	/* Where GN_READ_NOTE values land, in order, when `notes` is ...
	 * dev/MEASURED-gn25l95_cal_logic.h.md sec 8. */
	u8  *notes;
	u32 notes_max;
	u32 notes_n;
};

/* Where the sequence stopped. `op` is the index into the op ...
 * dev/MEASURED-gn25l95_cal_logic.h.md sec 4. */
struct gn_fail {
	u32 op;
	u8  reg, got, want, mask;
	int err;
};

/* Execute `ops` in order, stopping at the FIRST failure. -> ...
 * dev/MEASURED-gn25l95_cal_logic.h.md sec 5. */
int gn25l95_cal_apply(const struct gn_op *ops, u32 n, struct gn_io *io,
		      struct gn_fail *f);

/* POSITIVE IDENTIFICATION, exactly as both units' own stock ...
 * dev/MEASURED-gn25l95_cal_logic.h.md sec 6. */
#define GN_PROBE_OPS	11u
#define GN_PROBE_NOTES	3u

int gn25l95_probe_ops(struct gn_op *out, u32 out_max);

/* > true when the three noted bytes are a GN28L9x family id, ...
 * dev/MEASURED-gn25l95_cal_logic.h.md sec 7. */
bool gn25l95_is_gn28l9x(const u8 id[GN_PROBE_NOTES]);

#endif /* _GN25L95_CAL_LOGIC_H */
