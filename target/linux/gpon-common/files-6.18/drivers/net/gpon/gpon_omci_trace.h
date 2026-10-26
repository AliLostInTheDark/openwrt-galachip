/* SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef GPON_OMCI_TRACE_H
#define GPON_OMCI_TRACE_H

#include <linux/types.h>

/* Baseline OMCI PDU (G.988, all big-endian byte math) */
#define GPON_OMCI_MIN_HDR	8

/* G.988 Table 11.2.2-1.  "?" for a message type this table does not name --
 * never NULL, so no caller can forget the check. */
const char *gpon_omci_mt_name(u8 msg_type);

/* One line describing a PDU */
int gpon_omci_describe(const u8 *pdu, unsigned int len, char *out, size_t sz);

/* Which request is this?  Exposed because a caller branches on the type before
 * deciding which detail it may read out of the request -- a Create's ATTRIBUTE
 * BODY starts at octet 8 where a Get and a Set put a mask.  False for a runt. */
bool gpon_omci_is_get(const u8 *pdu, unsigned int len);
bool gpon_omci_is_set(const u8 *pdu, unsigned int len);
bool gpon_omci_is_create(const u8 *pdu, unsigned int len);

/* Does a baseline RESPONSE to this request carry a */
bool gpon_omci_has_result_code(const u8 *pdu, unsigned int len);

/* Is this one of the BULK message types */
bool gpon_omci_is_bulk(const u8 *pdu, unsigned int len);

/* Is this PDU a Create for @me_class? The CLASS is */
bool gpon_omci_is_create_of(const u8 *pdu, unsigned int len, u16 me_class);

/* The body of a baseline PDU and its length, CLAMPED to */
const u8 *gpon_omci_body(const u8 *pdu, unsigned int len, u8 *blen);

#endif /* GPON_OMCI_TRACE_H */
