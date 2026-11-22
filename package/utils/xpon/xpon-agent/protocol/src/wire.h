/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _NET_OMCI_WIRE_H
#define _NET_OMCI_WIRE_H

#include <linux/types.h>
#ifndef __KERNEL__
#include <stddef.h>
#endif

#define OMCI_BASELINE_DEV_ID		0x0a
#define OMCI_EXTENDED_DEV_ID		0x0b
#define OMCI_BASELINE_LEN		48
#define OMCI_BASELINE_LEN_NO_MIC	44
#define OMCI_EXTENDED_HEADER_LEN	10
#define OMCI_MIC_LEN			4

struct omci_wire_request {
	__u16 transaction_id;
	__u16 class_id;
	__u16 entity_id;
	__u16 payload_len;
	__u8 message_type;
	__u8 device_id;
	const __u8 *payload;
};

int omci_wire_decode(const void *data, size_t len,
		     struct omci_wire_request *request);
int omci_wire_encode_response(const struct omci_wire_request *request,
			      __u8 action, const void *payload,
			      size_t payload_len, void *data,
			      size_t capacity, size_t *encoded_len);

#endif /* _NET_OMCI_WIRE_H */
