/* SPDX-License-Identifier: GPL-2.0-or-later */
/* TIER: CORE, and in the STRICT host-buildable subset -- ...
 * dev/MEASURED-gpon_omci_mic.h.md sec 1. */
#ifndef GPON_OMCI_MIC_H
#define GPON_OMCI_MIC_H

#include <linux/crc32.h>
#include <linux/types.h>

#include "gpon_omci_core.h"	/* OMCI_LEN + omci_mic_compute(): the ONE
				 * AAL5-BE spelling.  This header must never
				 * respell it -- reaching for ~crc32_be here
				 * would recreate the exact defect above. */

/*
 * The conventions a downstream OMCI trailer can be carrying.
 *
 * SHORT is a value and not an error return: "cannot carry a MIC at all" is a
 * framing or GEM-reassembly fault upstream of OMCI, "carries one we do not
 * recognise" is corruption on a well-framed PDU, and gpon_omci_core.c already
 * keeps rx_runt and rx_bad_mic apart for that reason.  Collapsing them would
 * make a broken reassembler look like a noisy fibre.
 */
enum gpon_mic_conv {
	GPON_MIC_CONV_SHORT = 0,	/* < OMCI_LEN: no MIC to judge          */
	GPON_MIC_CONV_AAL5_BE,		/* ~crc32_be(~0, pdu, 44)   -- G.984.4  */
	GPON_MIC_CONV_ZLIB_LE,		/* crc32_le(~0, pdu, 44) ^ ~0 reflected */
	GPON_MIC_CONV_NEITHER,		/* corrupt, or a convention we lack     */
};

/* The MIC as the OLT stamped it: bytes 44..47, big-endian. ...
 * dev/MEASURED-gpon_omci_mic.h.md sec 2. */
static inline u32 gpon_omci_mic_stamped(const u8 *pdu)
{
	return ((u32)pdu[44] << 24) | ((u32)pdu[45] << 16) |
	       ((u32)pdu[46] << 8) | pdu[47];
}

/* The reflected (zlib) CRC-32 over bytes 0..43 -- the OTHER convention, spelled
 * ONCE, here.  It is NOT what we emit; it exists so a self-check can SAY that
 * a link is dead because the far end speaks the other dialect, instead of the
 * link simply going quiet.  Same OMCI_LEN precondition as above. */
static inline u32 gpon_omci_mic_zlib_le(const u8 *pdu)
{
	return crc32_le(~0u, pdu, 44) ^ ~0u;
}

/* Which convention stamped @pdu. Pure: no state, no counters, ...
 * dev/MEASURED-gpon_omci_mic.h.md sec 3. */
static inline enum gpon_mic_conv gpon_omci_mic_conv(const u8 *pdu,
						    unsigned int len)
{
	u32 want;

	if (!pdu || len < OMCI_LEN)
		return GPON_MIC_CONV_SHORT;

	want = gpon_omci_mic_stamped(pdu);
	if (omci_mic_compute(pdu) == want)
		return GPON_MIC_CONV_AAL5_BE;
	if (gpon_omci_mic_zlib_le(pdu) == want)
		return GPON_MIC_CONV_ZLIB_LE;
	return GPON_MIC_CONV_NEITHER;
}

/* Never NULL, exactly as gpon_omci_mt_name() promises -- so no caller can
 * forget the check and print a nul pointer into a console. */
static inline const char *gpon_mic_conv_name(enum gpon_mic_conv c)
{
	switch (c) {
	case GPON_MIC_CONV_AAL5_BE:
		return "AAL5-BE";
	case GPON_MIC_CONV_ZLIB_LE:
		return "ZLIB-LE";
	case GPON_MIC_CONV_SHORT:
		return "short";
	case GPON_MIC_CONV_NEITHER:
		break;
	}
	return "NEITHER";
}

#endif /* GPON_OMCI_MIC_H */
