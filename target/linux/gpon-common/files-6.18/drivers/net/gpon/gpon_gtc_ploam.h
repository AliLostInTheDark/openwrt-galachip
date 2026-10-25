/* SPDX-License-Identifier: GPL-2.0-only */
/* gpon_gtc_ploam.h -- the GTC downstream-PLOAM message ...
 * dev/MEASURED-gpon_gtc_ploam.h.md sec 1. */
#ifndef _GPON_GTC_PLOAM_H
#define _GPON_GTC_PLOAM_H

#include <linux/types.h>

#include "hwio.h"	/* the injected accessor */
#include "regtable.h"	/* reg_has(): the ask this tier owes before using an offset */
#include "gpon_ploam.h"	/* GPON_PLOAM_DS_LEN -- the one spelling of the 13-octet DS message */

/* gpon_gtc_ds_ploam_read() - unpack the latched downstream ...
 * dev/MEASURED-gpon_gtc_ploam.h.md sec 2. */
static inline bool gpon_gtc_ds_ploam_read(const struct hwio *io,
					  struct reg msg_off,
					  u8 m[GPON_PLOAM_DS_LEN])
{
	const unsigned int full = GPON_PLOAM_DS_LEN / 2u;	/* 6 two-octet words */
	unsigned int i;
	u32 base;

	if (!reg_has(msg_off))
		return false;
	base = reg_at(msg_off);
	for (i = 0; i < full; i++) {
		u32 w = hwio_rd(io, base + i * 4u);

		m[2 * i]     = (w >> 8) & 0xff;
		m[2 * i + 1] = w & 0xff;
	}
	m[GPON_PLOAM_DS_LEN - 1] = (hwio_rd(io, base + full * 4u) >> 8) & 0xff;
	return true;
}

#endif /* _GPON_GTC_PLOAM_H */
