/* SPDX-License-Identifier: GPL-2.0-only */
/* hwio.h -- the register accessor, injected, so LOGIC */
#ifndef _HWIO_H
#define _HWIO_H

#include <linux/types.h>

/* gpon/ is the lower layer: flowcore/Makefile carries -I .../drivers/net/gpon,
 * and this is where the ONE field-mask owner lives. */
#include "gpon_regseq.h"

/* struct hwio - how a caller reaches one register block. @rd */
struct hwio {
	u32  (*rd)(void *ctx, u32 off);
	void (*wr)(void *ctx, u32 off, u32 val);
	void *ctx;
};

/** Read @off. Returns 0 when @io is not wired -- a caller that cares must ask. */
static inline u32 hwio_rd(const struct hwio *io, u32 off)
{
	return (io && io->rd) ? io->rd(io->ctx, off) : 0u;
}

/** Write @off. A no-op when @io is not wired. */
static inline void hwio_wr(const struct hwio *io, u32 off, u32 val)
{
	if (io && io->wr)
		io->wr(io->ctx, off, val);
}

/* hwio_rmw() - read-modify-write the field [@msb:@lsb] at */
static inline void hwio_rmw(const struct hwio *io, u32 off, u8 msb, u8 lsb,
			    u32 val)
{
	hwio_wr(io, off, gpon_field_set(hwio_rd(io, off), msb, lsb, val));
}

#endif /* _HWIO_H */
