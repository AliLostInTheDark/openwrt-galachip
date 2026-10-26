/* SPDX-License-Identifier: GPL-2.0-or-later */
/* TIER: CORE, header-only and pure: no MMIO, no allocator, no clock -- the caller
 * hands every duration in.
 *
 * How long a protocol exchange took inside the ONU (a request in, its answer
 * out): the count, maximum and sum since boot plus the most recent samples, so a
 * reader computes any percentile without the kernel sorting anything.
 */
#ifndef GPON_LAT_H
#define GPON_LAT_H

#include <linux/types.h>

#define GPON_LAT_RING	256u

struct gpon_lat {
	u32 n;
	u32 max_us;
	u64 sum_us;
	u32 ring[GPON_LAT_RING];
};

static inline void gpon_lat_add(struct gpon_lat *l, u32 us)
{
	l->ring[l->n % GPON_LAT_RING] = us;
	l->n++;
	l->sum_us += us;
	if (us > l->max_us)
		l->max_us = us;
}

/* The samples still held, oldest first, into @out (GPON_LAT_RING slots). -> count */
static inline unsigned int gpon_lat_samples(const struct gpon_lat *l, u32 *out)
{
	unsigned int held = l->n < GPON_LAT_RING ? l->n : GPON_LAT_RING;
	unsigned int first = l->n - held, i;

	for (i = 0; i < held; i++)
		out[i] = l->ring[(first + i) % GPON_LAT_RING];
	return held;
}

#endif /* GPON_LAT_H */
