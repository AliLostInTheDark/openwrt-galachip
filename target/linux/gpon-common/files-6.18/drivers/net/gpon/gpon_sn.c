// SPDX-License-Identifier: GPL-2.0-only
/*
 * The G.984.3 ONU Serial Number codec. See gpon_sn.h for why it is here and
 * why it is written without a single library call.
 */
#include <linux/types.h>

#include "gpon_sn.h"

/* Local, so the file needs no <ctype.h>: the spec's alphabet is ASCII and the
 * kernel's ctype is locale-free anyway -- spelling it out costs four lines and
 * removes a dependency from a file that must also build on a host. */
/* A vendor ID character: printable ASCII, and not a space. */
static int sn_id_char_ok(char c)
{
	unsigned char u = (unsigned char)c;

	return u > 0x20u && u < 0x7fu;
}

int gpon_sn_parse(const char *s, u8 out[GPON_SN_BYTES])
{
	u8 tmp[GPON_SN_BYTES];
	int i, hi, lo;

	if (!s || !out)
		return -1;

	/* Exactly GPON_SN_TEXT_LEN characters -- counted here rather than with
	 * strlen() so a missing NUL cannot walk off the end: the loop stops at
	 * the length the spec allows either way. */
	for (i = 0; i < GPON_SN_TEXT_LEN; i++) {
		if (!s[i])
			return -1;		/* shorter than the spec */
	}
	if (s[GPON_SN_TEXT_LEN])
		return -1;			/* longer than the spec */

	for (i = 0; i < 4; i++) {
		if (!sn_id_char_ok(s[i]))
			return -1;
		tmp[i] = (u8)s[i];
	}
	for (i = 0; i < 4; i++) {
		hi = gpon_hex_digit(s[4 + 2 * i]);
		lo = gpon_hex_digit(s[5 + 2 * i]);
		if (hi < 0 || lo < 0)
			return -1;
		tmp[4 + i] = (u8)((hi << 4) | lo);
	}

	/* Commit only now: a refusal must leave the caller's identity intact. */
	for (i = 0; i < GPON_SN_BYTES; i++)
		out[i] = tmp[i];
	return 0;
}

void gpon_sn_format(const u8 sn[GPON_SN_BYTES], char *out)
{
	static const char hexd[] = "0123456789ABCDEF";
	int i;

	if (!sn || !out)
		return;
	for (i = 0; i < 4; i++)
		out[i] = (char)sn[i];
	for (i = 0; i < 4; i++) {
		out[4 + 2 * i]     = hexd[(sn[4 + i] >> 4) & 0xf];
		out[4 + 2 * i + 1] = hexd[sn[4 + i] & 0xf];
	}
	out[GPON_SN_TEXT_LEN] = '\0';
}

/* Is the 4-byte half at @p entirely one blank-storage value? An erased NOR/NAND
 * page and an unburnt efuse row read 0xff; a zeroed EEPROM or a never-written
 * struct reads 0x00. Neither is a value anybody chose, and the two are the only
 * patterns a half can carry that mean "nothing was programmed here". */
static bool sn_half_blank(const u8 *p)
{
	int i;

	for (i = 1; i < 4; i++)
		if (p[i] != p[0])
			return false;
	return p[0] == 0x00 || p[0] == 0xff;
}

bool gpon_sn_is_set(const u8 sn[GPON_SN_BYTES])
{
	if (!sn)
		return false;
	/* EITHER HALF BLANK IS "NOT PROVISIONED", not just both */
	return !sn_half_blank(sn) && !sn_half_blank(sn + 4);
}
