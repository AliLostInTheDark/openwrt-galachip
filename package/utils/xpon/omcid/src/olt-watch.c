// SPDX-License-Identifier: GPL-2.0-only
/*
 * One line per OMCC activation saying what the OLT did with the ONU: an
 * audit, a full reprovision after MIB Reset, or nothing. Counting stops
 * once the OLT has been quiet for a while, so steady state costs nothing.
 */
#include "daemon.h"
#include <fcntl.h>
#include <unistd.h>

#define OLT_WATCH_QUIET_MS	10000	/* the OLT is done after this much silence */
#define OLT_WATCH_MAX_MS	120000	/* report even if it keeps talking */

enum {
	OLT_MT_CREATE = 4, OLT_MT_DELETE = 6, OLT_MT_SET = 8, OLT_MT_GET = 9,
	OLT_MT_MIB_UPLOAD = 13, OLT_MT_MIB_UPLOAD_NEXT = 14, OLT_MT_MIB_RESET = 15,
};

void omcid_olt_watch_link(struct omcid_olt_watch *w, bool up, unsigned long now)
{
	if (!up) {
		if (!w->down)
			w->down_ms = now;
		w->down = true;
		w->active = false;
		return;
	}
	w->had_outage = w->down;
	w->outage_ms = w->down ? now - w->down_ms : 0;
	w->down = false;
	memset(w->count, 0, sizeof(w->count));
	memset(w->issues, 0, sizeof(w->issues));
	w->issue_count = 0;
	w->issue_overflow = 0;
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

static bool olt_watch_has_result(u8 mt)
{
	switch (mt) {
	case OLT_MT_CREATE:
	case OLT_MT_DELETE:
	case OLT_MT_SET:
	case OLT_MT_GET:
	case OLT_MT_MIB_RESET:
	case 18: /* Test */
	case 19: /* Start SW download */
	case 21: /* End SW download */
	case 22: /* Activate SW */
	case 23: /* Commit SW */
	case 24: /* Sync time */
	case 25: /* Reboot */
	case 26: /* Get next */
	case 29: /* Set table */
		return true;
	default:
		return false;
	}
}

void omcid_olt_watch_tx(struct omcid_olt_watch *w, const u8 *pdu, size_t len)
{
	u8 mt, rc;
	size_t rc_off;
	u16 class_id, inst;
	unsigned int i;

	if (!w || len < 9)
		return;
	if (!(pdu[2] & 0x20)) /* AK=1 responses only */
		return;
	mt = pdu[2] & 0x1f;
	if (!olt_watch_has_result(mt))
		return;
	rc_off = (pdu[3] == 0x0b) ? 10 : 8;
	if (len <= rc_off)
		return;
	rc = pdu[rc_off];
	if (rc == 0)
		return;
	class_id = ((u16)pdu[4] << 8) | pdu[5];
	inst = ((u16)pdu[6] << 8) | pdu[7];

	for (i = 0; i < w->issue_count; i++) {
		if (w->issues[i].class_id == class_id &&
		    w->issues[i].msg_type == mt &&
		    w->issues[i].result == rc) {
			if (w->issues[i].count < 0xffff)
				w->issues[i].count++;
			w->issues[i].inst = inst;
			return;
		}
	}
	if (w->issue_count < OMCID_OLT_ISSUE_MAX) {
		struct omcid_olt_issue *e = &w->issues[w->issue_count++];

		e->class_id = class_id;
		e->inst = inst;
		e->msg_type = mt;
		e->result = rc;
		e->count = 1;
	} else if (w->issue_overflow < 0xffff) {
		w->issue_overflow++;
	}
}

static const char *olt_watch_verdict(const struct omcid_olt_watch *w)
{
	unsigned int provision = w->count[OLT_MT_CREATE] + w->count[OLT_MT_SET] +
				 w->count[OLT_MT_DELETE];

	if (!w->messages)
		return "no OMCI";
	if (w->count[OLT_MT_MIB_RESET])
		return "OLT requested MIB reset";
	if (provision)
		return "reprovision without MIB reset";
	if (w->count[OLT_MT_MIB_UPLOAD] || w->count[OLT_MT_GET])
		return "audit only; no MIB reset observed";
	return "keepalive only";
}

/* Returns ms until the summary is due, or -1 when nothing is pending. */
long omcid_olt_watch_poll(struct omcid_olt_watch *w, unsigned long now,
			  u16 mib_sync, FILE *out, bool kernel_log, bool force)
{
	unsigned long due = w->messages ? w->last_ms + OLT_WATCH_QUIET_MS :
					  w->up_ms + OLT_WATCH_MAX_MS;
	char dark[48] = "activation";
	char line[512];
	int fd;

	if (!w->active)
		return -1;
	if (due > w->up_ms + OLT_WATCH_MAX_MS)
		due = w->up_ms + OLT_WATCH_MAX_MS;
	if (!force && (long)(due - now) > 0)
		return due - now;

	if (w->had_outage)
		snprintf(dark, sizeof(dark), "%lu s OMCC unavailable", w->outage_ms / 1000);
	snprintf(line, sizeof(line), "omcid: %sOLT after %s: %s; %u messages over %lu s "
		"(MIB reset %u, upload %u/%u, create %u, set %u, delete %u, get %u), "
		"MIB sync %u\n", w->messages ? "" : "[OMC-101] ", dark, olt_watch_verdict(w),
		w->messages,
		(w->last_ms - w->up_ms) / 1000, w->count[OLT_MT_MIB_RESET],
		w->count[OLT_MT_MIB_UPLOAD], w->count[OLT_MT_MIB_UPLOAD_NEXT],
		w->count[OLT_MT_CREATE], w->count[OLT_MT_SET], w->count[OLT_MT_DELETE],
		w->count[OLT_MT_GET], mib_sync);
	fputs(line, out);
	if (w->issue_count) {
		unsigned int i, show = w->issue_count < 8 ? w->issue_count : 8;

		fprintf(out, "omcid: OLT non-zero responses (%u unique%s):",
			w->issue_count, w->issue_overflow ? "+" : "");
		for (i = 0; i < show; i++) {
			const struct omcid_olt_issue *e = &w->issues[i];

			fprintf(out, " me=%u/0x%04x(mt=%u,rc=%u,x%u)",
				e->class_id, e->inst, e->msg_type, e->result, e->count);
		}
		if (w->issue_count > show || w->issue_overflow)
			fprintf(out, " ...(+%u)",
				(unsigned int)(w->issue_count - show) + w->issue_overflow);
		fputc('\n', out);
	}
	fflush(out);
	if (kernel_log) {
		FILE *dfp;

		fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC | O_NONBLOCK);
		if (fd >= 0) {
			if (dprintf(fd, "<6>%s", line) < 0)
				fprintf(stderr, "omcid: cannot write recovery summary to /dev/kmsg: %s\n", strerror(errno));
			close(fd);
		} else {
			fprintf(stderr, "omcid: cannot open /dev/kmsg for recovery summary: %s\n", strerror(errno));
		}
		dfp = fopen("/tmp/omcid-olt-diag.tmp", "w");
		if (dfp) {
			unsigned int i;

			fputs(line, dfp);
			if (!w->issue_count) {
				fputs("non_zero_responses: none\n", dfp);
			} else {
				fprintf(dfp, "non_zero_responses: %u (overflow %u)\n",
					w->issue_count, w->issue_overflow);
				for (i = 0; i < w->issue_count; i++) {
					const struct omcid_olt_issue *e = &w->issues[i];

					fprintf(dfp, "  class=%u inst=0x%04x mt=%u rc=%u count=%u\n",
						e->class_id, e->inst, e->msg_type,
						e->result, e->count);
				}
			}
			fclose(dfp);
			rename("/tmp/omcid-olt-diag.tmp", "/tmp/omcid-olt-diag.txt");
		}
	}
	w->active = false;
	w->down_ms = 0;
	return -1;
}
