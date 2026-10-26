// SPDX-License-Identifier: GPL-2.0-or-later
/* See gpon_olt_diag.h for what this is. */
#include <linux/kernel.h>
#include <linux/types.h>

#include "gpon_olt_diag.h"
#include "gpon_omci_core.h"

#define OLT_TAG		"olt1"
#define OLT_TAG_LEN	4
#define OSTATE_MAX	7		/* G.984.3 O1..O7 */

static const char hexd[] = "0123456789abcdef";

void gpon_olt_capture_reset(struct gpon_olt_capture *c)
{
	if (!c)
		return;
	c->head = 0;
	c->stored = 0;
	c->seq = 0;
	c->dropped = 0;
}

void gpon_olt_capture_add(struct gpon_olt_capture *c, u64 t_us, u8 ostate,
			  enum gpon_olt_dir dir, const u8 *pdu,
			  unsigned int len)
{
	struct gpon_olt_rec *r;
	unsigned int n, i;

	if (!c || !c->depth || !pdu || !len)
		return;
	n = len > GPON_OLT_PDU ? GPON_OLT_PDU : len;
	r = &c->rec[c->head];
	if (c->stored == c->depth)
		c->dropped++;		/* the ring ate one: say so, never hide it */
	r->t_us = t_us;
	r->seq = c->seq++;
	r->dir = (u8)dir;
	r->ostate = ostate > OSTATE_MAX ? GPON_OLT_OSTATE_UNKNOWN : ostate;
	r->len = (u8)n;
	r->truncated = len > GPON_OLT_PDU ? 1 : 0;
	for (i = 0; i < n; i++)
		r->pdu[i] = pdu[i];
	for (; i < GPON_OLT_PDU; i++)
		r->pdu[i] = 0;
	c->head = (c->head + 1) % c->depth;
	if (c->stored < c->depth)
		c->stored++;
}

static const struct gpon_olt_rec *olt_last(const struct gpon_olt_capture *c)
{
	return &c->rec[(c->head + c->depth - 1) % c->depth];
}

/* One entry per kind, the latest exchange of that kind in place; first-seen order. */
static void table_put(struct gpon_olt_capture *t, const struct gpon_olt_rec *req,
		      const struct gpon_olt_rec *resp,
		      bool (*same)(const u8 *a, const u8 *b))
{
	static const struct gpon_olt_rec none;
	unsigned int i, n;

	if (!t || t->depth < 2)
		return;
	n = t->stored / 2;
	for (i = 0; i < n; i++)
		if (same(t->rec[2 * i].pdu, req->pdu))
			break;
	if (i == n) {
		if (t->stored + 2 > t->depth) {
			t->dropped++;
			return;
		}
		t->stored += 2;
		t->head = t->stored % t->depth;
	}
	t->rec[2 * i] = *req;
	t->rec[2 * i + 1] = resp ? *resp : none;
}

static bool same_instance(const u8 *a, const u8 *b)
{
	return a[4] == b[4] && a[5] == b[5] && a[6] == b[6] && a[7] == b[7];
}

/* the message type, the instance, and the attribute mask where the type has one */
static bool same_kind(const u8 *a, const u8 *b)
{
	u8 mt = b[2] & 0x1f;

	return (a[2] & 0x1f) == mt && same_instance(a, b) &&
	       ((mt != OMCI_MT_SET && mt != OMCI_MT_GET) || (a[8] == b[8] && a[9] == b[9]));
}

static bool same_ploam_type(const u8 *a, const u8 *b)
{
	return a[1] == b[1];
}

void gpon_olt_ledger_note(struct gpon_olt_capture *l, const struct gpon_olt_rec *req,
			  const struct gpon_olt_rec *resp)
{
	unsigned int i, n, keep, mt;

	if (!l || !l->depth || !req || req->len < 10)
		return;
	mt = req->pdu[2] & 0x1f;
	if (mt == OMCI_MT_MIB_RESET) {
		gpon_olt_capture_reset(l);
		return;
	}
	if (mt == OMCI_MT_CREATE || mt == OMCI_MT_SET) {
		table_put(l, req, resp, same_kind);
		return;
	}
	if (mt != OMCI_MT_DELETE)
		return;
	n = l->stored / 2;
	for (i = 0, keep = 0; i < n; i++) {
		if (same_instance(l->rec[2 * i].pdu, req->pdu))
			continue;
		l->rec[2 * keep] = l->rec[2 * i];
		l->rec[2 * keep + 1] = l->rec[2 * i + 1];
		keep++;
	}
	l->stored = 2 * keep;
	l->head = l->stored % l->depth;
}

