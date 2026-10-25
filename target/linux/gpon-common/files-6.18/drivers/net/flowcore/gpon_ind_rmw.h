/* SPDX-License-Identifier: GPL-2.0-only */
/* gpon_ind_rmw.h -- write ONE entry of an indirect ... -- dev/MEASURED-gpon_ind_rmw.h.md sec 1. */
#ifndef _GPON_IND_RMW_H
#define _GPON_IND_RMW_H

#include <linux/errno.h>
#include <linux/types.h>
#include "regtable.h"	/* gpon_ind_go(), reg_rc(), struct hwio */

/* struct gpon_ind_tbl - one indirect ACCESS/DATA table, as ...
 * dev/MEASURED-gpon_ind_rmw.h.md sec 2. */
struct gpon_ind_tbl {
	struct reg	access;
	struct reg	data;
	u32		go;
	u32		wr;
	unsigned int	tries;
};

/* gpon_ind_fetch() - the READ half: load entry @idx into DATA
 * dev/MEASURED-gpon_ind_rmw.h.md sec 3. */
static inline int gpon_ind_fetch(const struct hwio *io,
				 const struct gpon_ind_tbl *t, u32 idx,
				 void (*pause)(void), u32 *stuck)
{
	u32 cmd = t->go | idx;
	int rc;

	if (!pause)
		return -EINVAL;
	rc = reg_rc(t->access);
	if (!rc)
		rc = reg_rc(t->data);
	if (rc)
		return rc;

	/* Retire a previous request before changing ACCESS or staging DATA.
	 * The caller serializes the complete transaction against other users. */
	rc = gpon_ind_poll(io, t->access, t->go, t->tries, pause);
	if (rc < 0) {
		if (stuck)
			*stuck = 0; /* This operation submitted no command. */
		return rc;
	}
	rc = gpon_ind_go(io, t->access, cmd, t->go, t->tries, pause);
	if (rc < 0) {
		if (stuck)
			*stuck = cmd;
		return rc;
	}
	return 0;
}

/* gpon_ind_commit() - the WRITE half: DATA = @word, then the ...
 * dev/MEASURED-gpon_ind_rmw.h.md sec 4. */
static inline int gpon_ind_commit(const struct hwio *io,
				  const struct gpon_ind_tbl *t, u32 idx,
				  u32 word, void (*pause)(void), u32 *stuck)
{
	u32 cmd = t->go | t->wr | idx;
	int rc;

	if (!pause)
		return -EINVAL;
	rc = reg_rc(t->access);
	if (!rc)
		rc = reg_rc(t->data);
	if (rc)
		return rc;

	/* Retire a previous request before changing ACCESS or staging DATA.
	 * The caller serializes the complete transaction against other users. */
	rc = gpon_ind_poll(io, t->access, t->go, t->tries, pause);
	if (rc < 0) {
		if (stuck)
			*stuck = 0; /* This operation submitted no command. */
		return rc;
	}
	hwio_wr(io, reg_at(t->data), word);
	rc = gpon_ind_go(io, t->access, cmd, t->go, t->tries, pause);
	if (rc < 0) {
		if (stuck)
			*stuck = cmd;
		return rc;
	}
	return 0;
}

/* gpon_ind_rmw() - entry @idx: DATA = (DATA & ~@clr) | @set, ...
 * dev/MEASURED-gpon_ind_rmw.h.md sec 5. */
static inline int gpon_ind_rmw(const struct hwio *io,
			       const struct gpon_ind_tbl *t, u32 idx,
			       u32 clr, u32 set, void (*pause)(void),
			       u32 *stuck)
{
	int rc = gpon_ind_fetch(io, t, idx, pause, stuck);
	if (rc < 0)
		return rc;
	return gpon_ind_commit(io, t, idx,
			       (hwio_rd(io, reg_at(t->data)) & ~clr) | set, pause,
			       stuck);
}

/* gpon_ind_set() - entry @idx: DATA = @word, both halves ...
 * dev/MEASURED-gpon_ind_rmw.h.md sec 6. */
static inline int gpon_ind_set(const struct hwio *io,
			       const struct gpon_ind_tbl *t, u32 idx,
			       u32 word, void (*pause)(void), u32 *stuck)
{
	int rc = gpon_ind_fetch(io, t, idx, pause, stuck);
	if (rc < 0)
		return rc;
	return gpon_ind_commit(io, t, idx, word, pause, stuck);
}

#endif /* _GPON_IND_RMW_H */
