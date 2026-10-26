/* SPDX-License-Identifier: GPL-2.0-only */
/* GN25L95 per-unit calibration */
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

/* The sequence that ranges the V2802RH, from its 1024-byte calibration file. */
int gn25l95_cal_ops(const u8 *cal, u32 cal_len, struct gn_op *out, u32 out_max);

/* The stock RTOS loader as read; LAN_TYPE comes from the hardware MIB. */
int gn25l95_v2802rh_cal_ops(const u8 *cal, u32 cal_len, u8 lan_type,
			  struct gn_op *out, u32 out_max);

#define GN_OPS_MAX	1200u

/* Byte I/O, supplied by the family shell. */
struct gn_io {
	void *ctx;
	int (*rd)(void *ctx, u8 slave, u8 reg, u8 *val);
	int (*wr)(void *ctx, u8 slave, u8 reg, u8 val);
	/* Where GN_READ_NOTE values land, in order, when `notes` */
	u8  *notes;
	u32 notes_max;
	u32 notes_n;
};

/* Where the sequence stopped. */
struct gn_fail {
	u32 op;
	u8  reg, got, want, mask;
	int err;
};

/* Execute `ops` in order, stopping at the FIRST failure. */
int gn25l95_cal_apply(const struct gn_op *ops, u32 n, struct gn_io *io,
		      struct gn_fail *f);

/* POSITIVE IDENTIFICATION, exactly as both units' own */
#define GN_PROBE_OPS	11u
#define GN_PROBE_NOTES	3u

int gn25l95_probe_ops(struct gn_op *out, u32 out_max);

/* > true when the three noted bytes are a GN28L9x family */
bool gn25l95_is_gn28l9x(const u8 id[GN_PROBE_NOTES]);

#endif /* _GN25L95_CAL_LOGIC_H */
