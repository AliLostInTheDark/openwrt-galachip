/* SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef GPON_OLT_DIAG_H
#define GPON_OLT_DIAG_H

#include <linux/types.h>

/* One baseline G.988 PDU, local so this header needs no ME model to include. */
#define GPON_OLT_PDU		48

/* Records per ring (CONFIG_GPON_OLT_DIAG_RECORDS; 256 when the host builds the
 * core).  A field provisioning was 680: an overflow is loud (`dropped`). */
#ifdef CONFIG_GPON_OLT_DIAG_RECORDS
#define GPON_OLT_RECORDS	CONFIG_GPON_OLT_DIAG_RECORDS
#else
#define GPON_OLT_RECORDS	256
#endif

/* The tables beside the ring, a fixed size each: one entry per kind, so the OLT's periodic traffic
 * cannot flood them, and a full one keeps the first and counts the rest in `dropped`. */
#define GPON_OLT_LEDGER_RECORDS		256
#define GPON_OLT_UNKNOWN_RECORDS	64
#define GPON_OLT_PLOAM_UNKNOWN_RECORDS	32

/* COULD NOT ASK.  `enum gpon_ostate` starts at 1, so 0 is free; renders `-`. */
#define GPON_OLT_OSTATE_UNKNOWN	0

enum gpon_olt_dir {
	GPON_OLT_DS = 0,	/* OLT -> ONU */
	GPON_OLT_US = 1,	/* ONU -> OLT */
};

/* Why a line could not be parsed -- a reason, never a bare failure. */
enum gpon_olt_parse_err {
	GPON_OLT_PARSE_OK	= 0,
	GPON_OLT_PARSE_TAG	= -1,	/* not our format, or no version tag  */
	GPON_OLT_PARSE_FIELD	= -2,	/* a field missing or not a number    */
	GPON_OLT_PARSE_DIR	= -3,	/* direction is neither 'd' nor 'u'   */
	GPON_OLT_PARSE_OSTATE	= -4,	/* not '-' and not O1..O7             */
	GPON_OLT_PARSE_LEN	= -5,	/* length above GPON_OLT_PDU          */
	GPON_OLT_PARSE_HEX	= -6,	/* payload not 2*len hex digits       */
	GPON_OLT_PARSE_TRAIL	= -7,	/* something after the payload        */
};

struct gpon_olt_rec {
	u64 t_us;		/* EXPLICIT INPUT. This tier never reads a clock */
	u32 seq;		/* monotonic within a capture, survives the ring  */
	u8  dir;		/* enum gpon_olt_dir                             */
	u8  ostate;		/* enum gpon_ostate, or GPON_OLT_OSTATE_UNKNOWN  */
	u8  len;		/* bytes STORED, <= GPON_OLT_PDU                 */
	u8  truncated;		/* the wire frame was longer than we store       */
	u8  pdu[GPON_OLT_PDU];
};

/* Caller-owned: the core allocates nothing, and the owner serializes access. */
struct gpon_olt_capture {
	struct gpon_olt_rec *rec;	/* caller-owned storage of `depth` records */
	u32 depth;
	u32 head;		/* next slot to write                         */
	u32 stored;		/* records currently held, <= GPON_OLT_RECORDS */
	u32 seq;		/* next sequence number                        */
	u32 dropped;		/* overwritten by the ring -- never silent     */
	/* Fed by gpon_olt_capture_exchange(), or NULL: the last Create/Set per (class,
	 * instance, mask) with our answer, and the last exchange of each kind this ONU
	 * did not identify (no answer, an error, or a class it holds no model of). */
	struct gpon_olt_capture *ledger;
	struct gpon_olt_capture *unknown;
	bool (*described)(u16 class_id);	/* the ME model; NULL counts every class as modelled */
};

/* A static capture with its storage: GPON_OLT_CAPTURE(x, n, .ledger = &y); */
#define GPON_OLT_CAPTURE(name, n, ...)						\
	static struct gpon_olt_rec name##_rec[n];				\
	static struct gpon_olt_capture name = { .rec = name##_rec, .depth = (n), ##__VA_ARGS__ }

void gpon_olt_capture_reset(struct gpon_olt_capture *c);

/* @len above GPON_OLT_PDU stores the first GPON_OLT_PDU and marks it cut. */
void gpon_olt_capture_add(struct gpon_olt_capture *c, u64 t_us, u8 ostate,
			  enum gpon_olt_dir dir, const u8 *pdu,
			  unsigned int len);

/* One exchange at one timestamp.  @resp_len <= 0 means the shell built no
 * response: the request is recorded ALONE, never an upstream record of zeros. */
void gpon_olt_capture_exchange(struct gpon_olt_capture *c, u64 t_us, u8 ostate,
			       const u8 *req, unsigned int req_len,
			       const u8 *resp, int resp_len);

void gpon_olt_ledger_note(struct gpon_olt_capture *l, const struct gpon_olt_rec *req,
			  const struct gpon_olt_rec *resp);
/* A downstream PLOAM this ONU does not handle, kept per type. */
void gpon_olt_ploam_unknown_note(struct gpon_olt_capture *u, const struct gpon_olt_rec *ds);

/* Oldest-first, so a reader walks the conversation in order. */
unsigned int gpon_olt_capture_count(const struct gpon_olt_capture *c);
const struct gpon_olt_rec *gpon_olt_capture_at(const struct gpon_olt_capture *c,
					       unsigned int i);

/* ONE record as one text line, no trailing newline.  Returns the count written,
 * 0 for an empty record (a table entry the shell answered with nothing).
 *
 * Format (positional, version-tagged, lossless):
 *	olt1 <seq> <t_us> <d|u> <ostate|-> <len> <0|1> <2*len hex>
 */
int gpon_olt_diag_line(const struct gpon_olt_rec *r, char *out, size_t sz);

/* The inverse.  @r is UNTOUCHED on any error, so a rejected line cannot reach
 * a replay half-filled. */
int gpon_olt_diag_parse(const char *line, struct gpon_olt_rec *r);

/* The header line a dump starts with: how complete the capture is. */
int gpon_olt_diag_header(const struct gpon_olt_capture *c, char *out, size_t sz);

#endif /* GPON_OLT_DIAG_H */