/* Not identified: no answer to a request that asked for one, an answer with a
 * non-zero result, or a class this ONU holds no model of (answered by policy). */
static bool olt_unidentified(const struct gpon_olt_capture *c, const struct gpon_olt_rec *req,
			     const struct gpon_olt_rec *resp)
{
	u8 mt = req->pdu[2] & 0x1f;

	if (!resp)
		return req->pdu[2] & 0x40;
	if (resp->len > 8 && resp->pdu[8] && (mt < OMCI_MT_GET_ALL_ALARMS || mt > OMCI_MT_MIB_UPLOAD_NX))
		return true;
	return c->described && !c->described((u16)(req->pdu[4] << 8 | req->pdu[5]));
}

void gpon_olt_ploam_unknown_note(struct gpon_olt_capture *u, const struct gpon_olt_rec *ds)
{
	if (ds && ds->len > 1)
		table_put(u, ds, NULL, same_ploam_type);
}

void gpon_olt_capture_exchange(struct gpon_olt_capture *c, u64 t_us, u8 ostate,
			       const u8 *req, unsigned int req_len,
			       const u8 *resp, int resp_len)
{
	const struct gpon_olt_rec *q, *a = NULL;

	if (!c || !c->depth || !req || req_len < 10)
		return;
	gpon_olt_capture_add(c, t_us, ostate, GPON_OLT_DS, req, req_len);
	q = olt_last(c);
	/* An absent response is ABSENT.  Recording zeros for "the shell built
	 * none" is how a silence becomes a far-end finding. */
	if (resp && resp_len > 0) {
		gpon_olt_capture_add(c, t_us, ostate, GPON_OLT_US, resp,
				     (unsigned int)resp_len);
		a = olt_last(c);
	}
	gpon_olt_ledger_note(c->ledger, q, a);
	if (c->unknown && olt_unidentified(c, q, a))
		table_put(c->unknown, q, a, same_kind);
}

unsigned int gpon_olt_capture_count(const struct gpon_olt_capture *c)
{
	return c ? c->stored : 0;
}

const struct gpon_olt_rec *gpon_olt_capture_at(const struct gpon_olt_capture *c,
					       unsigned int i)
{
	unsigned int first;

	if (!c || i >= c->stored)
		return NULL;
	first = (c->head + c->depth - c->stored) % GPON_OLT_RECORDS;
	return &c->rec[(first + i) % c->depth];
}

int gpon_olt_diag_header(const struct gpon_olt_capture *c, char *out, size_t sz)
{
	if (!out || !sz)
		return 0;
	if (!c)
		return scnprintf(out, sz, "%s-hdr records=0 dropped=n/a", OLT_TAG);
	return scnprintf(out, sz, "%s-hdr records=%u dropped=%u depth=%u",
			 OLT_TAG, c->stored, c->dropped,
			 (unsigned int)c->depth);
}

int gpon_olt_diag_line(const struct gpon_olt_rec *r, char *out, size_t sz)
{
	unsigned int i;
	int pos;

	if (!out || !sz)
		return 0;
	out[0] = '\0';
	if (!r || !r->len)
		return 0;
	pos = scnprintf(out, sz, "%s %u %llu %c ", OLT_TAG, r->seq,
			(unsigned long long)r->t_us,
			r->dir == GPON_OLT_US ? 'u' : 'd');
	/* n/a is never 0: an O-state nobody established renders as '-'. */
	if (r->ostate == GPON_OLT_OSTATE_UNKNOWN || r->ostate > OSTATE_MAX)
		pos += scnprintf(out + pos, sz - pos, "- ");
	else
		pos += scnprintf(out + pos, sz - pos, "%u ", r->ostate);
	pos += scnprintf(out + pos, sz - pos, "%u %u ", r->len,
			 r->truncated ? 1u : 0u);
	for (i = 0; i < r->len && (size_t)pos + 2 < sz; i++) {
		out[pos++] = hexd[(r->pdu[i] >> 4) & 0xf];
		out[pos++] = hexd[r->pdu[i] & 0xf];
	}
	out[pos] = '\0';
	return pos;
}

