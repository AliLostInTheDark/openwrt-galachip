/* SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef GPON_GEM_DIAG_H
#define GPON_GEM_DIAG_H

#include <linux/types.h>
#include "gpon_data_plan.h"

/* Render ONE line describing the data path right now. */
int gpon_gem_diag_line(const struct gpon_data_armed *armed,
		       const struct gpon_data_want *want,
		       char *out, size_t sz);

#endif /* GPON_GEM_DIAG_H */
