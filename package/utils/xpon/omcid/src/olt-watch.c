// SPDX-License-Identifier: GPL-2.0-only
/*
 * One line per OMCC activation saying what the OLT did with the ONU: an
 * audit, a full reprovision after MIB Reset, or nothing. Counting stops
 * once the OLT has been quiet for a while, so steady state costs nothing.
 */
#include "daemon.h"

#define OLT_WATCH_QUIET_MS	10000	/* the OLT is done after this much silence */
#define OLT_WATCH_MAX_MS	120000	/* report even if it keeps talking */

enum {
	OLT_MT_CREATE = 4, OLT_MT_DELETE = 6, OLT_MT_SET = 8, OLT_MT_GET = 9,
	OLT_MT_MIB_UPLOAD = 13, OLT_MT_MIB_UPLOAD_NEXT = 14, OLT_MT_MIB_RESET = 15,
};

void omcid_olt_watch_link(struct omcid_olt_watch *w, bool up, unsigned long now)
{
	if (!up) {
		if (!w->down_ms)
			w->down_ms = now ?: 1;
		w->active = false;
		return;
	}
	memset(w->count, 0, sizeof(w->count));
	w->messages = 0;
	w->up_ms = w->last_ms = now;
	w->active = true;
}

void omcid_olt_watch_rx(struct omcid_olt_watch *w, const u8 *pdu, size_t len,
			unsigned long now)
{
	if (!w->active || len < 3)
		return;
	w->count[pdu[2] & 0x1f]++;
	w->messages++;
	w->last_ms = now;
}

static const char *olt_watch_verdict(const struct omcid_olt_watch *w)
{
	unsigned int provision = w->count[OLT_MT_CREATE] + w->count[OLT_MT_SET] +
				 w->count[OLT_MT_DELETE];

	if (!w->messages)
		return "no OMCI";
	if (w->count[OLT_MT_MIB_RESET])
		return "MIB reset and full reprovision";
	if (provision)
		return "reprovision without MIB reset";
	if (w->count[OLT_MT_MIB_UPLOAD] || w->count[OLT_MT_GET])
		return "audit only";
	return "keepalive only";
}

/* Returns ms until the summary is due, or -1 when nothing is pending. */
long omcid_olt_watch_poll(struct omcid_olt_watch *w, unsigned long now,
			  u16 mib_sync, FILE *out)
{
	unsigned long due = w->messages ? w->last_ms + OLT_WATCH_QUIET_MS :
					  w->up_ms + OLT_WATCH_MAX_MS;
	char dark[32] = "activation";

	if (!w->active)
		return -1;
	if (due > w->up_ms + OLT_WATCH_MAX_MS)
		due = w->up_ms + OLT_WATCH_MAX_MS;
	if ((long)(due - now) > 0)
		return due - now;

	if (w->down_ms)
		snprintf(dark, sizeof(dark), "%lu s dark", (w->up_ms - w->down_ms) / 1000);
	fprintf(out, "omcid: OLT after %s: %s; %u messages over %lu s "
		"(MIB reset %u, upload %u/%u, create %u, set %u, delete %u, get %u), "
		"MIB sync %u\n", dark, olt_watch_verdict(w), w->messages,
		(w->last_ms - w->up_ms) / 1000, w->count[OLT_MT_MIB_RESET],
		w->count[OLT_MT_MIB_UPLOAD], w->count[OLT_MT_MIB_UPLOAD_NEXT],
		w->count[OLT_MT_CREATE], w->count[OLT_MT_SET], w->count[OLT_MT_DELETE],
		w->count[OLT_MT_GET], mib_sync);
	fflush(out);
	w->active = false;
	w->down_ms = 0;
	return -1;
}
