// SPDX-License-Identifier: GPL-2.0-only
#include <linux/types.h>
#include <linux/random.h>

#include "gpon_hwaddr.h"
#include "gpon_sn.h"

/* Local so this file also builds on a host (dev/rtl9607c-test). */
bool gpon_hwaddr_parse(const char *s, u8 out[GPON_HWADDR_BYTES])
{
	u8 tmp[GPON_HWADDR_BYTES];
	int i, n, v, d;

	if (!s || !out)
		return false;

	n = 0;
	for (i = 0; i < GPON_HWADDR_BYTES; i++) {
		if (i) {
			if (s[n] != ':')
				return false;
			n++;
		}
		d = gpon_hex_digit(s[n]);
		if (d < 0)
			return false;
		v = d;
		n++;
		d = gpon_hex_digit(s[n]);
		if (d >= 0) {		/* one or two digits per octet */
			v = (v << 4) | d;
			n++;
		}
		tmp[i] = (u8)v;
	}
	if (s[n])
		return false;

	for (i = 0; i < GPON_HWADDR_BYTES; i++)
		out[i] = tmp[i];
	return true;
}

bool gpon_hwaddr_usable(const u8 *mac)
{
	int i;

	if (!mac || (mac[0] & 0x01))
		return false;
	for (i = 0; i < GPON_HWADDR_BYTES; i++)
		if (mac[i])
			return true;
	return false;
}

static void hwaddr_copy(u8 *dst, const u8 *src)
{
	int i;

	for (i = 0; i < GPON_HWADDR_BYTES; i++)
		dst[i] = src[i];
}

enum gpon_hwaddr_src gpon_hwaddr_resolve(const char *bootarg, const u8 *dt,
					 const u8 *engine,
					 u8 out[GPON_HWADDR_BYTES])
{
	u8 tmp[GPON_HWADDR_BYTES];

	if (bootarg && gpon_hwaddr_parse(bootarg, tmp) &&
	    gpon_hwaddr_usable(tmp)) {
		hwaddr_copy(out, tmp);
		return GPON_HWADDR_BOOTARG;
	}
	if (gpon_hwaddr_usable(dt)) {
		hwaddr_copy(out, dt);
		return GPON_HWADDR_DT;
	}
	if (gpon_hwaddr_usable(engine)) {
		hwaddr_copy(out, engine);
		return GPON_HWADDR_ENGINE;
	}

	/* eth_random_addr() written out: <linux/etherdevice.h> would drag netdev
	 * in and this file must keep building on a host. |= 2 also makes the
	 * result non-zero, so it always satisfies gpon_hwaddr_usable(). */
	get_random_bytes(out, GPON_HWADDR_BYTES);
	out[0] &= 0xfe;
	out[0] |= 0x02;
	return GPON_HWADDR_RANDOM;
}

void gpon_hwaddr_derive(u8 out[GPON_HWADDR_BYTES],
			const u8 base[GPON_HWADDR_BYTES], unsigned int add)
{
	int i;

	hwaddr_copy(out, base);
	for (i = GPON_HWADDR_BYTES - 1; i >= 0 && add; i--) {
		unsigned int s = out[i] + (add & 0xff);

		out[i] = (u8)(s & 0xff);
		add = (add >> 8) + (s >> 8);
	}
}

const char *gpon_hwaddr_src_name(enum gpon_hwaddr_src src)
{
	switch (src) {
	case GPON_HWADDR_BOOTARG:
		return "the `mac=` boot parameter";
	case GPON_HWADDR_DT:
		return "DT/nvmem";
	case GPON_HWADDR_ENGINE:
		return "the MAC engine";
	case GPON_HWADDR_RANDOM:
		return "a random locally-administered address";
	}
	return "an unknown source";
}
