/* SPDX-License-Identifier: GPL-2.0-only */
/* The netdev station-address ladder, in the core so it exists ...
 * dev/MEASURED-gpon_hwaddr.h.md sec 1. */
#ifndef _GPON_HWADDR_H
#define _GPON_HWADDR_H

#include <linux/types.h>

#define GPON_HWADDR_BYTES	6

/* In PRECEDENCE ORDER: the enum is the rule. */
enum gpon_hwaddr_src {
	GPON_HWADDR_BOOTARG = 0,
	GPON_HWADDR_DT,
	GPON_HWADDR_ENGINE,
	GPON_HWADDR_RANDOM,
};

/* gpon_hwaddr_parse() - "xx:xx:xx:xx:xx:xx" -> six bytes. ...
 * dev/MEASURED-gpon_hwaddr.h.md sec 4. */
bool gpon_hwaddr_parse(const char *s, u8 out[GPON_HWADDR_BYTES]);

/** Return: true for a non-zero unicast address. Refuses NULL. */
bool gpon_hwaddr_usable(const u8 *mac);

/* gpon_hwaddr_resolve() - first usable candidate, else a ...
 * dev/MEASURED-gpon_hwaddr.h.md sec 2. */
enum gpon_hwaddr_src gpon_hwaddr_resolve(const char *bootarg, const u8 *dt,
					 const u8 *engine,
					 u8 out[GPON_HWADDR_BYTES]);

/** Return: the rung in words, for the probe log. */
const char *gpon_hwaddr_src_name(enum gpon_hwaddr_src src);

/* gpon_hwaddr_derive() - a second station address @add above ...
 * dev/MEASURED-gpon_hwaddr.h.md sec 3. */
void gpon_hwaddr_derive(u8 out[GPON_HWADDR_BYTES],
			const u8 base[GPON_HWADDR_BYTES], unsigned int add);

#endif /* _GPON_HWADDR_H */
