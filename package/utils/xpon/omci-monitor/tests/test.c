// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include "../src/netlink.h"
#include "../src/wire.h"

static unsigned int checks;
#define CHECK(x) do { assert(x); checks++; } while (0)

static void wire_tests(void)
{
	unsigned char pdu[OMCI_MAX_PDU_LEN + 4] = { 0x81, 0x23, 0x49, 0x0a, 1, 12, 1, 0 };
	unsigned char response[OMCI_MAX_PDU_LEN + 4];
	struct omci_wire_request r;
	size_t size = 0;
	unsigned int i;

	CHECK(omci_wire_decode(NULL, 48, &r) == -EINVAL);
	CHECK(omci_wire_decode(pdu, 48, NULL) == -EINVAL);
	for (i = 0; i <= sizeof(pdu); i++) {
		int ret = omci_wire_decode(pdu, i, &r);

		CHECK((ret == 0) == (i == 44 || i == 48));
	}
	CHECK(!omci_wire_decode(pdu, 48, &r));
	CHECK(r.transaction_id == 0x8123 && r.message_type == 0x49);
	CHECK(r.class_id == 268 && r.entity_id == 256 && r.payload_len == 32);
	CHECK(!omci_wire_encode_response(&r, 9, "\0\x80\0\x12", 4,
					 response, sizeof(response), &size));
	CHECK(size == 44 && response[2] == 0x29 && response[43] == 40);
	CHECK(!memcmp(response, pdu, 2) && !memcmp(response + 4, pdu + 4, 4));
	CHECK(!memcmp(response + 8, "\0\x80\0\x12", 4));
	CHECK(omci_wire_encode_response(&r, 9, pdu, 33, response, sizeof(response), &size) == -EMSGSIZE);
	CHECK(omci_wire_encode_response(&r, 9, NULL, 1, response, sizeof(response), &size) == -EINVAL);
	CHECK(omci_wire_encode_response(&r, 9, NULL, 0, response, 43, &size) == -EMSGSIZE);
	pdu[3] = 0x0b;
	for (i = 0; i <= OMCI_MAX_PDU_LEN; i++) {
		pdu[8] = i >> 8;
		pdu[9] = i;
		CHECK((omci_wire_decode(pdu, 10 + i, &r) == 0) == (i <= OMCI_MAX_PDU_LEN - 10));
	}
	pdu[8] = 0; pdu[9] = 7;
	CHECK(!omci_wire_decode(pdu, 17, &r));
	CHECK(!omci_wire_decode(pdu, 21, &r));
	CHECK(omci_wire_decode(pdu, 20, &r) == -EMSGSIZE);
	CHECK(omci_wire_decode(pdu, 16, &r) == -EMSGSIZE);
	CHECK(!omci_wire_encode_response(&r, 9, pdu, 7, response, sizeof(response), &size));
	CHECK(size == 17 && response[8] == 0 && response[9] == 7);
	pdu[3] = 0;
	CHECK(omci_wire_decode(pdu, 48, &r) == -EPROTO);
	CHECK(omci_wire_encode_response(&r, 9, NULL, 0, response, sizeof(response), &size) == -EPROTO);
}

struct message {
	struct nlmsghdr n;
	struct genlmsghdr g;
	unsigned char attrs[4096];
};

static void add(struct message *m, unsigned int type, const void *data, size_t len)
{
	struct nlattr *a = (void *)m + m->n.nlmsg_len;

	a->nla_type = type;
	a->nla_len = NLA_HDRLEN + len;
	memcpy((char *)a + NLA_HDRLEN, data, len);
	m->n.nlmsg_len += NLA_ALIGN(a->nla_len);
}

static void netlink_tests(void)
{
	struct message m = { .n.nlmsg_len = NLMSG_HDRLEN + GENL_HDRLEN, .g.cmd = OMCI_CMD_RX };
	struct monitor_record r;
	uint32_t generation = 9, flags = OMCI_F_MIC_VALID;
	uint64_t sequence = 12;
	unsigned char pdu[48] = { 0, 1, 0x49, 0x0a };
	unsigned int full, i;

	CHECK(monitor_parse(&m.n, &r) == -EPROTO);
	add(&m, OMCI_ATTR_GENERATION, &generation, sizeof(generation));
	CHECK(monitor_parse(&m.n, &r) == -EBADMSG);
	add(&m, OMCI_ATTR_SEQUENCE, &sequence, sizeof(sequence));
	add(&m, OMCI_ATTR_FLAGS, &flags, sizeof(flags));
	add(&m, OMCI_ATTR_PDU, pdu, sizeof(pdu));
	add(&m, OMCI_ATTR_PAD, &flags, 0);
	add(&m, OMCI_ATTR_PAD, &flags, 0);
	CHECK(!monitor_parse(&m.n, &r));
	CHECK(r.generation == 9 && r.sequence == 12 && r.flags == OMCI_F_MIC_VALID);
	CHECK(r.pdu_len == 48 && !memcmp(r.pdu, pdu, 48));
	m.g.cmd = OMCI_CMD_GET;
	CHECK(!monitor_parse(&m.n, &r));
	m.g.cmd = OMCI_CMD_RX;
	full = m.n.nlmsg_len;
	for (i = 0; i < NLMSG_HDRLEN + GENL_HDRLEN; i++) {
		m.n.nlmsg_len = i;
		CHECK(monitor_parse(&m.n, &r) == -EBADMSG);
	}
	m.n.nlmsg_len = full;
	add(&m, OMCI_ATTR_GENERATION, &generation, sizeof(generation));
	CHECK(monitor_parse(&m.n, &r) == -EBADMSG);
	m.n.nlmsg_len = full;
	((struct nlattr *)m.attrs)->nla_len = NLA_HDRLEN + 1;
	CHECK(monitor_parse(&m.n, &r) == -EBADMSG);
	((struct nlattr *)m.attrs)->nla_len = 0xffff;
	CHECK(monitor_parse(&m.n, &r) == -EBADMSG);
	((struct nlattr *)m.attrs)->nla_len = NLA_HDRLEN + 4;
	m.n.nlmsg_len--;
	CHECK(monitor_parse(&m.n, &r) == -EBADMSG);
}

int main(void)
{
	wire_tests();
	netlink_tests();
	printf("%u OMCI wire/transport checks passed\n", checks);
	return 0;
}
