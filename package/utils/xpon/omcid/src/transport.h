/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef OMCID_TRANSPORT_H
#define OMCID_TRANSPORT_H
#include <linux/genetlink.h>
#include <linux/netlink.h>
#include <linux/omci.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define OMCID_MESSAGE_MAX 8192
#define OMCID_BACKLOG_MAX 64
struct omcid_message { size_t len; unsigned char data[OMCID_MESSAGE_MAX] __attribute__((aligned(8))); };
struct omcid_transport {
	int fd;
	uint16_t family;
	uint32_t device, sequence, generation;
	struct omcid_message *pending[OMCID_BACKLOG_MAX];
	unsigned int head, count;
};
struct omcid_request {
	struct nlmsghdr header;
	struct genlmsghdr generic;
	unsigned char data[OMCID_MESSAGE_MAX - NLMSG_HDRLEN - GENL_HDRLEN];
};
void omcid_request_init(struct omcid_request *q, uint16_t family, uint8_t command);
int omcid_put(struct omcid_request *q, uint16_t type, const void *data, size_t len);
int omcid_parse(const struct nlmsghdr *n, const struct nlattr **tb, unsigned int max);
int omcid_scalar(const struct nlattr *a, void *value, size_t size);
int omcid_rpc(struct omcid_transport *t, struct omcid_request *q, bool reply, struct omcid_message *out);
int omcid_transport_open(struct omcid_transport *t, uint32_t device);
void omcid_transport_close(struct omcid_transport *t);
int omcid_transport_next(struct omcid_transport *t, struct omcid_message *message, int timeout);
static inline const void *omcid_data(const struct nlattr *a) { return (const char *)a + NLA_HDRLEN; }
static inline size_t omcid_length(const struct nlattr *a) { return a->nla_len - NLA_HDRLEN; }
#endif
