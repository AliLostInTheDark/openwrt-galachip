// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * gpon_omci_trace -- see gpon_omci_trace.h.  G.988 byte math, nothing else.
 */
#include <linux/kernel.h>
#include <linux/types.h>

#include "gpon_omci_core.h"	/* OMCI_MT_*: the ONE numbering of Table 11.2.2-1 */
#include "gpon_omci_trace.h"

/* G.988 Table 11.2.2-1. 32 entries because the message type ...
 * dev/MEASURED-gpon_omci_trace.c.md sec 1. */
static const char *const gpon_omci_mt[32] = {
	[OMCI_MT_CREATE]		= "Create",
	[OMCI_MT_DELETE]		= "Delete",
	[OMCI_MT_SET]			= "Set",
	[OMCI_MT_GET]			= "Get",
	[OMCI_MT_GET_ALL_ALARMS]	= "Get-all-alarms",
	[OMCI_MT_GET_ALL_ALRM_NX]	= "Get-all-alarms-next",
	[OMCI_MT_MIB_UPLOAD]		= "MIB-upload",
	[OMCI_MT_MIB_UPLOAD_NX]		= "MIB-upload-next",
	[OMCI_MT_MIB_RESET]		= "MIB-reset",
	[OMCI_MT_ALARM]			= "Alarm",
	[OMCI_MT_AVC]			= "AVC",
	[OMCI_MT_TEST]			= "Test",
	[OMCI_MT_START_SW_DL]		= "Start-SW-dl",
	[OMCI_MT_DOWNLOAD_SEC]		= "DL-section",
	[OMCI_MT_END_SW_DL]		= "End-SW-dl",
	[OMCI_MT_ACTIVATE_SW]		= "Activate-SW",
	[OMCI_MT_COMMIT_SW]		= "Commit-SW",
	[OMCI_MT_SYNC_TIME]		= "Sync-time",
	[OMCI_MT_REBOOT]		= "Reboot",
	[OMCI_MT_GET_NEXT]		= "Get-next",
	[27]				= "Test-result",
	[28]				= "Get-current-data",
	[29]				= "Set-table",
	/* ⚠ 27/28/29 stay NUMERIC: the core declares no OMCI_MT_* for them, and
	 * naming one here would be a second spelling again -- the exact defect
	 * this change removes.  They are owed a core #define, not a local one. */
};

const char *gpon_omci_mt_name(u8 msg_type)
{
	const char *n = gpon_omci_mt[msg_type & 0x1f];

	return n ? n : "?";
}

bool gpon_omci_is_get(const u8 *pdu, unsigned int len)
{
	return pdu && len >= 3 && (pdu[2] & 0x1f) == OMCI_MT_GET;
}

bool gpon_omci_is_set(const u8 *pdu, unsigned int len)
{
	return pdu && len >= 3 && (pdu[2] & 0x1f) == OMCI_MT_SET;
}

bool gpon_omci_is_create(const u8 *pdu, unsigned int len)
{
	return pdu && len >= 3 && (pdu[2] & 0x1f) == OMCI_MT_CREATE;
}

bool gpon_omci_has_result_code(const u8 *pdu, unsigned int len)
{
	u8 mt;

	if (!pdu || len < 3)
		return false;		/* unclassifiable: render nothing */

	mt = pdu[2] & 0x1f;
	return mt != OMCI_MT_GET_ALL_ALARMS &&
	       mt != OMCI_MT_GET_ALL_ALRM_NX &&
	       mt != OMCI_MT_MIB_UPLOAD &&
	       mt != OMCI_MT_MIB_UPLOAD_NX;
}

bool gpon_omci_is_bulk(const u8 *pdu, unsigned int len)
{
	u8 mt;

	if (!pdu || len < 3)
		return false;			/* unclassifiable: never bulk */

	mt = pdu[2] & 0x1f;
	return mt == OMCI_MT_GET ||
	       mt == OMCI_MT_MIB_UPLOAD ||
	       mt == OMCI_MT_MIB_UPLOAD_NX;
}

int gpon_omci_describe(const u8 *pdu, unsigned int len, char *out, size_t sz)
{
	u8 mt;

	if (!out || !sz)
		return 0;
	out[0] = '\0';
	if (!pdu || len < GPON_OMCI_MIN_HDR)
		return 0;

	mt = pdu[2];
	return scnprintf(out, sz,
			 "len=%u tci=0x%02x%02x mt=%u(%s)%s%s dev=0x%02x me=%u/%u",
			 len, pdu[0], pdu[1], mt & 0x1f, gpon_omci_mt_name(mt),
			 (mt & 0x40) ? " AR" : "", (mt & 0x20) ? " AK" : "",
			 pdu[3],
			 ((u16)pdu[4] << 8) | pdu[5],
			 ((u16)pdu[6] << 8) | pdu[7]);
}

bool gpon_omci_is_create_of(const u8 *pdu, unsigned int len, u16 me_class)
{
	return pdu && len >= GPON_OMCI_MIN_HDR &&
	       (pdu[2] & 0x1f) == OMCI_MT_CREATE &&
	       (((u16)pdu[4] << 8) | pdu[5]) == me_class;
}

const u8 *gpon_omci_body(const u8 *pdu, unsigned int len, u8 *blen)
{
	unsigned int bounded = len > OMCI_LEN ?
			       OMCI_LEN : len;

	if (blen)
		*blen = bounded > GPON_OMCI_MIN_HDR ?
			(u8)(bounded - GPON_OMCI_MIN_HDR) : 0;
	if (!pdu || bounded <= GPON_OMCI_MIN_HDR)
		return NULL;
	return pdu + GPON_OMCI_MIN_HDR;
}
