/* SPDX-License-Identifier: GPL-2.0-or-later */
/* TIER: CORE, strict host-buildable subset -- G.988 byte math ...
 * dev/MEASURED-gpon_omci_trace.h.md sec 1. */
#ifndef GPON_OMCI_TRACE_H
#define GPON_OMCI_TRACE_H

#include <linux/types.h>

/* Baseline OMCI PDU (G.988, all big-endian byte math): [0:1] ...
 * dev/MEASURED-gpon_omci_trace.h.md sec 2. */
#define GPON_OMCI_MIN_HDR	8

/* G.988 Table 11.2.2-1.  "?" for a message type this table does not name --
 * never NULL, so no caller can forget the check. */
const char *gpon_omci_mt_name(u8 msg_type);

/* One line describing a PDU: length, TCI, message type + its ...
 * dev/MEASURED-gpon_omci_trace.h.md sec 3. */
int gpon_omci_describe(const u8 *pdu, unsigned int len, char *out, size_t sz);

/* Which request is this?  Exposed because a caller branches on the type before
 * deciding which detail it may read out of the request -- a Create's ATTRIBUTE
 * BODY starts at octet 8 where a Get and a Set put a mask.  False for a runt. */
bool gpon_omci_is_get(const u8 *pdu, unsigned int len);
bool gpon_omci_is_set(const u8 *pdu, unsigned int len);
bool gpon_omci_is_create(const u8 *pdu, unsigned int len);

/* Does a baseline RESPONSE to this request carry a RESULT ...
 * dev/MEASURED-gpon_omci_trace.h.md sec 4. */
bool gpon_omci_has_result_code(const u8 *pdu, unsigned int len);

/* Is this one of the BULK message types -- the ones an OLT ...
 * dev/MEASURED-gpon_omci_trace.h.md sec 5. */
bool gpon_omci_is_bulk(const u8 *pdu, unsigned int len);

/* Is this PDU a Create for @me_class? The CLASS is the ...
 * dev/MEASURED-gpon_omci_trace.h.md sec 6. */
bool gpon_omci_is_create_of(const u8 *pdu, unsigned int len, u16 me_class);

/* The body of a baseline PDU and its length, CLAMPED to the ...
 * dev/MEASURED-gpon_omci_trace.h.md sec 7. */
const u8 *gpon_omci_body(const u8 *pdu, unsigned int len, u8 *blen);

#endif /* GPON_OMCI_TRACE_H */
