/* SPDX-License-Identifier: GPL-2.0-only */
/* gpon_sn.h -- the ITU-T G.984.3 ONU Serial Number */
#ifndef _GPON_SN_H
#define _GPON_SN_H

#include <linux/types.h>

static inline int gpon_hex_digit(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

/** Bytes in a G.984.3 ONU-SN. */
#define GPON_SN_BYTES		8
/** Characters in its printable form, excluding the NUL. */
#define GPON_SN_TEXT_LEN	12
/** Buffer a caller must provide to gpon_sn_format(): text + NUL. */
#define GPON_SN_TEXT_SIZE	(GPON_SN_TEXT_LEN + 1)

/* gpon_sn_parse() - decode "AAAAhhhhhhhh" into the 8 SN */
int gpon_sn_parse(const char *s, u8 out[GPON_SN_BYTES]);

/* gpon_sn_format() - encode the 8 SN bytes as "AAAAhhhhhhhh". */
void gpon_sn_format(const u8 sn[GPON_SN_BYTES], char *out);

/* gpon_sn_is_set() - has a serial number been provisioned */
bool gpon_sn_is_set(const u8 sn[GPON_SN_BYTES]);

#endif /* _GPON_SN_H */
