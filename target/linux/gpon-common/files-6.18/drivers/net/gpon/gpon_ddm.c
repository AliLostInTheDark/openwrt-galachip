// SPDX-License-Identifier: GPL-2.0-only
/* gpon_ddm.c -- optical diagnostic (DDM) unit conversion. ONE ...
 * dev/MEASURED-gpon_ddm.c.md sec 1. */

#include "gpon_ddm.h"

/* log10 of the mantissa, in milli-decades, sampled every 1/16 ...
 * dev/MEASURED-gpon_ddm.c.md sec 2. */
static const u16 MANT_LOG10[17] = {
	  0,  26,  51,  75,  97, 118, 138, 158, 176,
	194, 211, 227, 243, 258, 273, 287, 301,
};

s32 gpon_ddm_uw10_to_cdbm(u32 raw)
{
	u32 mantissa, index, weight;
	int octave = 0;
	s32 milli;

	if (!raw)
		return 0;	/* undefined input; the caller owns the zero */

	/* log10(raw) = octave*log10(2) + log10(mantissa), 1 <= mantissa < 2 */
	while (raw >> (octave + 1))
		octave++;

	/* mantissa - 1, as a fraction of 256 */
	mantissa = octave ? (((raw - (1u << octave)) << 8) >> octave) : 0;
	index  = mantissa >> 4;		/* which of the 16 intervals   */
	weight = mantissa & 0xf;	/* how far into it, out of 16  */

	/* 30103 = round(100000 * log10(2)); the +50 rounds the /100 */
	milli = (octave * 30103 + 50) / 100;

	/* linear interpolation between the two bracketing table entries */
	milli += MANT_LOG10[index] +
		 (s32)(MANT_LOG10[index + 1] - MANT_LOG10[index]) * (s32)weight / 16;

	return milli - 4000;		/* 0.1 uW = -40.00 dBm is the origin */
}

/* ANI-G counts per centi-dBm: the attribute's step is 0.002 dB, ours is 0.01. */
#define GPON_ANIG_PER_CDBM	5

s16 gpon_ddm_cdbm_to_anig(s32 cdbm)
{
	s32 level = cdbm * GPON_ANIG_PER_CDBM;

	if (level > 32767)
		return 32767;
	if (level < -32768)
		return -32768;
	return (s16)level;
}