static int nib(char ch)
{
	if (ch >= '0' && ch <= '9')
		return ch - '0';
	if (ch >= 'a' && ch <= 'f')
		return ch - 'a' + 10;
	return -1;
}

/* Decimal field ending at a single space.  -1 on anything else, so a missing
 * field can never be read as a zero. */
static int dec_u64(const char *s, unsigned int *at, u64 *out)
{
	unsigned int i = *at, digits = 0;
	u64 v = 0;

	while (s[i] >= '0' && s[i] <= '9') {
		u64 d = (u64)(s[i] - '0');

		if (v > ((u64)0xffffffffffffffffULL - d) / 10u)
			return -1;		/* a field that does not fit is
						 * rejected, never wrapped */
		v = v * 10u + d;
		i++;
		digits++;
	}
	if (!digits || s[i] != ' ')
		return -1;
	*at = i + 1;
	*out = v;
	return 0;
}

int gpon_olt_diag_parse(const char *line, struct gpon_olt_rec *r)
{
	struct gpon_olt_rec t;
	unsigned int at = 0, i;
	u64 v;

	if (!line || !r)
		return GPON_OLT_PARSE_TAG;
	for (i = 0; i < OLT_TAG_LEN; i++)
		if (line[i] != OLT_TAG[i])
			return GPON_OLT_PARSE_TAG;
	if (line[OLT_TAG_LEN] != ' ')
		return GPON_OLT_PARSE_TAG;
	at = OLT_TAG_LEN + 1;

	if (dec_u64(line, &at, &v) || v > 0xffffffffULL)
		return GPON_OLT_PARSE_FIELD;
	t.seq = (u32)v;
	if (dec_u64(line, &at, &v))
		return GPON_OLT_PARSE_FIELD;
	t.t_us = v;

	if (line[at] == 'd')
		t.dir = GPON_OLT_DS;
	else if (line[at] == 'u')
		t.dir = GPON_OLT_US;
	else
		return GPON_OLT_PARSE_DIR;
	if (line[at + 1] != ' ')
		return GPON_OLT_PARSE_DIR;
	at += 2;

	if (line[at] == '-' && line[at + 1] == ' ') {
		t.ostate = GPON_OLT_OSTATE_UNKNOWN;
		at += 2;
	} else {
		if (dec_u64(line, &at, &v))
			return GPON_OLT_PARSE_OSTATE;
		if (!v || v > OSTATE_MAX)
			return GPON_OLT_PARSE_OSTATE;
		t.ostate = (u8)v;
	}

	if (dec_u64(line, &at, &v))
		return GPON_OLT_PARSE_FIELD;
	if (v > GPON_OLT_PDU)
		return GPON_OLT_PARSE_LEN;
	t.len = (u8)v;
	if (dec_u64(line, &at, &v) || v > 1)
		return GPON_OLT_PARSE_FIELD;
	t.truncated = (u8)v;

	for (i = 0; i < t.len; i++) {
		int hi = nib(line[at + 2u * i]);
		int lo = nib(line[at + 2u * i + 1u]);

		if (hi < 0 || lo < 0)
			return GPON_OLT_PARSE_HEX;
		t.pdu[i] = (u8)((hi << 4) | lo);
	}
	for (; i < GPON_OLT_PDU; i++)
		t.pdu[i] = 0;
	at += 2u * t.len;
	if (line[at] != '\0' && line[at] != '\n' && line[at] != '\r')
		return GPON_OLT_PARSE_TRAIL;

	*r = t;			/* committed only once the whole line parsed */
	return GPON_OLT_PARSE_OK;
}
